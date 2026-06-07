#ifndef FASTCSV_H
#define FASTCSV_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    CSV_OK            =  0,
    CSV_ERR_OOM       = -1,
    CSV_ERR_IO        = -2,
    CSV_ERR_MALFORMED = -3,
    CSV_ERR_OVERFLOW  = -4,
} CsvError;

typedef enum {
    CSV_ON_ERROR_STRICT  = 0,
    CSV_ON_ERROR_SKIP    = 1,
    CSV_ON_ERROR_REPLACE = 2,
} CsvErrorMode;

typedef struct {
    const char *data;
    uint32_t    len;
    uint8_t     quoted;
} CsvField;

typedef struct {
    CsvField  *fields;
    uint32_t   num_fields;
    uint64_t   line_num;
} CsvRow;

typedef struct {
    char         delimiter;
    char         quote_char;
    int          has_header;
    CsvErrorMode error_mode;
    uint32_t     max_field_len;
} CsvOptions;

typedef struct CsvParser CsvParser;

CsvOptions  csv_default_options(void);
uint64_t    fastcsv_count_rows_and_partitions(const char *buf, size_t len, char quote, int nproc, CsvErrorMode err_mode, size_t **out_part_offsets, uint64_t **out_part_rows);
CsvParser  *csv_parser_new(const char *buf, size_t len, CsvOptions opts);
CsvParser  *csv_parser_from_file(const char *path, CsvOptions opts);
int         csv_next_row(CsvParser *p, CsvRow *out);
void        csv_parser_free(CsvParser *p);
const char *csv_strerror(CsvError err);

#endif
