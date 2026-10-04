/* fpnat.c - BASIC's arithmetic on the C float.
 *
 * The operations are the compiler's, so on a Cortex-M4F they are the
 * FPU's instructions.  What the FPU does not have is written here:
 * exp and log as short series after range reduction, sin and cos with
 * the argument reduced by pi/2 kept in four pieces so that k times
 * each piece is exact, atan by its series after two argument halvings,
 * power as 2 to the y log2 x with the integer part of that product
 * kept apart from the fraction, and integer powers done by squaring.
 *
 * The conversions between binary and decimal text are exact.  A float
 * is m 2^e and a decimal is d 10^k, so each is the other times a
 * power of five and a power of two; scale_exact() forms that product
 * in a 64-bit word, multiplying by five or dividing by it as many
 * times as the exponents say, and what falls off the bottom of the
 * word only ever decides a tie.  The digits printed are the correctly
 * rounded ones and a constant typed in is the nearest float.
 *
 * Overflow is a fault and underflow gives zero, the rules BASIC-11
 * had on the PDP-11's FP11.  A result that is infinite is a fault, so
 * an infinity is never stored in a variable and a NaN cannot arise:
 * every operation that could produce one checks its operands first.
 */
#include "fpnat.h"

fpac_t fp_pi = 0x1.921fb6p+1f;
fpac_t fp_one = 1.0f;

#define FLT_BIG   3.4028234663852886e38f      /* the largest float */
#define TWO23     8388608.0f
#define TWO31     2147483648.0f
#define INV_LN2   0x1.715476p+0f
#define LN2       0x1.62e43p-1f
#define LN2_HI    0x1.62e4p-1f                /* 15 bits: k * LN2_HI is exact */
#define LN2_LO    0x1.7f7d1cp-20f
#define TWO_OVER_PI 0x1.45f306p-1f
#define PIO2_1    0x1.92p+0f                  /* pi/2 in four pieces; the */
#define PIO2_2    0x1.fcp-12f                 /* first three have 8 bits, so */
#define PIO2_3   -0x1.58p-21f                 /* k * piece is exact below */
#define PIO2_4    0x1.10b46p-30f              /* k = 2^16 */
#define PIO2      0x1.921fb6p+0f
#define SQRT2     0x1.6a09e6p+0f

static uint32_t bits_of(float f)
{
    union { float f; uint32_t u; } u;

    u.f = f;
    return u.u;
}

static float float_of(uint32_t b)
{
    union { float f; uint32_t u; } u;

    u.u = b;
    return u.f;
}

/* An infinite result (or a NaN, which cannot happen) is an overflow. */
static float checked(float x)
{
    if (!(x <= FLT_BIG && x >= -FLT_BIG)) fp_fault(FP_ERR_OVERFLOW);
    return x;
}

static float trunc_(float x)
{
    if (x > -TWO23 && x < TWO23) return (float)(int32_t)x;
    return x;                                   /* already an integer */
}

/* The square root from the bits, correctly rounded, for a core with no
 * FPU and no libm: x = m 2^e with e even, and the root of m 2^23 taken
 * a bit at a time in a 64-bit word is the 24-bit significand.  x >= 0;
 * the infinities and NaNs never reach it. */
static float sqrt_bits(float x)
{
    uint32_t b = bits_of(x), m = b & 0x7fffffu, r = 0, bit;
    int e = (int)(b >> 23);
    uint64_t n, rem, trial;

    if (x == 0) return 0;
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
    n = (uint64_t)m << 23;                      /* [2^46, 2^48) */
    rem = 0;
    for (bit = 24; bit-- > 0; ) {               /* r < 2^24 */
        rem = (rem << 2) | (n >> 46);
        n = (n << 2) & ((1ull << 48) - 1);
        trial = ((uint64_t)r << 2) | 1u;
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

static float sqrt_(float x)
{
#if defined(__ARM_FP) && !defined(BAS_HOST)
    float r;

    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
#elif defined(BAS_HOST) && !defined(BAS_SOFT_SQRT)
    return __builtin_sqrtf(x);
#else
    return sqrt_bits(x);
#endif
}

/* x * 2^n, in steps that stay within the range of a float */
static float ldexp_(float x, int n)
{
    if (x == 0) return 0;
    while (n > 127) {
        x = checked(x * 0x1p127f);
        n -= 127;
    }
    while (n < -126) {
        x *= 0x1p-126f;
        n += 126;
        if (x == 0) return 0;
    }
    return checked(x * float_of((uint32_t)(n + 127) << 23));
}

/* ------------------------------------------------------------------ */
/* The operations a program calls                                      */

void fp_zero(fpac_t *d)
{
    *d = 0;
}

int fp_iszero(const fpac_t *a)
{
    return *a == 0;
}

int fp_isneg(const fpac_t *a)
{
    return *a < 0;
}

void fp_neg(fpac_t *a)
{
    if (*a != 0) *a = -*a;                      /* no negative zero */
}

void fp_abs(fpac_t *a)
{
    if (*a < 0) *a = -*a;
}

int fp_cmp(const fpac_t *a, const fpac_t *b)
{
    return (*a > *b) - (*a < *b);
}

void fp_add(fpac_t *a, const fpac_t *b)
{
    *a = checked(*a + *b);
}

void fp_sub(fpac_t *a, const fpac_t *b)
{
    *a = checked(*a - *b);
}

void fp_mul(fpac_t *a, const fpac_t *b)
{
    *a = checked(*a * *b);
}

void fp_div(fpac_t *a, const fpac_t *b)
{
    if (*b == 0) fp_fault(FP_ERR_DIVZERO);
    *a = checked(*a / *b);
}

void fp_from_int(fpac_t *d, int32_t v)
{
    *d = (float)v;
}

void fp_from_uint(fpac_t *d, uint32_t v)
{
    *d = (float)v;
}

/* Toward zero, a fault when the result does not fit. */
int32_t fp_to_int(const fpac_t *a)
{
    if (!(*a >= -TWO31 && *a < TWO31)) fp_fault(FP_ERR_RANGE);
    return (int32_t)*a;
}

void fp_trunc(fpac_t *a)
{
    *a = trunc_(*a);
}

void fp_floor(fpac_t *a)
{
    float t = trunc_(*a);

    if (t > *a) t -= 1.0f;
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
    if (*a < 0) fp_fault(FP_ERR_DOMAIN);
    *a = sqrt_(*a);
}

/* e^r for |r| <= ln2 / 2, by its series */
static float exp_small(float r)
{
    float p;

    p = 1.0f / 5040;
    p = p * r + 1.0f / 720;
    p = p * r + 1.0f / 120;
    p = p * r + 1.0f / 24;
    p = p * r + 1.0f / 6;
    p = p * r + 0.5f;
    p = p * r + 1.0f;
    p = p * r + 1.0f;
    return p;
}

static float exp_(float x)
{
    float r;
    int k;

    if (x > 89.0f) fp_fault(FP_ERR_OVERFLOW);
    if (x < -104.0f) return 0;                  /* below the last denormal */
    /* x = k ln2 + r, |r| <= ln2 / 2 */
    r = x * INV_LN2;
    k = (int)(r + (r < 0 ? -0.5f : 0.5f));
    r = (x - (float)k * LN2_HI) - (float)k * LN2_LO;
    return ldexp_(exp_small(r), k);
}

void fp_exp(fpac_t *a)
{
    *a = exp_(*a);
}

/* ln f, where x = f 2^e with f in [1/sqrt2, sqrt2); *pe = e. */
static float log_frac(float x, int *pe)
{
    uint32_t b;
    float f, z, z2, p;
    int e = 0;

    if (!(x > 0)) fp_fault(FP_ERR_DOMAIN);
    b = bits_of(x);
    if ((b >> 23) == 0) {                       /* denormal: normalize first */
        x *= 0x1p24f;
        b = bits_of(x);
        e = -24;
    }
    e += (int)(b >> 23) - 127;                  /* x = f 2^e, f in [1, 2) */
    f = float_of((b & 0x007fffffu) | 0x3f800000u);
    if (f > SQRT2) {                            /* bring f into [1/sqrt2, sqrt2) */
        f *= 0.5f;
        e++;
    }
    z = (f - 1.0f) / (f + 1.0f);                /* ln f = 2 atanh z, |z| < .172 */
    z2 = z * z;
    p = 2.0f / 9;
    p = p * z2 + 2.0f / 7;
    p = p * z2 + 2.0f / 5;
    p = p * z2 + 2.0f / 3;
    p = p * z2 + 2.0f;
    *pe = e;
    return p * z;
}

static float log_(float x)
{
    float p;
    int e;

    p = log_frac(x, &e);
    return (float)e * LN2_HI + (p + (float)e * LN2_LO);
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
static float pow_(float x, float y)
{
    float l, u, yh, yl, a, t, r;
    int e, k, k2;

    l = log_frac(x, &e) * INV_LN2;              /* |l| <= 1/2 */
    u = y * ((float)e + l);                     /* about log2 of the result */
    if (u > 130.0f) fp_fault(FP_ERR_OVERFLOW);
    if (u < -152.0f) return 0;
    yh = float_of(bits_of(y) & 0xfffff000u);    /* 12 bits: yh * e is exact */
    yl = y - yh;                                /* 12 bits: so is yl * e */
    a = yh * (float)e;
    k = (int)(a + (a < 0 ? -0.5f : 0.5f));
    t = ((a - (float)k) + yl * (float)e) + y * l;
    k2 = (int)(t + (t < 0 ? -0.5f : 0.5f));     /* 2^t = 2^k2 e^(r) */
    r = (t - (float)k2) * LN2;
    return ldexp_(exp_small(r), k + k2);
}

/* sin (q = 0) or cos (q = 1) of x: reduce to |r| <= pi/4 in the
 * quadrant k, then the series. */
static float sincos_(float x, int q)
{
    float fk, r, r2, p;
    int k;

    fk = x * TWO_OVER_PI;
    if (!(fk >= -TWO31 && fk < TWO31)) fp_fault(FP_ERR_RANGE);
    k = (int)(fk + (fk < 0 ? -0.5f : 0.5f));
    r = x - (float)k * PIO2_1;                  /* exact: Sterbenz */
    r = r - (float)k * PIO2_2;
    r = r - (float)k * PIO2_3;
    r = r - (float)k * PIO2_4;
    q = (q + k) & 3;
    r2 = r * r;
    if (q & 1) {
        p = -1.0f / 3628800;
        p = p * r2 + 1.0f / 40320;
        p = p * r2 - 1.0f / 720;
        p = p * r2 + 1.0f / 24;
        p = p * r2 - 0.5f;
        p = p * r2 + 1.0f;
    } else {
        p = -1.0f / 39916800;
        p = p * r2 + 1.0f / 362880;
        p = p * r2 - 1.0f / 5040;
        p = p * r2 + 1.0f / 120;
        p = p * r2 - 1.0f / 6;
        p = p * r2 + 1.0f;
        p = p * r;
    }
    return q >= 2 ? -p : p;
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
    float x = *a, x2, p;
    int neg, inv, i;

    if (x == 0) return;
    neg = x < 0;
    if (neg) x = -x;
    inv = x > 1.0f;                             /* atan x = pi/2 - atan 1/x */
    if (inv) x = 1.0f / x;
    for (i = 0; i < 2; i++)                     /* atan x = 2 atan x/(1+sqrt(1+x^2)) */
        x = x / (1.0f + sqrt_(1.0f + x * x));
    x2 = x * x;                                 /* now |x| < .199 */
    p = -1.0f / 11;
    p = p * x2 + 1.0f / 9;
    p = p * x2 - 1.0f / 7;
    p = p * x2 + 1.0f / 5;
    p = p * x2 - 1.0f / 3;
    p = p * x2 + 1.0f;
    p = p * x * 4.0f;
    if (inv) p = PIO2 - p;
    *a = neg ? -p : p;
}

void fp_pow(fpac_t *a, const fpac_t *b)
{
    float x = *a, y = *b, r, base;
    int32_t n;
    int neg;

    if (y == 0) {
        *a = 1.0f;
        return;
    }
    if (x == 0) {
        if (y < 0) fp_fault(FP_ERR_DIVZERO);
        return;
    }
    if (trunc_(y) == y && y > -65536.0f && y < 65536.0f) {
        n = (int32_t)y;                         /* an integer power: multiply */
        neg = n < 0;
        if (neg) n = -n;
        base = x;
        r = 1.0f;
        while (n) {
            if (n & 1) r = checked(r * base);
            n >>= 1;
            if (n) base = checked(base * base);
        }
        if (neg) {
            if (r == 0) fp_fault(FP_ERR_OVERFLOW);
            r = 1.0f / r;
        }
        *a = r;
        return;
    }
    if (x < 0) fp_fault(FP_ERR_DOMAIN);
    *a = pow_(x, y);
}

/* ------------------------------------------------------------------ */
/* Decimal conversion                                                  */

/* m 10^k as acc 2^e with bit 63 of acc set: *pacc, *pe.  The product
 * is formed as m 5^k 2^k.  Multiplying by five drops bits off the
 * bottom of the word only when it would otherwise overflow, and
 * dividing by five keeps a remainder; either way *sticky says that
 * something nonzero was below acc, which is all a rounding needs to
 * know.  m is not zero. */
static void scale_exact(uint64_t m, int k, uint64_t *pacc, int *pe, int *sticky)
{
    uint64_t acc = m, d, q, rem;
    int e = k, st = 0, c, cc, i;

    for (i = 0; i < k; i++) {
        while (acc >> 61) {
            st |= (int)(acc & 1);
            acc >>= 1;
            e++;
        }
        acc *= 5;
    }
    while (!(acc >> 63)) {
        acc <<= 1;
        e--;
    }
    for (c = -k; c > 0; c -= cc) {
        cc = c > 27 ? 27 : c;                   /* 5^27 is the largest that fits */
        d = 1;
        for (i = 0; i < cc; i++) d *= 5;
        q = acc / d;
        rem = acc % d;
        while (!(q >> 63)) {                    /* quotient bits until it is full */
            rem <<= 1;
            q <<= 1;
            e--;
            if (rem >= d) {
                rem -= d;
                q |= 1;
            }
        }
        st |= rem != 0;
        acc = q;
    }
    *pacc = acc;
    *pe = e;
    *sticky = st;
}

/* The float nearest to acc 2^e; ties to even.  A fault past the top
 * of the range, a denormal or zero below the bottom. */
static float pack(uint64_t acc, int e, int sticky)
{
    int top = e + 63, keep, shift;              /* value = 1.xxx 2^top */
    uint64_t s, frac, half;

    if (top > 127) fp_fault(FP_ERR_OVERFLOW);
    if (top < -150) return 0;
    keep = top >= -126 ? 24 : top + 150;        /* significand bits that fit */
    shift = 64 - keep;
    s = keep ? acc >> shift : 0;
    frac = keep ? acc & (~(uint64_t)0 >> keep) : acc;
    half = (uint64_t)1 << (shift - 1);
    if (frac > half || (frac == half && (sticky || (s & 1)))) s++;
    if (top < -126) return float_of((uint32_t)s);   /* denormal; 2^keep is the first normal */
    if (s == ((uint64_t)1 << 24)) {
        s >>= 1;
        top++;
        if (top > 127) fp_fault(FP_ERR_OVERFLOW);
    }
    return float_of(((uint32_t)(top + 127) << 23) | ((uint32_t)s & 0x007fffffu));
}

int fp_parse(const char *s, fpac_t *d)
{
    uint64_t mant = 0, acc;
    int i = 0, nsig = 0, exp10 = 0, esign = 1, e = 0, any = 0, e2, sticky;

    while (is_digit(s[i])) {
        any = 1;
        if (nsig < 18) {
            if (nsig || s[i] != '0') nsig++;
            mant = mant * 10 + (uint64_t)(s[i] - '0');
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
                mant = mant * 10 + (uint64_t)(s[i] - '0');
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
    if (mant == 0 || exp10 < -120) {            /* 18 digits times 1E-120 is below the last denormal */
        *d = 0;
        return i;
    }
    if (exp10 > 60) fp_fault(FP_ERR_OVERFLOW);
    scale_exact(mant, exp10, &acc, &e2, &sticky);
    *d = pack(acc, e2, sticky);
    return i;
}

/* round(m 2^e 10^k), half up; the caller has chosen k so that the
 * result is small. */
static uint32_t digits_of(uint32_t m, int e, int k)
{
    uint64_t acc, n;
    int e2, sticky, shift;

    scale_exact(m, k, &acc, &e2, &sticky);
    e2 += e;
    if (e2 >= 0) return 0xffffffffu;            /* acc alone is 2^63: k was too big */
    shift = -e2;
    if (shift > 63) return 0;                   /* below one: k was too small */
    n = acc >> shift;
    if (acc & ((uint64_t)1 << (shift - 1))) n++;
    return (uint32_t)n;
}

/* Up to six significant digits, the way a BASIC prints them: an
 * integer as itself, a fraction with a bare point, and E notation
 * outside 1E-5 .. 1E6.  No leading or trailing blank. */
int fp_format(const fpac_t *a, char *buf)
{
    float x = *a;
    uint32_t n, b, m;
    char dig[6];
    int e2, est, nd, i, k, E, len = 0, neg;

    if (x == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    neg = x < 0;
    if (neg) x = -x;
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
    /* the constants are literals; nothing to compute */
}
