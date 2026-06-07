#ifndef FASTCSV_SIMD_H
#define FASTCSV_SIMD_H

#include <stdint.h>
#include <stddef.h>

/* Returns a bitmask where bit i=1 means buf[i] is a special character
   (delimiter, quote, \n, or \r). buf must be readable for 32 bytes
   on the AVX2 path and 16 bytes on the SSE4.2 path.
   width_out is set to 32 (AVX2), 16 (SSE4.2), or 1 (scalar).        */
uint32_t fastcsv_scan_chunk(const char *buf,
                             char delim, char quote,
                             int *width_out);

uint32_t fastcsv_scan_newlines(const char *buf, char quote, int *width_out);

/* Runtime CPU feature detection. Called once at startup.
   Sets internal flags for AVX2 / SSE4.2 availability.               */
void fastcsv_detect_cpu(void);

/* The width the scanner will use on this CPU: 32, 16, or 1.         */
int fastcsv_simd_width(void);

/* Direct scan without width_out overhead. Call fastcsv_detect_cpu() first. */
uint32_t fastcsv_scan_direct(const char *buf, char delim, char quote);

#endif
