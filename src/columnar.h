#ifndef FASTCSV_COLUMNAR_H
#define FASTCSV_COLUMNAR_H

#include <stdint.h>
#include <stddef.h>
#include "type_infer.h"
#include "fastcsv.h"

typedef struct {
    ColType   type;
    uint64_t  len;        /* rows filled */
    uint64_t  capacity;   /* rows allocated */

    int64_t  *int_data;
    double   *float_data;

    /* STR columns: offsets into str_arena */
    uint32_t *str_offsets; /* str_offsets[i] = start of row i in str_arena */
    uint32_t *str_lens;    /* byte length of each string */
    char     *str_arena;   /* flat byte buffer, all strings packed */
    size_t    arena_len;
    size_t    arena_cap;
} Column;

typedef struct {
    Column   *cols;
    uint32_t  num_cols;
    uint64_t  num_rows;
} ColumnarTable;

/* Allocate table with num_cols columns, initial row capacity. */
ColumnarTable *columnar_new(uint32_t num_cols, uint64_t row_capacity);

/* Append one parsed row. Grows buffers if needed.
   fields[i] is matched to cols[i].
   Runs type_infer_update then writes value into the correct buffer. */
int columnar_append_row(ColumnarTable *t, CsvField *fields,
                        uint32_t num_fields);

/* Free everything. Safe with NULL. */
void columnar_free(ColumnarTable *t);

#endif
