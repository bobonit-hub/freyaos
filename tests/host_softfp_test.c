/*
 * Freya - single-precision helpers against the host's IEEE float.
 *
 * src/softfp.c is what the Cortex-M3 calls instead of an FPU.  The host
 * has a real one, so each helper is checked by doing the same operation
 * in hardware and comparing the bits.  A NaN matches any NaN: payloads
 * are not required to agree.  Signed zero does not: +0 and -0 are
 * different results.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t __aeabi_fadd(uint32_t, uint32_t);
uint32_t __aeabi_fsub(uint32_t, uint32_t);
uint32_t __aeabi_fmul(uint32_t, uint32_t);
uint32_t __aeabi_fdiv(uint32_t, uint32_t);
uint32_t __aeabi_i2f(int32_t);
uint32_t __aeabi_ui2f(uint32_t);
int32_t  __aeabi_f2iz(uint32_t);
uint32_t __aeabi_f2uiz(uint32_t);
int32_t  __aeabi_fcmpeq(uint32_t, uint32_t);
int32_t  __aeabi_fcmplt(uint32_t, uint32_t);
int32_t  __aeabi_fcmple(uint32_t, uint32_t);
int32_t  __aeabi_fcmpgt(uint32_t, uint32_t);
int32_t  __aeabi_fcmpge(uint32_t, uint32_t);
int32_t  __aeabi_fcmpun(uint32_t, uint32_t);

static int checks, fails, shown;

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float ffrom(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int is_nan(uint32_t u)
{
    return ((u & 0x7F800000u) == 0x7F800000u) && (u & 0x007FFFFFu);
}

static int same(uint32_t got, uint32_t exp)
{
    if (got == exp) return 1;
    return is_nan(got) && is_nan(exp);
}

static void note(const char *op, uint32_t a, uint32_t b, uint32_t got, uint32_t exp)
{
    if (shown == 8) return;
    shown++;
    printf("  FAIL  %s %08x %08x -> %08x, host %08x\n", op, a, b, got, exp);
}

static uint32_t host_add(uint32_t a, uint32_t b)
{
    volatile float x = ffrom(a), y = ffrom(b), z = x + y;
    return fbits(z);
}

static uint32_t host_sub(uint32_t a, uint32_t b)
{
    volatile float x = ffrom(a), y = ffrom(b), z = x - y;
    return fbits(z);
}

static uint32_t host_mul(uint32_t a, uint32_t b)
{
    volatile float x = ffrom(a), y = ffrom(b), z = x * y;
    return fbits(z);
}

static uint32_t host_div(uint32_t a, uint32_t b)
{
    volatile float x = ffrom(a), y = ffrom(b), z = x / y;
    return fbits(z);
}

static void check_bits(const char *what, uint32_t got, uint32_t exp)
{
    checks++;
    if (same(got, exp)) return;
    fails++;
    if (shown < 8) {
        shown++;
        printf("  FAIL  %s -> %08x, expected %08x\n", what, got, exp);
    }
}

static void check_pair(const char *op, uint32_t a, uint32_t b,
                       uint32_t (*fn)(uint32_t, uint32_t),
                       uint32_t (*host)(uint32_t, uint32_t))
{
    uint32_t got = fn(a, b);
    uint32_t exp = host(a, b);
    checks++;
    if (same(got, exp)) return;
    fails++;
    note(op, a, b, got, exp);
}

static uint32_t rng = 0xA5A5F00Du;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* Values that sit on the boundaries: zeros, subnormals, powers of two,
 * infinities and both kinds of NaN. */
static const uint32_t edge[] = {
    0x00000000u, 0x80000000u,
    0x00000001u, 0x00000002u, 0x007FFFFFu, 0x80000001u,
    0x00800000u, 0x80800000u,
    0x3F800000u, 0xBF800000u,
    0x40000000u, 0xC0000000u,
    0x3F000000u, 0x3EAAAAABu,
    0x7F7FFFFFu, 0xFF7FFFFFu,
    0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0xFFC00000u, 0x7F800001u,
    0x4F000000u, 0xCF000000u,
    0x4F800000u, 0x1u << 23,
    0x33800000u, 0x00000080u,
};

static int finite_i32(uint32_t u)
{
    float f = ffrom(u);
    return ((u >> 23) & 0xFFu) != 0xFFu && f >= -2147483648.0f && f < 2147483648.0f;
}

static int finite_u32(uint32_t u)
{
    float f = ffrom(u);
    return ((u >> 23) & 0xFFu) != 0xFFu && f >= 0.0f && f < 4294967296.0f;
}

int main(void)
{
    unsigned i, j, n;
    int before;

    for (i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        for (j = 0; j < sizeof edge / sizeof edge[0]; j++) {
            check_pair("add", edge[i], edge[j], __aeabi_fadd, host_add);
            check_pair("sub", edge[i], edge[j], __aeabi_fsub, host_sub);
            check_pair("mul", edge[i], edge[j], __aeabi_fmul, host_mul);
            check_pair("div", edge[i], edge[j], __aeabi_fdiv, host_div);
        }
    }
    for (n = 0; n < 20000; n++) {
        uint32_t a = rnd(), b = rnd();
        check_pair("add", a, b, __aeabi_fadd, host_add);
        check_pair("sub", a, b, __aeabi_fsub, host_sub);
        check_pair("mul", a, b, __aeabi_fmul, host_mul);
        check_pair("div", a, b, __aeabi_fdiv, host_div);
    }
    if (fails == 0) printf("  ok    add, sub, mul and div match IEEE single precision\n");

    before = fails;
    for (i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        for (j = 0; j < sizeof edge / sizeof edge[0]; j++) {
            uint32_t a = edge[i], b = edge[j];
            float fa = ffrom(a), fb = ffrom(b);
            checks++;
            if (__aeabi_fcmpeq(a, b) != (fa == fb)) { fails++; note("eq", a, b, 0, 0); }
            checks++;
            if (__aeabi_fcmplt(a, b) != (fa < fb)) { fails++; note("lt", a, b, 0, 0); }
            checks++;
            if (__aeabi_fcmple(a, b) != (fa <= fb)) { fails++; note("le", a, b, 0, 0); }
            checks++;
            if (__aeabi_fcmpgt(a, b) != (fa > fb)) { fails++; note("gt", a, b, 0, 0); }
            checks++;
            if (__aeabi_fcmpge(a, b) != (fa >= fb)) { fails++; note("ge", a, b, 0, 0); }
            checks++;
            if (__aeabi_fcmpun(a, b) != (is_nan(a) || is_nan(b))) {
                fails++;
                note("un", a, b, 0, 0);
            }
        }
    }
    if (fails == before) printf("  ok    comparisons, including NaN and signed zero\n");

    before = fails;
    for (int k = -100000; k <= 100000; k++) {
        volatile float h = (float)k;
        check_bits("i2f", __aeabi_i2f(k), fbits(h));
    }
    check_bits("i2f INT_MIN", __aeabi_i2f((int32_t)0x80000000u), fbits(-2147483648.0f));
    check_bits("i2f INT_MAX", __aeabi_i2f(0x7FFFFFFF), fbits(2147483647.0f));
    for (n = 0; n < 10000; n++) {
        uint32_t u = rnd();
        volatile float h = (float)u;
        check_bits("ui2f", __aeabi_ui2f(u), fbits(h));
    }
    check_bits("ui2f UINT_MAX", __aeabi_ui2f(0xFFFFFFFFu), fbits(4294967295.0f));
    if (fails == before) printf("  ok    integer to float\n");

    before = fails;
    for (n = 0; n < 20000; n++) {
        uint32_t u = rnd();
        if (finite_i32(u)) {
            volatile float f = ffrom(u);
            check_bits("f2iz", (uint32_t)__aeabi_f2iz(u), (uint32_t)(int32_t)f);
        }
        if (finite_u32(u)) {
            volatile float f = ffrom(u);
            check_bits("f2uiz", __aeabi_f2uiz(u), (uint32_t)f);
        }
    }
    check_bits("f2iz +inf", (uint32_t)__aeabi_f2iz(0x7F800000u), 0x7FFFFFFFu);
    check_bits("f2iz -inf", (uint32_t)__aeabi_f2iz(0xFF800000u), 0x80000000u);
    check_bits("f2iz nan", (uint32_t)__aeabi_f2iz(0x7FC00000u), 0);
    check_bits("f2iz 2^31", (uint32_t)__aeabi_f2iz(0x4F000000u), 0x7FFFFFFFu);
    check_bits("f2iz -2^31", (uint32_t)__aeabi_f2iz(0xCF000000u), 0x80000000u);
    check_bits("f2uiz -1", __aeabi_f2uiz(0xBF800000u), 0);
    check_bits("f2uiz +inf", __aeabi_f2uiz(0x7F800000u), 0xFFFFFFFFu);
    check_bits("f2uiz 2^32", __aeabi_f2uiz(0x4F800000u), 0xFFFFFFFFu);
    if (fails == before) printf("  ok    float to integer\n");

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
