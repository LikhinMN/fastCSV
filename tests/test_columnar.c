#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/columnar.h"

int tests_passed = 0;
int tests_failed = 0;

#define ASSERT_TEST(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

void test_3_col_int() {
    ColumnarTable *t = columnar_new(3, 10);
    CsvField f[3] = {
        {"1", 1, 0}, {"2", 1, 0}, {"3", 1, 0}
    };
    for(int i=0; i<5; i++) {
        columnar_append_row(t, f, 3);
    }
    ASSERT_TEST(t->num_rows == 5);
    for(int i=0; i<3; i++) {
        ASSERT_TEST(t->cols[i].type == COL_TYPE_INT);
        ASSERT_TEST(t->cols[i].len == 5);
        for(int j=0; j<5; j++) {
            ASSERT_TEST(t->cols[i].int_data[j] == i + 1);
        }
    }
    columnar_free(t);
    printf("PASS: 3-col table, append 5 rows of ints\n");
    tests_passed++;
}

void test_float_downgrade() {
    ColumnarTable *t = columnar_new(1, 10);
    CsvField f1 = {"1", 1, 0};
    columnar_append_row(t, &f1, 1);
    ASSERT_TEST(t->cols[0].type == COL_TYPE_INT);
    
    CsvField f2 = {"2.5", 3, 0};
    columnar_append_row(t, &f2, 1);
    ASSERT_TEST(t->cols[0].type == COL_TYPE_FLOAT);
    ASSERT_TEST(t->cols[0].float_data[1] == 2.5);
    
    columnar_free(t);
    printf("PASS: 3-col table, append rows with float -> col downgrades to float\n");
    tests_passed++;
}

void test_mixed_cols() {
    ColumnarTable *t = columnar_new(3, 10);
    CsvField f[3] = {
        {"1", 1, 0}, {"2.5", 3, 0}, {"abc", 3, 0}
    };
    columnar_append_row(t, f, 3);
    ASSERT_TEST(t->cols[0].type == COL_TYPE_INT);
    ASSERT_TEST(t->cols[1].type == COL_TYPE_FLOAT);
    ASSERT_TEST(t->cols[2].type == COL_TYPE_STR);
    columnar_free(t);
    printf("PASS: Mixed int/float/str columns -> each col has correct type\n");
    tests_passed++;
}

void test_str_packing() {
    ColumnarTable *t = columnar_new(1, 10);
    CsvField f[1];
    f[0].data = "abc"; f[0].len = 3; f[0].quoted = 0;
    columnar_append_row(t, f, 1);
    f[0].data = "defg"; f[0].len = 4; f[0].quoted = 0;
    columnar_append_row(t, f, 1);
    
    ASSERT_TEST(t->cols[0].type == COL_TYPE_STR);
    ASSERT_TEST(t->cols[0].arena_len == 7);
    ASSERT_TEST(t->cols[0].str_offsets[0] == 0);
    ASSERT_TEST(t->cols[0].str_lens[0] == 3);
    ASSERT_TEST(t->cols[0].str_offsets[1] == 3);
    ASSERT_TEST(t->cols[0].str_lens[1] == 4);
    ASSERT_TEST(strncmp(t->cols[0].str_arena, "abcdefg", 7) == 0);
    
    columnar_free(t);
    printf("PASS: STR column: arena packing correct, offsets + lens correct\n");
    tests_passed++;
}

void test_short_row() {
    ColumnarTable *t = columnar_new(3, 10);
    CsvField f[1] = {{"1", 1, 0}};
    columnar_append_row(t, f, 1);
    ASSERT_TEST(t->cols[0].type == COL_TYPE_INT);
    ASSERT_TEST(t->cols[0].int_data[0] == 1);
    ASSERT_TEST(t->cols[1].type == COL_TYPE_STR); // empty field downgrades to STR
    ASSERT_TEST(t->cols[1].str_lens[0] == 0);
    ASSERT_TEST(t->cols[2].type == COL_TYPE_STR);
    columnar_free(t);
    printf("PASS: Short row (fewer fields than cols)\n");
    tests_passed++;
}

void test_long_row() {
    ColumnarTable *t = columnar_new(1, 10);
    CsvField f[3] = {{"1", 1, 0}, {"2", 1, 0}, {"3", 1, 0}};
    columnar_append_row(t, f, 3);
    ASSERT_TEST(t->num_rows == 1);
    ASSERT_TEST(t->cols[0].int_data[0] == 1);
    columnar_free(t);
    printf("PASS: Long row (more fields than cols)\n");
    tests_passed++;
}

void test_capacity_growth() {
    ColumnarTable *t = columnar_new(1, 2);
    CsvField f[1] = {{"1", 1, 0}};
    for(int i=0; i<5000; i++) {
        columnar_append_row(t, f, 1);
    }
    ASSERT_TEST(t->num_rows == 5000);
    ASSERT_TEST(t->cols[0].capacity >= 5000);
    ASSERT_TEST(t->cols[0].int_data[4999] == 1);
    columnar_free(t);
    printf("PASS: Capacity growth: append 5000 rows\n");
    tests_passed++;
}

void test_free_null() {
    columnar_free(NULL);
    printf("PASS: columnar_free(NULL) -> no crash\n");
    tests_passed++;
}

int main(void) {
    test_3_col_int();
    test_float_downgrade();
    test_mixed_cols();
    test_str_packing();
    test_short_row();
    test_long_row();
    test_capacity_growth();
    test_free_null();
    
    if (tests_failed == 0) return 0;
    return 1;
}
