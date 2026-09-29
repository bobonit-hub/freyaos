/*
 * Freya - the BASIC float arithmetic against the C library.
 *
 * basic/fpnat.c is the arithmetic of the BASIC that runs on the board
 * itself: single-precision floats, which the Cortex-M4F computes in
 * hardware, and on top of them the functions a BASIC needs, written
 * without a libm.  This test runs each of them on tens of thousands of
 * arguments and compares with libm's double results, to a few units in
 * the last place of a float; the conversions to and from decimal text
 * are checked for the shapes BASIC prints and for the round trip.
 *
 * Built by tests/run_tests.sh; no hardware involved.  The sqrt is the
 * compiler's builtin here and a VSQRT instruction on the board.
 */
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAS_HOST 1
#include "../basic/fpnat.c"

static jmp_buf jb;
static int lastfault;

void fp_fault(int code)
{
    lastfault = code;
    longjmp(jb, 1);
}

static int fails, checks;

/* Relative error against the double, in units of a float's epsilon. */
static void chk(const char *what, double got, double want, double ulps)
{
    double err, rel;

    checks++;
    err = fabs(got - want);
    rel = err / (fabs(want) > 1e-37 ? fabs(want) : 1e-37);
    if (!(rel <= ulps * 5.9604644775390625e-8)) {
        if (fails < 20)
            printf("  FAIL  %s: got %.9g want %.9g rel %g\n", what, got, want, rel);
        fails++;
    }
}

/* Absolute error, for results near zero. */
static void chka(const char *what, double got, double want, double tol)
{
    checks++;
    if (!(fabs(got - want) <= tol)) {
        if (fails < 20)
            printf("  FAIL  %s: got %.9g want %.9g\n", what, got, want);
        fails++;
    }
}

/* x to six significant digits, half up, as d.ddddde+XX: the reference
 * for fp_format().  printf rounds half to even and would disagree on
 * an exact tie such as 100120.5, so the rounding is done here on the
 * exact decimal expansion, which %.70e prints in full for a float. */
static void six_digits(float x, char *out)
{
    char s[128], dig[8];
    int e, i;

    snprintf(s, sizeof s, "%.70e", fabs(x));
    e = atoi(strchr(s, 'e') + 1);
    dig[0] = s[0];
    for (i = 1; i < 7; i++) dig[i] = s[i + 1];   /* s[1] is the point */
    if (dig[6] >= '5') {
        for (i = 5; i >= 0; i--) {
            if (dig[i] == '9') {
                dig[i] = '0';
            } else {
                dig[i]++;
                break;
            }
        }
        if (i < 0) {
            dig[0] = '1';
            e++;
        }
    }
    snprintf(out, 32, "%s%c.%.5se%+03d", x < 0 ? "-" : "", dig[0], dig + 1, e);
}

static void text(const char *s, const char *want)
{
    fpac_t a;
    char buf[40];

    checks++;
    fp_parse(s[0] == '-' ? s + 1 : s, &a);          /* the sign is BASIC's */
    if (s[0] == '-') fp_neg(&a);
    buf[fp_format(&a, buf)] = 0;
    if (strcmp(buf, want) != 0) {
        printf("  FAIL  %s formats as \"%s\", wanted \"%s\"\n", s, buf, want);
        fails++;
    }
}

/* The operations that must fault, and how. */
static void f_mul(void)    { fpac_t a = 1e30f, b = 1e10f; fp_mul(&a, &b); }
static void f_div(void)    { fpac_t a = 1, b = 0; fp_div(&a, &b); }
static void f_sqrt(void)   { fpac_t a = -1; fp_sqrt(&a); }
static void f_log(void)    { fpac_t a = 0; fp_log(&a); }
static void f_exp(void)    { fpac_t a = 100; fp_exp(&a); }
static void f_toint(void)  { fpac_t a = 3e9f; fp_to_int(&a); }
static void f_pow(void)    { fpac_t a = 10, b = 50; fp_pow(&a, &b); }
static void f_parse(void)  { fpac_t a; fp_parse("1E39", &a); }
static void f_zeropow(void){ fpac_t a = 0, b = -1; fp_pow(&a, &b); }
static void f_sin(void)    { fpac_t a = 1e30f; fp_sin(&a); }

static const struct {
    const char *what;
    void (*fn)(void);
    int code;
} faultcases[] = {
    { "overflow on multiply",       f_mul,     FP_ERR_OVERFLOW },
    { "division by zero",           f_div,     FP_ERR_DIVZERO },
    { "square root of a negative",  f_sqrt,    FP_ERR_DOMAIN },
    { "log of zero",                f_log,     FP_ERR_DOMAIN },
    { "exp overflow",               f_exp,     FP_ERR_OVERFLOW },
    { "integer out of range",       f_toint,   FP_ERR_RANGE },
    { "power overflow",             f_pow,     FP_ERR_OVERFLOW },
    { "constant too large",         f_parse,   FP_ERR_OVERFLOW },
    { "zero to a negative power",   f_zeropow, FP_ERR_DIVZERO },
    { "sin of a huge argument",     f_sin,     FP_ERR_RANGE },
};

int main(void)
{
    fpac_t a, b, r;
    float x, y;
    unsigned seed = 12345;
    char buf[40], t[64];
    int i;
    volatile int j;

    if (setjmp(jb)) {
        printf("  FAIL  unexpected floating point fault %d\n", lastfault);
        return 1;
    }
    fp_init();
    chk("pi", fp_pi, M_PI, 1);

    for (i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        x = (float)(((int)(seed >> 8) - (1 << 23)) / 65536.0);
        seed = seed * 1103515245u + 12345u;
        y = (float)(((int)(seed >> 8) - (1 << 23)) / 65536.0);
        if (i % 3 == 0) x = x * 1e10f;
        if (i % 5 == 0) y = y / 1e7f;
        if (i % 7 == 0) x = (float)(int)x;
        a = x;
        b = y;

        /* the FPU's own operations: exactly what the C compiler gives */
        r = a; fp_add(&r, &b); chk("add", r, x + y, 0);
        r = a; fp_sub(&r, &b); chk("sub", r, x - y, 0);
        r = a; fp_mul(&r, &b); chk("mul", r, x * y, 0);
        if (y != 0) {
            r = a; fp_div(&r, &b); chk("div", r, x / y, 0);
        }
        chk("cmp", fp_cmp(&a, &b), (x > y) - (x < y), 0);
        r = a; fp_trunc(&r); chk("trunc", r, truncf(x), 0);
        r = a; fp_floor(&r); chk("floor", r, floorf(x), 0);
        if (fabsf(x) < 2e9f) chk("toint", fp_to_int(&a), (double)(int)x, 0);
        fp_from_int(&r, (int)y); chk("fromint", r, (double)(int)y, 0);
        r = a; fp_ldexp(&r, -24); chk("ldexp", r, ldexp(x, -24), 0);

        /* the functions: against the double, within a few float ulps */
        if (x > 0) {
            r = a; fp_sqrt(&r); chk("sqrt", r, sqrt(x), 1);
            r = a; fp_log(&r); chka("log", r, log(x), 3e-7 * (fabs(log(x)) + 1));
        }
        if (fabsf(x) < 80 && (i % 3)) {
            r = a; fp_exp(&r); chk("exp", r, exp(x), 4);
        }
        if (fabsf(x) < 1000) {
            r = a; fp_sin(&r); chka("sin", r, sin(x), 4e-7);
            r = a; fp_cos(&r); chka("cos", r, cos(x), 4e-7);
        }
        r = a; fp_atan(&r); chk("atan", r, atan(x), 4);
        /* the error grows with y, not with the size of the result */
        if (x > 0 && fabsf(y) < 30 && fabs(y * log10(x)) < 37) {
            r = a; fp_pow(&r, &b); chk("pow", r, pow(x, y), 8 + 2 * fabs(y));
        }

        /* the text is the correctly rounded six digits, and reading
         * them back gives the float nearest to them */
        buf[fp_format(&a, buf)] = 0;
        six_digits(x, t);
        checks++;
        if (strtof(buf, NULL) != strtof(t, NULL)) {
            if (fails < 20) printf("  FAIL  %.9g formats as %s, wanted %s\n", x, buf, t);
            fails++;
        }
        fp_parse(buf[0] == '-' ? buf + 1 : buf, &r);
        if (buf[0] == '-') fp_neg(&r);
        chk("format", r, strtof(buf, NULL), 0);
        snprintf(t, sizeof t, "%.9g", fabsf(y));
        fp_parse(t, &r);
        if (y < 0) fp_neg(&r);
        chk("parse", r, y, 0);
    }

    /* the shapes BASIC prints */
    text("0", "0");
    text("1", "1");
    text("100000", "100000");
    text("999999", "999999");
    text("1000000", "1E+06");
    text("0.1", ".1");
    text("1.5", "1.5");
    text("123456", "123456");
    text("1234567", "1.23457E+06");
    text("0.001", ".001");
    text("1e-5", ".00001");
    text("1e-6", "1E-06");
    text("3.14159265358979", "3.14159");
    text("2.5e10", "2.5E+10");
    text("1e38", "1E+38");
    text("-12.5", "-12.5");
    text("0.30000001192092896", ".3");
    text("16777216", "1.67772E+07");
    text("100120.5", "100121");                 /* a tie goes up */
    text("3.4028235e38", "3.40282E+38");
    text("1e-40", "9.99995E-41");               /* denormals: fewer bits */
    text("1.4e-45", "1.4013E-45");
    text("1e-46", "0");
    text("0.000123456789", ".000123457");
    checks++;
    fp_parse("16777217", &a);                   /* a tie between two floats: to even */
    if (a != 16777216.0f) {
        printf("  FAIL  16777217 parses as %.9g\n", a);
        fails++;
    }
    checks++;
    fp_parse("16777219", &a);
    if (a != 16777220.0f) {
        printf("  FAIL  16777219 parses as %.9g\n", a);
        fails++;
    }

    a = 0.1f;
    b = 0.2f;
    fp_add(&a, &b);
    buf[fp_format(&a, buf)] = 0;
    checks++;
    if (strcmp(buf, ".3") != 0) {
        printf("  FAIL  .1 + .2 prints as %s\n", buf);
        fails++;
    }

    /* the faults */
    for (j = 0; j < (int)(sizeof faultcases / sizeof faultcases[0]); j++) {
        checks++;
        lastfault = 0;
        if (setjmp(jb) == 0) {
            faultcases[j].fn();
            printf("  FAIL  %s did not fault\n", faultcases[j].what);
            fails++;
        } else if (lastfault != faultcases[j].code) {
            printf("  FAIL  %s: fault %d, wanted %d\n", faultcases[j].what,
                   lastfault, faultcases[j].code);
            fails++;
        }
    }
    if (setjmp(jb)) {
        printf("  FAIL  unexpected floating point fault %d\n", lastfault);
        return 1;
    }

    /* underflow is zero, not a fault */
    a = 1e-30f; b = 1e-30f; fp_mul(&a, &b);
    chk("underflow", a, 0, 0);
    a = -200; fp_exp(&a);
    chk("exp underflow", a, 0, 0);

    printf("  %s    fpnat: %d checks, %d failures\n", fails ? "FAIL" : "ok  ", checks, fails);
    return fails != 0;
}
