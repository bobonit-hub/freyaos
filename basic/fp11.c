/* fp11.c - PDP-11 FP11 double precision arithmetic in 32-bit integers.
 *
 * addfp11(), mulfp11(), divfp11(), frac_mulfp11() and round_and_pack()
 * are the SIMH routines of the same names, with the 64-bit macros of
 * that file turned into functions on a high and a low word.  The unit
 * runs as an FP11 set to double precision and rounding (FPS_D set,
 * FPS_T clear), with the underflow interrupt disabled - an underflow
 * gives zero - and the overflow one enabled - an overflow is a fault.
 *
 * Everything from fp_sqrt() down is new: the elementary functions as
 * series over these operations, and the conversions between D format
 * and decimal text that a BASIC needs.
 *
 *   Copyright (c) 1993-2015, Robert M Supnik (the FP11 arithmetic)
 *   MIT licence; see the SIMH source for the full text.
 */
#include "fp11.h"

#define FP_V_SIGN   31
#define FP_V_EXP    23
#define FP_V_HB     23                          /* hidden bit, unpacked */
#define FP_M_EXP    0377u
#define FP_SIGN     0x80000000u
#define FP_EXP      (FP_M_EXP << FP_V_EXP)
#define FP_HB       (1u << FP_V_HB)
#define FP_FRACH    (FP_HB - 1u)
#define FP_BIAS     0200
#define FP_GUARD    3

#define GET_SIGN(h)     (((h) >> FP_V_SIGN) & 1u)
#define GET_EXP(h)      (((h) >> FP_V_EXP) & FP_M_EXP)
#define GET_BIT(w, n)   (((w) >> (n)) & 1u)

fpac_t fp_pi;
fpac_t fp_one;
static fpac_t fp_ln2, fp_half_pi, fp_hp_hi, fp_hp_lo, fp_rsqrt2, fp_half;
static fpac_t fp_p10[6];                        /* 10^1, 10^2, 10^4 ... 10^32 */

/* ------------------------------------------------------------------ */
/* 64-bit fractions as two words                                      */

static void set64(fpac_t *d, uint32_t h, uint32_t l)
{
    d->h = h;
    d->l = l;
}

static void lsh64(fpac_t *d, int n)
{
    uint32_t h = d->h, l = d->l;

    if (n <= 0) return;
    if (n >= 64) { d->h = 0; d->l = 0; return; }
    if (n >= 32) { d->h = l << (n - 32); d->l = 0; return; }
    d->h = (h << n) | (l >> (32 - n));
    d->l = l << n;
}

static void rsh64(fpac_t *d, int n)
{
    uint32_t h = d->h, l = d->l;

    if (n <= 0) return;
    if (n >= 64) { d->h = 0; d->l = 0; return; }
    if (n >= 32) { d->l = h >> (n - 32); d->h = 0; return; }
    d->l = (l >> n) | (h << (32 - n));
    d->h = h >> n;
}

/* d = a + b */
static void add64(fpac_t *d, const fpac_t *a, const fpac_t *b)
{
    uint32_t l = a->l + b->l;
    uint32_t h = a->h + b->h + (l < b->l);

    d->h = h;
    d->l = l;
}

/* d = a - b */
static void sub64(fpac_t *d, const fpac_t *a, const fpac_t *b)
{
    uint32_t h = a->h - b->h - (a->l < b->l);
    uint32_t l = a->l - b->l;

    d->h = h;
    d->l = l;
}

static int lt64(const fpac_t *a, const fpac_t *b)
{
    return a->h < b->h || (a->h == b->h && a->l < b->l);
}

static int iszero64(const fpac_t *a)
{
    return (a->h | a->l) == 0;
}

/* |a| < |b| on the packed form: the exponent sits above the fraction. */
static int lt_ap(const fpac_t *a, const fpac_t *b)
{
    uint32_t ah = a->h & ~FP_SIGN, bh = b->h & ~FP_SIGN;

    return ah < bh || (ah == bh && a->l < b->l);
}

/* The 56-bit fraction with its hidden bit, right justified. */
static void get_frac(const fpac_t *s, fpac_t *d)
{
    d->l = s->l;
    d->h = (s->h & FP_FRACH) | FP_HB;
}

/* ------------------------------------------------------------------ */
/* The FP11                                                            */

/* Round a fraction with three guard bits, watch the exponent, pack.
 * The sign is already in place in *facp.  Returns non-zero on overflow;
 * an underflow is a zero. */
static int round_and_pack(fpac_t *facp, int exp, const fpac_t *fracp, int r)
{
    fpac_t frac = *fracp, rnd;

    if (r) {
        set64(&rnd, 0, 1u << (FP_GUARD - 1));
        add64(&frac, &frac, &rnd);
        if (GET_BIT(frac.h, FP_V_HB + FP_GUARD + 1)) {
            rsh64(&frac, 1);
            exp = exp + 1;
        }
    }
    rsh64(&frac, FP_GUARD);
    if (exp > 0377) {
        set64(facp, 0, 0);
        return 1;
    }
    if (exp <= 0) {
        set64(facp, 0, 0);
        return 0;
    }
    facp->l = frac.l;
    facp->h = (facp->h & FP_SIGN) | ((uint32_t)exp << FP_V_EXP) | (frac.h & FP_FRACH);
    return 0;
}

static int addfp11(fpac_t *facp, fpac_t *fsrcp)
{
    int facexp, fsrcexp, ediff;
    fpac_t facfrac, fsrcfrac, t;

    if (lt_ap(facp, fsrcp)) {                  /* |fac| < |fsrc|: swap */
        t = *facp;
        *facp = *fsrcp;
        *fsrcp = t;
    }
    facexp = GET_EXP(facp->h);
    fsrcexp = GET_EXP(fsrcp->h);
    if (facexp == 0) {
        if (fsrcexp) *facp = *fsrcp;
        else set64(facp, 0, 0);
        return 0;
    }
    if (fsrcexp == 0) return 0;
    ediff = facexp - fsrcexp;
    if (ediff >= 60) return 0;
    get_frac(facp, &facfrac);
    get_frac(fsrcp, &fsrcfrac);
    lsh64(&facfrac, FP_GUARD);
    lsh64(&fsrcfrac, FP_GUARD);
    if (GET_SIGN(facp->h) != GET_SIGN(fsrcp->h)) {
        if (ediff) rsh64(&fsrcfrac, ediff);
        sub64(&facfrac, &facfrac, &fsrcfrac);
        if (iszero64(&facfrac)) {
            set64(facp, 0, 0);
            return 0;
        }
        if (ediff <= 1) {                       /* big normalize */
            if ((facfrac.h & (0x00FFFFFFu << FP_GUARD)) == 0) {
                lsh64(&facfrac, 24);
                facexp = facexp - 24;
            }
            if ((facfrac.h & (0x00FFF000u << FP_GUARD)) == 0) {
                lsh64(&facfrac, 12);
                facexp = facexp - 12;
            }
            if ((facfrac.h & (0x00FC0000u << FP_GUARD)) == 0) {
                lsh64(&facfrac, 6);
                facexp = facexp - 6;
            }
        }
        while (GET_BIT(facfrac.h, FP_V_HB + FP_GUARD) == 0) {
            lsh64(&facfrac, 1);
            facexp = facexp - 1;
        }
    } else {
        if (ediff) rsh64(&fsrcfrac, ediff);
        add64(&facfrac, &facfrac, &fsrcfrac);
        if (GET_BIT(facfrac.h, FP_V_HB + FP_GUARD + 1)) {
            rsh64(&facfrac, 1);                 /* carry out */
            facexp = facexp + 1;
        }
    }
    return round_and_pack(facp, facexp, &facfrac, 1);
}

/* Shift-and-add: the inputs are unguarded fractions, the product is
 * guarded.  Only the top 64 bits of the 112-bit product are formed;
 * with normalized inputs that is the 56 bits plus guard that matter. */
static void frac_mulfp11(fpac_t *f1p, const fpac_t *f2p)
{
    uint32_t rh = 0, rl = 0, mh, ml, yh, yl;
    int i;

    yh = f1p->h;
    yl = f1p->l;
    mh = (f2p->h << FP_GUARD) | (f2p->l >> (32 - FP_GUARD));
    ml = f2p->l << FP_GUARD;
    if ((yl | ml) == 0) {                       /* 24b x 24b */
        for (i = 0; i < 24; i++) {
            if (yh & 1u) rh = rh + mh;
            rl = (rl >> 1) | (rh << 31);
            rh = rh >> 1;
            yh = yh >> 1;
        }
    } else {
        if (yl != 0) {
            for (i = 0; i < 32; i++) {
                if (yl & 1u) {
                    rl = rl + ml;
                    rh = rh + mh + (rl < ml);
                }
                rl = (rl >> 1) | (rh << 31);
                rh = rh >> 1;
                yl = yl >> 1;
            }
        }
        for (i = 0; i < 24; i++) {
            if (yh & 1u) {
                rl = rl + ml;
                rh = rh + mh + (rl < ml);
            }
            rl = (rl >> 1) | (rh << 31);
            rh = rh >> 1;
            yh = yh >> 1;
        }
    }
    f1p->h = rh;
    f1p->l = rl;
}

static int mulfp11(fpac_t *facp, const fpac_t *fsrcp)
{
    int facexp, fsrcexp;
    fpac_t facfrac, fsrcfrac;

    facexp = GET_EXP(facp->h);
    fsrcexp = GET_EXP(fsrcp->h);
    if (facexp == 0 || fsrcexp == 0) {
        set64(facp, 0, 0);
        return 0;
    }
    get_frac(facp, &facfrac);
    get_frac(fsrcp, &fsrcfrac);
    facexp = facexp + fsrcexp - FP_BIAS;
    facp->h = facp->h ^ fsrcp->h;
    frac_mulfp11(&facfrac, &fsrcfrac);
    /* [.5,1) x [.5,1) is [.25,1): at most one bit of normalization */
    if (GET_BIT(facfrac.h, FP_V_HB + FP_GUARD) == 0) {
        lsh64(&facfrac, 1);
        facexp = facexp - 1;
    }
    return round_and_pack(facp, facexp, &facfrac, 1);
}

/* The caller has checked the divisor for zero. */
static int divfp11(fpac_t *facp, const fpac_t *fsrcp)
{
    int facexp, fsrcexp, i;
    uint32_t dh, dl, sh, sl, qh = 0, ql = 0;
    fpac_t facfrac, fsrcfrac, quo;

    fsrcexp = GET_EXP(fsrcp->h);
    facexp = GET_EXP(facp->h);
    if (facexp == 0) {
        set64(facp, 0, 0);
        return 0;
    }
    get_frac(facp, &facfrac);
    get_frac(fsrcp, &fsrcfrac);
    lsh64(&facfrac, FP_GUARD);
    lsh64(&fsrcfrac, FP_GUARD);
    facexp = facexp - fsrcexp + FP_BIAS + 1;
    facp->h = facp->h ^ fsrcp->h;
    dh = facfrac.h;
    dl = facfrac.l;
    sh = fsrcfrac.h;
    sl = fsrcfrac.l;
    for (i = FP_V_HB + FP_GUARD + 33; i > 0 && (dh | dl) != 0; i--) {
        qh = (qh << 1) | (ql >> 31);
        ql = ql << 1;
        if (!(dh < sh || (dh == sh && dl < sl))) {  /* divd >= divr */
            dh = dh - sh - (dl < sl);
            dl = dl - sl;
            ql = ql | 1u;
        }
        dh = (dh << 1) | (dl >> 31);
        dl = dl << 1;
    }
    set64(&quo, qh, ql);
    if (i > 0) lsh64(&quo, i);                  /* early exit */
    /* [.5,1) / [.5,1) is [.5,2): at most one bit of normalization */
    if (GET_BIT(quo.h, FP_V_HB + FP_GUARD) == 0) {
        lsh64(&quo, 1);
        facexp = facexp - 1;
    }
    return round_and_pack(facp, facexp, &quo, 1);
}

/* ------------------------------------------------------------------ */
/* The operations a program calls                                      */

void fp_zero(fpac_t *d)
{
    set64(d, 0, 0);
}

int fp_iszero(const fpac_t *a)
{
    return GET_EXP(a->h) == 0;
}

int fp_isneg(const fpac_t *a)
{
    return GET_EXP(a->h) != 0 && GET_SIGN(a->h);
}

void fp_neg(fpac_t *a)
{
    if (GET_EXP(a->h)) a->h ^= FP_SIGN;
}

void fp_abs(fpac_t *a)
{
    a->h &= ~FP_SIGN;
}

int fp_cmp(const fpac_t *a, const fpac_t *b)
{
    int sa = GET_EXP(a->h) ? (GET_SIGN(a->h) ? -1 : 1) : 0;
    int sb = GET_EXP(b->h) ? (GET_SIGN(b->h) ? -1 : 1) : 0;
    int m;

    if (sa != sb) return sa < sb ? -1 : 1;
    if (sa == 0) return 0;
    if (lt_ap(a, b)) m = -1;
    else if ((a->h & ~FP_SIGN) == (b->h & ~FP_SIGN) && a->l == b->l) m = 0;
    else m = 1;
    return sa < 0 ? -m : m;
}

void fp_add(fpac_t *a, const fpac_t *b)
{
    fpac_t s = *b;

    if (addfp11(a, &s)) fp_fault(FP_ERR_OVERFLOW);
}

void fp_sub(fpac_t *a, const fpac_t *b)
{
    fpac_t s = *b;

    if (GET_EXP(s.h)) s.h ^= FP_SIGN;
    if (addfp11(a, &s)) fp_fault(FP_ERR_OVERFLOW);
}

void fp_mul(fpac_t *a, const fpac_t *b)
{
    if (mulfp11(a, b)) fp_fault(FP_ERR_OVERFLOW);
}

void fp_div(fpac_t *a, const fpac_t *b)
{
    if (GET_EXP(b->h) == 0) fp_fault(FP_ERR_DIVZERO);
    if (divfp11(a, b)) fp_fault(FP_ERR_OVERFLOW);
}

/* LDCLD: a 32-bit integer is exact in 56 bits. */
void fp_from_int(fpac_t *d, int32_t v)
{
    uint32_t u, sign = 0;
    int i;

    if (v == 0) {
        set64(d, 0, 0);
        return;
    }
    if (v < 0) {
        sign = FP_SIGN;
        u = (uint32_t)0 - (uint32_t)v;
    } else {
        u = (uint32_t)v;
    }
    for (i = 0; (u & 0x80000000u) == 0; i++) u = u << 1;
    d->h = sign | ((uint32_t)(FP_BIAS + 32 - i) << FP_V_EXP) | ((u >> 8) & FP_FRACH);
    d->l = u << 24;
}

/* STCDL: toward zero, a fault when the result does not fit. */
int32_t fp_to_int(const fpac_t *a)
{
    int exp = GET_EXP(a->h), sign = GET_SIGN(a->h);
    fpac_t f;
    uint32_t lim;

    if (exp <= FP_BIAS) return 0;
    if (exp > FP_BIAS + 32) fp_fault(FP_ERR_RANGE);
    get_frac(a, &f);
    rsh64(&f, FP_V_HB + 1 + FP_BIAS + 32 - exp);
    lim = sign ? 0x80000001u : 0x80000000u;
    if (f.l >= lim) fp_fault(FP_ERR_RANGE);
    return sign ? -(int32_t)f.l : (int32_t)f.l;
}

void fp_trunc(fpac_t *a)
{
    int exp = GET_EXP(a->h), nbits;

    if (exp <= FP_BIAS) {
        set64(a, 0, 0);
        return;
    }
    nbits = FP_BIAS + 56 - exp;                 /* fraction bits below the point */
    if (nbits <= 0) return;
    if (nbits >= 32) {
        a->l = 0;
        a->h &= ~((1u << (nbits - 32)) - 1u);
    } else {
        a->l &= ~((1u << nbits) - 1u);
    }
}

void fp_floor(fpac_t *a)
{
    fpac_t t = *a;

    fp_trunc(&t);
    if (fp_isneg(a) && fp_cmp(&t, a) != 0) fp_sub(&t, &fp_one);
    *a = t;
}

void fp_ldexp(fpac_t *a, int n)
{
    int exp = GET_EXP(a->h);

    if (exp == 0) return;
    exp += n;
    if (exp > 0377) fp_fault(FP_ERR_OVERFLOW);
    if (exp <= 0) {
        set64(a, 0, 0);
        return;
    }
    a->h = (a->h & ~FP_EXP) | ((uint32_t)exp << FP_V_EXP);
}

/* ------------------------------------------------------------------ */
/* Elementary functions                                                */

/* True once |term| is below the last bit of |sum|. */
static int negligible(const fpac_t *term, const fpac_t *sum)
{
    int et = GET_EXP(term->h), es = GET_EXP(sum->h);

    return et == 0 || et + 58 < es;
}

void fp_sqrt(fpac_t *a)
{
    fpac_t x, y, q;
    int e, i;

    if (fp_isneg(a)) fp_fault(FP_ERR_DOMAIN);
    if (fp_iszero(a)) return;
    /* a = f * 2^e with f in [.5, 1); start from 2^(e/2) and let Newton
     * do the rest: the guess is within a factor of two, so eight
     * rounds are more than the 56 bits need. */
    e = GET_EXP(a->h) - FP_BIAS;
    x = fp_one;
    fp_ldexp(&x, (e + 1) >> 1);
    for (i = 0; i < 10; i++) {
        q = *a;
        fp_div(&q, &x);
        y = x;
        fp_add(&y, &q);
        fp_ldexp(&y, -1);
        if (fp_cmp(&y, &x) == 0) break;
        x = y;
    }
    *a = x;
}

void fp_exp(fpac_t *a)
{
    fpac_t k, r, term, sum, n;
    int ki, i;

    if (fp_iszero(a)) {
        *a = fp_one;
        return;
    }
    k = *a;                                     /* k = round(a / ln 2) */
    fp_div(&k, &fp_ln2);
    fp_add(&k, &fp_half);
    fp_floor(&k);
    if (GET_EXP(k.h) > FP_BIAS + 8) {           /* |k| >= 256: out of range */
        if (!fp_isneg(&k)) fp_fault(FP_ERR_OVERFLOW);
        set64(a, 0, 0);
        return;
    }
    ki = fp_to_int(&k);
    if (ki > 128) fp_fault(FP_ERR_OVERFLOW);
    if (ki < -130) {
        set64(a, 0, 0);
        return;
    }
    r = fp_ln2;                                 /* r = a - k ln 2, |r| <= ln2 / 2 */
    fp_mul(&r, &k);
    fp_neg(&r);
    fp_add(&r, a);
    sum = fp_one;
    term = fp_one;
    for (i = 1; i < 40; i++) {
        fp_mul(&term, &r);
        fp_from_int(&n, i);
        fp_div(&term, &n);
        fp_add(&sum, &term);
        if (negligible(&term, &sum)) break;
    }
    fp_ldexp(&sum, ki);
    *a = sum;
}

void fp_log(fpac_t *a)
{
    fpac_t f, z, z2, term, sum, q, n;
    int e, i;

    if (fp_isneg(a) || fp_iszero(a)) fp_fault(FP_ERR_DOMAIN);
    e = GET_EXP(a->h) - FP_BIAS;                /* a = f 2^e, f in [.5, 1) */
    f = *a;
    f.h = (f.h & ~FP_EXP) | ((uint32_t)FP_BIAS << FP_V_EXP);
    if (fp_cmp(&f, &fp_rsqrt2) < 0) {           /* bring f into [1/sqrt2, sqrt2) */
        fp_ldexp(&f, 1);
        e = e - 1;
    }
    z = f;                                      /* z = (f - 1) / (f + 1) */
    fp_sub(&z, &fp_one);
    q = f;
    fp_add(&q, &fp_one);
    fp_div(&z, &q);
    z2 = z;
    fp_mul(&z2, &z);
    sum = z;
    term = z;
    for (i = 3; i < 80; i += 2) {
        fp_mul(&term, &z2);
        q = term;
        fp_from_int(&n, i);
        fp_div(&q, &n);
        fp_add(&sum, &q);
        if (negligible(&q, &sum)) break;
    }
    fp_ldexp(&sum, 1);                          /* ln f = 2 atanh z */
    fp_from_int(&q, e);
    fp_mul(&q, &fp_ln2);
    fp_add(&sum, &q);
    *a = sum;
}

/* sin or cos of r, |r| <= pi/4, by the Taylor series. */
static void sincos_small(fpac_t *r, int cosine)
{
    fpac_t r2, term, sum, n;
    int i;

    r2 = *r;
    fp_mul(&r2, r);
    if (cosine) {
        sum = fp_one;
        term = fp_one;
        i = 1;
    } else {
        sum = *r;
        term = *r;
        i = 2;
    }
    for (; i < 60; i += 2) {
        fp_mul(&term, &r2);
        fp_from_int(&n, i * (i + 1));
        fp_div(&term, &n);
        fp_neg(&term);
        fp_add(&sum, &term);
        if (negligible(&term, &sum)) break;
    }
    *r = sum;
}

/* Quadrant reduction shared by sin and cos: q counts quarter turns. */
static void sincos_q(fpac_t *a, int q)
{
    fpac_t k, k2, r;
    int ki;

    k = *a;                                     /* k = round(a / (pi/2)) */
    fp_div(&k, &fp_half_pi);
    fp_add(&k, &fp_half);
    fp_floor(&k);
    ki = fp_to_int(&k);
    /* pi/2 in two parts: k times the short high part is exact, so
     * what is lost is only k times the last bit of the low part */
    r = fp_hp_hi;
    fp_mul(&r, &k);
    fp_neg(&r);
    fp_add(&r, a);
    k2 = fp_hp_lo;
    fp_mul(&k2, &k);
    fp_sub(&r, &k2);
    q = (q + ki) & 3;
    sincos_small(&r, q & 1);
    if (q >= 2) fp_neg(&r);
    *a = r;
}

void fp_sin(fpac_t *a)
{
    sincos_q(a, 0);
}

void fp_cos(fpac_t *a)
{
    sincos_q(a, 1);
}

void fp_atan(fpac_t *a)
{
    fpac_t x, x2, term, sum, q, n;
    int neg, inv, i;

    if (fp_iszero(a)) return;
    neg = fp_isneg(a);
    x = *a;
    fp_abs(&x);
    inv = fp_cmp(&x, &fp_one) > 0;
    if (inv) {                                  /* atan x = pi/2 - atan 1/x */
        q = fp_one;
        fp_div(&q, &x);
        x = q;
    }
    for (i = 0; i < 2; i++) {                   /* atan x = 2 atan x/(1+sqrt(1+x^2)) */
        q = x;
        fp_mul(&q, &x);
        fp_add(&q, &fp_one);
        fp_sqrt(&q);
        fp_add(&q, &fp_one);
        fp_div(&x, &q);
    }
    x2 = x;
    fp_mul(&x2, &x);
    sum = x;
    term = x;
    for (i = 3; i < 80; i += 2) {
        fp_mul(&term, &x2);
        fp_neg(&term);
        q = term;
        fp_from_int(&n, i);
        fp_div(&q, &n);
        fp_add(&sum, &q);
        if (negligible(&q, &sum)) break;
    }
    fp_ldexp(&sum, 2);
    if (inv) {
        q = fp_half_pi;
        fp_sub(&q, &sum);
        sum = q;
    }
    if (neg) fp_neg(&sum);
    *a = sum;
}

void fp_pow(fpac_t *a, const fpac_t *b)
{
    fpac_t t, base, r;
    int n, neg;

    if (fp_iszero(b)) {
        *a = fp_one;
        return;
    }
    if (fp_iszero(a)) {
        if (fp_isneg(b)) fp_fault(FP_ERR_DIVZERO);
        return;
    }
    t = *b;
    fp_trunc(&t);
    if (fp_cmp(&t, b) == 0 && GET_EXP(t.h) <= FP_BIAS + 16) {
        n = fp_to_int(&t);                      /* an integer power: multiply */
        neg = n < 0;
        if (neg) n = -n;
        base = *a;
        r = fp_one;
        while (n) {
            if (n & 1) fp_mul(&r, &base);
            n >>= 1;
            if (n) fp_mul(&base, &base);
        }
        if (neg) {
            t = fp_one;
            fp_div(&t, &r);
            r = t;
        }
        *a = r;
        return;
    }
    if (fp_isneg(a)) fp_fault(FP_ERR_DOMAIN);
    fp_log(a);
    fp_mul(a, b);
    fp_exp(a);
}

/* ------------------------------------------------------------------ */
/* Decimal conversion                                                  */

/* a *= 10^k, in steps the D format's range allows. */
static void scale10(fpac_t *a, int k)
{
    int i;

    while (k > 32) {
        fp_mul(a, &fp_p10[5]);
        k -= 32;
    }
    while (k < -32) {
        fp_div(a, &fp_p10[5]);
        k += 32;
    }
    if (k > 0) {
        for (i = 0; i < 6; i++)
            if (k & (1 << i)) fp_mul(a, &fp_p10[i]);
    } else if (k < 0) {
        k = -k;
        for (i = 0; i < 6; i++)
            if (k & (1 << i)) fp_div(a, &fp_p10[i]);
    }
}

/* An unsigned 64-bit integer to D format, rounded once. */
static void from_u64(fpac_t *d, uint32_t h, uint32_t l)
{
    fpac_t n, frac;
    int s = 0;
    uint32_t sticky;

    if ((h | l) == 0) {
        set64(d, 0, 0);
        return;
    }
    set64(&n, h, l);
    while ((n.h & 0x80000000u) == 0) {
        lsh64(&n, 1);
        s++;
    }
    /* 64 bits down to 56 plus guard; anything shifted out is sticky */
    sticky = n.l & 0x1fu;
    frac = n;
    rsh64(&frac, 5);
    if (sticky) frac.l |= 1u;
    d->h = 0;
    if (round_and_pack(d, FP_BIAS + 64 - s, &frac, 1)) fp_fault(FP_ERR_OVERFLOW);
}

/* The integer part of a non-negative a below 2^63, as 64 bits. */
static void to_u64(const fpac_t *a, fpac_t *d)
{
    int exp = GET_EXP(a->h), shift;

    if (exp <= FP_BIAS) {
        set64(d, 0, 0);
        return;
    }
    get_frac(a, d);
    shift = exp - FP_BIAS - 56;
    if (shift >= 0) lsh64(d, shift);
    else rsh64(d, -shift);
}

/* d = d / 10, returns the remainder.  Two 16-bit steps keep the
 * dividend inside 32 bits. */
static int divmod10(fpac_t *d)
{
    uint32_t qh, r, t, q1, q0;

    qh = d->h / 10u;
    r = d->h - qh * 10u;
    t = (r << 16) | (d->l >> 16);
    q1 = t / 10u;
    r = t - q1 * 10u;
    t = (r << 16) | (d->l & 0xffffu);
    q0 = t / 10u;
    r = t - q0 * 10u;
    d->h = qh;
    d->l = (q1 << 16) | q0;
    return (int)r;
}

static void mul10add(fpac_t *d, int digit)
{
    fpac_t t = *d, a;

    lsh64(d, 3);                                /* 8d + 2d + digit */
    lsh64(&t, 1);
    add64(d, d, &t);
    set64(&a, 0, (uint32_t)digit);
    add64(d, d, &a);
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

int fp_parse(const char *s, fpac_t *d)
{
    fpac_t mant;
    int i = 0, nsig = 0, exp10 = 0, esign = 1, e = 0, any = 0;

    set64(&mant, 0, 0);
    while (is_digit(s[i])) {
        any = 1;
        if (nsig < 18) {
            if (nsig || s[i] != '0') nsig++;
            mul10add(&mant, s[i] - '0');
        } else {
            exp10++;
        }
        i++;
    }
    if (s[i] == '.') {
        i++;
        while (is_digit(s[i])) {
            any = 1;
            if (nsig < 18) {
                if (nsig || s[i] != '0') nsig++;
                mul10add(&mant, s[i] - '0');
                exp10--;
            }
            i++;
        }
    }
    if (!any) return 0;
    if ((s[i] == 'E' || s[i] == 'e') &&
        (is_digit(s[i + 1]) ||
         ((s[i + 1] == '+' || s[i + 1] == '-') && is_digit(s[i + 2])))) {
        i++;
        if (s[i] == '+') i++;
        else if (s[i] == '-') { esign = -1; i++; }
        while (is_digit(s[i])) {
            if (e < 1000) e = e * 10 + (s[i] - '0');
            i++;
        }
        exp10 += esign * e;
    }
    from_u64(d, mant.h, mant.l);
    if (!fp_iszero(d)) {
        if (exp10 > 60) fp_fault(FP_ERR_OVERFLOW);
        if (exp10 < -100) set64(d, 0, 0);
        else scale10(d, exp10);
    }
    return i;
}

/* Up to fifteen significant digits, the way a BASIC prints them: an
 * integer as itself, a fraction with a bare point, and E notation
 * outside 1E-5 .. 1E15.  No leading or trailing blank. */
int fp_format(const fpac_t *a, char *buf)
{
    fpac_t x, n, lim;
    char dig[20];
    int e2, est, nd, i, k, E, len = 0, neg;

    if (fp_iszero(a)) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    neg = fp_isneg(a);
    x = *a;
    fp_abs(&x);
    /* x = f 2^e2 with f in [.5, 1): its decimal exponent is est or est+1 */
    e2 = GET_EXP(x.h) - FP_BIAS;
    est = (e2 - 1) * 30103;
    est = est >= 0 ? est / 100000 : -((-est + 99999) / 100000);
    scale10(&x, 15 - est);                      /* now 16 or 17 digits */
    to_u64(&x, &n);
    set64(&lim, 0x002386f2u, 0x6fc10000u);      /* 10^16 */
    if (!lt64(&n, &lim)) {
        divmod10(&n);
        est++;
    }
    /* sixteen digits in n; round to fifteen */
    set64(&lim, 0, 5);
    add64(&n, &n, &lim);
    divmod10(&n);
    set64(&lim, 0x00038d7eu, 0xa4c68000u);      /* 10^15 */
    if (!lt64(&n, &lim)) {
        divmod10(&n);
        est++;
    }
    for (i = 0; i < 15; i++) dig[14 - i] = (char)('0' + divmod10(&n));
    nd = 15;
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    E = est;
    if (neg) buf[len++] = '-';
    if (E >= 0 && E < 15) {
        for (i = 0; i <= E; i++) buf[len++] = i < nd ? dig[i] : '0';
        if (nd > E + 1) {
            buf[len++] = '.';
            for (i = E + 1; i < nd; i++) buf[len++] = dig[i];
        }
    } else if (E < 0 && E >= -5) {
        buf[len++] = '.';
        for (i = -1; i > E; i--) buf[len++] = '0';
        for (i = 0; i < nd; i++) buf[len++] = dig[i];
    } else {
        buf[len++] = dig[0];
        if (nd > 1) {
            buf[len++] = '.';
            for (i = 1; i < nd; i++) buf[len++] = dig[i];
        }
        buf[len++] = 'E';
        if (E < 0) {
            buf[len++] = '-';
            E = -E;
        } else {
            buf[len++] = '+';
        }
        k = E >= 100 ? 100 : 10;
        for (; k > 0; k /= 10) buf[len++] = (char)('0' + (E / k) % 10);
    }
    buf[len] = 0;
    return len;
}

void fp_init(void)
{
    int i;

    fp_from_int(&fp_one, 1);
    fp_half = fp_one;
    fp_ldexp(&fp_half, -1);
    fp_from_int(&fp_p10[0], 10);
    for (i = 1; i < 6; i++) {
        fp_p10[i] = fp_p10[i - 1];
        fp_mul(&fp_p10[i], &fp_p10[i - 1]);
    }
    fp_parse("3.14159265358979323846", &fp_pi);
    fp_parse("0.693147180559945309417", &fp_ln2);
    fp_parse("0.707106781186547524400", &fp_rsqrt2);
    fp_half_pi = fp_pi;
    fp_ldexp(&fp_half_pi, -1);
    /* pi/2 = hi + lo with hi holding 24 bits (exactly representable)
     * and lo parsed separately, so together they carry about 80 bits */
    fp_parse("1.5707962512969970703125", &fp_hp_hi);
    fp_parse("7.549789954891882169163975E-8", &fp_hp_lo);
}
