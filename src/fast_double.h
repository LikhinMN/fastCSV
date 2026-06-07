/*
 * fast_double.h - Fast double-precision floating-point parser
 *
 * Avoids glibc strtod overhead (no locale lookup, no errno setting) by
 * parsing directly into a uint64_t mantissa and applying a table-based
 * power-of-10.  Falls back to strtod for edge cases that exceed the
 * table range or mantissa precision.
 *
 * Usage:
 *   int ok;
 *   double val = fastcsv_parse_double(buf, len, &ok);
 *   if (!ok) { ... handle error ... }
 */

#ifndef FASTCSV_FAST_DOUBLE_H
#define FASTCSV_FAST_DOUBLE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* Exact powers of 10 representable in IEEE-754 double (1e0 .. 1e22). */
static const double fastcsv_pow10_table[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/*
 * fastcsv_parse_double - parse a double from a length-delimited string.
 *
 * @s   : pointer to the first character
 * @len : number of characters to parse (NOT null-terminated required)
 * @ok  : set to 1 on success, 0 on failure
 *
 * Returns the parsed value, or 0.0 on failure.
 */
static inline double
fastcsv_parse_double(const char *s, size_t len, int *ok)
{
    const char *p   = s;
    const char *end = s + len;

    uint64_t mantissa       = 0;
    int      mantissa_digits = 0;
    int      decimal_places  = 0;
    int      negative        = 0;
    int      has_digits      = 0;

    /* Reject empty or absurdly long input immediately. */
    if (len == 0 || len > 128)
        goto fallback;

    /* --- Sign --------------------------------------------------------- */
    if (*p == '-') {
        negative = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    /* --- Integer part ------------------------------------------------- */
    while (p < end && (unsigned)(*p - '0') <= 9) {
        unsigned d = (unsigned)(*p - '0');
        if (mantissa_digits < 19) {
            mantissa = mantissa * 10 + d;
            /* Don't count leading zeros toward the limit. */
            if (mantissa != 0 || d != 0)
                mantissa_digits++;
        } else {
            /* Too many significant digits — fall back to strtod. */
            goto fallback;
        }
        has_digits = 1;
        p++;
    }

    /* --- Fractional part ---------------------------------------------- */
    if (p < end && *p == '.') {
        p++;
        while (p < end && (unsigned)(*p - '0') <= 9) {
            unsigned d = (unsigned)(*p - '0');
            if (mantissa_digits < 19) {
                mantissa = mantissa * 10 + d;
                if (mantissa != 0 || d != 0)
                    mantissa_digits++;
                decimal_places++;
            } else {
                /*
                 * Extra fractional digits beyond 19 significant digits:
                 * they don't affect the mantissa but we still need to
                 * consume them.  Fall back for full precision.
                 */
                goto fallback;
            }
            has_digits = 1;
            p++;
        }
    }

    if (!has_digits)
        goto fallback;

    /* --- Exponent (e / E) --------------------------------------------- */
    int exponent = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        int exp_negative = 0;

        if (p < end && *p == '-') {
            exp_negative = 1;
            p++;
        } else if (p < end && *p == '+') {
            p++;
        }

        if (p >= end || !((unsigned)(*p - '0') <= 9))
            goto fallback;   /* 'e' with no digits */

        while (p < end && (unsigned)(*p - '0') <= 9) {
            exponent = exponent * 10 + (int)(*p - '0');
            if (exponent > 999)  /* way out of range */
                goto fallback;
            p++;
        }

        if (exp_negative)
            exponent = -exponent;
    }

    /* --- Must have consumed everything -------------------------------- */
    if (p != end)
        goto fallback;

    /* --- Combine mantissa + exponent ---------------------------------- */
    int combined_exp = exponent - decimal_places;

    /* Check range: double can represent 10^-308 .. 10^308. */
    if (combined_exp < -308 || combined_exp > 308)
        goto fallback;

    double result = (double)mantissa;

    if (combined_exp > 0) {
        if (combined_exp <= 22) {
            result *= fastcsv_pow10_table[combined_exp];
        } else if (combined_exp <= 44) {
            /* Chain two table lookups to stay within exact range. */
            result *= fastcsv_pow10_table[22];
            result *= fastcsv_pow10_table[combined_exp - 22];
        } else {
            goto fallback;
        }
    } else if (combined_exp < 0) {
        int abs_exp = -combined_exp;
        if (abs_exp <= 22) {
            result /= fastcsv_pow10_table[abs_exp];
        } else if (abs_exp <= 44) {
            result /= fastcsv_pow10_table[22];
            result /= fastcsv_pow10_table[abs_exp - 22];
        } else {
            goto fallback;
        }
    }
    /* combined_exp == 0: result is just (double)mantissa. */

    if (negative)
        result = -result;

    *ok = 1;
    return result;

fallback:
    {
        /*
         * Copy into a stack buffer so we can null-terminate for strtod.
         * We already rejected len > 128 above, but guard again for
         * safety after a direct goto.
         */
        if (len == 0 || len > 128) {
            *ok = 0;
            return 0.0;
        }

        char buf[132];  /* 128 + room for '\0' + small safety margin */
        memcpy(buf, s, len);
        buf[len] = '\0';

        char *endptr = NULL;
        double val = strtod(buf, &endptr);

        if (endptr == buf || endptr != buf + len) {
            *ok = 0;
            return 0.0;
        }

        *ok = 1;
        return val;
    }
}

#endif /* FASTCSV_FAST_DOUBLE_H */
