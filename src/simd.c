#include "simd.h"
#include "fastcsv.h"

static int g_simd_width = 0;  /* 0 = not detected yet */

typedef uint32_t (*fastcsv_scan_fn_t)(const char *, char, char);
static fastcsv_scan_fn_t g_scan_fn = NULL;

/* --- scan implementations --- */

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <nmmintrin.h>

FASTCSV_TARGET_AVX2
static uint32_t scan_avx2(const char *buf, char delim, char quote) {
#if defined(_MSC_VER)
    /* MSVC doesn't have __builtin_prefetch, ignore it or use _mm_prefetch */
    _mm_prefetch(buf + 256, _MM_HINT_T0);
#else
    __builtin_prefetch(buf + 256, 0, 0);  /* prefetch ahead */
#endif
    __m256i chunk  = _mm256_loadu_si256((const __m256i *)buf);
    __m256i vd     = _mm256_set1_epi8(delim);
    __m256i vq     = _mm256_set1_epi8(quote);
    __m256i vnl    = _mm256_set1_epi8('\n');
    __m256i vcr    = _mm256_set1_epi8('\r');

    uint32_t mask =
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vd))  |
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vq))  |
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vnl)) |
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vcr));
    return mask;
}

FASTCSV_TARGET_SSE42
static uint32_t scan_sse42(const char *buf, char delim, char quote) {
    __m128i chunk  = _mm_loadu_si128((const __m128i *)buf);
    __m128i vd     = _mm_set1_epi8(delim);
    __m128i vq     = _mm_set1_epi8(quote);
    __m128i vnl    = _mm_set1_epi8('\n');
    __m128i vcr    = _mm_set1_epi8('\r');

    uint32_t mask =
        (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vd))  |
        (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vq))  |
        (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vnl)) |
        (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vcr));
    return mask;
}
#endif

static uint32_t scan_scalar(const char *buf, char delim, char quote) {
    uint32_t mask = 0;
    char c = buf[0];
    if (c == delim || c == quote || c == '\n' || c == '\r') mask = 1;
    return mask;
}

/* --- public API --- */

void fastcsv_detect_cpu(void) {
#if defined(__x86_64__) || defined(_M_X64)
    if (__builtin_cpu_supports("avx2"))       { g_simd_width = 32; g_scan_fn = scan_avx2; }
    else if (__builtin_cpu_supports("sse4.2")) { g_simd_width = 16; g_scan_fn = scan_sse42; }
    else                                       { g_simd_width =  1; g_scan_fn = scan_scalar; }
#else
    g_simd_width = 1;
    g_scan_fn = scan_scalar;
#endif
}

int fastcsv_simd_width(void) {
    if (g_simd_width == 0) fastcsv_detect_cpu();
    return g_simd_width;
}

uint32_t fastcsv_scan_chunk(const char *buf, char delim, char quote, int *width_out) {
    int w = fastcsv_simd_width();
    *width_out = w;
#if defined(__x86_64__) || defined(_M_X64)
    if (w == 32) return scan_avx2(buf, delim, quote);
    if (w == 16) return scan_sse42(buf, delim, quote);
#endif
    return scan_scalar(buf, delim, quote);
}

uint32_t fastcsv_scan_direct(const char *buf, char delim, char quote) {
    return g_scan_fn(buf, delim, quote);
}

#if defined(__x86_64__) || defined(_M_X64)
FASTCSV_TARGET_AVX2
static uint32_t scan_nl_avx2(const char *buf, char quote) {
    __m256i chunk  = _mm256_loadu_si256((const __m256i *)buf);
    __m256i vq     = _mm256_set1_epi8(quote);
    __m256i vnl    = _mm256_set1_epi8('\n');
    __m256i vcr    = _mm256_set1_epi8('\r');

    return (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vq))  |
           (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vnl)) |
           (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vcr));
}

FASTCSV_TARGET_SSE42
static uint32_t scan_nl_sse42(const char *buf, char quote) {
    __m128i chunk  = _mm_loadu_si128((const __m128i *)buf);
    __m128i vq     = _mm_set1_epi8(quote);
    __m128i vnl    = _mm_set1_epi8('\n');
    __m128i vcr    = _mm_set1_epi8('\r');

    return (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vq))  |
           (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vnl)) |
           (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vcr));
}
#endif

static uint32_t scan_nl_scalar(const char *buf, char quote) {
    uint32_t mask = 0;
    char c = buf[0];
    if (c == quote || c == '\n' || c == '\r') mask = 1;
    return mask;
}

uint32_t fastcsv_scan_newlines(const char *buf, char quote, int *width_out) {
    int w = fastcsv_simd_width();
    *width_out = w;
#if defined(__x86_64__) || defined(_M_X64)
    if (w == 32) return scan_nl_avx2(buf, quote);
    if (w == 16) return scan_nl_sse42(buf, quote);
#endif
    return scan_nl_scalar(buf, quote);
}
