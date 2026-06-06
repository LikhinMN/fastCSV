#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/fastcsv.h"
#include "../src/simd.h"

int main(void) {
    fastcsv_detect_cpu();
    printf("SIMD width: %d bytes\n", fastcsv_simd_width());

    /* Generate ~10MB CSV in memory */
    size_t sz = 10 * 1024 * 1024;
    char *buf = malloc(sz);
    memset(buf, 'a', sz);
    /* Sprinkle delimiters and newlines */
    for (size_t i = 0; i < sz; i++) {
        if (i % 10 == 9)  buf[i] = ',';
        if (i % 60 == 59) buf[i] = '\n';
    }

    CsvOptions opts = csv_default_options();
    double total = 0.0;
    int runs = 5;

    for (int r = 0; r < runs; r++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        CsvParser *p = csv_parser_new(buf, sz, opts);
        CsvRow row;
        while (csv_next_row(p, &row) == CSV_OK) {}
        csv_parser_free(p);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double s = (t1.tv_sec - t0.tv_sec) +
                   (t1.tv_nsec - t0.tv_nsec) / 1e9;
        double mbs = (sz / 1024.0 / 1024.0) / s;
        printf("  run %d: %.0f MB/s\n", r + 1, mbs);
        total += mbs;
    }

    printf("Average: %.0f MB/s\n", total / runs);
    free(buf);
    return 0;
}
