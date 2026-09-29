/*
 * Freya - the BASIC floating point library against the C library.
 *
 * basic/fp11.c is the FP11 arithmetic of the PDP-11 in software: D format
 * numbers with a 56-bit fraction and the functions BASIC needs on top of
 * it.  This test converts doubles to that format, runs each operation on
 * a few tens of thousands of arguments and compares with libm.  A D
 * format number holds every double exactly, so the results have to agree
 * to within a few units in the last place of the double.
 *
 * Built by tests/run_tests.sh; no hardware and no VM involved.
 */
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAS_HOST 1
#include "../basic/fp11.c"

static jmp_buf jb;
static int lastfault;

void fp_fault(int code)
{
    lastfault = code;
    longjmp(jb, 1);
}

/* double -> D format; the double's 53 bits fit in the 56-bit fraction */
static void d2f(double d, fpac_t *f)
{
    int e;
    double m;
    uint64_t bits;

    if (d == 0) {
        f->h = f->l = 0;
        return;
    }
    m = frexp(fabs(d), &e);                  /* d = m * 2^e, m in [.5, 1) */
    bits = (uint64_t)ldexp(m, 56);           /* fraction with the hidden bit */
    f->h = (d < 0 ? 0x80000000u : 0)
         | ((uint32_t)(e + 128) << 23)
         | ((uint32_t)(bits >> 32) & 0x7fffffu);
    f->l = (uint32_t)bits;
}

static double f2d(const fpac_t *f)
{
    int e = (int)((f->h >> 23) & 0xff);
    uint64_t bits;
    double m;

    if (e == 0) return 0;
    bits = (((uint64_t)(f->h & 0x7fffffu) | 0x800000u) << 32) | f->l;
    m = ldexp((double)bits, -56);
    m = ldexp(m, e - 128);
    return (f->h & 0x80000000u) ? -m : m;
}

static int fails, checks;

static void chk(const char *what, double got, double want, double tol)
{
    double err, rel;

    checks++;
    err = fabs(got - want);
    rel = err / (fabs(want) > 1e-300 ? fabs(want) : 1);
    if (!(rel <= tol)) {
        if (fails < 20)
            printf("  FAIL  %s: got %.17g want %.17g rel %g\n", what, got, want, rel);
        fails++;
    }
}

static void text(const char *s, const char *want)
{
    fpac_t a;
    char buf[40];

    checks++;
    fp_parse(s, &a);
    buf[fp_format(&a, buf)] = 0;
    if (strcmp(buf, want) != 0) {
        printf("  FAIL  %s formats as \"%s\", wanted \"%s\"\n", s, buf, want);
        fails++;
    }
}

int main(void)
{
    fpac_t a, b, r;
    double x, y;
    unsigned seed = 12345;
    char buf[40], t[64];
    int i;

    if (setjmp(jb)) {
        printf("  FAIL  unexpected floating point fault %d\n", lastfault);
        return 1;
    }
    fp_init();
    chk("pi", f2d(&fp_pi), M_PI, 1e-15);
    chk("ln2", f2d(&fp_ln2), M_LN2, 1e-15);

    for (i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        x = ((int)(seed >> 8) - (1 << 23)) / 65536.0;
        seed = seed * 1103515245u + 12345u;
        y = ((int)(seed >> 8) - (1 << 23)) / 65536.0;
        if (i % 3 == 0) x = x * 1e10;
        if (i % 5 == 0) y = y / 1e7;
        if (i % 7 == 0) x = (int)x;
        d2f(x, &a);
        d2f(y, &b);

        r = a; fp_add(&r, &b); chk("add", f2d(&r), x + y, 1e-15);
        r = a; fp_sub(&r, &b); chk("sub", f2d(&r), x - y, 1e-15);
        r = a; fp_mul(&r, &b); chk("mul", f2d(&r), x * y, 1e-15);
        if (y != 0) {
            r = a; fp_div(&r, &b); chk("div", f2d(&r), x / y, 1e-15);
        }
        chk("cmp", fp_cmp(&a, &b), (x > y) - (x < y), 0);
        r = a; fp_trunc(&r); chk("trunc", f2d(&r), trunc(x), 0);
        r = a; fp_floor(&r); chk("floor", f2d(&r), floor(x), 0);
        if (fabs(x) < 2e9) chk("toint", fp_to_int(&a), (double)(int)x, 0);
        fp_from_int(&r, (int)y); chk("fromint", f2d(&r), (double)(int)y, 0);
        if (x > 0) {
            r = a; fp_sqrt(&r); chk("sqrt", f2d(&r), sqrt(x), 2e-15);
            r = a; fp_log(&r); chk("log", f2d(&r), log(x), 4e-15);
        }
        if (fabs(x) < 80 && (i % 3)) {
            r = a; fp_exp(&r); chk("exp", f2d(&r), exp(x), 4e-15);
        }
        if (fabs(x) < 1000) {
            r = a; fp_sin(&r); chk("sin", f2d(&r), sin(x), 1e-13);
            r = a; fp_cos(&r); chk("cos", f2d(&r), cos(x), 1e-13);
        }
        r = a; fp_atan(&r); chk("atan", f2d(&r), atan(x), 4e-15);
        if (x > 0 && fabs(y) < 30 && fabs(y * log10(x)) < 37) {
            r = a; fp_pow(&r, &b); chk("pow", f2d(&r), pow(x, y), 1e-13);
        }

        /* the round trip through text: 15 significant digits */
        buf[fp_format(&a, buf)] = 0;
        fp_parse(buf[0] == '-' ? buf + 1 : buf, &r);
        if (buf[0] == '-') fp_neg(&r);
        chk("format", f2d(&r), x, 5e-15);
        snprintf(t, sizeof t, "%.17g", fabs(y));
        fp_parse(t, &r);
        if (y < 0) fp_neg(&r);
        chk("parse", f2d(&r), y, 1e-15);
    }

    /* the shapes BASIC prints */
    text("0", "0");
    text("1", "1");
    text("100000", "100000");
    text("0.1", ".1");
    text("1.5", "1.5");
    text("123456789", "123456789");
    text("1e15", "1E+15");
    text("0.001", ".001");
    text("1e-5", ".00001");
    text("1e-6", "1E-06");
    text("3.14159265358979", "3.14159265358979");
    text("0.30000000000000004", ".3");
    text("1e38", "1E+38");

    d2f(0.1, &a);
    d2f(0.2, &b);
    fp_add(&a, &b);
    buf[fp_format(&a, buf)] = 0;
    checks++;
    if (strcmp(buf, ".3") != 0) {
        printf("  FAIL  .1 + .2 prints as %s\n", buf);
        fails++;
    }

    printf("  %s    fp11: %d checks, %d failures\n", fails ? "FAIL" : "ok  ", checks, fails);
    return fails != 0;
}
