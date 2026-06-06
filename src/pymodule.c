#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

#include "fastcsv.h"
#include "mmap_io.h"
#include "parser.h"
#include "columnar.h"
#include "simd.h"

static void set_csv_error(int err) {
    switch (err) {
        case CSV_ERR_OOM: 
            PyErr_NoMemory(); 
            break;
        case CSV_ERR_IO: 
            PyErr_SetString(PyExc_IOError, "IO error"); 
            break;
        case CSV_ERR_MALFORMED: 
            PyErr_SetString(PyExc_ValueError, "malformed CSV: unclosed quote"); 
            break;
        case CSV_ERR_OVERFLOW: 
            PyErr_SetString(PyExc_ValueError, "field exceeds max_field_len"); 
            break;
        default: 
            PyErr_SetString(PyExc_RuntimeError, "unknown error"); 
            break;
    }
}

static PyObject *table_to_dict(ColumnarTable *t, PyObject *header_names) {
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;
    
    npy_intp dims[1] = { t->num_rows };
    
    for (uint32_t i = 0; i < t->num_cols; i++) {
        Column *c = &t->cols[i];
        PyObject *key = NULL;
        
        if (header_names && i < (uint32_t)PyList_Size(header_names)) {
            key = PyList_GetItem(header_names, i);
            Py_INCREF(key);
        } else {
            char name[32];
            snprintf(name, sizeof(name), "col_%u", i);
            key = PyUnicode_FromString(name);
        }
        
        PyObject *arr = NULL;
        if (c->type == COL_TYPE_INT) {
            arr = PyArray_SimpleNewFromData(1, dims, NPY_INT64, c->int_data);
            if (arr) {
                PyArray_ENABLEFLAGS((PyArrayObject *)arr, NPY_ARRAY_OWNDATA);
                c->int_data = NULL;
            }
        } else if (c->type == COL_TYPE_FLOAT) {
            arr = PyArray_SimpleNewFromData(1, dims, NPY_DOUBLE, c->float_data);
            if (arr) {
                PyArray_ENABLEFLAGS((PyArrayObject *)arr, NPY_ARRAY_OWNDATA);
                c->float_data = NULL;
            }
        } else {
            arr = PyArray_SimpleNew(1, dims, NPY_OBJECT);
            if (arr) {
                PyObject **ptr = (PyObject **)PyArray_DATA((PyArrayObject *)arr);
                for (uint64_t r = 0; r < t->num_rows; r++) {
                    uint32_t offset = c->str_offsets[r];
                    uint32_t len = c->str_lens[r];
                    PyObject *str_obj = PyUnicode_FromStringAndSize(c->str_arena + offset, len);
                    if (!str_obj) {
                        Py_DECREF(arr);
                        Py_DECREF(key);
                        Py_DECREF(dict);
                        return NULL;
                    }
                    ptr[r] = str_obj;
                }
            }
        }
        
        if (!arr) {
            Py_DECREF(key);
            Py_DECREF(dict);
            return NULL;
        }
        
        PyDict_SetItem(dict, key, arr);
        Py_DECREF(key);
        Py_DECREF(arr);
    }
    return dict;
}

#ifdef _WIN32
#include <windows.h>
static int get_nproc() {
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
}
#else
#include <pthread.h>
#include <unistd.h>
static int get_nproc() {
    int n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 4;
}
#endif

typedef struct {
    const char *buf;
    size_t len;
    size_t *row_offsets;
    uint64_t start_row;
    uint64_t end_row;
    uint64_t total_rows;
    uint64_t **col_buffers;
    ColType *local_types;
    uint32_t num_cols;
    char delim;
    char quote;
} ParseThreadCtx;

#ifdef _WIN32
static DWORD WINAPI parse_thread(LPVOID arg) {
#else
static void *parse_thread(void *arg) {
#endif
    ParseThreadCtx *ctx = (ParseThreadCtx *)arg;
    for (uint64_t r = ctx->start_row; r < ctx->end_row; r++) {
        size_t pos = ctx->row_offsets[r];
        size_t end_pos = (r + 1 < ctx->total_rows) ? ctx->row_offsets[r+1] : ctx->len;
        
        size_t actual_end = end_pos;
        if (actual_end > pos && ctx->buf[actual_end - 1] == '\n') actual_end--;
        if (actual_end > pos && ctx->buf[actual_end - 1] == '\r') actual_end--;
        
        uint32_t c = 0;
        int quoted = 0;
        size_t field_start = pos;
        
        struct { uint32_t start; uint32_t len; } batch[64];
        int batch_cnt = 0;
        int restarted = 0;
        
#define PROCESS_BATCH() do { \
    for (int i=0; i<batch_cnt; i++) { \
        if (c >= ctx->num_cols) { c++; continue; } \
        size_t fstart = batch[i].start; \
        size_t fl = batch[i].len; \
        int is_quoted = (fl >= 2 && ctx->buf[fstart] == ctx->quote && ctx->buf[fstart+fl-1] == ctx->quote); \
        if (is_quoted) { fstart++; fl -= 2; } \
        CsvField f = { .data = ctx->buf + fstart, .len = fl, .quoted = is_quoted }; \
        ColType t = ctx->local_types[c]; \
        ColType new_t = t; \
        type_infer_update(&new_t, &f); \
        if (new_t != t) { \
            if (new_t == COL_TYPE_FLOAT) { \
                for (uint64_t j = ctx->start_row; j < r; j++) { \
                    double d_val = (double)(int64_t)ctx->col_buffers[c][j]; \
                    memcpy(&ctx->col_buffers[c][j], &d_val, 8); \
                } \
            } else if (new_t == COL_TYPE_STR) { \
                ctx->local_types[c] = COL_TYPE_STR; \
                restarted = 1; break; \
            } \
            ctx->local_types[c] = new_t; \
            t = new_t; \
        } \
        if (t == COL_TYPE_INT) { \
            int64_t val = 0; type_infer_parse(t, &f, &val, NULL, NULL); \
            ctx->col_buffers[c][r] = val; \
        } else if (t == COL_TYPE_FLOAT) { \
            double val = 0.0; type_infer_parse(t, &f, NULL, &val, NULL); \
            memcpy(&ctx->col_buffers[c][r], &val, 8); \
        } else { \
            ctx->col_buffers[c][r] = ((uint64_t)fstart << 30) | (fl & 0x3FFFFFFF); \
        } \
        c++; \
    } \
    batch_cnt = 0; \
} while(0)

        while (pos < actual_end) {
            int w = fastcsv_simd_width();
            if (pos + w <= actual_end) {
                int width;
                uint32_t mask = fastcsv_scan_chunk(ctx->buf + pos, ctx->delim, ctx->quote, &width);
                if (mask == 0) { pos += width; continue; }
                while (mask != 0) {
                    int bit = __builtin_ctz(mask);
                    if (pos + bit >= actual_end) break;
                    char ch = ctx->buf[pos + bit];
                    if (ch == ctx->quote) {
                        quoted = !quoted;
                    } else if (!quoted && ch == ctx->delim) {
                        batch[batch_cnt].start = field_start;
                        batch[batch_cnt].len = (pos + bit) - field_start;
                        batch_cnt++;
                        field_start = pos + bit + 1;
                        if (batch_cnt == 64) {
                            PROCESS_BATCH();
                        }
                    }
                    mask &= mask - 1;
                }
                if (restarted) break;
                pos += width;
            } else {
                char ch = ctx->buf[pos];
                if (ch == ctx->quote) quoted = !quoted;
                else if (!quoted && ch == ctx->delim) {
                    batch[batch_cnt].start = field_start;
                    batch[batch_cnt].len = pos - field_start;
                    batch_cnt++;
                    field_start = pos + 1;
                    if (batch_cnt == 64) {
                        PROCESS_BATCH();
                    }
                }
                pos++;
            }
        }
        if (restarted) { r = ctx->start_row - 1; continue; }
        
        batch[batch_cnt].start = field_start;
        batch[batch_cnt].len = actual_end - field_start;
        batch_cnt++;
        PROCESS_BATCH();
        if (restarted) { r = ctx->start_row - 1; continue; }
#undef PROCESS_BATCH
    }
    return 0;
}

static PyObject *fastcsv_read_csv(PyObject *self, PyObject *args, PyObject *kwds) {
    (void)self;
    const char *path;
    const char *delimiter_str = ",";
    int has_header = 1;
    const char *error_mode_str = "strict";
    int raw_mode = 0;
    
    static char *kwlist[] = {"path", "delimiter", "has_header", "error_mode", "raw", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|spsp", kwlist,
                                     &path, &delimiter_str, &has_header, &error_mode_str, &raw_mode)) {
        return NULL;
    }
    
    if (strlen(delimiter_str) != 1) {
        PyErr_SetString(PyExc_ValueError, "delimiter must be exactly 1 char");
        return NULL;
    }
    
    CsvErrorMode error_mode;
    if (strcmp(error_mode_str, "strict") == 0) error_mode = CSV_ON_ERROR_STRICT;
    else if (strcmp(error_mode_str, "skip") == 0) error_mode = CSV_ON_ERROR_SKIP;
    else if (strcmp(error_mode_str, "replace") == 0) error_mode = CSV_ON_ERROR_REPLACE;
    else {
        PyErr_SetString(PyExc_ValueError, "error_mode must be 'strict', 'skip', or 'replace'");
        return NULL;
    }
    
    CsvOptions opts = csv_default_options();
    opts.delimiter = delimiter_str[0];
    opts.error_mode = error_mode;
    
    CsvParser *p = NULL;
    PyObject *header_names = NULL;
    PyObject *dict = NULL;
    size_t *row_offsets = NULL;
    uint64_t total_rows = 0;
    uint64_t **col_buffers = NULL;
    ParseThreadCtx *threads = NULL;
    int nproc = 0;
    int header_consumed = 0;
    
#ifdef _WIN32
    HANDLE *handles = NULL;
#else
    pthread_t *pthreads = NULL;
#endif

    p = csv_parser_from_file(path, opts);
    if (!p) {
        PyErr_SetFromErrnoWithFilename(PyExc_IOError, path);
        goto cleanup;
    }
    
    const char *buf = p->buf;
    size_t len = p->len;
    if (len >= 3 && (unsigned char)buf[0] == 0xef && (unsigned char)buf[1] == 0xbb && (unsigned char)buf[2] == 0xbf) {
        buf += 3;
        len -= 3;
    }
    
    Py_BEGIN_ALLOW_THREADS
    total_rows = fastcsv_find_row_offsets(buf, len, opts.delimiter, opts.quote_char, &row_offsets);
    Py_END_ALLOW_THREADS

    if (total_rows == 0) {
        dict = PyDict_New();
        goto cleanup;
    }
    
    CsvRow row;
    int res = csv_next_row(p, &row);
    if (res < 0) { set_csv_error(res); goto cleanup; }
    if (res == 1) { dict = PyDict_New(); goto cleanup; }
    
    uint32_t num_cols = row.num_fields;
    
    if (has_header) {
        header_names = PyList_New(num_cols);
        if (!header_names) goto cleanup;
        for (uint32_t i = 0; i < num_cols; i++) {
            PyObject *str_obj = PyUnicode_FromStringAndSize(row.fields[i].data, row.fields[i].len);
            if (!str_obj) goto cleanup;
            PyList_SET_ITEM(header_names, i, str_obj);
        }
        total_rows--; // header consumed
        row_offsets++; // advance row_offsets pointer
        header_consumed = 1;
    }
    
    if (total_rows == 0) {
        dict = PyDict_New();
        goto cleanup;
    }
    
    col_buffers = malloc(num_cols * sizeof(uint64_t *));
    if (!col_buffers) { PyErr_NoMemory(); goto cleanup; }
    for (uint32_t c = 0; c < num_cols; c++) {
        col_buffers[c] = PyDataMem_NEW(total_rows * sizeof(uint64_t));
        if (!col_buffers[c]) { PyErr_NoMemory(); goto cleanup; }
    }
    
    nproc = get_nproc();
    if (nproc > 32) nproc = 32;
    if (total_rows < (uint64_t)nproc * 100) nproc = 1;
    
    threads = calloc(nproc, sizeof(ParseThreadCtx));
    uint64_t rows_per_thread = total_rows / nproc;

#ifdef _WIN32
    handles = malloc(nproc * sizeof(HANDLE));
#else
    pthreads = malloc(nproc * sizeof(pthread_t));
#endif

    for (int t = 0; t < nproc; t++) {
        threads[t].buf = buf;
        threads[t].len = len;
        threads[t].row_offsets = row_offsets;
        threads[t].start_row = t * rows_per_thread;
        threads[t].end_row = (t == nproc - 1) ? total_rows : (t + 1) * rows_per_thread;
        threads[t].total_rows = total_rows;
        threads[t].col_buffers = col_buffers;
        threads[t].local_types = malloc(num_cols * sizeof(ColType));
        for (uint32_t c = 0; c < num_cols; c++) threads[t].local_types[c] = COL_TYPE_INT;
        threads[t].num_cols = num_cols;
        threads[t].delim = opts.delimiter;
        threads[t].quote = opts.quote_char;
    }

    Py_BEGIN_ALLOW_THREADS
    for (int t = 0; t < nproc; t++) {
#ifdef _WIN32
        handles[t] = CreateThread(NULL, 0, parse_thread, &threads[t], 0, NULL);
#else
        pthread_create(&pthreads[t], NULL, parse_thread, &threads[t]);
#endif
    }
#ifdef _WIN32
    WaitForMultipleObjects(nproc, handles, TRUE, INFINITE);
#else
    for (int t = 0; t < nproc; t++) pthread_join(pthreads[t], NULL);
#endif
    Py_END_ALLOW_THREADS

    ColType *global_types = malloc(num_cols * sizeof(ColType));
    for (uint32_t c = 0; c < num_cols; c++) {
        global_types[c] = COL_TYPE_INT;
        for (int t = 0; t < nproc; t++) {
            if (threads[t].local_types[c] > global_types[c]) {
                global_types[c] = threads[t].local_types[c];
            }
        }
        if (global_types[c] == COL_TYPE_FLOAT) {
            for (int t = 0; t < nproc; t++) {
                if (threads[t].local_types[c] == COL_TYPE_INT) {
                    for (uint64_t r = threads[t].start_row; r < threads[t].end_row; r++) {
                        double d = (double)(int64_t)col_buffers[c][r];
                        memcpy(&col_buffers[c][r], &d, 8);
                    }
                }
            }
        }
    }
    
    dict = PyDict_New();
    for (uint32_t c = 0; c < num_cols; c++) {
        PyObject *key = NULL;
        if (header_names && c < (uint32_t)PyList_Size(header_names)) {
            key = PyList_GetItem(header_names, c);
            Py_INCREF(key);
        } else {
            char name[32]; snprintf(name, sizeof(name), "col_%u", c);
            key = PyUnicode_FromString(name);
        }
        
        PyObject *arr = NULL;
        npy_intp dims[1] = { total_rows };
        if (global_types[c] == COL_TYPE_INT) {
            arr = PyArray_SimpleNewFromData(1, dims, NPY_INT64, col_buffers[c]);
            PyArray_ENABLEFLAGS((PyArrayObject *)arr, NPY_ARRAY_OWNDATA);
            col_buffers[c] = NULL;
        } else if (global_types[c] == COL_TYPE_FLOAT) {
            arr = PyArray_SimpleNewFromData(1, dims, NPY_DOUBLE, col_buffers[c]);
            PyArray_ENABLEFLAGS((PyArrayObject *)arr, NPY_ARRAY_OWNDATA);
            col_buffers[c] = NULL;
        } else {
            arr = PyArray_SimpleNew(1, dims, NPY_OBJECT);
            PyObject **obj_data = (PyObject **)PyArray_DATA((PyArrayObject *)arr);
            for (uint64_t r = 0; r < total_rows; r++) {
                int t_idx = 0;
                for (int t = 0; t < nproc; t++) {
                    if (r >= threads[t].start_row && r < threads[t].end_row) { t_idx = t; break; }
                }
                ColType t_type = threads[t_idx].local_types[c];
                if (t_type == COL_TYPE_INT) {
                    char temp[32];
                    int n = snprintf(temp, sizeof(temp), "%ld", (long)(int64_t)col_buffers[c][r]);
                    obj_data[r] = raw_mode ? PyBytes_FromStringAndSize(temp, n) : PyUnicode_FromStringAndSize(temp, n);
                } else if (t_type == COL_TYPE_FLOAT) {
                    char temp[64];
                    double d; memcpy(&d, &col_buffers[c][r], 8);
                    int n = snprintf(temp, sizeof(temp), "%g", d);
                    obj_data[r] = raw_mode ? PyBytes_FromStringAndSize(temp, n) : PyUnicode_FromStringAndSize(temp, n);
                } else {
                    uint64_t packed = col_buffers[c][r];
                    uint32_t offset = packed >> 30;
                    uint32_t flen = packed & 0x3FFFFFFF;
                    int has_quote = (memchr(buf + offset, opts.quote_char, flen) != NULL);
                    if (!has_quote) {
                        obj_data[r] = raw_mode ? PyBytes_FromStringAndSize(buf + offset, flen) : PyUnicode_FromStringAndSize(buf + offset, flen);
                    } else {
                        char *esc = malloc(flen);
                        size_t esc_len = 0;
                        for(size_t i=0; i<flen; i++) {
                            esc[esc_len++] = buf[offset+i];
                            if (buf[offset+i] == opts.quote_char && i+1 < flen && buf[offset+i+1] == opts.quote_char) i++;
                        }
                        obj_data[r] = raw_mode ? PyBytes_FromStringAndSize(esc, esc_len) : PyUnicode_FromStringAndSize(esc, esc_len);
                        free(esc);
                    }
                }
            }
            PyDataMem_FREE(col_buffers[c]);
            col_buffers[c] = NULL;
        }
        PyDict_SetItem(dict, key, arr);
        Py_DECREF(key);
        Py_DECREF(arr);
    }
    free(global_types);

cleanup:
    if (threads) {
        for (int t = 0; t < nproc; t++) free(threads[t].local_types);
        free(threads);
    }
#ifdef _WIN32
    if (handles) free(handles);
#else
    if (pthreads) free(pthreads);
#endif
    if (col_buffers) {
        for (uint32_t c = 0; c < num_cols; c++) {
            if (col_buffers[c]) PyDataMem_FREE(col_buffers[c]);
        }
        free(col_buffers);
    }
    if (header_consumed && row_offsets) row_offsets--; // restore pointer for free
    if (row_offsets) free(row_offsets);
    if (p) csv_parser_free(p);
    Py_XDECREF(header_names);
    return dict;
}

typedef struct {
    PyObject_HEAD
    CsvParser *p;
    PyObject *header_names;
    int chunk_size;
    int cols_guess;
} CsvReaderObject;

static void CsvReader_dealloc(CsvReaderObject *self) {
    if (self->p) {
        csv_parser_free(self->p);
    }
    Py_XDECREF(self->header_names);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *CsvReader_iternext(CsvReaderObject *self) {
    if (!self->p) {
        return NULL;
    }
    
    ColumnarTable *t = columnar_new(self->cols_guess, self->chunk_size);
    if (!t && self->cols_guess > 0) {
        return PyErr_NoMemory();
    }
    
    CsvRow row;
    int res = CSV_OK;
    int parsed_rows = 0;
    
    Py_BEGIN_ALLOW_THREADS
    while (parsed_rows < self->chunk_size && (res = csv_next_row(self->p, &row)) == CSV_OK) {
        if (t->num_cols == 0 && row.num_fields > 0) {
            columnar_free(t);
            t = columnar_new(row.num_fields, self->chunk_size);
            if (!t) {
                res = CSV_ERR_OOM;
                break;
            }
            self->cols_guess = row.num_fields;
        }
        if (columnar_append_row(t, row.fields, row.num_fields) < 0) {
            res = CSV_ERR_OOM;
            break;
        }
        parsed_rows++;
    }
    Py_END_ALLOW_THREADS
    
    if (res < 0) {
        set_csv_error(res);
        columnar_free(t);
        csv_parser_free(self->p);
        self->p = NULL;
        return NULL;
    }
    
    if (parsed_rows == 0) {
        columnar_free(t);
        csv_parser_free(self->p);
        self->p = NULL;
        return NULL; // stop iteration
    }
    
    PyObject *dict = table_to_dict(t, self->header_names);
    columnar_free(t);
    return dict;
}

static PyTypeObject CsvReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fastcsv.reader",
    .tp_doc = "fastcsv reader iterator",
    .tp_basicsize = sizeof(CsvReaderObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)CsvReader_dealloc,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)CsvReader_iternext,
};

static PyObject *fastcsv_reader(PyObject *self, PyObject *args, PyObject *kwds) {
    (void)self;
    const char *path;
    int chunk_size = 10000;
    const char *delimiter_str = ",";
    int has_header = 1;
    const char *error_mode_str = "strict";
    
    static char *kwlist[] = {"path", "chunk_size", "delimiter", "has_header", "error_mode", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|isps", kwlist,
                                     &path, &chunk_size, &delimiter_str, &has_header, &error_mode_str)) {
        return NULL;
    }
    
    if (strlen(delimiter_str) != 1) {
        PyErr_SetString(PyExc_ValueError, "delimiter must be exactly 1 char");
        return NULL;
    }
    
    CsvErrorMode error_mode;
    if (strcmp(error_mode_str, "strict") == 0) error_mode = CSV_ON_ERROR_STRICT;
    else if (strcmp(error_mode_str, "skip") == 0) error_mode = CSV_ON_ERROR_SKIP;
    else if (strcmp(error_mode_str, "replace") == 0) error_mode = CSV_ON_ERROR_REPLACE;
    else {
        PyErr_SetString(PyExc_ValueError, "error_mode must be 'strict', 'skip', or 'replace'");
        return NULL;
    }
    
    CsvOptions opts = csv_default_options();
    opts.delimiter = delimiter_str[0];
    opts.error_mode = error_mode;
    
    CsvParser *p = csv_parser_from_file(path, opts);
    if (!p) {
        PyErr_SetFromErrnoWithFilename(PyExc_IOError, path);
        return NULL;
    }
    
    PyObject *header_names = NULL;
    int cols_guess = 0;
    CsvRow row;
    if (has_header) {
        int res = csv_next_row(p, &row);
        if (res < 0) {
            set_csv_error(res);
            csv_parser_free(p);
            return NULL;
        }
        if (res == 0) {
            cols_guess = row.num_fields;
            header_names = PyList_New(row.num_fields);
            for (uint32_t i = 0; i < row.num_fields; i++) {
                PyObject *str_obj = PyUnicode_FromStringAndSize(row.fields[i].data, row.fields[i].len);
                PyList_SET_ITEM(header_names, i, str_obj);
            }
        }
    }
    
    CsvReaderObject *reader = PyObject_New(CsvReaderObject, &CsvReaderType);
    if (!reader) {
        csv_parser_free(p);
        Py_XDECREF(header_names);
        return NULL;
    }
    reader->p = p;
    reader->header_names = header_names;
    reader->chunk_size = chunk_size;
    reader->cols_guess = cols_guess;
    
    return (PyObject *)reader;
}

static PyObject *py_simd_width(PyObject *self, PyObject *args) {
    (void)self; (void)args;
    return PyLong_FromLong(fastcsv_simd_width());
}

static PyMethodDef fastcsv_methods[] = {
    {"read_csv", (PyCFunction)fastcsv_read_csv, METH_VARARGS | METH_KEYWORDS, "Read entire CSV into dict of numpy arrays"},
    {"reader", (PyCFunction)fastcsv_reader, METH_VARARGS | METH_KEYWORDS, "Return iterator reading chunks"},
    {"simd_width", py_simd_width, METH_NOARGS, "SIMD width in bytes."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef mod = {
    PyModuleDef_HEAD_INIT, "fastcsv", NULL, -1, fastcsv_methods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_fastcsv(void) {
    fastcsv_detect_cpu();
    import_array();
    
    if (PyType_Ready(&CsvReaderType) < 0) {
        return NULL;
    }
    
    return PyModule_Create(&mod);
}
