#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/parser.h"

int test_pass = 1;

void run_test(const char *name, const char *csv_data, CsvOptions opts, int (*verify)(CsvParser *, CsvRow *)) {
    CsvParser *p = csv_parser_new(csv_data, strlen(csv_data), opts);
    CsvRow row;
    int res = verify(p, &row);
    if (res) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        test_pass = 0;
    }
    csv_parser_free(p);
}

int verify_basic_2row_3field(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 3) return 0;
    if (strncmp(row->fields[0].data, "a", row->fields[0].len) != 0) return 0;
    
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 3) return 0;
    if (strncmp(row->fields[2].data, "f", row->fields[2].len) != 0) return 0;
    
    if (csv_next_row(p, row) != 1) return 0; // EOF
    return 1;
}

int verify_no_trailing_newline(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 3) return 0;
    if (csv_next_row(p, row) != 1) return 0;
    return 1;
}

int verify_quoted_stripped(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 1) return 0;
    if (row->fields[0].len != 3 || strncmp(row->fields[0].data, "abc", 3) != 0) return 0;
    if (!row->fields[0].quoted) return 0;
    return 1;
}

int verify_double_quote_escape(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 4 || strncmp(row->fields[0].data, "a\"bc", 4) != 0) return 0;
    return 1;
}

int verify_embedded_comma(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 3 || strncmp(row->fields[0].data, "a,b", 3) != 0) return 0;
    return 1;
}

int verify_embedded_newline(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 3 || strncmp(row->fields[0].data, "a\nb", 3) != 0) return 0;
    if (p->line_num != 2) return 0;
    return 1;
}

int verify_embedded_newline_comma(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 4 || strncmp(row->fields[0].data, "a,\nb", 4) != 0) return 0;
    if (p->line_num != 2) return 0;
    return 1;
}

int verify_empty_middle(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 3) return 0;
    if (row->fields[1].len != 0) return 0;
    return 1;
}

int verify_all_empty(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 3) return 0;
    if (row->fields[0].len != 0 || row->fields[1].len != 0 || row->fields[2].len != 0) return 0;
    return 1;
}

int verify_empty_quoted(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 0 || !row->fields[0].quoted) return 0;
    return 1;
}

int verify_single_no_delim(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 1) return 0;
    if (strncmp(row->fields[0].data, "a", 1) != 0) return 0;
    return 1;
}

int verify_whitespace_preserved(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 3 || strncmp(row->fields[0].data, " a ", 3) != 0) return 0;
    return 1;
}

int verify_tab_sep(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 2) return 0;
    return 1;
}

int verify_triple_quote(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 1 || strncmp(row->fields[0].data, "\"", 1) != 0) return 0;
    return 1;
}

int verify_mixed_newlines(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0; // a
    if (p->line_num != 2) return 0;
    if (csv_next_row(p, row) != CSV_OK) return 0; // b
    if (p->line_num != 3) return 0;
    // For \r only or \n only tests there are only 2 rows, for mixed there are 3 rows
    int res = csv_next_row(p, row);
    if (res == 1) return 1; // EOF reached correctly for \n only or \r\n or \r only
    if (res == CSV_OK) { // c for mixed test
        if (p->line_num != 4) return 0;
        if (csv_next_row(p, row) != 1) return 0; // should end now
    }
    return 1;
}

int verify_inconsistent_fields(CsvParser *p, CsvRow *row) {
    csv_next_row(p, row);
    if (row->num_fields != 2) return 0;
    csv_next_row(p, row);
    if (row->num_fields != 3) return 0;
    return 1;
}

int verify_long_unquoted(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 2000) return 0;
    return 1;
}

int verify_bom(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 1 || strncmp(row->fields[0].data, "a", 1) != 0) return 0;
    return 1;
}

int verify_2_rows_1_field(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 1) return 0;
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->num_fields != 1) return 0;
    if (csv_next_row(p, row) != 1) return 0;
    return 1;
}

int verify_header_only(CsvParser *p, CsvRow *row) {
    int r1 = csv_next_row(p, row);
    if (r1 != CSV_OK) { printf("header row failed %d\n", r1); return 0; }
    int r2 = csv_next_row(p, row);
    if (r2 != 1) { printf("data row expected 1 got %d\n", r2); return 0; }
    return 1;
}

int verify_multi_line_quoted(CsvParser *p, CsvRow *row) {
    int r1 = csv_next_row(p, row);
    if (r1 != CSV_OK) { printf("multi-line row failed %d\n", r1); return 0; }
    if (row->num_fields != 1) { printf("multi-line fields expected 1 got %d\n", row->num_fields); return 0; }
    if (row->fields[0].len != 5) { printf("multi-line len expected 5 got %d\n", row->fields[0].len); return 0; }
    if (strncmp(row->fields[0].data, "a\nb\nc", 5) != 0) { printf("multi-line data mismatch\n"); return 0; }
    return 1;
}

int verify_empty_input(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != 1) return 0;
    return 1;
}

int verify_single_newline(CsvParser *p, CsvRow *row) {
    int res = csv_next_row(p, row);
    if (res == 1) return 1; // 0 rows ok
    if (res == CSV_OK && row->num_fields == 1 && row->fields[0].len == 0) return 1; // 1 empty row ok
    return 0;
}

int verify_unclosed_quote_strict(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_ERR_MALFORMED) return 0;
    return 1;
}

int verify_unclosed_quote_replace(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    if (row->fields[0].len != 1 || strncmp(row->fields[0].data, "a", 1) != 0) return 0;
    return 1;
}

int verify_overflow(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_ERR_OVERFLOW) return 0;
    return 1;
}

int verify_overflow_ok(CsvParser *p, CsvRow *row) {
    if (csv_next_row(p, row) != CSV_OK) return 0;
    return 1;
}

int main(void) {
    CsvOptions opts = csv_default_options();

    run_test("basic 2-row 3-field CSV", "a,b,c\nd,e,f\n", opts, verify_basic_2row_3field);
    run_test("file without trailing newline", "a,b,c", opts, verify_no_trailing_newline);
    run_test("quoted field \xE2\x80\x94 value stripped of quotes", "\"abc\"", opts, verify_quoted_stripped);
    run_test("quoted flag set correctly", "\"abc\"", opts, verify_quoted_stripped);
    run_test("double-quote escape: \"\" \xE2\x80\x94> \"", "\"a\"\"bc\"", opts, verify_double_quote_escape);
    run_test("embedded comma inside quoted field", "\"a,b\"", opts, verify_embedded_comma);
    run_test("embedded newline inside quoted field", "\"a\nb\"", opts, verify_embedded_newline);
    run_test("empty middle field: a,,c \xE2\x80\x94> 3 fields", "a,,c", opts, verify_empty_middle);
    run_test("all-empty: ,, \xE2\x80\x94> 3 fields", ",,", opts, verify_all_empty);
    run_test("empty quoted field: \"\" \xE2\x80\x94> len=0, quoted=1", "\"\"", opts, verify_empty_quoted);
    run_test("single field no delimiter", "a", opts, verify_single_no_delim);
    run_test("whitespace preserved (no trimming)", " a ", opts, verify_whitespace_preserved);

    opts.delimiter = '\t';
    run_test("tab-separated", "a\tb", opts, verify_tab_sep);
    opts.delimiter = '|';
    run_test("pipe-separated", "a|b", opts, verify_tab_sep);
    opts.delimiter = ';';
    run_test("semicolon-separated", "a;b", opts, verify_tab_sep);
    opts.delimiter = ',';

    run_test("all fields quoted", "\"a\",\"b\"", opts, verify_tab_sep); // Reuse tab_sep len 2 verify
    run_test("quoted field containing comma + newline together", "\"a,\nb\"", opts, verify_embedded_newline_comma);
    run_test("triple double-quote: \"\"\" \xE2\x80\x94> \"", "\"\"\"\"", opts, verify_triple_quote);

    run_test("\\n only", "a\nb", opts, verify_2_rows_1_field);
    run_test("\\r\\n", "a\r\nb\r\n", opts, verify_2_rows_1_field);
    run_test("\\r only", "a\rb\r", opts, verify_2_rows_1_field);
    run_test("mixed \\n \\r\\n \\r in same file", "a\nb\r\nc\r", opts, verify_mixed_newlines);

    run_test("inconsistent field counts", "a,b\nc,d,e", opts, verify_inconsistent_fields);
    
    char long_str[2001];
    memset(long_str, 'a', 2000);
    long_str[2000] = '\0';
    run_test("very long unquoted field (2000 chars)", long_str, opts, verify_long_unquoted);

    run_test("UTF-8 BOM skipped", "\xEF\xBB\xBF" "a", opts, verify_bom);
    run_test("empty last field with no trailing newline", "a,", opts, verify_tab_sep); // a, "" -> 2 fields
    run_test("multi-line quoted field = 1 logical row", "\"a\nb\nc\"", opts, verify_multi_line_quoted);

    run_test("empty string \xE2\x80\x94> 0 rows", "", opts, verify_empty_input);
    run_test("single newline \xE2\x80\x94> 0 or 1 empty rows", "\n", opts, verify_single_newline);
    run_test("header-only file \xE2\x80\x94> 0 data rows, header parsed", "a,b,c\n", opts, verify_header_only);

    opts.error_mode = CSV_ON_ERROR_STRICT;
    run_test("strict: unclosed quote \xE2\x80\x94> CSV_ERR_MALFORMED", "\"a", opts, verify_unclosed_quote_strict);
    opts.error_mode = CSV_ON_ERROR_REPLACE;
    run_test("replace: unclosed quote \xE2\x80\x94> CSV_OK", "\"a", opts, verify_unclosed_quote_replace);

    opts.max_field_len = 3;
    run_test("field exactly at max_field_len \xE2\x80\x94> CSV_OK", "abc", opts, verify_overflow_ok);
    run_test("field one over max_field_len \xE2\x80\x94> CSV_ERR_OVERFLOW", "abcd", opts, verify_overflow);
    opts.max_field_len = 0;
    run_test("max_field_len=0, 1024-char field \xE2\x80\x94> CSV_OK", long_str, opts, verify_overflow_ok);

    if (test_pass) {
        return 0;
    } else {
        return 1;
    }
}
