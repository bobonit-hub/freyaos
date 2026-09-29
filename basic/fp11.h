/* fp11.h - PDP-11 FP11 double precision arithmetic in 32-bit integers.
 *
 * A number is the FP11's D format in two 32-bit words.  The Freya VM
 * and cproc for it have no 64-bit integers, so the fraction is always
 * handled as a high and a low word, the way the 32-bit build of the
 * SIMH floating point unit does it; the arithmetic here is a port of
 * that code (PDP11/pdp11_fp.c, Robert M Supnik, MIT licence).
 */
#ifndef FP11_H
#define FP11_H

#include "bas.h"

/* h: sign, 8-bit exponent (excess 128), the top 23 fraction bits.
 * l: the low 32 fraction bits.  The fraction is 0.1xxx with the
 * leading one hidden.  Zero is exponent 0, and always {0, 0} here. */
typedef struct {
    uint32_t h, l;
} fpac_t;

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
