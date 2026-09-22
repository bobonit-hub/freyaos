/*
 * Freya - host test for the forth sample.
 *
 * samples/forth/main.c is compiled unchanged for the host, with a service
 * table that captures what the program prints instead of sending it to a
 * USART.  Each case is a line of Forth and the exact output it must
 * produce, so the interpreter, the compiler and the machine that runs the
 * compiled code are all checked without a board.
 *
 * The program hands real addresses to Forth and takes them back through
 * a 32-bit cell, so the test is linked -no-pie to keep its static data
 * inside the low 4 GiB where that round trip is lossless.
 *
 * With -i and any further arguments the same binary runs the program's
 * own main loop on stdin and stdout instead, which is the quickest way
 * to try the sample without a board:
 *
 *     build/tests/hostforth -i samples/forth/demo.fs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* The service table the sample sees.  Defined before including it so the
 * stubs can be named in the table below. */
#include "freya_api.h"

static char s_out[16384];
static int  s_outn;
static int  s_live;             /* -i: talk to the terminal, capture nothing */

/* Carriage returns are dropped so the expected strings stay readable. */
static void h_putc(char c)
{
    if (s_live) {
        if (c != '\r')
            putchar(c);
        return;
    }
    if (c != '\r' && s_outn < (int)sizeof s_out - 1)
        s_out[s_outn++] = c;
}

static void h_puts(const char *s)
{
    while (*s)
        h_putc(*s++);
}

static int h_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n, i;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (i = 0; i < n && buf[i]; i++)
        h_putc(buf[i]);
    return n;
}

static int h_getc(void)
{
    int c;

    if (!s_live)
        return -1;
    c = getchar();
    return c == EOF ? -1 : c;
}

static int      h_getc_timeout(uint32_t ms)   { (void)ms; return -1; }
static int      h_kbhit(void)                 { return 0; }
static void    *h_malloc(uint32_t n)          { return malloc(n); }
static void     h_free(void *p)               { free(p); }
static uint32_t h_ticks(void)                 { return 0; }
static void     h_delay(uint32_t ms)          { (void)ms; }
static int      h_should_stop(void)           { return 0; }
static void     h_yield(void)                 { }
static void     h_exit(int code)              { exit(code); }
static void     h_led(int on)                 { (void)on; }
static uint32_t h_cpu_hz(void)                { return 72000000u; }

static FILE *s_files[4];

static int h_open(const char *path, int flags)
{
    int i;

    if (!(flags & FREYA_O_RDONLY))
        return -1;
    for (i = 0; i < 4; i++) {
        if (!s_files[i]) {
            s_files[i] = fopen(path, "rb");
            return s_files[i] ? i : -1;
        }
    }
    return -1;
}

static int h_read(int fd, void *buf, int len)
{
    if (fd < 0 || fd >= 4 || !s_files[fd])
        return -1;
    return (int)fread(buf, 1, (size_t)len, s_files[fd]);
}

static int h_close(int fd)
{
    if (fd < 0 || fd >= 4 || !s_files[fd])
        return -1;
    fclose(s_files[fd]);
    s_files[fd] = NULL;
    return 0;
}

static const freya_api_t k_api = {
    .size         = sizeof(freya_api_t),
    .version      = FREYA_ABI_VERSION,
    .putc         = h_putc,
    .puts         = h_puts,
    .printf       = h_printf,
    .getc         = h_getc,
    .getc_timeout = h_getc_timeout,
    .kbhit        = h_kbhit,
    .malloc       = h_malloc,
    .free         = h_free,
    .ticks_ms     = h_ticks,
    .delay_ms     = h_delay,
    .should_stop  = h_should_stop,
    .yield        = h_yield,
    .exit         = h_exit,
    .open         = h_open,
    .close        = h_close,
    .read         = h_read,
    .led          = h_led,
    .cpu_hz       = h_cpu_hz,
};

#include "../samples/forth/main.c"

static int s_checks, s_fails;

static void report(const char *what, const char *want, const char *got)
{
    s_checks++;
    if (strcmp(want, got) == 0) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected \"%s\", got \"%s\"\n", what, want, got);
        s_fails++;
    }
}

/* Runs one line the way the REPL would, and compares everything it
 * printed with what it should have printed. */
static void forth(const char *line, const char *want)
{
    s_outn = 0;
    if (!s_state)
        s_dsp = 0;
    interpret(line, (int)strlen(line));
    if (s_err)
        recover();              /* whatever recovery says is part of the output */
    s_out[s_outn] = '\0';
    report(line, want, s_out);
}

static void expect_int(const char *what, long want, long got)
{
    s_checks++;
    if (want == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %ld, got %ld\n", what, want, got);
        s_fails++;
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    char include_line[256];
    const char *src = "build/tests/demo.fs";
    uint16_t dp_before;

    if ((uintptr_t)(void *)s_dict > 0xFFFFFFFFu) {
        printf("  --    static data above 4 GiB, link with -no-pie\n");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "-i") == 0) {
        s_live = 1;
        setvbuf(stdout, NULL, _IONBF, 0);
        return app_main(&k_api, argc - 1, argv + 1);
    }

    g = &k_api;
    s_latest = NO_WORD;
    s_defining = NO_WORD;
    s_base = 10;

    printf("\n--- the interpreter ---\n");
    forth("1 2 + .",                        "3 ");
    forth("17 5 - .",                       "12 ");
    forth("6 7 * .",                        "42 ");
    forth("10 3 /mod . .",                  "3 1 ");
    forth("-8 2/ .",                        "-4 ");
    forth("1 30 lshift .",                  "1073741824 ");
    forth("-1 u.",                          "4294967295 ");
    forth("3 4 < . 4 3 < .",                "-1 0 ");
    forth("5 0= . 0 0= .",                  "0 -1 ");
    forth("1 2 3 rot . . .",                "1 3 2 ");
    forth("1 2 3 -rot . . .",               "2 1 3 ");
    forth("1 2 nip .",                      "2 ");
    forth("1 2 tuck . . .",                 "2 1 2 ");
    forth("1 2 2dup . . . .",               "2 1 2 1 ");
    forth("1 2 3 4 2swap . . . .",          "2 1 4 3 ");
    forth("0 ?dup .",                       "0 ");
    forth("5 ?dup . .",                     "5 5 ");
    forth("1 2 3 .s",                       "<3> 1 2 3 \n");
    forth("depth .",                        "0 ");
    forth("65 emit",                        "A");
    forth("3 spaces",                       "   ");
    forth("cr",                             "\n");

    printf("\n--- numbers and bases ---\n");
    forth("255 hex . decimal",              "ff ");
    forth("hex 255 decimal .",              "597 ");  /* read as hex, shown as decimal */
    forth("$ff .",                          "255 ");
    forth("%1010 .",                        "10 ");
    forth("#42 .",                          "42 ");
    forth("'A' .",                          "65 ");
    forth("char A .",                       "65 ");
    forth("base @ .",                       "10 ");
    forth("16 base ! ff . decimal",         "ff ");
    forth("0 base ! 5 . decimal",           "5 ");  /* a base of 0 would divide by zero */

    printf("\n--- the compiler ---\n");
    forth(": square dup * ;",               "");
    forth("7 square .",                     "49 ");
    forth(": sgn dup 0< if drop -1 else 0> if 1 else 0 then then ;", "");
    forth("-5 sgn . 5 sgn . 0 sgn .",       "-1 1 0 ");
    forth(": greet .\" hi there\" ; greet",  "hi there");
    forth(": shout s\" abc\" type ; shout",  "abc");
    forth("s\" hello\" type",               "hello");
    forth("s\" abc\" drop c@ .",            "97 ");
    forth(": bang [char] ! emit ; bang",    "!");
    forth(": five 5 ; five 3 max .",        "5 ");
    forth("( a comment ) 1 .",              "1 ");
    forth("2 . \\ and the rest of the line", "2 ");

    printf("\n--- control flow ---\n");
    forth(": tens 10 0 do i . loop ; tens", "0 1 2 3 4 5 6 7 8 9 ");
    forth(": evens 10 0 do i . 2 +loop ; evens", "0 2 4 6 8 ");
    forth(": lv 10 0 do i 3 = if leave then i . loop ; lv", "0 1 2 ");
    forth(": pairs 2 0 do 2 0 do j . i . loop loop ; pairs", "0 0 0 1 1 0 1 1 ");
    forth(": cd 10 begin dup 0> while dup . 1- repeat drop ; cd",
                                            "10 9 8 7 6 5 4 3 2 1 ");
    forth(": upto 0 begin 1+ dup 5 > until ; upto .", "6 ");
    forth(": fact dup 1 > if dup 1- recurse * then ; 5 fact .", "120 ");
    forth(": early 1 . exit 2 . ; early",   "1 ");
    forth(": rt 5 >r 6 r> + ; rt .",        "11 ");

    printf("\n--- the dictionary ---\n");
    forth("variable v 42 v ! v @ .",        "42 ");
    forth("3 v +! v @ .",                   "45 ");
    forth("7 constant seven seven .",       "7 ");
    forth("create buf 8 allot buf 8 65 fill buf 3 type", "AAA");
    forth("5 ' dup execute . .",            "5 5 ");
    forth("3 ' square execute .",           "9 ");
    forth(": apply ['] square execute ; 4 apply .", "16 ");
    forth("forget fact fact",               " ? fact\n");

    dp_before = s_dp;
    forth(": doomed 1 2 nosuchword ;",      " ? nosuchword\n"
                                            "   definition abandoned\n");
    expect_int("a failed definition leaves the dictionary where it was",
               dp_before, s_dp);
    forth("doomed",                         " ? doomed\n");
    expect_int("and the interpreter is not left compiling", 0, s_state);

    printf("\n--- errors are caught, not fatal ---\n");
    forth("1 0 /",                          " ? division by zero\n");
    forth("drop",                           " ? stack underflow\n");
    forth("frobnicate",                     " ? frobnicate\n");
    forth("if",                             " ? compile only\n");
    forth("60000 allot",                    " ? dictionary full\n");
    forth("1 2 + .",                        "3 ");

    printf("\n--- multi-line definitions ---\n");
    forth(": add2 2",                       "");
    expect_int("an unfinished definition stays in compile state", 1, s_state);
    forth("+ ; 5 add2 .",                   "7 ");

    printf("\n--- source files ---\n");
    write_file(src,
               "\\ a Forth source file\n"
               ": double 2 * ;\n"
               ": triple ( n -- 3n )\n"
               "    dup double + ;\n");
    snprintf(include_line, sizeof include_line, "include %s", src);
    forth(include_line,                     "");
    forth("21 double .",                    "42 ");
    forth("14 triple .",                    "42 ");
    snprintf(include_line, sizeof include_line, "include %s/nope.fs", src);
    s_outn = 0;
    interpret(include_line, (int)strlen(include_line));
    s_checks++;
    if (strstr(s_out, "cannot open")) {
        printf("  ok    a missing source file is an error, not a crash\n");
    } else {
        printf("  FAIL  a missing source file: got \"%s\"\n", s_out);
        s_fails++;
    }
    recover();

    printf("\n--- the word list ---\n");
    s_outn = 0;
    s_dsp = 0;
    interpret("words", 5);
    s_out[s_outn] = '\0';
    s_checks++;
    if (strstr(s_out, "dup ") && strstr(s_out, "double ") &&
        strstr(s_out, "+loop ") && strstr(s_out, "words")) {
        printf("  ok    words lists definitions and primitives\n");
    } else {
        printf("  FAIL  words: got \"%s\"\n", s_out);
        s_fails++;
    }

    printf("\n%d checks, %d failures\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
