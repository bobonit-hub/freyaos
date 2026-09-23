/*
 * Freya - single-precision floating point for cores with no FPU.
 *
 * The STM32F103 is a Cortex-M3.  It has no floating-point hardware, so a
 * program built -mfloat-abi=soft never emits a VFP instruction: every
 * float add, subtract, multiply, divide, compare and conversion to or
 * from a 32-bit integer is a call to one of the helpers below.  The names
 * and the register convention are the ARM runtime ABI.  Arguments and
 * results are the IEEE 754 bits in r0/r1, which is why the C prototypes
 * use uint32_t and the bodies never operate on a float - doing so would
 * call back into these same functions.
 *
 * Rounding is to nearest, ties to even, and subnormals are preserved.
 * There is no status register: an invalid operation returns a quiet NaN
 * and an overflow returns an infinity.  Double precision is not
 * implemented.  A program that never mentions float does not carry this
 * file; the linker's section garbage collection drops every helper it
 * does not call.
 *
 * The Black Pill has an FPU and is built -mfloat-abi=hard, so this file
 * compiles to nothing there.  GCC defines __ARM_FP only in that case.
 * The host test defines FREYA_HOST and checks the same code on a machine
 * that does have an FPU.
 */
#if !defined(__ARM_FP) || defined(FREYA_HOST)

#include <stdint.h>

#define F32_SIGN  0x80000000u
#define F32_EXP   0x7F800000u
#define F32_QUIET 0x00400000u
#define F32_INF   0x7F800000u
#define F32_NAN   0x7FC00000u
#define F32_FRAC  0x007FFFFFu
#define F32_HIDDEN 0x00800000u

/* Significands are held with the hidden bit at bit 26 and three extra
 * bits below the 24-bit field: bit 2 guard, bit 1 round, bit 0 sticky.
 * Everything that falls off the bottom is OR-ed into the sticky bit, so
 * a later round still sees that those bits were not all zero. */

static int is_nan(uint32_t x)
{
    return (x & F32_EXP) == F32_EXP && (x & F32_FRAC) != 0;
}

static int is_inf(uint32_t x)
{
    return (x & 0x7FFFFFFFu) == F32_INF;
}

static int is_zero(uint32_t x)
{
    return (x & 0x7FFFFFFFu) == 0;
}

static uint32_t quiet(uint32_t x)
{
    return x | F32_QUIET;
}

typedef struct {
    uint32_t sign;
    int exp;            /* unbiased.  subnormals are normalised, so this
                         * can be below -126; zero has sig == 0. */
    uint32_t sig;       /* 24 bits, hidden bit at bit 23, or 0 for zero */
} unpacked;

static unpacked unpack(uint32_t x)
{
    unpacked f;
    uint32_t exp = (x >> 23) & 0xFFu;
    uint32_t frac = x & F32_FRAC;

    f.sign = x >> 31;
    if (exp == 0) {
        if (frac == 0) {
            f.exp = -126;
            f.sig = 0;
            return f;
        }
        /* Shift the leading 1 up to bit 23 and charge the exponent. */
        int shift = __builtin_clz(frac) - 8;
        f.sig = frac << shift;
        f.exp = -126 - shift;
        return f;
    }
    f.exp = (int)exp - 127;
    f.sig = frac | F32_HIDDEN;
    return f;
}

/* r is in the guard/round/sticky layout described above, and either zero
 * or already normalised so bit 26 is set.  exp is the unbiased exponent
 * that layout corresponds to. */
static uint32_t round_pack(uint32_t sign, int exp, uint32_t r)
{
    uint32_t sig, lsb, grs;

    if (r == 0) return sign << 31;

    /* Too small for a normal: shift the hidden bit down into the
     * fraction, then round.  Doing it in that order rounds once. */
    if (exp <= -127) {
        int shift = -126 - exp;
        if (shift >= 32) {
            r = (r != 0);
        } else {
            uint32_t lost = r & ((1u << shift) - 1);
            r >>= shift;
            if (lost) r |= 1u;
        }
        exp = -126;
    }

    lsb = (r >> 3) & 1u;
    grs = r & 7u;
    sig = r >> 3;
    /* grs > 4 is above a tie; grs == 4 is an exact tie, kept even. */
    if (grs > 4u || (grs == 4u && lsb)) {
        sig++;
        if (sig == 0x1000000u) {
            sig >>= 1;
            exp++;
        }
    }

    if (exp >= 128) return (sign << 31) | F32_INF;
    if (sig == 0) return sign << 31;
    if ((sig & F32_HIDDEN) == 0) return (sign << 31) | sig;
    return (sign << 31) | ((uint32_t)(exp + 127) << 23) | (sig & F32_FRAC);
}

static uint32_t fadd(uint32_t a, uint32_t b)
{
    unpacked fa, fb;
    uint32_t sa, sb, r, sign;
    int diff;

    if (is_nan(a)) return quiet(a);
    if (is_nan(b)) return quiet(b);
    if (is_inf(a) && is_inf(b)) {
        if ((a ^ b) & F32_SIGN) return F32_NAN;
        return a & (F32_SIGN | F32_INF);
    }
    if (is_inf(a)) return a & (F32_SIGN | F32_INF);
    if (is_inf(b)) return b & (F32_SIGN | F32_INF);

    fa = unpack(a);
    fb = unpack(b);
    if (fa.sig == 0 && fb.sig == 0) return (fa.sign & fb.sign) << 31;
    if (fa.sig == 0) return b;
    if (fb.sig == 0) return a;

    if (fa.exp < fb.exp) {
        unpacked t = fa;
        fa = fb;
        fb = t;
    }

    sa = fa.sig << 3;
    sb = fb.sig << 3;
    diff = fa.exp - fb.exp;
    if (diff >= 31) {
        sb = (sb != 0);
    } else if (diff > 0) {
        uint32_t lost = sb & ((1u << diff) - 1);
        sb >>= diff;
        if (lost) sb |= 1u;
    }

    sign = fa.sign;
    if (fa.sign == fb.sign) {
        r = sa + sb;
        if (r & (1u << 27)) {
            uint32_t lost = r & 1u;
            r >>= 1;
            if (lost) r |= 1u;
            fa.exp++;
        }
    } else {
        if (sa < sb) {
            r = sb - sa;
            sign = fb.sign;
        } else {
            r = sa - sb;
        }
        /* Equal magnitudes cancel to +0, whichever sign they had. */
        if (r == 0) return 0;
        while ((r & (1u << 26)) == 0) {
            r <<= 1;
            fa.exp--;
        }
    }
    return round_pack(sign, fa.exp, r);
}

/* 24-bit × 24-bit → 48-bit, split so it does not call a 64-bit multiply. */
static void mul24(uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo)
{
    uint32_t a0 = a & 0xFFFFu, a1 = a >> 16;
    uint32_t b0 = b & 0xFFFFu, b1 = b >> 16;
    uint32_t p0 = a0 * b0;
    uint32_t p1 = a1 * b0;
    uint32_t p2 = a0 * b1;
    uint32_t p3 = a1 * b1;
    uint32_t mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);

    *lo = (p0 & 0xFFFFu) | (mid << 16);
    *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

static uint32_t fmul(uint32_t a, uint32_t b)
{
    unpacked fa, fb;
    uint32_t hi, lo, lost, r, sign;
    int exp;

    if (is_nan(a)) return quiet(a);
    if (is_nan(b)) return quiet(b);
    sign = (a ^ b) >> 31;
    if (is_inf(a) || is_inf(b)) {
        if (is_zero(a) || is_zero(b)) return F32_NAN;
        return (sign << 31) | F32_INF;
    }
    if (is_zero(a) || is_zero(b)) return sign << 31;

    fa = unpack(a);
    fb = unpack(b);
    exp = fa.exp + fb.exp;
    mul24(fa.sig, fb.sig, &hi, &lo);

    /* The product of two [2^23, 2^24) significands lands in [2^46, 2^48).
     * Bit 47 set means it is in [2, 4) relative to exp, so shift it down
     * and bump the exponent.  Afterwards bit 46 is the hidden bit. */
    if (hi & (1u << 15)) {
        lost = lo & 1u;
        lo = (lo >> 1) | (hi << 31);
        hi >>= 1;
        if (lost) lo |= 1u;
        exp++;
    }

    /* Move hidden bit 46 to bit 26.  Product bits [19:0] join sticky. */
    lost = lo & ((1u << 20) - 1);
    r = (hi << 12) | (lo >> 20);
    if (lost) r |= 1u;
    return round_pack(sign, exp, r);
}

static uint32_t fdiv(uint32_t a, uint32_t b)
{
    unpacked fa, fb;
    uint64_t q;
    uint32_t rem, sign;
    int i, exp;

    if (is_nan(a)) return quiet(a);
    if (is_nan(b)) return quiet(b);
    sign = (a ^ b) >> 31;
    if ((is_zero(a) && is_zero(b)) || (is_inf(a) && is_inf(b))) return F32_NAN;
    if (is_zero(a) || is_inf(b)) return sign << 31;
    if (is_zero(b) || is_inf(a)) return (sign << 31) | F32_INF;

    fa = unpack(a);
    fb = unpack(b);

    /* q = (sig_a << 27) / sig_b, built a bit at a time so the remainder
     * stays in a 32-bit word.  Both significands are in [2^23, 2^24), so
     * q lands in [2^26, 2^28) and bit 26 or 27 is the hidden bit. */
    q = 0;
    rem = 0;
    for (i = 0; i < 24 + 27; i++) {
        uint32_t bit = (i < 24) ? ((fa.sig >> (23 - i)) & 1u) : 0;
        rem = (rem << 1) | bit;
        q <<= 1;
        if (rem >= fb.sig) {
            rem -= fb.sig;
            q |= 1u;
        }
    }
    if (rem) q |= 1u;

    if (q & (1ULL << 27)) {
        uint32_t lost = (uint32_t)q & 1u;
        q >>= 1;
        if (lost) q |= 1u;
        exp = fa.exp - fb.exp;
    } else {
        exp = fa.exp - fb.exp - 1;
    }
    return round_pack(sign, exp, (uint32_t)q);
}

/* A uint32 magnitude becomes a float.  The top set bit is the hidden
 * bit and decides the exponent; bits that do not fit the 24-bit field
 * are rounded. */
static uint32_t from_u32(uint32_t sign, uint32_t v)
{
    int lz, exp, sh;
    uint32_t r, lost;

    if (v == 0) return sign << 31;
    lz = __builtin_clz(v);
    exp = 31 - lz;
    /* Put the top bit at r bit 26, which is a right shift of 5 from bit 31. */
    sh = lz - 5;
    if (sh >= 0) {
        r = v << sh;
    } else {
        lost = v & ((1u << -sh) - 1);
        r = v >> -sh;
        if (lost) r |= 1u;
    }
    return round_pack(sign, exp, r);
}

/* Truncation is toward zero.  A magnitude below 1 becomes 0.  A value
 * past the end of the range saturates, and a NaN becomes 0.  -2^31 is
 * the one negative value that fits in int32 with no positive twin. */
static int32_t to_i32(uint32_t x)
{
    unpacked f;
    int sh;
    uint32_t mag;

    if (is_nan(x)) return 0;
    if (is_inf(x)) return (x & F32_SIGN) ? (int32_t)0x80000000u : 0x7FFFFFFF;
    f = unpack(x);
    if (f.sig == 0 || f.exp < 0) return 0;
    if (f.sign && f.exp == 31 && f.sig == F32_HIDDEN) return (int32_t)0x80000000u;
    if (f.exp >= 31) return f.sign ? (int32_t)0x80000000u : 0x7FFFFFFF;

    sh = f.exp - 23;
    mag = (sh >= 0) ? (f.sig << sh) : (f.sig >> -sh);
    if (f.sign) return -(int32_t)mag;
    return (int32_t)mag;
}

static uint32_t to_u32(uint32_t x)
{
    unpacked f;
    int sh;

    if (is_nan(x) || is_zero(x)) return 0;
    if (is_inf(x)) return (x & F32_SIGN) ? 0 : 0xFFFFFFFFu;
    f = unpack(x);
    if (f.sign || f.exp < 0) return 0;
    if (f.exp >= 32) return 0xFFFFFFFFu;
    sh = f.exp - 23;
    return (sh >= 0) ? (f.sig << sh) : (f.sig >> -sh);
}

static int cmp(uint32_t a, uint32_t b)
{
    uint32_t ma, mb;
    int c;

    if (is_nan(a) || is_nan(b)) return 2;
    if (((a | b) & 0x7FFFFFFFu) == 0) return 0;
    if ((a ^ b) & F32_SIGN) return (a & F32_SIGN) ? -1 : 1;
    ma = a & 0x7FFFFFFFu;
    mb = b & 0x7FFFFFFFu;
    c = (ma > mb) - (ma < mb);
    return (a & F32_SIGN) ? -c : c;
}

uint32_t __aeabi_fadd(uint32_t a, uint32_t b) { return fadd(a, b); }
uint32_t __aeabi_fsub(uint32_t a, uint32_t b) { return fadd(a, b ^ F32_SIGN); }
uint32_t __aeabi_frsub(uint32_t a, uint32_t b) { return fadd(b, a ^ F32_SIGN); }
uint32_t __aeabi_fmul(uint32_t a, uint32_t b) { return fmul(a, b); }
uint32_t __aeabi_fdiv(uint32_t a, uint32_t b) { return fdiv(a, b); }

uint32_t __aeabi_i2f(int32_t v)
{
    if (v < 0) {
        if (v == (int32_t)0x80000000u) return 0xCF000000u;
        return from_u32(1, (uint32_t)(-v));
    }
    return from_u32(0, (uint32_t)v);
}

uint32_t __aeabi_ui2f(uint32_t v) { return from_u32(0, v); }

int32_t __aeabi_f2iz(uint32_t x) { return to_i32(x); }
uint32_t __aeabi_f2uiz(uint32_t x) { return to_u32(x); }

int32_t __aeabi_fcmpeq(uint32_t a, uint32_t b) { return cmp(a, b) == 0; }
int32_t __aeabi_fcmplt(uint32_t a, uint32_t b) { return cmp(a, b) < 0; }
int32_t __aeabi_fcmple(uint32_t a, uint32_t b) { int c = cmp(a, b); return c < 0 || c == 0; }
int32_t __aeabi_fcmpgt(uint32_t a, uint32_t b) { return cmp(a, b) == 1; }
int32_t __aeabi_fcmpge(uint32_t a, uint32_t b) { int c = cmp(a, b); return c == 1 || c == 0; }
int32_t __aeabi_fcmpun(uint32_t a, uint32_t b) { return cmp(a, b) == 2; }

/* The generic libgcc names, for a file that was not compiled as ARM EABI.
 * Same entry points, no extra code. */
uint32_t __addsf3(uint32_t a, uint32_t b) __attribute__((alias("__aeabi_fadd")));
uint32_t __subsf3(uint32_t a, uint32_t b) __attribute__((alias("__aeabi_fsub")));
uint32_t __mulsf3(uint32_t a, uint32_t b) __attribute__((alias("__aeabi_fmul")));
uint32_t __divsf3(uint32_t a, uint32_t b) __attribute__((alias("__aeabi_fdiv")));
uint32_t __floatsisf(int32_t v) __attribute__((alias("__aeabi_i2f")));
uint32_t __floatunsisf(uint32_t v) __attribute__((alias("__aeabi_ui2f")));
int32_t __fixsfsi(uint32_t x) __attribute__((alias("__aeabi_f2iz")));
uint32_t __fixunssfsi(uint32_t x) __attribute__((alias("__aeabi_f2uiz")));

#endif /* !__ARM_FP || FREYA_HOST */
