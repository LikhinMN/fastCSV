#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/type_infer.h"

int tests_passed = 0;
int tests_failed = 0;

void run_test_single(const char *val, ColType expected_type) {
    ColType t = COL_TYPE_INT;
    CsvField f;
    f.data = val;
    f.len = strlen(val);
    f.quoted = 0;
    
    type_infer_update(&t, &f);
    if (t == expected_type) {
        printf("PASS: single '%s'\n", val);
        tests_passed++;
    } else {
        printf("FAIL: single '%s', expected %d got %d\n", val, expected_type, t);
        tests_failed++;
    }
}

void run_test_seq(const char **vals, int num_vals, ColType expected_type, const char *name) {
    ColType t = COL_TYPE_INT;
    for (int i = 0; i < num_vals; i++) {
        CsvField f;
        f.data = vals[i];
        f.len = strlen(vals[i]);
        f.quoted = 0;
        type_infer_update(&t, &f);
    }
    if (t == expected_type) {
        printf("PASS: seq %s\n", name);
        tests_passed++;
    } else {
        printf("FAIL: seq %s, expected %d got %d\n", name, expected_type, t);
        tests_failed++;
    }
}

int main(void) {
    // INT tests
    run_test_single("42", COL_TYPE_INT);
    run_test_single("0", COL_TYPE_INT);
    run_test_single("-7", COL_TYPE_INT);
    run_test_single("1000000", COL_TYPE_INT);

    // FLOAT tests
    run_test_single("3.14", COL_TYPE_FLOAT);
    run_test_single("-1.5", COL_TYPE_FLOAT);
    run_test_single("2e10", COL_TYPE_FLOAT);
    run_test_single("1.0", COL_TYPE_FLOAT);

    // STR tests
    run_test_single("hello", COL_TYPE_STR);
    run_test_single("", COL_TYPE_STR);
    run_test_single("12abc", COL_TYPE_STR);
    run_test_single("1.2.3", COL_TYPE_STR);

    // Downgrades
    const char *seq1[] = {"1", "2", "3.0"};
    run_test_seq(seq1, 3, COL_TYPE_FLOAT, "INT->FLOAT");

    const char *seq2[] = {"1", "abc"};
    run_test_seq(seq2, 2, COL_TYPE_STR, "INT->STR");

    const char *seq3[] = {"1.0", "xyz"};
    run_test_seq(seq3, 2, COL_TYPE_STR, "FLOAT->STR");
    
    // No upgrade
    const char *seq4[] = {"abc", "1"};
    run_test_seq(seq4, 2, COL_TYPE_STR, "STR->STR (no upgrade)");
    
    // Test parsing
    CsvField f;
    int64_t i_val = 0;
    double d_val = 0.0;
    const char *s_val = NULL;
    
    f.data = "42"; f.len = 2;
    assert(type_infer_parse(COL_TYPE_INT, &f, &i_val, &d_val, &s_val) == 0);
    assert(i_val == 42);

    f.data = "3.14"; f.len = 4;
    assert(type_infer_parse(COL_TYPE_FLOAT, &f, &i_val, &d_val, &s_val) == 0);
    assert(d_val == 3.14);

    f.data = "hello"; f.len = 5;
    assert(type_infer_parse(COL_TYPE_STR, &f, &i_val, &d_val, &s_val) == 0);
    assert(strncmp(s_val, "hello", 5) == 0);
    
    if (tests_failed == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        printf("%d tests failed.\n", tests_failed);
        return 1;
    }
}
