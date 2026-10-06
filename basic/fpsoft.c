/* fpsoft.c - IEEE single precision with integers only.
 *
 * For the BASIC built with BAS_SOFTFLOAT, which is the BASIC for the
 * virtual machine: cproc has no floating point there, and no 64-bit
 * integers either.  A float is its 32 bits.  These are the operations
 * fpnat.c is written in terms of, each giving the bits a Cortex-M4F's
 * FPU gives for the same operands: add, subtract, multiply and divide
 * rounded to nearest with ties to even, subnormals kept, conversions
 * to and from a 32-bit integer and an ordered comparison.  fpnat.c
 * itself never makes a NaN or keeps an infinity, so those are kept
 * only as far as an operation that meets one gives the FPU's answer.
 *
 * The working is src/softfp.c's, the soft float of the Blue Pill:
 * significands held with the hidden bit at bit 26 and three bits
 * below the 24-bit field, guard, round and sticky, and everything
 * that falls off the bottom OR-ed into the sticky bit.  What differs
 * is that the quotient is built in a 32-bit word and the leading zero
 * count is a loop.
 *
 * Included by fpnat.c.
 */

#define F32_SIGN   0x80000000u
#define F32_EXP    0x7F800000u
#define F32_QUIET  0x00400000u
#define F32_INF    0x7F800000u
#define F32_NAN    0x7FC00000u
#define F32_FRAC   0x007FFFFFu
#define F32_HIDDEN 0x00800000u

static int sf_is_nan(uint32_t x)
{
    return (x & F32_EXP) == F32_EXP && (x & F32_FRAC) != 0;
}

static int sf_is_inf(uint32_t x)
{
    return (x & 0x7FFFFFFFu) == F32_INF;
}

static int sf_is_zero(uint32_t x)
{
    return (x & 0x7FFFFFFFu) == 0;
}

/* leading zeros of a nonzero word */
static int sf_clz(uint32_t x)
{
    int n = 0;

    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

/* x as sign, unbiased exponent and 24-bit significand with the hidden
 * bit at bit 23; a subnormal is normalised, so *exp may be below -126,
 * and zero has a significand of 0. */
static void sf_unpack(uint32_t x, uint32_t *sign, int *exp, uint32_t *sig)
{
    uint32_t e = (x >> 23) & 0xFFu, frac = x & F32_FRAC;
    int shift;

    *sign = x >> 31;
    if (e == 0) {
        if (frac == 0) {
            *exp = -126;
            *sig = 0;
            return;
        }
        shift = sf_clz(frac) - 8;
        *sig = frac << shift;
        *exp = -126 - shift;
        return;
    }
    *exp = (int)e - 127;
    *sig = frac | F32_HIDDEN;
}

/* r has its hidden bit at bit 26, or is zero; exp is its exponent. */
static uint32_t sf_round_pack(uint32_t sign, int exp, uint32_t r)
{
    uint32_t sig, lsb, grs, lost;
    int shift;

    if (r == 0) return sign << 31;
    /* too small for a normal: shift into the fraction, then round once */
    if (exp <= -127) {
        shift = -126 - exp;
        if (shift >= 32) {
            r = r != 0;
        } else {
            lost = r & ((1u << shift) - 1);
            r >>= shift;
            if (lost) r |= 1u;
        }
        exp = -126;
    }
    lsb = (r >> 3) & 1u;
    grs = r & 7u;
    sig = r >> 3;
    if (grs > 4u || (grs == 4u && lsb)) {       /* above a tie, or a tie to even */
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

static uint32_t sf_add(uint32_t a, uint32_t b)
{
    uint32_t sa, sb, r, sign, signa, signb, siga, sigb, lost, t;
    int expa, expb, diff, te;

    if (sf_is_nan(a)) return a | F32_QUIET;
    if (sf_is_nan(b)) return b | F32_QUIET;
    if (sf_is_inf(a) && sf_is_inf(b)) {
        if ((a ^ b) & F32_SIGN) return F32_NAN;
        return a & (F32_SIGN | F32_INF);
    }
    if (sf_is_inf(a)) return a & (F32_SIGN | F32_INF);
    if (sf_is_inf(b)) return b & (F32_SIGN | F32_INF);

    sf_unpack(a, &signa, &expa, &siga);
    sf_unpack(b, &signb, &expb, &sigb);
    if (siga == 0 && sigb == 0) return (signa & signb) << 31;
    if (siga == 0) return b;
    if (sigb == 0) return a;
    if (expa < expb) {
        t = signa; signa = signb; signb = t;
        t = siga; siga = sigb; sigb = t;
        te = expa; expa = expb; expb = te;
    }
    sa = siga << 3;
    sb = sigb << 3;
    diff = expa - expb;
    if (diff >= 31) {
        sb = sb != 0;
    } else if (diff > 0) {
        lost = sb & ((1u << diff) - 1);
        sb >>= diff;
        if (lost) sb |= 1u;
    }
    sign = signa;
    if (signa == signb) {
        r = sa + sb;
        if (r & (1u << 27)) {
            lost = r & 1u;
            r >>= 1;
            if (lost) r |= 1u;
            expa++;
        }
    } else {
        if (sa < sb) {
            r = sb - sa;
            sign = signb;
        } else {
            r = sa - sb;
        }
        if (r == 0) return 0;                   /* x - x is +0 */
        while ((r & (1u << 26)) == 0) {
            r <<= 1;
            expa--;
        }
    }
    return sf_round_pack(sign, expa, r);
}

static uint32_t sf_sub(uint32_t a, uint32_t b)
{
    return sf_add(a, b ^ F32_SIGN);
}

static uint32_t sf_mul(uint32_t a, uint32_t b)
{
    uint32_t signa, signb, siga, sigb, sign, a0, a1, b0, b1, p0, p1, p2, p3;
    uint32_t mid, hi, lo, lost, r;
    int expa, expb, exp;

    if (sf_is_nan(a)) return a | F32_QUIET;
    if (sf_is_nan(b)) return b | F32_QUIET;
    sign = (a ^ b) >> 31;
    if (sf_is_inf(a) || sf_is_inf(b)) {
        if (sf_is_zero(a) || sf_is_zero(b)) return F32_NAN;
        return (sign << 31) | F32_INF;
    }
    if (sf_is_zero(a) || sf_is_zero(b)) return sign << 31;

    sf_unpack(a, &signa, &expa, &siga);
    sf_unpack(b, &signb, &expb, &sigb);
    exp = expa + expb;
    /* the 48-bit product from 16-bit halves */
    a0 = siga & 0xFFFFu; a1 = siga >> 16;
    b0 = sigb & 0xFFFFu; b1 = sigb >> 16;
    p0 = a0 * b0;
    p1 = a1 * b0;
    p2 = a0 * b1;
    p3 = a1 * b1;
    mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);
    lo = (p0 & 0xFFFFu) | (mid << 16);
    hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
    /* in [2^46, 2^48): bring the hidden bit to 46 */
    if (hi & (1u << 15)) {
        lost = lo & 1u;
        lo = (lo >> 1) | (hi << 31);
        hi >>= 1;
        if (lost) lo |= 1u;
        exp++;
    }
    /* then to bit 26; product bits 19..0 are the sticky bit */
    lost = lo & ((1u << 20) - 1);
    r = (hi << 12) | (lo >> 20);
    if (lost) r |= 1u;
    return sf_round_pack(sign, exp, r);
}

static uint32_t sf_div(uint32_t a, uint32_t b)
{
    uint32_t signa, signb, siga, sigb, sign, q, rem, bit, lost;
    int expa, expb, exp, i;

    if (sf_is_nan(a)) return a | F32_QUIET;
    if (sf_is_nan(b)) return b | F32_QUIET;
    sign = (a ^ b) >> 31;
    if ((sf_is_zero(a) && sf_is_zero(b)) || (sf_is_inf(a) && sf_is_inf(b))) return F32_NAN;
    if (sf_is_zero(a) || sf_is_inf(b)) return sign << 31;
    if (sf_is_zero(b) || sf_is_inf(a)) return (sign << 31) | F32_INF;

    sf_unpack(a, &signa, &expa, &siga);
    sf_unpack(b, &signb, &expb, &sigb);
    /* q = (siga << 27) / sigb a bit at a time: both are in [2^23, 2^24),
     * so q is in [2^26, 2^28) and never needs more than 28 bits */
    q = 0;
    rem = 0;
    for (i = 0; i < 24 + 27; i++) {
        bit = i < 24 ? (siga >> (23 - i)) & 1u : 0;
        rem = (rem << 1) | bit;
        q <<= 1;
        if (rem >= sigb) {
            rem -= sigb;
            q |= 1u;
        }
    }
    if (rem) q |= 1u;
    if (q & (1u << 27)) {
        lost = q & 1u;
        q >>= 1;
        if (lost) q |= 1u;
        exp = expa - expb;
    } else {
        exp = expa - expb - 1;
    }
    return sf_round_pack(sign, exp, q);
}

/* a magnitude v with the sign given, rounded to 24 bits */
static uint32_t sf_from_mag(uint32_t sign, uint32_t v)
{
    int lz, sh;
    uint32_t r, lost;

    if (v == 0) return sign << 31;
    lz = sf_clz(v);
    sh = lz - 5;                                /* the top bit to bit 26 */
    if (sh >= 0) {
        r = v << sh;
    } else {
        lost = v & ((1u << -sh) - 1);
        r = v >> -sh;
        if (lost) r |= 1u;
    }
    return sf_round_pack(sign, 31 - lz, r);
}

static uint32_t sf_from_i32(int32_t v)
{
    if (v < 0) return sf_from_mag(1, (uint32_t)0 - (uint32_t)v);
    return sf_from_mag(0, (uint32_t)v);
}

static uint32_t sf_from_u32(uint32_t v)
{
    return sf_from_mag(0, v);
}

/* Toward zero; saturates as VCVT does, a NaN is 0. */
static int32_t sf_to_i32(uint32_t x)
{
    uint32_t sign, sig, mag;
    int exp, sh;

    if (sf_is_nan(x)) return 0;
    if (sf_is_inf(x)) return (x & F32_SIGN) ? (int32_t)0x80000000u : 0x7FFFFFFF;
    sf_unpack(x, &sign, &exp, &sig);
    if (sig == 0 || exp < 0) return 0;
    if (sign && exp == 31 && sig == F32_HIDDEN) return (int32_t)0x80000000u;
    if (exp >= 31) return sign ? (int32_t)0x80000000u : 0x7FFFFFFF;
    sh = exp - 23;
    mag = sh >= 0 ? sig << sh : sig >> -sh;
    return sign ? (int32_t)((uint32_t)0 - mag) : (int32_t)mag;
}

/* -1, 0 or 1, and 2 when either is a NaN; -0 equals +0 */
static int sf_cmp(uint32_t a, uint32_t b)
{
    uint32_t ma, mb;
    int c;

    if (sf_is_nan(a) || sf_is_nan(b)) return 2;
    if (((a | b) & 0x7FFFFFFFu) == 0) return 0;
    if ((a ^ b) & F32_SIGN) return (a & F32_SIGN) ? -1 : 1;
    ma = a & 0x7FFFFFFFu;
    mb = b & 0x7FFFFFFFu;
    c = (ma > mb) - (ma < mb);
    return (a & F32_SIGN) ? -c : c;
}
