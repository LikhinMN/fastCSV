#include "type_infer.h"
#include <stdlib.h>
#include <string.h>

void type_infer_update(ColType *type, const CsvField *field) {
    if (*type == COL_TYPE_STR) return; // terminal state

    if (field->len == 0) {
        *type = COL_TYPE_STR;
        return;
    }

    int has_dot = 0;
    int has_e = 0;
    int has_digits = 0;
    int has_digits_after_e = 0;
    ColType current = *type;

    for (uint32_t i = 0; i < field->len; i++) {
        char c = field->data[i];
        
        if (c >= '0' && c <= '9') {
            has_digits = 1;
            if (has_e) has_digits_after_e = 1;
        } else if (c == '-') {
            if (i == 0) {
                // leading '-' allowed
            } else if (current == COL_TYPE_FLOAT && (field->data[i-1] == 'e' || field->data[i-1] == 'E')) {
                // allowed in FLOAT after 'e'
            } else {
                *type = COL_TYPE_STR;
                return;
            }
        } else if (c == '+') {
            if (current == COL_TYPE_FLOAT && i == 0) {
                // allowed in FLOAT at start
            } else if (current == COL_TYPE_FLOAT && (field->data[i-1] == 'e' || field->data[i-1] == 'E')) {
                // allowed in FLOAT after 'e'
            } else {
                *type = COL_TYPE_STR;
                return;
            }
        } else if (c == '.') {
            if (has_dot || has_e) {
                *type = COL_TYPE_STR;
                return;
            }
            current = COL_TYPE_FLOAT;
            has_dot = 1;
        } else if (c == 'e' || c == 'E') {
            if (has_e || !has_digits) {
                *type = COL_TYPE_STR;
                return;
            }
            current = COL_TYPE_FLOAT;
            has_e = 1;
        } else {
            *type = COL_TYPE_STR;
            return;
        }
    }

    if (!has_digits || (has_e && !has_digits_after_e)) {
        *type = COL_TYPE_STR;
        return;
    }
    
    *type = current;
}

int type_infer_parse(ColType type, const CsvField *field,
                     int64_t *out_int, double *out_float,
                     const char **out_str) {
    if (type == COL_TYPE_STR) {
        *out_str = field->data;
        return 0;
    }

    char buf[64];
    if (field->len >= sizeof(buf)) {
        char *dyn = malloc(field->len + 1);
        if (!dyn) return -1;
        memcpy(dyn, field->data, field->len);
        dyn[field->len] = '\0';
        
        if (type == COL_TYPE_INT) {
            *out_int = strtoll(dyn, NULL, 10);
        } else if (type == COL_TYPE_FLOAT) {
            *out_float = strtod(dyn, NULL);
        }
        free(dyn);
        return 0;
    }

    memcpy(buf, field->data, field->len);
    buf[field->len] = '\0';

    if (type == COL_TYPE_INT) {
        *out_int = strtoll(buf, NULL, 10);
    } else if (type == COL_TYPE_FLOAT) {
        *out_float = strtod(buf, NULL);
    }

    return 0;
}
