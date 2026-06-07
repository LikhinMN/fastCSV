#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

#include "fastcsv.h"
#include "mmap_io.h"
#include "parser.h"
#include "columnar.h"
#include "simd.h"
#include "simd_parse.h"
#include "fast_double.h"

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
    uint64_t start_row;
    uint64_t end_row;
    size_t start_pos;
    size_t end_pos;
    uint64_t total_rows;
    uint64_t **col_buffers;
    ColType *local_types;
    uint32_t *max_len;
    uint32_t num_cols;
    char delim;
    char quote;
    uint64_t *str_bytes;
} ParseThreadCtx;

/* CopyCtx and copy_thread removed: string columns now use NPY_OBJECT */

#ifdef _WIN32
static DWORD WINAPI parse_thread(LPVOID arg) {
#else
static void *parse_thread(void *arg) {
#endif
    ParseThreadCtx *ctx = (ParseThreadCtx *)arg;
    int w = fastcsv_simd_width();  /* cache SIMD width once */
    #define BATCH_ROWS 128
        uint64_t *field_batch = malloc(ctx->num_cols * BATCH_ROWS * sizeof(uint64_t));
        uint32_t batch_row_cnt = 0;
        
        size_t pos = ctx->start_pos;
        size_t end_pos = ctx->end_pos;
        
        uint64_t current_batch_start_row = 0;

        uint32_t c = 0;
        int quoted = 0;
        size_t field_start = pos;
        
        int restarted = 0;

#define FLUSH_BATCH() do { \
        for (uint32_t col = 0; col < ctx->num_cols; col++) { \
            ColType t = ctx->local_types[col]; \
            if (t == COL_TYPE_INT) { \
                for (uint32_t i = 0; i < batch_row_cnt; i++) { \
                    uint64_t packed = field_batch[col * BATCH_ROWS + i]; \
                    size_t fstart = packed >> 30; size_t fl = packed & 0x3FFFFFFF; \
                    int is_quoted = (fl >= 2 && ctx->buf[fstart] == ctx->quote && ctx->buf[fstart+fl-1] == ctx->quote); \
                    if (is_quoted) { fstart++; fl -= 2; } \
                    uint64_t curr_r = ctx->start_row + current_batch_start_row + i; \
                    if (curr_r >= ctx->end_row) continue; \
                    int int_ok = 0; \
                    int64_t val = fastcsv_parse_int_fast(ctx->buf + fstart, fl, &int_ok); \
                    if (int_ok) { ctx->col_buffers[col][curr_r] = (uint64_t)val; } \
                    else { ctx->local_types[col] = COL_TYPE_STR; restarted = 1; break; } \
                } \
            } else if (t == COL_TYPE_FLOAT) { \
                for (uint32_t i = 0; i < batch_row_cnt; i++) { \
                    uint64_t packed = field_batch[col * BATCH_ROWS + i]; \
                    size_t fstart = packed >> 30; size_t fl = packed & 0x3FFFFFFF; \
                    int is_quoted = (fl >= 2 && ctx->buf[fstart] == ctx->quote && ctx->buf[fstart+fl-1] == ctx->quote); \
                    if (is_quoted) { fstart++; fl -= 2; } \
                    uint64_t curr_r = ctx->start_row + current_batch_start_row + i; \
                    if (curr_r >= ctx->end_row) continue; \
                    if (fl == 0) { double val = 0.0; memcpy(&ctx->col_buffers[col][curr_r], &val, 8); continue; } \
                    int float_ok = 0; \
                    double val = fastcsv_parse_double(ctx->buf + fstart, fl, &float_ok); \
                    if (float_ok) { memcpy(&ctx->col_buffers[col][curr_r], &val, 8); } \
                    else { ctx->local_types[col] = COL_TYPE_STR; restarted = 1; break; } \
                } \
            } else { \
                for (uint32_t i = 0; i < batch_row_cnt; i++) { \
                    uint64_t packed = field_batch[col * BATCH_ROWS + i]; \
                    size_t fstart = packed >> 30; size_t fl = packed & 0x3FFFFFFF; \
                    int is_quoted = (fl >= 2 && ctx->buf[fstart] == ctx->quote && ctx->buf[fstart+fl-1] == ctx->quote); \
                    if (is_quoted) { fstart++; fl -= 2; } \
                    uint64_t curr_r = ctx->start_row + current_batch_start_row + i; \
                    if (curr_r >= ctx->end_row) continue; \
                    uint32_t quoted_bit = is_quoted ? 1 : 0; \
                    ctx->col_buffers[col][curr_r] = ((uint64_t)fstart << 30) | ((uint64_t)quoted_bit << 29) | fl; \
                    if (fl > ctx->max_len[col]) ctx->max_len[col] = fl; \
                    ctx->str_bytes[col] += fl; \
                } \
            } \
            if (restarted) break; \
        } \
        if (restarted) { \
            pos = ctx->start_pos; \
            current_batch_start_row = 0; \
            batch_row_cnt = 0; \
            c = 0; \
            quoted = 0; \
            field_start = pos; \
            restarted = 0; \
        } else { \
            current_batch_start_row += batch_row_cnt; \
            batch_row_cnt = 0; \
        } \
    } while(0)

        while (pos < end_pos || (pos == end_pos && field_start <= end_pos)) {
            if (pos + w <= end_pos) {
                int width;
                uint32_t mask = fastcsv_scan_chunk(ctx->buf + pos, ctx->delim, ctx->quote, &width);
                if (mask == 0) { pos += width; continue; }
                while (mask != 0) {
                    int bit = fastcsv_ctz(mask);
                    if (pos + bit >= end_pos) {
                        pos = end_pos;
                        break;
                    }
                    char ch = ctx->buf[pos + bit];
                    if (ch == ctx->quote) {
                        quoted = !quoted;
                    } else if (!quoted && (ch == ctx->delim || ch == '\n' || ch == '\r')) {
                        if (ch == '\r') {
                            // just skip it, handled by adjusting flen if \n follows
                        } else if (ch == ctx->delim) {
                            if (c < ctx->num_cols) {
                                size_t flen = (pos + bit) - field_start;
                                field_batch[c * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30) | (flen & 0x3FFFFFFF);
                            }
                            c++;
                            field_start = pos + bit + 1;
                        } else if (ch == '\n') {
                            if (c < ctx->num_cols) {
                                size_t flen = (pos + bit) - field_start;
                                if (flen > 0 && ctx->buf[pos + bit - 1] == '\r') flen--;
                                field_batch[c * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30) | (flen & 0x3FFFFFFF);
                            }
                            for (uint32_t missing = c + 1; missing < ctx->num_cols; missing++) {
                                field_batch[missing * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30);
                            }
                            batch_row_cnt++;
                            c = 0;
                            field_start = pos + bit + 1;
                            
                            if (batch_row_cnt == BATCH_ROWS) {
                                FLUSH_BATCH();
                                if (restarted) continue; // restarts the while loop naturally because pos is reset
                            }
                        }
                    }
                    mask &= mask - 1;
                }
                if (pos < end_pos) pos += width;
            } else {
                if (pos >= end_pos) {
                    if (field_start < end_pos || (field_start == end_pos && c > 0)) {
                        if (c < ctx->num_cols) {
                            size_t flen = end_pos - field_start;
                            if (flen > 0 && ctx->buf[end_pos - 1] == '\r') flen--;
                            field_batch[c * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30) | (flen & 0x3FFFFFFF);
                        }
                        for (uint32_t missing = c + 1; missing < ctx->num_cols; missing++) {
                            field_batch[missing * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30);
                        }
                        batch_row_cnt++;
                    }
                    pos++;
                    FLUSH_BATCH();
                    break;
                }
                
                char ch = ctx->buf[pos];
                if (ch == ctx->quote) quoted = !quoted;
                else if (!quoted && (ch == ctx->delim || ch == '\n' || ch == '\r')) {
                    if (ch == '\r') {
                        // skip
                    } else if (ch == ctx->delim) {
                        if (c < ctx->num_cols) {
                            size_t flen = pos - field_start;
                            field_batch[c * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30) | (flen & 0x3FFFFFFF);
                        }
                        c++;
                        field_start = pos + 1;
                    } else if (ch == '\n') {
                        if (c < ctx->num_cols) {
                            size_t flen = pos - field_start;
                            if (flen > 0 && ctx->buf[pos - 1] == '\r') flen--;
                            field_batch[c * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30) | (flen & 0x3FFFFFFF);
                        }
                        for (uint32_t missing = c + 1; missing < ctx->num_cols; missing++) {
                            field_batch[missing * BATCH_ROWS + batch_row_cnt] = ((uint64_t)field_start << 30);
                        }
                        batch_row_cnt++;
                        c = 0;
                        field_start = pos + 1;
                        
                        if (batch_row_cnt == BATCH_ROWS) {
                            FLUSH_BATCH();
                        }
                    }
                }
                pos++;
            }
        }
        
        FLUSH_BATCH();
        
        free(field_batch);
#undef FLUSH_BATCH
#undef BATCH_ROWS
    return 0;
}

/* Multithreaded string packing context */
typedef struct {
    uint64_t *col_buffer;
    int64_t *offsets;
    char *str_data;
    const char *buf;
    uint64_t start_row;
    uint64_t end_row;
    int64_t start_offset;
    char quote_char;
    int64_t final_offset; // filled by thread
} PackThreadCtx;

#ifdef _WIN32
static DWORD WINAPI pack_thread_func(LPVOID arg) {
#else
static void *pack_thread_func(void *arg) {
#endif
    PackThreadCtx *ctx = (PackThreadCtx *)arg;
    int64_t cur_off = ctx->start_offset;
    for (uint64_t r = ctx->start_row; r < ctx->end_row; r++) {
        ctx->offsets[r] = cur_off;
        uint64_t packed = ctx->col_buffer[r];
        uint32_t str_off = (uint32_t)(packed >> 30);
        uint32_t is_quoted = (uint32_t)((packed >> 29) & 1);
        uint32_t flen = (uint32_t)(packed & 0x1FFFFFFF);
        
        if (is_quoted) {
            int has_q = 0;
            if (flen <= 128) {
                for (uint32_t qi = 0; qi < flen; qi++) {
                    if (ctx->buf[str_off + qi] == ctx->quote_char) { has_q = 1; break; }
                }
            } else {
                has_q = (memchr(ctx->buf + str_off, ctx->quote_char, flen) != NULL);
            }
            if (has_q) {
                for (uint32_t qi = 0; qi < flen; qi++) {
                    ctx->str_data[cur_off++] = ctx->buf[str_off + qi];
                    if (ctx->buf[str_off + qi] == ctx->quote_char && qi + 1 < flen && ctx->buf[str_off + qi + 1] == ctx->quote_char) qi++;
                }
            } else {
                memcpy(ctx->str_data + cur_off, ctx->buf + str_off, flen);
                cur_off += flen;
            }
        } else {
            memcpy(ctx->str_data + cur_off, ctx->buf + str_off, flen);
            cur_off += flen;
        }
    }
    ctx->final_offset = cur_off;
    return 0;
}

static PyObject *fastcsv_read_csv(PyObject *self, PyObject *args, PyObject *kwds) {
    (void)self;
    const char *path;
    const char *delimiter_str = ",";
    int has_header = 1;
    const char *error_mode_str = "strict";
    int arrow_strings = 1;
    int raw = 0;
    
    static char *kwlist[] = {"path", "delimiter", "has_header", "error_mode", "_arrow_strings", "raw", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|spspp", kwlist,
                                     &path, &delimiter_str, &has_header, &error_mode_str, &arrow_strings, &raw)) {
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
    size_t *part_offsets = NULL;
    uint64_t *part_rows = NULL;
    uint64_t total_rows = 0;
    uint64_t **col_buffers = NULL;
    ParseThreadCtx *threads = NULL;
    int nproc = 0;
    int header_consumed = 0;
    uint32_t num_cols = 0;
    ColType *sampled_types = NULL;
    
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    nproc = sysinfo.dwNumberOfProcessors;
    HANDLE *handles = NULL;
#else
    nproc = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 1;
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
    fastcsv_detect_cpu();
    total_rows = fastcsv_count_rows_and_partitions(buf, len, opts.quote_char, nproc, opts.error_mode, &part_offsets, &part_rows);
    Py_END_ALLOW_THREADS

    if (total_rows == (uint64_t)-1) {
        set_csv_error(CSV_ERR_MALFORMED);
        goto cleanup;
    }

    if (total_rows == 0) {
        dict = PyDict_New();
        goto cleanup;
    }
    
    CsvRow row;
    int res = csv_next_row(p, &row);
    if (res < 0) { set_csv_error(res); goto cleanup; }
    if (res == 1) { dict = PyDict_New(); goto cleanup; }
    
    num_cols = row.num_fields;
    
    if (has_header) {
        header_names = PyList_New(num_cols);
        if (!header_names) goto cleanup;
        for (uint32_t i = 0; i < num_cols; i++) {
            PyObject *str_obj = PyUnicode_FromStringAndSize(row.fields[i].data, row.fields[i].len);
            if (!str_obj) goto cleanup;
            PyList_SET_ITEM(header_names, i, str_obj);
        }
        total_rows--; // header consumed
        part_offsets[0] = p->pos - (buf - p->buf); // skip header bytes, adjusting for BOM if present
        for (int i = 0; i <= nproc; i++) {
            if (part_rows[i] > 0) part_rows[i]--;
        }
        header_consumed = 1;
    }
    
    if (total_rows == 0) {
        dict = PyDict_New();
        goto cleanup;
    }
    
    /* ---- Sample-based type inference (P6) ---- */
    sampled_types = malloc(num_cols * sizeof(ColType));
    if (!sampled_types) { PyErr_NoMemory(); goto cleanup; }
    for (uint32_t c = 0; c < num_cols; c++) sampled_types[c] = COL_TYPE_INT;
    {
        uint64_t sample_count = (total_rows < 1000) ? total_rows : 1000;
        size_t spos = part_offsets[0];
        
        for (uint64_t s = 0; s < sample_count; s++) {
            if (spos >= len) break;
            
            int w;
            uint32_t mask = fastcsv_scan_newlines(buf + spos, opts.quote_char, &w);
            size_t send = len;
            if (mask != 0) {
                int bit = fastcsv_ctz(mask);
                send = spos + bit;
            } else {
                for (size_t i = spos; i < len; i++) {
                    if (buf[i] == '\n' || buf[i] == '\r') { send = i; break; }
                }
            }
            if (send > len) send = len;
            
            uint32_t sample_c = 0;
            int q = 0;
            size_t fstart = spos;
            for (size_t i = spos; i <= send; i++) {
                char ch = (i < send) ? buf[i] : '\n';
                if (ch == opts.quote_char) q = !q;
                else if (!q && (ch == opts.delimiter || i == send)) {
                    if (sample_c < num_cols) {
                        size_t fl = i - fstart;
                        int is_quoted = (fl >= 2 && buf[fstart] == opts.quote_char && buf[fstart+fl-1] == opts.quote_char);
                        if (is_quoted) { fstart++; fl -= 2; }
                        CsvField f = { .data = buf + fstart, .len = fl, .quoted = is_quoted };
                        type_infer_update(&sampled_types[sample_c], &f);
                    }
                    sample_c++;
                    fstart = i + 1;
                }
            }
            spos = send + 1;
            if (spos < len && buf[spos-1] == '\r' && buf[spos] == '\n') spos++;
        }
    }

    col_buffers = malloc(num_cols * sizeof(uint64_t *));
    if (!col_buffers) { PyErr_NoMemory(); goto cleanup; }
    for (uint32_t c = 0; c < num_cols; c++) {
        col_buffers[c] = PyDataMem_NEW(total_rows * sizeof(uint64_t));
        if (!col_buffers[c]) { PyErr_NoMemory(); goto cleanup; }
    }
    
    if (nproc > 32) nproc = 32;
    if (total_rows < (uint64_t)nproc * 100) {
        nproc = 1;
        part_offsets[1] = len;
        part_rows[1] = total_rows;
    }
    
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
        threads[t].start_row = part_rows[t];
        threads[t].end_row = part_rows[t+1];
        threads[t].start_pos = part_offsets[t];
        threads[t].end_pos = part_offsets[t+1];
        threads[t].total_rows = total_rows;
        threads[t].col_buffers = col_buffers;
        threads[t].local_types = malloc(num_cols * sizeof(ColType));
        memcpy(threads[t].local_types, sampled_types, num_cols * sizeof(ColType));  /* use sampled types */
        threads[t].max_len = calloc(num_cols, sizeof(uint32_t));
        threads[t].str_bytes = calloc(num_cols, sizeof(uint64_t));
        threads[t].num_cols = num_cols;
        threads[t].delim = opts.delimiter;
        threads[t].quote = opts.quote_char;
    }

    free(sampled_types);
    sampled_types = NULL;

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
    
    PyObject *pa = NULL;
    for (uint32_t c = 0; c < num_cols; c++) {
        if (global_types[c] == COL_TYPE_STR) {
            pa = PyImport_ImportModule("pyarrow");
            break;
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
        } else if (arrow_strings && !raw) {
            /* Arrow zero-copy path (P1-B): pack strings into offsets+data buffers */
            uint64_t total_str_bytes = 0;
            int64_t *thread_start_offsets = malloc(nproc * sizeof(int64_t));
            
            for (int t = 0; t < nproc; t++) {
                uint64_t t_start = threads[t].start_row;
                uint64_t t_end = threads[t].end_row;
                if (t_end > total_rows) t_end = total_rows;
                
                uint64_t bytes = threads[t].str_bytes[c];
                thread_start_offsets[t] = total_str_bytes;
                total_str_bytes += bytes;
            }
            
            /* Create int64 offsets array (Arrow large_utf8 format) */
            npy_intp off_dims[1] = { (npy_intp)(total_rows + 1) };
            PyObject *offsets_arr = PyArray_SimpleNew(1, off_dims, NPY_INT64);
            if (!offsets_arr) { free(thread_start_offsets); Py_DECREF(key); Py_DECREF(dict); dict = NULL; goto cleanup; }
            int64_t *offsets_ptr = (int64_t *)PyArray_DATA((PyArrayObject *)offsets_arr);
            
            /* Create contiguous data buffer */
            PyObject *data_bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)total_str_bytes);
            if (!data_bytes) { free(thread_start_offsets); Py_DECREF(offsets_arr); Py_DECREF(key); Py_DECREF(dict); dict = NULL; goto cleanup; }
            char *str_data = PyBytes_AS_STRING(data_bytes);
            
            PackThreadCtx *pack_ctxs = malloc(nproc * sizeof(PackThreadCtx));
            for (int t = 0; t < nproc; t++) {
                uint64_t t_start = threads[t].start_row;
                uint64_t t_end = threads[t].end_row;
                if (t_end > total_rows) t_end = total_rows;
                
                pack_ctxs[t].col_buffer = col_buffers[c];
                pack_ctxs[t].offsets = offsets_ptr;
                pack_ctxs[t].str_data = str_data;
                pack_ctxs[t].buf = buf;
                pack_ctxs[t].start_row = t_start;
                pack_ctxs[t].end_row = t_end;
                pack_ctxs[t].start_offset = thread_start_offsets[t];
                pack_ctxs[t].quote_char = opts.quote_char;
            }
            
            Py_BEGIN_ALLOW_THREADS
            for (int t = 0; t < nproc; t++) {
#ifdef _WIN32
                handles[t] = CreateThread(NULL, 0, pack_thread_func, &pack_ctxs[t], 0, NULL);
#else
                pthread_create(&pthreads[t], NULL, pack_thread_func, &pack_ctxs[t]);
#endif
            }
#ifdef _WIN32
            WaitForMultipleObjects(nproc, handles, TRUE, INFINITE);
#else
            for (int t = 0; t < nproc; t++) pthread_join(pthreads[t], NULL);
#endif
            Py_END_ALLOW_THREADS
            
            int64_t cur_off = pack_ctxs[nproc - 1].final_offset;
            offsets_ptr[total_rows] = cur_off;
            
            free(thread_start_offsets);
            free(pack_ctxs);
            
            /* Shrink buffer if quote unescaping reduced total size */
            if (cur_off < (int64_t)total_str_bytes) {
                _PyBytes_Resize(&data_bytes, (Py_ssize_t)cur_off);
            }
            
            /* Return as (offsets, data) tuple — Python wrapper converts to Arrow */
            arr = PyTuple_Pack(2, offsets_arr, data_bytes);
            Py_DECREF(offsets_arr);
            Py_DECREF(data_bytes);
            PyDataMem_FREE(col_buffers[c]);
            col_buffers[c] = NULL;
        } else {
            /* String column: use NPY_OBJECT for compact Python str output */
            arr = PyArray_SimpleNew(1, dims, NPY_OBJECT);
            if (!arr) { Py_DECREF(key); Py_DECREF(dict); goto cleanup; }
            PyObject **str_ptrs = (PyObject **)PyArray_DATA((PyArrayObject *)arr);
            
            for (uint64_t r = 0; r < total_rows; r++) {
                uint64_t packed = col_buffers[c][r];
                uint32_t offset = (uint32_t)(packed >> 30);
                uint32_t flen = (uint32_t)(packed & 0x3FFFFFFF);
                
                /* Check for escaped quotes */
                if (flen > 0 && memchr(buf + offset, opts.quote_char, flen) != NULL) {
                    char *tmp = (char *)malloc(flen);
                    size_t out_len = 0;
                    for (uint32_t i = 0; i < flen; i++) {
                        tmp[out_len++] = buf[offset + i];
                        if (buf[offset + i] == opts.quote_char && i + 1 < flen && buf[offset + i + 1] == opts.quote_char) i++;
                    }
                    if (raw) str_ptrs[r] = PyBytes_FromStringAndSize(tmp, out_len);
                    else str_ptrs[r] = PyUnicode_FromStringAndSize(tmp, out_len);
                    free(tmp);
                } else {
                    if (raw) str_ptrs[r] = PyBytes_FromStringAndSize(buf + offset, flen);
                    else str_ptrs[r] = PyUnicode_FromStringAndSize(buf + offset, flen);
                }
                if (!str_ptrs[r]) {
                    for (uint64_t j = 0; j < r; j++) { Py_XDECREF(str_ptrs[j]); }
                    Py_DECREF(arr); Py_DECREF(key); Py_DECREF(dict);
                    dict = NULL; goto cleanup;
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
        for (int t = 0; t < nproc; t++) {
            if (threads[t].local_types) free(threads[t].local_types);
            if (threads[t].max_len) free(threads[t].max_len);
            if (threads[t].str_bytes) free(threads[t].str_bytes);
        }
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
    if (sampled_types) free(sampled_types);
    if (part_offsets) free(part_offsets);
    if (part_rows) free(part_rows);
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

