/* fpnat.h - BASIC's numbers as the C compiler's float.
 *
 * A number is a single-precision IEEE float: what the FPU of a
 * Cortex-M4F computes in hardware, and what the STM32F103 lacks.
 *
 * A float has a 24-bit fraction, about seven decimal digits, and
 * magnitudes up to 3.4E38.  fp_format() prints six of those digits,
 * the way BASIC-11 did in single precision.  There is no libm: the
 * elementary functions are computed here, from square root, which
 * the FPU has, and short series.
 */
#ifndef FPNAT_H
#define FPNAT_H

#include "bas.h"

typedef float fpac_t;

#define FP_ERR_OVERFLOW 1
#define FP_ERR_DIVZERO  2
#define FP_ERR_DOMAIN   3   /* LOG of <= 0, SQR of < 0, 0 ^ negative */
#define FP_ERR_RANGE    4   /* conversion to an integer out of range */

/* Supplied by the program using the library; does not return. */
void fp_fault(int code);

void    fp_zero(fpac_t *d);
int     fp_iszero(const fpac_t *a);
int     fp_isneg(const fpac_t *a);
void    fp_neg(fpac_t *a);
void    fp_abs(fpac_t *a);
int     fp_cmp(const fpac_t *a, const fpac_t *b);      /* -1, 0 or 1 */
void    fp_add(fpac_t *a, const fpac_t *b);            /* a += b */
void    fp_sub(fpac_t *a, const fpac_t *b);            /* a -= b */
void    fp_mul(fpac_t *a, const fpac_t *b);            /* a *= b */
void    fp_div(fpac_t *a, const fpac_t *b);            /* a /= b */
void    fp_from_int(fpac_t *d, int32_t v);
void    fp_from_uint(fpac_t *d, uint32_t v);
int32_t fp_to_int(const fpac_t *a);                    /* toward zero; faults out of range */
void    fp_trunc(fpac_t *a);                           /* toward zero */
void    fp_floor(fpac_t *a);
void    fp_ldexp(fpac_t *a, int n);                    /* a *= 2^n */
void    fp_sqrt(fpac_t *a);
void    fp_exp(fpac_t *a);
void    fp_log(fpac_t *a);
void    fp_sin(fpac_t *a);
void    fp_cos(fpac_t *a);
void    fp_atan(fpac_t *a);
void    fp_pow(fpac_t *a, const fpac_t *b);            /* a = a ^ b */
int     fp_parse(const char *s, fpac_t *d);            /* characters used, 0 if no number */
int     fp_format(const fpac_t *a, char *buf);         /* length; buf holds 32 bytes */
void    fp_init(void);

extern fpac_t fp_pi;
extern fpac_t fp_one;

#endif
