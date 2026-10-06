/* fpnat.c - BASIC's arithmetic on the IEEE single-precision float.
 *
 * The operations on a float are the compiler's, so on a Cortex-M4F
 * they are the FPU's instructions; built with BAS_SOFTFLOAT they are
 * fpsoft.c's, on the bits, for the virtual machine, whose compiler has
 * no floating point.  Everything below the few lines that choose
 * between the two is written in terms of f_add(), f_mul() and the
 * rest, one rounding each, so both builds make the same operations in
 * the same order and get the same bits.  (A compiler that fused a
 * multiply and an add into one rounding would break that; the builds
 * say -ffp-contract=off.)
 *
 * What the FPU does not have is written here: exp and log as short
 * series after range reduction, sin and cos with the argument reduced
 * by pi/2 kept in four pieces so that k times each piece is exact,
 * atan by its series after two argument halvings, power as 2 to the
 * y log2 x with the integer part of that product kept apart from the
 * fraction, and integer powers done by squaring.
 *
 * The conversions between binary and decimal text are exact.  A float
 * is m 2^e and a decimal is d 10^k, so each is the other times a
 * power of five and a power of two; scale_exact() forms that product
 * in a 64-bit word, multiplying by five or dividing by it as many
 * times as the exponents say, and what falls off the bottom of the
 * word only ever decides a tie.  The word is two 32-bit halves, since
 * the virtual machine has no 64-bit integers, and both builds use it.
 * The digits printed are the correctly rounded ones and a constant
 * typed in is the nearest float.
 *
 * Overflow is a fault and underflow gives zero, the rules BASIC-11
 * had on the PDP-11's FP11.  A result that is infinite is a fault, so
 * an infinity is never stored in a variable and a NaN cannot arise:
 * every operation that could produce one checks its operands first.
 */
#include "fpnat.h"

/* ------------------------------------------------------------------ */
/* The operations on a float                                           */

#ifdef BAS_SOFTFLOAT
#include "fpsoft.c"

typedef uint32_t f32;

static f32 float_of(uint32_t b) { return b; }
static uint32_t bits_of(f32 f) { return f; }
static f32 f_add(f32 a, f32 b) { return sf_add(a, b); }
static f32 f_sub(f32 a, f32 b) { return sf_sub(a, b); }
static f32 f_mul(f32 a, f32 b) { return sf_mul(a, b); }
static f32 f_div(f32 a, f32 b) { return sf_div(a, b); }
static f32 f_neg(f32 a) { return a ^ 0x80000000u; }
static int f_eq(f32 a, f32 b) { return sf_cmp(a, b) == 0; }
static int f_lt(f32 a, f32 b) { return sf_cmp(a, b) == -1; }
static int f_le(f32 a, f32 b) { int c = sf_cmp(a, b); return c == -1 || c == 0; }
static f32 f_from_i(int32_t v) { return sf_from_i32(v); }
static f32 f_from_u(uint32_t v) { return sf_from_u32(v); }
static int32_t f_to_i(f32 a) { return sf_to_i32(a); }
#else
typedef float f32;

static inline f32 float_of(uint32_t b)
{
    union { float f; uint32_t u; } u;

    u.u = b;
    return u.f;
}

static inline uint32_t bits_of(f32 f)
{
    union { float f; uint32_t u; } u;

    u.f = f;
    return u.u;
}

static inline f32 f_add(f32 a, f32 b) { return a + b; }
static inline f32 f_sub(f32 a, f32 b) { return a - b; }
static inline f32 f_mul(f32 a, f32 b) { return a * b; }
static inline f32 f_div(f32 a, f32 b) { return a / b; }
static inline f32 f_neg(f32 a) { return -a; }
static inline int f_eq(f32 a, f32 b) { return a == b; }
static inline int f_lt(f32 a, f32 b) { return a < b; }
static inline int f_le(f32 a, f32 b) { return a <= b; }
static inline f32 f_from_i(int32_t v) { return (float)v; }
static inline f32 f_from_u(uint32_t v) { return (float)v; }
static inline int32_t f_to_i(f32 a) { return (int32_t)a; }
#endif

/* The constants, as their bits, so that both builds have one list. */
#define K(bits)     float_of(bits##u)
#define K_ZERO      K(0x00000000)
#define K_ONE       K(0x3f800000)
#define K_HALF      K(0x3f000000)
#define K_MHALF     K(0xbf000000)               /* -0.5 */
#define K_TWO       K(0x40000000)
#define K_FOUR      K(0x40800000)
#define K_PI        K(0x40490fdb)               /* 0x1.921fb6p+1 */
#define K_BIG       K(0x7f7fffff)               /* the largest float */
#define K_MBIG      K(0xff7fffff)
#define K_TWO23     K(0x4b000000)
#define K_MTWO23    K(0xcb000000)
#define K_TWO31     K(0x4f000000)
#define K_MTWO31    K(0xcf000000)
#define K_TWO127    K(0x7f000000)               /* 2^127 */
#define K_TWOM126   K(0x00800000)               /* 2^-126 */
#define K_TWO24     K(0x4b800000)
#define K_INV_LN2   K(0x3fb8aa3b)               /* 0x1.715476p+0 */
#define K_LN2       K(0x3f317218)               /* 0x1.62e43p-1 */
#define K_LN2_HI    K(0x3f317200)               /* 15 bits: k * LN2_HI is exact */
#define K_LN2_LO    K(0x35bfbe8e)               /* 0x1.7f7d1cp-20 */
#define K_2_OVER_PI K(0x3f22f983)               /* 0x1.45f306p-1 */
#define K_PIO2_1    K(0x3fc90000)               /* pi/2 in four pieces; the */
#define K_PIO2_2    K(0x39fe0000)               /* first three have 8 bits, so */
#define K_PIO2_3    K(0xb52c0000)               /* k * piece is exact below */
#define K_PIO2_4    K(0x30885a30)               /* k = 2^16 */
#define K_PIO2      K(0x3fc90fdb)               /* 0x1.921fb6p+0 */
#define K_SQRT2     K(0x3fb504f3)               /* 0x1.6a09e6p+0 */
#define K_EXP_HI    K(0x42b20000)               /* 89 */
#define K_EXP_LO    K(0xc2d00000)               /* -104 */
#define K_POW_HI    K(0x43020000)               /* 130 */
#define K_POW_LO    K(0xc3180000)               /* -152 */
#define K_INTPOW    K(0x47800000)               /* 65536 */
#define K_MINTPOW   K(0xc7800000)

/* The series, highest power first. */
static const uint32_t exp_coef[] = {            /* e^r */
    0x39500d01u, 0x3ab60b61u, 0x3c088889u,      /* 1/5040, 1/720, 1/120 */
    0x3d2aaaabu, 0x3e2aaaabu, 0x3f000000u,      /* 1/24, 1/6, 1/2 */
    0x3f800000u, 0x3f800000u                    /* 1, 1 */
};
static const uint32_t log_coef[] = {            /* 2 atanh z / z */
    0x3e638e39u, 0x3e924925u, 0x3ecccccdu,      /* 2/9, 2/7, 2/5 */
    0x3f2aaaabu, 0x40000000u                    /* 2/3, 2 */
};
static const uint32_t cos_coef[] = {
    0xb493f27eu, 0x37d00d01u, 0xbab60b61u,      /* -1/10!, 1/8!, -1/6! */
    0x3d2aaaabu, 0xbf000000u, 0x3f800000u       /* 1/4!, -1/2, 1 */
};
static const uint32_t sin_coef[] = {            /* sin r / r */
    0xb2d7322bu, 0x3638ef1du, 0xb9500d01u,      /* -1/11!, 1/9!, -1/7! */
    0x3c088889u, 0xbe2aaaabu, 0x3f800000u       /* 1/5!, -1/3!, 1 */
};
static const uint32_t atan_coef[] = {           /* atan x / x */
    0xbdba2e8cu, 0x3de38e39u, 0xbe124925u,      /* -1/11, 1/9, -1/7 */
    0x3e4ccccdu, 0xbeaaaaabu, 0x3f800000u       /* 1/5, -1/3, 1 */
};

fpac_t fp_pi;
fpac_t fp_one;

/* c[0] x^(n-1) + ... + c[n-1], by Horner's rule: a multiply and an
 * add, each rounded, for every coefficient after the first. */
static f32 horner(f32 x, const uint32_t *c, int n)
{
    f32 p = float_of(c[0]);
    int i;

    for (i = 1; i < n; i++) p = f_add(f_mul(p, x), float_of(c[i]));
    return p;
}

/* An infinite result (or a NaN, which cannot happen) is an overflow. */
static f32 checked(f32 x)
{
    if (!(f_le(x, K_BIG) && f_le(K_MBIG, x))) fp_fault(FP_ERR_OVERFLOW);
    return x;
}

static f32 trunc_(f32 x)
{
    if (f_lt(K_MTWO23, x) && f_lt(x, K_TWO23)) return f_from_i(f_to_i(x));
    return x;                                   /* already an integer */
}

/* The square root: VSQRT on an FPU, the C library's on the PC unless
 * BAS_SOFT_SQRT says otherwise, and sqrt_bits() everywhere else. */
#if defined(BAS_SOFTFLOAT)
#define SQRT_BITS
#elif defined(BAS_HOST)
#ifdef BAS_SOFT_SQRT
#define SQRT_BITS
#endif
#elif !defined(__ARM_FP)
#define SQRT_BITS
#endif

#ifdef SQRT_BITS
/* The square root from the bits, correctly rounded, for a core with no
 * FPU and no libm: x = m 2^e with e even, and the root of m 2^23 taken
 * two bits of m 2^23 at a time is the 24-bit significand.  x >= 0;
 * the infinities and NaNs never reach it. */
static f32 sqrt_bits(f32 x)
{
    uint32_t b = bits_of(x), m = b & 0x7fffffu, r = 0, rem = 0, trial, pair;
    int e = (int)(b >> 23), i, sh;

    if (f_eq(x, K_ZERO)) return K_ZERO;
    if (e == 0) {                               /* subnormal: normalize */
        e = 1;
        while (!(m & 0x800000u)) {
            m <<= 1;
            e--;
        }
    } else {
        m |= 0x800000u;
    }
    e -= 127;
    if (e & 1) {                                /* make the exponent even */
        m <<= 1;
        e--;
    }
    /* m 2^23 is in [2^46, 2^48); its bit pairs from the top are those
     * of m from bit 24 down, then zeros.  rem <= 2 r < 2^25. */
    for (i = 0; i < 24; i++) {
        sh = 23 - 2 * i;
        pair = sh >= 0 ? (m >> sh) & 3u : sh == -1 ? (m << 1) & 3u : 0;
        rem = (rem << 2) | pair;
        trial = (r << 2) | 1u;
        r <<= 1;
        if (rem >= trial) {
            rem -= trial;
            r |= 1u;
        }
    }
    if (rem > r) r++;                           /* nearest; a tie cannot be */
    if (r == 0x1000000u) {
        r >>= 1;
        e += 2;
    }
    return float_of((uint32_t)(e / 2 + 127) << 23 | (r & 0x7fffffu));
}

#endif

static f32 sqrt_(f32 x)
{
#if defined(SQRT_BITS)
    return sqrt_bits(x);
#elif defined(BAS_HOST)
    return __builtin_sqrtf(x);
#else
    float r;

    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
#endif
}

/* x * 2^n, in steps that stay within the range of a float */
static f32 ldexp_(f32 x, int n)
{
    if (f_eq(x, K_ZERO)) return K_ZERO;
    while (n > 127) {
        x = checked(f_mul(x, K_TWO127));
        n -= 127;
    }
    while (n < -126) {
        x = f_mul(x, K_TWOM126);
        n += 126;
        if (f_eq(x, K_ZERO)) return K_ZERO;
    }
    return checked(f_mul(x, float_of((uint32_t)(n + 127) << 23)));
}

/* x to the nearest integer, halves away from zero */
static int32_t round_(f32 x)
{
    return f_to_i(f_add(x, f_lt(x, K_ZERO) ? K_MHALF : K_HALF));
}

/* ------------------------------------------------------------------ */
/* The operations a program calls                                      */

void fp_zero(fpac_t *d)
{
    *d = K_ZERO;
}

int fp_iszero(const fpac_t *a)
{
    return f_eq(*a, K_ZERO);
}

int fp_isneg(const fpac_t *a)
{
    return f_lt(*a, K_ZERO);
}

void fp_neg(fpac_t *a)
{
    if (!f_eq(*a, K_ZERO)) *a = f_neg(*a);      /* no negative zero */
}

void fp_abs(fpac_t *a)
{
    if (f_lt(*a, K_ZERO)) *a = f_neg(*a);
}

int fp_cmp(const fpac_t *a, const fpac_t *b)
{
    return f_lt(*b, *a) - f_lt(*a, *b);
}

void fp_add(fpac_t *a, const fpac_t *b)
{
    *a = checked(f_add(*a, *b));
}

void fp_sub(fpac_t *a, const fpac_t *b)
{
    *a = checked(f_sub(*a, *b));
}

void fp_mul(fpac_t *a, const fpac_t *b)
{
    *a = checked(f_mul(*a, *b));
}

void fp_div(fpac_t *a, const fpac_t *b)
{
    if (f_eq(*b, K_ZERO)) fp_fault(FP_ERR_DIVZERO);
    *a = checked(f_div(*a, *b));
}

void fp_from_int(fpac_t *d, int32_t v)
{
    *d = f_from_i(v);
}

void fp_from_uint(fpac_t *d, uint32_t v)
{
    *d = f_from_u(v);
}

/* Toward zero, a fault when the result does not fit. */
int32_t fp_to_int(const fpac_t *a)
{
    if (!(f_le(K_MTWO31, *a) && f_lt(*a, K_TWO31))) fp_fault(FP_ERR_RANGE);
    return f_to_i(*a);
}

void fp_trunc(fpac_t *a)
{
    *a = trunc_(*a);
}

void fp_floor(fpac_t *a)
{
    f32 t = trunc_(*a);

    if (f_lt(*a, t)) t = f_sub(t, K_ONE);
    *a = t;
}

void fp_ldexp(fpac_t *a, int n)
{
    *a = ldexp_(*a, n);
}

/* ------------------------------------------------------------------ */
/* Elementary functions                                                */

void fp_sqrt(fpac_t *a)
{
    if (f_lt(*a, K_ZERO)) fp_fault(FP_ERR_DOMAIN);
    *a = sqrt_(*a);
}

/* e^r for |r| <= ln2 / 2, by its series */
static f32 exp_small(f32 r)
{
    return horner(r, exp_coef, 8);
}

static f32 exp_(f32 x)
{
    f32 r, fk;
    int k;

    if (f_lt(K_EXP_HI, x)) fp_fault(FP_ERR_OVERFLOW);
    if (f_lt(x, K_EXP_LO)) return K_ZERO;       /* below the last denormal */
    /* x = k ln2 + r, |r| <= ln2 / 2 */
    k = round_(f_mul(x, K_INV_LN2));
    fk = f_from_i(k);
    r = f_sub(f_sub(x, f_mul(fk, K_LN2_HI)), f_mul(fk, K_LN2_LO));
    return ldexp_(exp_small(r), k);
}

void fp_exp(fpac_t *a)
{
    *a = exp_(*a);
}

/* ln f, where x = f 2^e with f in [1/sqrt2, sqrt2); *pe = e. */
static f32 log_frac(f32 x, int *pe)
{
    uint32_t b;
    f32 f, z;
    int e = 0;

    if (!f_lt(K_ZERO, x)) fp_fault(FP_ERR_DOMAIN);
    b = bits_of(x);
    if ((b >> 23) == 0) {                       /* denormal: normalize first */
        x = f_mul(x, K_TWO24);
        b = bits_of(x);
        e = -24;
    }
    e += (int)(b >> 23) - 127;                  /* x = f 2^e, f in [1, 2) */
    f = float_of((b & 0x007fffffu) | 0x3f800000u);
    if (f_lt(K_SQRT2, f)) {                     /* bring f into [1/sqrt2, sqrt2) */
        f = f_mul(f, K_HALF);
        e++;
    }
    z = f_div(f_sub(f, K_ONE), f_add(f, K_ONE));    /* ln f = 2 atanh z, |z| < .172 */
    *pe = e;
    return f_mul(horner(f_mul(z, z), log_coef, 5), z);
}

static f32 log_(f32 x)
{
    f32 p, fe;
    int e;

    p = log_frac(x, &e);
    fe = f_from_i(e);
    return f_add(f_mul(fe, K_LN2_HI), f_add(p, f_mul(fe, K_LN2_LO)));
}

void fp_log(fpac_t *a)
{
    *a = log_(*a);
}

/* x^y for x > 0 and a y that is not a small integer.  With
 * log2 x = e + l, the result is 2^(y e + y l).  The whole part of
 * y e is taken out exactly - y is split so that each half times e
 * fits a float - and only the fraction goes through the series, so
 * the error does not grow with the size of the result the way it
 * does in exp(y log x). */
static f32 pow_(f32 x, f32 y)
{
    f32 l, u, yh, yl, a, t, r, fe;
    int e, k, k2;

    l = f_mul(log_frac(x, &e), K_INV_LN2);      /* |l| <= 1/2 */
    fe = f_from_i(e);
    u = f_mul(y, f_add(fe, l));                 /* about log2 of the result */
    if (f_lt(K_POW_HI, u)) fp_fault(FP_ERR_OVERFLOW);
    if (f_lt(u, K_POW_LO)) return K_ZERO;
    yh = float_of(bits_of(y) & 0xfffff000u);    /* 12 bits: yh * e is exact */
    yl = f_sub(y, yh);                          /* 12 bits: so is yl * e */
    a = f_mul(yh, fe);
    k = round_(a);
    t = f_add(f_add(f_sub(a, f_from_i(k)), f_mul(yl, fe)), f_mul(y, l));
    k2 = round_(t);                             /* 2^t = 2^k2 e^(r) */
    r = f_mul(f_sub(t, f_from_i(k2)), K_LN2);
    return ldexp_(exp_small(r), k + k2);
}

/* sin (q = 0) or cos (q = 1) of x: reduce to |r| <= pi/4 in the
 * quadrant k, then the series. */
static f32 sincos_(f32 x, int q)
{
    f32 fk, r, r2, p;
    int k;

    fk = f_mul(x, K_2_OVER_PI);
    if (!(f_le(K_MTWO31, fk) && f_lt(fk, K_TWO31))) fp_fault(FP_ERR_RANGE);
    k = round_(fk);
    fk = f_from_i(k);
    r = f_sub(x, f_mul(fk, K_PIO2_1));          /* exact: Sterbenz */
    r = f_sub(r, f_mul(fk, K_PIO2_2));
    r = f_sub(r, f_mul(fk, K_PIO2_3));
    r = f_sub(r, f_mul(fk, K_PIO2_4));
    q = (q + k) & 3;
    r2 = f_mul(r, r);
    if (q & 1)
        p = horner(r2, cos_coef, 6);
    else
        p = f_mul(horner(r2, sin_coef, 6), r);
    return q >= 2 ? f_neg(p) : p;
}

void fp_sin(fpac_t *a)
{
    *a = sincos_(*a, 0);
}

void fp_cos(fpac_t *a)
{
    *a = sincos_(*a, 1);
}

void fp_atan(fpac_t *a)
{
    f32 x = *a, p;
    int neg, inv, i;

    if (f_eq(x, K_ZERO)) return;
    neg = f_lt(x, K_ZERO);
    if (neg) x = f_neg(x);
    inv = f_lt(K_ONE, x);                       /* atan x = pi/2 - atan 1/x */
    if (inv) x = f_div(K_ONE, x);
    for (i = 0; i < 2; i++)                     /* atan x = 2 atan x/(1+sqrt(1+x^2)) */
        x = f_div(x, f_add(K_ONE, sqrt_(f_add(K_ONE, f_mul(x, x)))));
    /* now |x| < .199 */
    p = f_mul(f_mul(horner(f_mul(x, x), atan_coef, 6), x), K_FOUR);
    if (inv) p = f_sub(K_PIO2, p);
    *a = neg ? f_neg(p) : p;
}

void fp_pow(fpac_t *a, const fpac_t *b)
{
    f32 x = *a, y = *b, r, base;
    int32_t n;
    int neg;

    if (f_eq(y, K_ZERO)) {
        *a = K_ONE;
        return;
    }
    if (f_eq(x, K_ZERO)) {
        if (f_lt(y, K_ZERO)) fp_fault(FP_ERR_DIVZERO);
        return;
    }
    if (f_eq(trunc_(y), y) && f_lt(K_MINTPOW, y) && f_lt(y, K_INTPOW)) {
        n = f_to_i(y);                          /* an integer power: multiply */
        neg = n < 0;
        if (neg) n = -n;
        base = x;
        r = K_ONE;
        while (n) {
            if (n & 1) r = checked(f_mul(r, base));
            n >>= 1;
            if (n) base = checked(f_mul(base, base));
        }
        if (neg) {
            if (f_eq(r, K_ZERO)) fp_fault(FP_ERR_OVERFLOW);
            r = checked(f_div(K_ONE, r));       /* 1 / a denormal is too big */
        }
        *a = r;
        return;
    }
    if (f_lt(x, K_ZERO)) fp_fault(FP_ERR_DOMAIN);
    *a = pow_(x, y);
}

/* ------------------------------------------------------------------ */
/* A 64-bit word in two halves                                         */

typedef struct {
    uint32_t hi, lo;
} u64_t;

static void u64_set(u64_t *a, uint32_t v)
{
    a->hi = 0;
    a->lo = v;
}

static int u64_iszero(const u64_t *a)
{
    return (a->hi | a->lo) == 0;
}

static int u64_cmp(const u64_t *a, const u64_t *b)
{
    if (a->hi != b->hi) return a->hi < b->hi ? -1 : 1;
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    return 0;
}

static void u64_shl1(u64_t *a)
{
    a->hi = (a->hi << 1) | (a->lo >> 31);
    a->lo <<= 1;
}

static void u64_shr1(u64_t *a)
{
    a->lo = (a->lo >> 1) | (a->hi << 31);
    a->hi >>= 1;
}

/* a >> n for n in 0..64 */
static void u64_shr(u64_t *a, int n)
{
    if (n >= 64) {
        a->hi = a->lo = 0;
    } else if (n >= 32) {
        a->lo = a->hi >> (n - 32);
        a->hi = 0;
    } else if (n > 0) {
        a->lo = (a->lo >> n) | (a->hi << (32 - n));
        a->hi >>= n;
    }
}

/* bit n of a, n in 0..63 */
static int u64_bit(const u64_t *a, int n)
{
    return n >= 32 ? (a->hi >> (n - 32)) & 1 : (a->lo >> n) & 1;
}

/* a = a * m + c, for m and c below 2^16; what passes 2^64 is lost */
static void u64_muladd(u64_t *a, uint32_t m, uint32_t c)
{
    uint32_t p0 = (a->lo & 0xffffu) * m + c;
    uint32_t p1 = (a->lo >> 16) * m + (p0 >> 16);

    a->lo = (p0 & 0xffffu) | (p1 << 16);
    a->hi = a->hi * m + (p1 >> 16);
}

static void u64_sub(u64_t *a, const u64_t *b)
{
    uint32_t borrow = a->lo < b->lo;

    a->lo -= b->lo;
    a->hi -= b->hi + borrow;
}

/* *q = n / d and *r = n % d, a bit at a time; d < 2^63 */
static void u64_divmod(const u64_t *n, const u64_t *d, u64_t *q, u64_t *r)
{
    int i;

    u64_set(q, 0);
    u64_set(r, 0);
    for (i = 63; i >= 0; i--) {
        u64_shl1(r);
        r->lo |= (uint32_t)u64_bit(n, i);
        u64_shl1(q);
        if (u64_cmp(r, d) >= 0) {
            u64_sub(r, d);
            q->lo |= 1u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Decimal conversion                                                  */

/* m 10^k as acc 2^e with bit 63 of acc set: *pacc, *pe.  The product
 * is formed as m 5^k 2^k.  Multiplying by five drops bits off the
 * bottom of the word only when it would otherwise overflow, and
 * dividing by five keeps a remainder; either way *sticky says that
 * something nonzero was below acc, which is all a rounding needs to
 * know.  m is not zero. */
static void scale_exact(const u64_t *m, int k, u64_t *pacc, int *pe, int *sticky)
{
    u64_t acc = *m, d, q, rem;
    int e = k, st = 0, c, cc, i;

    for (i = 0; i < k; i++) {
        while (acc.hi >> 29) {
            st |= (int)(acc.lo & 1u);
            u64_shr1(&acc);
            e++;
        }
        u64_muladd(&acc, 5, 0);
    }
    while (!(acc.hi >> 31)) {
        u64_shl1(&acc);
        e--;
    }
    for (c = -k; c > 0; c -= cc) {
        cc = c > 27 ? 27 : c;                   /* 5^27 is the largest that fits */
        u64_set(&d, 1);
        for (i = 0; i < cc; i++) u64_muladd(&d, 5, 0);
        u64_divmod(&acc, &d, &q, &rem);
        while (!(q.hi >> 31)) {                 /* quotient bits until it is full */
            u64_shl1(&rem);
            u64_shl1(&q);
            e--;
            if (u64_cmp(&rem, &d) >= 0) {
                u64_sub(&rem, &d);
                q.lo |= 1u;
            }
        }
        st |= !u64_iszero(&rem);
        acc = q;
    }
    *pacc = acc;
    *pe = e;
    *sticky = st;
}

/* The float nearest to acc 2^e; ties to even.  A fault past the top
 * of the range, a denormal or zero below the bottom. */
static f32 pack(const u64_t *acc, int e, int sticky)
{
    int top = e + 63, keep, shift, c;           /* value = 1.xxx 2^top */
    u64_t t, frac, half;
    uint32_t s;

    if (top > 127) fp_fault(FP_ERR_OVERFLOW);
    if (top < -150) return K_ZERO;
    keep = top >= -126 ? 24 : top + 150;        /* significand bits that fit */
    shift = 64 - keep;
    t = *acc;
    u64_shr(&t, shift);
    s = t.lo;                                   /* the top keep bits */
    frac = *acc;                                /* and the shift bits below */
    if (shift < 32) {
        frac.hi = 0;
        frac.lo &= (1u << shift) - 1u;
    } else if (shift < 64) {
        frac.hi &= (1u << (shift - 32)) - 1u;
    }
    u64_set(&half, 0);
    if (shift - 1 >= 32) half.hi = 1u << (shift - 33);
    else half.lo = 1u << (shift - 1);
    c = u64_cmp(&frac, &half);
    if (c > 0 || (c == 0 && (sticky || (s & 1u)))) s++;
    if (top < -126) return float_of(s);         /* denormal; 2^keep is the first normal */
    if (s == 0x1000000u) {
        s >>= 1;
        top++;
        if (top > 127) fp_fault(FP_ERR_OVERFLOW);
    }
    return float_of(((uint32_t)(top + 127) << 23) | (s & 0x007fffffu));
}

int fp_parse(const char *s, fpac_t *d)
{
    u64_t mant, acc;
    int i = 0, nsig = 0, exp10 = 0, esign = 1, e = 0, any = 0, e2, sticky;

    u64_set(&mant, 0);
    while (is_digit(s[i])) {
        any = 1;
        if (nsig < 18) {
            if (nsig || s[i] != '0') nsig++;
            u64_muladd(&mant, 10, (uint32_t)(s[i] - '0'));
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
                u64_muladd(&mant, 10, (uint32_t)(s[i] - '0'));
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
    if (u64_iszero(&mant) || exp10 < -120) {    /* 18 digits times 1E-120 is below the last denormal */
        *d = K_ZERO;
        return i;
    }
    if (exp10 > 60) fp_fault(FP_ERR_OVERFLOW);
    scale_exact(&mant, exp10, &acc, &e2, &sticky);
    *d = pack(&acc, e2, sticky);
    return i;
}

/* round(m 2^e 10^k), half up; the caller has chosen k so that the
 * result is small. */
static uint32_t digits_of(uint32_t m, int e, int k)
{
    u64_t mm, acc;
    int e2, sticky, shift, up;

    u64_set(&mm, m);
    scale_exact(&mm, k, &acc, &e2, &sticky);
    e2 += e;
    if (e2 >= 0) return 0xffffffffu;            /* acc alone is 2^63: k was too big */
    shift = -e2;
    if (shift > 63) return 0;                   /* below one: k was too small */
    up = u64_bit(&acc, shift - 1);
    u64_shr(&acc, shift);
    return acc.lo + (uint32_t)up;
}

/* Up to six significant digits, the way a BASIC prints them: an
 * integer as itself, a fraction with a bare point, and E notation
 * outside 1E-5 .. 1E6.  No leading or trailing blank. */
int fp_format(const fpac_t *a, char *buf)
{
    f32 x = *a;
    uint32_t n, b, m;
    char dig[6];
    int e2, est, nd, i, k, E, len = 0, neg;

    if (f_eq(x, K_ZERO)) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    neg = f_lt(x, K_ZERO);
    if (neg) x = f_neg(x);
    b = bits_of(x);
    if (b >> 23) {                              /* x = m 2^e2, m an integer */
        m = (b & 0x007fffffu) | 0x00800000u;
        e2 = (int)(b >> 23) - 150;
    } else {
        m = b;
        e2 = -149;
    }
    /* x is in [2^(e2+23), 2^(e2+24)) for a normal; its decimal
     * exponent is est or est + 1, and a denormal's is lower still */
    est = (e2 + 23) * 30103;
    est = est >= 0 ? est / 100000 : -((-est + 99999) / 100000);
    n = digits_of(m, e2, 5 - est);              /* six digits before the point */
    while (n < 100000u) {
        est--;
        n = digits_of(m, e2, 5 - est);
    }
    if (n >= 1000000u) {                        /* seven: est was one low, or 999999.5 rounded up */
        est++;
        n = digits_of(m, e2, 5 - est);
    }
    for (i = 5; i >= 0; i--) {
        dig[i] = (char)('0' + n % 10);
        n /= 10;
    }
    nd = 6;
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    E = est;
    if (neg) buf[len++] = '-';
    if (E >= 0 && E < 6) {
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
        for (k = 10; k > 0; k /= 10) buf[len++] = (char)('0' + (E / k) % 10);
    }
    buf[len] = 0;
    return len;
}

void fp_init(void)
{
    fp_pi = K_PI;
    fp_one = K_ONE;
}
