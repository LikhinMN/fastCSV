/*
 * simd_parse.h - SIMD-accelerated integer parser
 *
 * Uses SSSE3 _mm_maddubs_epi16 / _mm_madd_epi16 to convert up to 16
 * ASCII decimal digits into a 64-bit integer in very few instructions.
 * Falls back to scalar code on non-x86 or when SSSE3 is unavailable.
 *
 * Usage:
 *   int ok;
 *   int64_t val = fastcsv_parse_int_fast(buf, len, &ok);
 *   if (!ok) { ... handle error ... }
 */

#ifndef FASTCSV_SIMD_PARSE_H
#define FASTCSV_SIMD_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Platform detection                                                 */
/* ------------------------------------------------------------------ */
#if defined(__SSSE3__) || defined(__AVX2__) || defined(__AVX__)
  #define FASTCSV_HAVE_SSSE3 1
  #include <immintrin.h>
#else
  #define FASTCSV_HAVE_SSSE3 0
#endif

/* ------------------------------------------------------------------ */
/*  Scalar fallback (used on non-x86 and for len > 18)                 */
/* ------------------------------------------------------------------ */

/*
 * fastcsv_parse_uint_scalar - parse an unsigned decimal integer (scalar).
 *
 * @s   : pointer to first digit
 * @len : number of characters (must all be '0'-'9')
 *
 * Caller is responsible for validating digits beforehand.
 * Returns the parsed uint64_t value.
 */
static inline uint64_t
fastcsv_parse_uint_scalar(const char *s, size_t len)
{
    uint64_t val = 0;
    for (size_t i = 0; i < len; i++)
        val = val * 10 + (uint64_t)(s[i] - '0');
    return val;
}

/* ------------------------------------------------------------------ */
/*  SSSE3 fast path                                                    */
/* ------------------------------------------------------------------ */
#if FASTCSV_HAVE_SSSE3

/*
 * fastcsv_ssse3_8digits - parse up to 8 right-aligned ASCII digits
 *                         using SSSE3 multiply-add chains.
 *
 * The input is an 8-byte buffer where unused leading positions are
 * filled with '0'.
 *
 * Algorithm:
 *   1. Subtract '0' from each byte         → 8 × 8-bit values
 *   2. _mm_maddubs_epi16 with {10,1,…}     → 4 × 16-bit values
 *   3. _mm_madd_epi16    with {100,1,…}    → 2 × 32-bit values
 *   4. Combine: hi * 10000 + lo
 */
__attribute__((target("ssse3")))
static inline uint64_t
fastcsv_ssse3_8digits(const uint8_t buf8[8])
{
    /* Load 8 bytes into the low half of a 128-bit register. */
    __m128i v = _mm_loadl_epi64((const __m128i *)buf8);

    /* Subtract ASCII '0' to get numeric values 0-9. */
    __m128i zero_ascii = _mm_set1_epi8('0');
    v = _mm_sub_epi8(v, zero_ascii);

    /*
     * Multiply adjacent pairs and add:
     *   (d0*10+d1, d2*10+d3, d4*10+d5, d6*10+d7) as 16-bit values.
     *
     * _mm_maddubs_epi16 treats first arg as unsigned bytes, second as
     * signed bytes, multiplies pairwise and adds horizontally to 16-bit.
     * Use _mm_setr_epi8 for natural memory order (byte 0 first).
     */
    __m128i mul10 = _mm_setr_epi8(10,1,10,1, 10,1,10,1, 0,0,0,0, 0,0,0,0);
    v = _mm_maddubs_epi16(v, mul10);

    /*
     * Multiply adjacent 16-bit pairs and add to 32-bit:
     *   (pair0*100 + pair1, pair2*100 + pair3) as 32-bit values.
     * Use _mm_setr_epi16 for natural memory order.
     */
    __m128i mul100 = _mm_setr_epi16(100, 1, 100, 1, 0, 0, 0, 0);
    v = _mm_madd_epi16(v, mul100);

    /* Extract the two 32-bit results.
     * vals[0] = d0*1000 + d1*100 + d2*10 + d3  (high 4 digits)
     * vals[1] = d4*1000 + d5*100 + d6*10 + d7  (low 4 digits)
     */
    uint32_t vals[4];
    _mm_storeu_si128((__m128i *)vals, v);

    return (uint64_t)vals[0] * 10000ULL + (uint64_t)vals[1];
}

/*
 * fastcsv_parse_uint_ssse3 - parse unsigned decimal digits with SSSE3.
 *
 * @s   : pointer to first digit (must all be '0'-'9')
 * @len : number of digits (1..18)
 *
 * For len <= 8: right-align into a single 8-byte chunk.
 * For len 9-16: split into high-part and low 8-digit part.
 * For len 17-18: split into high-part and low 8-digit part.
 *
 * Caller validates digits and bounds before calling.
 */
__attribute__((target("ssse3")))
static inline uint64_t
fastcsv_parse_uint_ssse3(const char *s, size_t len)
{
    if (len <= 8) {
        /*
         * Right-align the digits in an 8-byte buffer, padding the
         * leading positions with '0' so the SIMD path produces
         * the correct positional values.
         */
        uint8_t buf8[8];
        memset(buf8, '0', 8);
        memcpy(buf8 + (8 - len), s, len);
        return fastcsv_ssse3_8digits(buf8);
    }

    /*
     * For 9-18 digits: split into a high part (first len-8 digits)
     * and a low part (last 8 digits).
     *
     *   value = high_part * 10^8 + low_part
     */
    size_t hi_len = len - 8;

    /* Parse the high part (1-10 digits). */
    uint8_t hi_buf[8];
    memset(hi_buf, '0', 8);
    memcpy(hi_buf + (8 - hi_len), s, hi_len);
    uint64_t hi = fastcsv_ssse3_8digits(hi_buf);

    /* Parse the low 8 digits. */
    uint8_t lo_buf[8];
    memcpy(lo_buf, s + hi_len, 8);
    uint64_t lo = fastcsv_ssse3_8digits(lo_buf);

    return hi * 100000000ULL + lo;
}

#endif /* FASTCSV_HAVE_SSSE3 */

/* ------------------------------------------------------------------ */
/*  Digit validation helper                                            */
/* ------------------------------------------------------------------ */

/*
 * fastcsv_all_digits - check that all characters in [s, s+len) are
 *                      ASCII digits '0'-'9'.
 *
 * Uses the branchless (unsigned)(c - '0') <= 9 idiom.
 * Returns 1 if all digits, 0 otherwise.
 */
static inline int
fastcsv_all_digits(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if ((unsigned)(s[i] - '0') > 9)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/*
 * fastcsv_parse_int_fast - parse a signed 64-bit integer from a
 *                          length-delimited string.
 *
 * @s   : pointer to the first character
 * @len : number of characters to parse
 * @ok  : set to 1 on success, 0 on failure (non-digit chars, overflow)
 *
 * Returns the parsed value, or 0 on failure.
 */
static inline int64_t
fastcsv_parse_int_fast(const char *s, size_t len, int *ok)
{
    const char *digits = s;
    size_t      dlen   = len;
    int         neg    = 0;

    *ok = 0;

    if (len == 0)
        return 0;

    /* Handle sign prefix. */
    if (*digits == '-') {
        neg = 1;
        digits++;
        dlen--;
    } else if (*digits == '+') {
        digits++;
        dlen--;
    }

    /* Must have at least one digit. */
    if (dlen == 0)
        return 0;

    /* Overflow risk: int64_t can hold at most 19 digits
     * (9223372036854775807).  For simplicity, reject > 18 digits
     * and fall back to scalar for 19 digits to handle the boundary. */
    if (dlen > 18) {
        /*
         * Scalar fallback for very long numbers.
         * Validate digits first.
         */
        if (dlen > 19 || !fastcsv_all_digits(digits, dlen))
            return 0;

        uint64_t val = fastcsv_parse_uint_scalar(digits, dlen);

        /* Check overflow for int64_t. */
        if (!neg && val > (uint64_t)INT64_MAX)
            return 0;
        if (neg && val > (uint64_t)INT64_MAX + 1ULL)
            return 0;

        *ok = 1;
        return neg ? -(int64_t)val : (int64_t)val;
    }

    /* Validate all characters are digits. */
    if (!fastcsv_all_digits(digits, dlen))
        return 0;

    uint64_t uval;

#if FASTCSV_HAVE_SSSE3
    uval = fastcsv_parse_uint_ssse3(digits, dlen);
#else
    uval = fastcsv_parse_uint_scalar(digits, dlen);
#endif

    *ok = 1;
    return neg ? -(int64_t)uval : (int64_t)uval;
}

#endif /* FASTCSV_SIMD_PARSE_H */
