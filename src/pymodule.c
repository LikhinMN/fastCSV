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

static PyObject *fastcsv_read_csv(PyObject *self, PyObject *args, PyObject *kwds) {
    (void)self;
    const char *path;
    const char *delimiter_str = ",";
    int has_header = 1;
    const char *error_mode_str = "strict";
    
    static char *kwlist[] = {"path", "delimiter", "has_header", "error_mode", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|sps", kwlist,
                                     &path, &delimiter_str, &has_header, &error_mode_str)) {
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
    ColumnarTable *t = NULL;
    PyObject *dict = NULL;
    int cols_guess = 0;
    int res = CSV_OK;
    
    p = csv_parser_from_file(path, opts);
    if (!p) {
        PyErr_SetFromErrnoWithFilename(PyExc_IOError, path);
        goto cleanup;
    }
    
    CsvRow row;
    if (has_header) {
        res = csv_next_row(p, &row);
        if (res < 0) {
            set_csv_error(res);
            goto cleanup;
        }
        if (res == 1) { // EOF
            dict = PyDict_New();
            goto cleanup;
        }
        cols_guess = row.num_fields;
        header_names = PyList_New(row.num_fields);
        if (!header_names) {
            goto cleanup;
        }
        for (uint32_t i = 0; i < row.num_fields; i++) {
            PyObject *str_obj = PyUnicode_FromStringAndSize(row.fields[i].data, row.fields[i].len);
            if (!str_obj) goto cleanup;
            PyList_SET_ITEM(header_names, i, str_obj);
        }
    }
    
    t = columnar_new(cols_guess, 4096);
    if (!t && cols_guess > 0) {
        PyErr_NoMemory();
        goto cleanup;
    }
    
    Py_BEGIN_ALLOW_THREADS
    while ((res = csv_next_row(p, &row)) == CSV_OK) {
        if (t->num_cols == 0 && row.num_fields > 0) {
            columnar_free(t);
            t = columnar_new(row.num_fields, 4096);
            if (!t) {
                res = CSV_ERR_OOM;
                break;
            }
        }
        if (columnar_append_row(t, row.fields, row.num_fields) < 0) {
            res = CSV_ERR_OOM;
            break;
        }
    }
    Py_END_ALLOW_THREADS
    
    if (res < 0) {
        set_csv_error(res);
        goto cleanup;
    }
    
    dict = table_to_dict(t, header_names);

cleanup:
    if (p) csv_parser_free(p);
    if (t) columnar_free(t);
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
