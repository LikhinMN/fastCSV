#ifndef FASTCSV_TYPE_INFER_H
#define FASTCSV_TYPE_INFER_H

#include <stdint.h>
#include "fastcsv.h"

typedef enum {
    COL_TYPE_INT   = 0,
    COL_TYPE_FLOAT = 1,
    COL_TYPE_STR   = 2,
} ColType;

/* Call once per field value. Downgrades type in-place.
   Starts at INT, downgrades to FLOAT, then STR. Never upgrades. */
void type_infer_update(ColType *type, const CsvField *field);

/* Parse a field into the correct C type based on final ColType.
   out_int / out_float / out_str: only the matching one is written. */
int type_infer_parse(ColType type, const CsvField *field,
                     int64_t *out_int, double *out_float,
                     const char **out_str);

#endif
