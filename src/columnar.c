#include "columnar.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_ARENA_CAP 65536

ColumnarTable *columnar_new(uint32_t num_cols, uint64_t row_capacity) {
    if (row_capacity == 0) row_capacity = 4096;

    ColumnarTable *t = calloc(1, sizeof(ColumnarTable));
    if (!t) return NULL;

    t->num_cols = num_cols;
    if (num_cols == 0) return t;

    t->cols = calloc(num_cols, sizeof(Column));
    if (!t->cols) {
        free(t);
        return NULL;
    }

    for (uint32_t i = 0; i < num_cols; i++) {
        Column *c = &t->cols[i];
        c->type = COL_TYPE_INT; // default
        c->capacity = row_capacity;
        
        c->int_data = malloc(row_capacity * sizeof(int64_t));
        c->float_data = malloc(row_capacity * sizeof(double));
        c->str_offsets = malloc(row_capacity * sizeof(uint32_t));
        c->str_lens = malloc(row_capacity * sizeof(uint32_t));
        
        c->arena_cap = INITIAL_ARENA_CAP;
        c->str_arena = malloc(INITIAL_ARENA_CAP);
        
        if (!c->int_data || !c->float_data || !c->str_offsets || !c->str_lens || !c->str_arena) {
            columnar_free(t);
            return NULL;
        }
    }
    
    return t;
}

void columnar_free(ColumnarTable *t) {
    if (!t) return;
    if (t->cols) {
        for (uint32_t i = 0; i < t->num_cols; i++) {
            Column *c = &t->cols[i];
            free(c->int_data);
            free(c->float_data);
            free(c->str_offsets);
            free(c->str_lens);
            free(c->str_arena);
        }
        free(t->cols);
    }
    free(t);
}

int columnar_append_row(ColumnarTable *t, CsvField *fields, uint32_t num_fields) {
    for (uint32_t i = 0; i < t->num_cols; i++) {
        Column *c = &t->cols[i];
        
        if (c->len >= c->capacity) {
            uint64_t new_cap = c->capacity * 2;
            if (new_cap == 0) new_cap = 4096;
            
            void *p1 = realloc(c->int_data, new_cap * sizeof(int64_t));
            void *p2 = realloc(c->float_data, new_cap * sizeof(double));
            void *p3 = realloc(c->str_offsets, new_cap * sizeof(uint32_t));
            void *p4 = realloc(c->str_lens, new_cap * sizeof(uint32_t));
            
            if (!p1 || !p2 || !p3 || !p4) {
                if (p1) c->int_data = p1;
                if (p2) c->float_data = p2;
                if (p3) c->str_offsets = p3;
                if (p4) c->str_lens = p4;
                return -1;
            }
            c->int_data = p1;
            c->float_data = p2;
            c->str_offsets = p3;
            c->str_lens = p4;
            c->capacity = new_cap;
        }

        CsvField empty_field = {"", 0, 0};
        const CsvField *f = (i < num_fields) ? &fields[i] : &empty_field;
        
        type_infer_update(&c->type, f);
        
        int64_t i_val = 0;
        double d_val = 0.0;
        const char *s_val = NULL;
        
        type_infer_parse(c->type, f, &i_val, &d_val, &s_val);
        
        c->int_data[c->len] = i_val;
        c->float_data[c->len] = d_val;
        c->str_offsets[c->len] = 0;
        c->str_lens[c->len] = 0;
        
        if (c->type == COL_TYPE_STR) {
            size_t needed = c->arena_len + f->len;
            if (needed > c->arena_cap) {
                size_t new_cap = c->arena_cap * 2;
                if (new_cap < needed) new_cap = needed * 2;
                void *p = realloc(c->str_arena, new_cap);
                if (!p) return -1;
                c->str_arena = p;
                c->arena_cap = new_cap;
            }
            c->str_offsets[c->len] = (uint32_t)c->arena_len;
            c->str_lens[c->len] = (uint32_t)f->len;
            if (f->len > 0 && s_val) {
                memcpy(c->str_arena + c->arena_len, s_val, f->len);
            }
            c->arena_len += f->len;
        }
        
        c->len++;
    }
    t->num_rows++;
    return 0;
}
