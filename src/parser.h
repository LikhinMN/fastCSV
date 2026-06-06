#ifndef FASTCSV_PARSER_H
#define FASTCSV_PARSER_H

#include "fastcsv.h"
#include "mmap_io.h"

struct CsvParser {
    const char  *buf;        /* pointer to data (mmap or caller buffer) */
    size_t       len;        /* total byte length                        */
    size_t       pos;        /* current read position                    */
    uint64_t     line_num;   /* current 1-based line counter             */
    CsvOptions   opts;
    MmapFile     mmap;       /* valid only if opened from file           */
    int          owns_mmap;  /* 1 if we must close mmap on free          */

    /* Row-level scratch buffer — reused across csv_next_row() calls     */
    CsvField    *fields;     /* heap-allocated array                     */
    uint32_t     fields_cap; /* allocated capacity                       */

    /* Buffer for fields with escaped quotes ("") */
    char        *escape_buf;
    size_t       escape_buf_cap;
};

uint64_t fastcsv_find_row_offsets(const char *buf, size_t len, char delim, char quote, size_t **out_offsets);

#endif
