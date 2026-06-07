#include <stdio.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "src/parser.h"

int main() {
    int fd = open("bench/data/data_1m.csv", O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    char *buf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    
    size_t *row_offsets;
    uint64_t total = fastcsv_find_row_offsets(buf, st.st_size, ',', '"', &row_offsets);
    
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    printf("total_rows = %llu, find_row_offsets took: %f ms\n", (unsigned long long)total, ms);
    return 0;
}
