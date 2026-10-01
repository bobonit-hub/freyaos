/*
 * Freya - host test for the Rust bindings and the rustdemo sample.
 *
 * samples/rustdemo is built unchanged for the host as a static library,
 * against rust/freya, and linked with this service table.  Each case
 * runs app_main() in a child process, so that exit() really ends the run
 * and a panic really ends it with 101, and checks what it printed and the
 * status it ended with.
 *
 * On the board the table's layout is checked when the crate builds (see
 * rust/freya/build.rs); here both sides are the host's, so what is tested
 * is the bindings' behaviour: printing, arguments, the heap, files, the
 * directory listing, a timer handler, a thread, a table too short for a
 * call, a panic, and exit() from a handler.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/wait.h>

#include "freya_api.h"

int app_main(const freya_api_t *api, int argc, char **argv);

static int checks, fails;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

/* ------------------------------------------------- the service table */

static uint32_t s_ticks;
static int      s_delays;           /* should_stop() turns true after 3 s */

static void h_putc(char c)                { if (c != '\r') putchar(c); }
static void h_puts(const char *s)         { while (*s) h_putc(*s++); }

static int h_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n, i;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (i = 0; i < n && i < (int)sizeof buf - 1; i++)
        h_putc(buf[i]);
    return n;
}

static void h_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }

static void    *h_malloc(uint32_t n)      { return malloc(n); }
static void     h_free(void *p)           { free(p); }
static uint32_t h_ticks(void)             { return s_ticks; }
static int      h_should_stop(void)       { return s_delays >= 3; }
static void     h_yield(void)             { }
static void     h_exit(int code)          { fflush(stdout); _exit(code & 255); }
static void     h_led(int on)             { (void)on; }
static uint32_t h_cpu_hz(void)            { return 100000000u; }

static int h_last_exit(freya_exit_t *st)
{
    memset(st, 0, sizeof *st);
    st->status = 130;
    st->reason = FREYA_STOP_CTRLC;
    st->run_ms = 1234;
    strcpy(st->name, "hello");
    return 0;
}

static const char *h_reason(int r)        { return r == FREYA_STOP_CTRLC ? "stopped by Ctrl-C" : "?"; }

static int h_rtc_get(freya_rtc_t *t)
{
    t->year = 2026; t->mon = 10; t->day = 1;
    t->hour = 12; t->min = 34; t->sec = 56;
    return 0;
}

/* One timer; delay_ms() fires its handler once per 250 ms that pass. */
static freya_irq_fn s_tfn;
static void        *s_targ;
static int          s_trunning;
static uint32_t     s_tcount;

static int h_timer_open(uint32_t us, int flags, freya_irq_fn fn, void *arg)
{
    (void)flags;
    if (us != 250000) return FREYA_ERR_ARG;
    s_tfn = fn; s_targ = arg;
    return 2;
}
static int      h_timer_start(int t)      { s_trunning = (t == 2); return t == 2 ? 0 : FREYA_ERR_ARG; }
static int      h_timer_close(int t)      { s_trunning = 0; return t == 2 ? 0 : FREYA_ERR_ARG; }
static uint32_t h_timer_count(int t)      { return t == 2 ? s_tcount : 0; }

static void h_delay(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 250) {
        s_ticks += 250;
        if (s_trunning) {
            s_tcount++;
            if (s_tfn) s_tfn(2, s_targ);
        }
    }
    s_delays++;
}

/* A thread runs at once, here, until its first sleep asks it to stop. */
static int s_thread_sleeps;
static int h_thread_create(const char *name, int prio, freya_thread_fn fn, void *arg)
{
    if (strcmp(name, "heartbeat") || prio != FREYA_PRIO_NORMAL) return FREYA_ERR_ARG;
    fn(arg);
    return 3;
}
static int h_thread_sleep(uint32_t ms)    { (void)ms; return s_thread_sleeps++ < 2 ? 0 : -1; }

/* One file, kept in memory, and a fixed root directory. */
static char s_file[256];
static int  s_flen, s_fpos, s_fopen;

static int h_open(const char *path, int flags)
{
    if (strcmp(path, "/rustdemo.txt")) return -3;
    if (s_fopen) return -10;
    if (flags & FREYA_O_TRUNC) s_flen = 0;
    s_fpos = (flags & FREYA_O_APPEND) ? s_flen : 0;
    s_fopen = 1;
    return 5;
}
static int h_close(int fd)                { if (fd != 5 || !s_fopen) return -6; s_fopen = 0; return 0; }
static int h_read(int fd, void *buf, int len)
{
    int n = s_flen - s_fpos;
    if (fd != 5) return -6;
    if (n > len) n = len;
    memcpy(buf, s_file + s_fpos, (size_t)n);
    s_fpos += n;
    return n;
}
static int h_write(int fd, const void *buf, int len)
{
    if (fd != 5 || s_fpos + len > (int)sizeof s_file) return -5;
    memcpy(s_file + s_fpos, buf, (size_t)len);
    s_fpos += len;
    if (s_fpos > s_flen) s_flen = s_fpos;
    return len;
}
static int     h_seek(int fd, int32_t off, int whence)
{
    if (fd != 5 || whence != FREYA_SEEK_SET || off < 0 || off > s_flen) return -6;
    s_fpos = off;
    return 0;
}
static int32_t h_tell(int fd)             { return fd == 5 ? s_fpos : -6; }
static int32_t h_fsize(int fd)            { return fd == 5 ? s_flen : -6; }

static int s_dirpos;
static int h_opendir(const char *path)    { s_dirpos = 0; return strcmp(path, "/") ? -3 : 1; }
static int h_closedir(int dd)             { return dd == 1 ? 0 : -6; }
static int h_readdir(int dd, freya_stat_t *st)
{
    static const struct { const char *name; uint32_t size; int dir; } e[] = {
        { "hello.bin", 2048, 0 }, { "logs", 0, 1 }, { "rustdemo.txt", 0, 0 },
    };
    if (dd != 1 || s_dirpos >= 3) return -1;
    memset(st, 0, sizeof *st);
    strcpy(st->name, e[s_dirpos].name);
    st->size = e[s_dirpos].name[0] == 'r' ? (uint32_t)s_flen : e[s_dirpos].size;
    st->is_dir = (uint8_t)e[s_dirpos].dir;
    s_dirpos++;
    return 0;
}

static freya_api_t s_api = {
    .size = sizeof(freya_api_t), .version = FREYA_ABI_VERSION,
    .putc = h_putc, .puts = h_puts, .printf = h_printf,
    .malloc = h_malloc, .free = h_free,
    .ticks_ms = h_ticks, .delay_ms = h_delay,
    .should_stop = h_should_stop, .yield = h_yield, .exit = h_exit,
    .open = h_open, .close = h_close, .read = h_read, .write = h_write,
    .seek = h_seek, .tell = h_tell, .fsize = h_fsize,
    .opendir = h_opendir, .readdir = h_readdir, .closedir = h_closedir,
    .led = h_led, .cpu_hz = h_cpu_hz, .log = h_log,
    .last_exit = h_last_exit, .exit_reason_str = h_reason,
    .timer_open = h_timer_open, .timer_close = h_timer_close,
    .timer_start = h_timer_start, .timer_count = h_timer_count,
    .thread_create = h_thread_create, .thread_sleep = h_thread_sleep,
    .rtc_get = h_rtc_get,
};

/* ---------------------------------------------------------- the cases */

/* Run the program with 'arg' (or none) in a child, with the table cut
 * short at 'size' bytes, and return its output and status. */
static int run(const char *arg, uint32_t size, char *out, size_t cap)
{
    int fd[2], status;
    size_t n = 0;
    ssize_t r;
    pid_t pid;

    if (pipe(fd) != 0) return -1;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        char *argv[] = { "rustdemo", (char *)arg, NULL };

        close(fd[0]);
        dup2(fd[1], 1);
        s_api.size = size;
        h_exit(app_main(&s_api, arg ? 2 : 1, argv));
    }
    close(fd[1]);
    while (n < cap - 1 && (r = read(fd[0], out + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = 0;
    close(fd[0]);
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int has(const char *out, const char *s)
{
    if (strstr(out, s)) return 1;
    printf("        missing: \"%s\"\n", s);
    return 0;
}

int main(void)
{
    static char out[16384];
    uint32_t full = sizeof(freya_api_t);
    int st;

    printf("--- a run until Ctrl-C\n");
    st = run(NULL, full, out, sizeof out);
    check("returns status 0 once should_stop() is true", st == 0);
    check("prints the banner and the table",
          has(out, "hello from Rust on Freya") &&
          has(out, "api version 3, table ") && has(out, "cpu 100000000 Hz"));
    check("reads the clock", has(out, "the clock says 2026-10-01 12:34:56"));
    check("reports the previous run",
          has(out, "previous run: hello stopped by Ctrl-C, status 130, 1234 ms"));
    check("lists its arguments", has(out, "argv[0] = \"rustdemo\""));
    check("uses the heap through Vec and String",
          has(out, "46 primes below 200, sum 4227, largest: 199 197 193 191 181"));
    check("writes a file with write! and reads its tail back",
          has(out, "/rustdemo.txt is 22 bytes, last line: \"rustdemo ran at 0 ms\""));
    check("lists the root directory",
          has(out, "  hello.bin                2048\n") &&
          has(out, "  logs                     <dir>\n") &&
          has(out, "  rustdemo.txt             22\n"));
    check("counts timer interrupts and thread heartbeats",
          has(out, "    1 s: 4 timer interrupts, 2 heartbeats") &&
          has(out, "    3 s: 12 timer interrupts, 2 heartbeats"));
    check("finishes after the three seconds", has(out, "finished after 3000 ms"));

    printf("--- a kernel whose table stops before rtc_get\n");
    st = run(NULL, (uint32_t)offsetof(freya_api_t, rtc_get), out, sizeof out);
    check("still runs to the end", st == 0);
    check("reports the call as unsupported", has(out, "no clock: not supported on this board"));

    printf("--- exit(7)\n");
    st = run("7", full, out, sizeof out);
    check("ends with status 7", st == 7);
    check("says so first", has(out, "exiting with status 7"));
    check("does not reach the loop", !strstr(out, "blinking"));

    printf("--- a panic\n");
    st = run("panic", full, out, sizeof out);
    check("ends with status 101", st == 101);
    check("prints where and why",
          has(out, "[rust] panicked at src/lib.rs:") &&
          has(out, "index out of bounds: the len is 0 but the index is 2"));

    printf("--- exit() from the timer handler\n");
    st = run("irqexit", full, out, sizeof out);
    check("ends with status 9", st == 9);
    check("on the fourth tick", has(out, "exiting from the timer handler") &&
                                 !strstr(out, "1 s:"));

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
