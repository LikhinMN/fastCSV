#include <stdio.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include "src/simd.h"

uint64_t fastcsv_count_rows_and_partitions(const char *buf, size_t len, char quote, int nproc, size_t **out_part_offsets, uint64_t **out_part_rows) {
    size_t *p_offsets = malloc((nproc + 1) * sizeof(size_t));
    uint64_t *p_rows = malloc((nproc + 1) * sizeof(uint64_t));
    
    uint64_t count = 0;
    size_t pos = 0;
    
    p_offsets[0] = 0;
    p_rows[0] = 0;
    
    size_t target_chunk = len / nproc;
    int current_part = 1;
    
    int w = fastcsv_simd_width();
    int quoted = 0;
    
    while (pos < len) {
        if (pos + w <= len) {
            int width;
            uint32_t mask = fastcsv_scan_newlines(buf + pos, quote, &width);
            if (mask == 0) { pos += width; continue; }
            while (mask != 0) {
                int bit = fastcsv_ctz(mask);
                char c = buf[pos + bit];
                if (c == quote) {
                    quoted = !quoted;
                } else if (!quoted && c == '\n') {
                    count++;
                    size_t nl_pos = pos + bit + 1;
                    if (current_part < nproc && nl_pos >= current_part * target_chunk) {
                        p_offsets[current_part] = nl_pos;
                        p_rows[current_part] = count;
                        current_part++;
                    }
                } else if (!quoted && c == '\r') {
                    // skip
                }
                mask &= mask - 1;
            }
            pos += width;
        } else {
            char c = buf[pos];
            if (c == quote) quoted = !quoted;
            else if (!quoted && c == '\n') {
                count++;
                size_t nl_pos = pos + 1;
                if (current_part < nproc && nl_pos >= current_part * target_chunk) {
                    p_offsets[current_part] = nl_pos;
                    p_rows[current_part] = count;
                    current_part++;
                }
            }
            pos++;
        }
    }
    
    while (current_part <= nproc) {
        p_offsets[current_part] = len;
        p_rows[current_part] = count;
        current_part++;
    }
    
    *out_part_offsets = p_offsets;
    *out_part_rows = p_rows;
    return count;
}

int main() {
    int fd = open("bench/data/data_wide.csv", O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    char *buf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    struct timespec t0, t1;
    fastcsv_detect_cpu();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    
    size_t *p_off;
    uint64_t *p_rows;
    uint64_t total = fastcsv_count_rows_and_partitions(buf, st.st_size, '"', 12, &p_off, &p_rows);
    
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    printf("total_rows = %llu, count took: %f ms\n", (unsigned long long)total, ms);
    return 0;
}
