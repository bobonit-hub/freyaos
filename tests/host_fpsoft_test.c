/*
 * Freya - BASIC's arithmetic on integers against the same on the FPU.
 *
 * basic/fpnat.c is built two ways: on the C float, which is the FPU on
 * the board, and with BAS_SOFTFLOAT on the integers of basic/fpsoft.c,
 * which is how the image for the virtual machine has it.  Both have to
 * give the same bits for everything, or a program prints differently
 * on the two.  tests/run_tests.sh builds this file both ways and runs
 * each on the same two million operations - arithmetic, the elementary
 * functions, conversions both ways between numbers and text, faults -
 * on arguments drawn from the same sequence; each prints one hash of
 * every result, and the two hashes have to be equal.
 *
 * The soft build checks its primitives against the PC's FPU as well:
 * add, subtract, multiply, divide, the conversions and the comparison
 * on special and random operands, and the square root, which the
 * FPU build takes from the C library.
 *
 * No hardware involved.
 */
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAS_HOST 1
#include "../basic/fpnat.c"

#define OPS 2000000L

static jmp_buf jb;

void fp_fault(int code)
{
    longjmp(jb, code);
}

static uint64_t rs = 88172645463325252ull;

static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)rs;
}

static uint32_t bits(fpac_t a)
{
    uint32_t u;

    memcpy(&u, &a, 4);
    return u;
}

static fpac_t num(uint32_t u)
{
    fpac_t a;

    memcpy(&a, &u, 4);
    return a;
}

/* An argument: finite, often small, often an integer or a half. */
static uint32_t pick(void)
{
    uint32_t r = rnd();
    fpac_t f, h;

    switch (r & 3) {
    case 0:  return (r & 0x807fffffu) | ((96u + rnd() % 64u) << 23);
    case 1:  return (r & 0x807fffffu) | ((rnd() % 254u) << 23);
    case 2:  return (r & 0x807fffffu) | ((120u + rnd() % 14u) << 23);
    default:
        fp_from_int(&f, (int32_t)(rnd() % 2001u) - 1000);
        if (r & 4) {
            fp_from_int(&h, 2);
            fp_div(&f, &h);
        }
        return bits(f);
    }
}

static unsigned long hash = 1469598103934665603ul;

static void mix(uint32_t v)
{
    hash = (hash ^ v) * 1099511628211ul;
}

static void run_ops(void)
{
    volatile long i;                    /* kept across a longjmp() */
    int j, n;

    rs = 88172645463325252ull;          /* the same arguments both ways */
    for (i = 0; i < OPS; i++) {
        uint32_t xa = pick(), xb = pick();
        volatile uint32_t r2 = 0;       /* so is this */
        fpac_t a = num(xa), b = num(xb), p;
        char buf[40], s[40];
        volatile int code;

        if (!(code = setjmp(jb))) {
            switch (i % 22) {
            case 0:  fp_add(&a, &b); break;
            case 1:  fp_sub(&a, &b); break;
            case 2:  fp_mul(&a, &b); break;
            case 3:  fp_div(&a, &b); break;
            case 4:  fp_exp(&a); break;
            case 5:  fp_log(&a); break;
            case 6:  fp_sin(&a); break;
            case 7:  fp_cos(&a); break;
            case 8:  fp_atan(&a); break;
            case 9:  fp_pow(&a, &b); break;
            case 10: r2 = (uint32_t)fp_to_int(&a); break;
            case 11: fp_floor(&a); break;
            case 12: fp_trunc(&a); break;
            case 13: r2 = (uint32_t)fp_cmp(&a, &b); break;
            case 14: fp_sqrt(&a); break;
            case 15:                    /* to text and back */
                n = fp_format(&a, buf);
                for (j = 0; j < n; j++) mix((unsigned char)buf[j]);
                fp_parse(buf[0] == '-' ? buf + 1 : buf, &p);
                r2 = bits(p);
                break;
            case 16:
                snprintf(s, sizeof s, "%u.%uE%d", rnd() % 100000u, rnd() % 1000000u,
                         (int)(rnd() % 90u) - 45);
                fp_parse(s, &a);
                break;
            case 17:
                snprintf(s, sizeof s, "%u%u", rnd(), rnd());
                fp_parse(s, &a);
                break;
            case 18: a = num(xa & 0x7fffffffu); fp_sqrt(&a); break;
            case 19: fp_from_int(&p, (int32_t)(rnd() % 41u) - 20); fp_pow(&a, &p); break;
            case 20: fp_ldexp(&a, (int)(rnd() % 400u) - 200); break;
            default:
                fp_from_int(&a, (int32_t)rnd() >> (rnd() % 32u));
                fp_from_uint(&b, rnd() >> (rnd() % 32u));
                fp_add(&a, &b);
                break;
            }
            mix(bits(a));
        } else {
            mix(0xdead0000u | (uint32_t)code);
        }
        mix(r2);
    }
}

#ifdef BAS_SOFTFLOAT
static float fl(uint32_t u)
{
    float f;

    memcpy(&f, &u, 4);
    return f;
}

static uint32_t ul(float f)
{
    uint32_t u;

    memcpy(&u, &f, 4);
    return u;
}

static int is_nan(uint32_t x)
{
    return (x & 0x7f800000u) == 0x7f800000u && (x & 0x7fffffu);
}

/* special values, then random ones over the whole range */
static uint32_t operand(void)
{
    static const uint32_t edge[] = {
        0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x00800000u, 0x7f7fffffu,
        0xff7fffffu, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u,
        0x4b000000u, 0x4f000000u, 0x34000000u
    };
    uint32_t r = rnd();

    switch (r % 4u) {
    case 0:  return edge[rnd() % (sizeof edge / sizeof edge[0])];
    case 1:  return (r & 0x807fffffu) | ((rnd() % 30u) << 23);        /* subnormal and tiny */
    case 2:  return (r & 0x807fffffu) | ((110u + rnd() % 40u) << 23);
    default: return (r & 0x7f800000u) == 0x7f800000u ? r & 0xbfffffffu : r;
    }
}

static int check_primitives(void)
{
    long i, bad = 0;
    uint32_t a, b, hw = 0, sw = 0, u;
    int op;

    for (i = 0; i < 4000000L; i++) {
        a = operand();
        b = operand();
        op = (int)(i % 7);
        switch (op) {
        case 0: hw = ul(fl(a) + fl(b)); sw = sf_add(a, b); break;
        case 1: hw = ul(fl(a) - fl(b)); sw = sf_sub(a, b); break;
        case 2: hw = ul(fl(a) * fl(b)); sw = sf_mul(a, b); break;
        case 3: hw = ul(fl(a) / fl(b)); sw = sf_div(a, b); break;
        case 4: {
            int32_t v = (int32_t)rnd() >> (rnd() % 32u);

            hw = ul((float)v);
            sw = sf_from_i32(v);
            break;
        }
        case 5:
            if (is_nan(a) || !(fl(a) > -2147483648.0f && fl(a) < 2147483648.0f)) continue;
            hw = (uint32_t)(int32_t)fl(a);
            sw = (uint32_t)sf_to_i32(a);
            break;
        default:
            hw = is_nan(a) || is_nan(b) ? 2u : (uint32_t)((fl(a) > fl(b)) - (fl(a) < fl(b)));
            sw = (uint32_t)sf_cmp(a, b);
            break;
        }
        if (hw != sw && !(is_nan(hw) && is_nan(sw))) {
            if (bad++ < 10)
                printf("  FAIL  op %d of %08x, %08x: FPU %08x, integers %08x\n", op, a, b, hw, sw);
        }
    }
    /* the square root: every 61st float, which reaches every exponent */
    for (u = 0; u < 0x7f800000u; u += 61u) {
        hw = ul(sqrtf(fl(u)));
        sw = sqrt_bits(u);
        if (hw != sw && bad++ < 10)
            printf("  FAIL  sqrt of %08x: FPU %08x, integers %08x\n", u, hw, sw);
    }
    if (bad) printf("  FAIL  fpsoft: %ld primitives differ from the FPU\n", bad);
    else printf("  ok      fpsoft: the primitives agree with the FPU\n");
    return bad != 0;
}
#endif

int main(void)
{
    int fail = 0;

    fp_init();
#ifdef BAS_SOFTFLOAT
    fail = check_primitives();
#endif
    run_ops();
    printf("%016lx\n", hash);
    return fail;
}
