/* hostrt.c - run the interpreter natively, for testing.
 *
 *   cc -DBAS_HOST basic/basic.c basic/hostrt.c -lm -o basic-host
 *   basic-host [-m kbytes] [-r program.bas]
 *
 * Supplies the system calls of bas.h with stdio, so the same basic.c
 * that goes into the program for the board runs on the PC, where a
 * debugger and the sanitizers can reach it.  runbasic.c, which runs
 * the image for the virtual machine, serves that image's system calls
 * with these same functions; built with BAS_VMHOST this file leaves
 * main() to it.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bas.h"

int bas_main(uint8_t *heap, uint32_t heap_size, uint32_t flags);

static volatile sig_atomic_t got_int;
static const char *queued[4];
static int nqueued;

static void on_int(int sig)
{
    (void)sig;
    got_int = 1;
}

void sys_exit(int code)
{
    fflush(stdout);
    exit(code);
}

void sys_putc(int c)
{
    putchar(c & 0xff);
    if ((c & 0xff) == '\n') fflush(stdout);
}

int sys_readline(char *buf, int max, int running)
{
    int n = 0, c;

    if (nqueued) {
        const char *s = queued[0];
        memmove(queued, queued + 1, (size_t)(--nqueued) * sizeof queued[0]);
        while (*s && n < max - 1) buf[n++] = *s++;
        buf[n] = 0;
        return n;
    }
    fflush(stdout);
    for (;;) {
        c = getchar();
        if (c == EOF) {
            if (errno == EINTR) {
                /* ^C while typing: an empty line, and the break is seen */
                clearerr(stdin);
                buf[0] = 0;
                return 0;
            }
            if (n == 0) return -1;
            break;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < max - 1) buf[n++] = (char)c;
    }
    buf[n] = 0;
    return n;
}

int sys_break(void)
{
    int r = got_int;

    got_int = 0;
    return r;
}

int sys_open(const char *path, int mode)
{
    return open(path, mode ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0666);
}

int sys_close(int fd)
{
    return close(fd);
}

int sys_read(int fd, void *buf, int len)
{
    return (int)read(fd, buf, (size_t)len);
}

int sys_write(int fd, const void *buf, int len)
{
    return (int)write(fd, buf, (size_t)len);
}

/* Milliseconds since the first call, as the board counts from boot.
 * The wall clock itself is far too large for a BASIC number to hold to
 * the millisecond. */
uint32_t sys_ticks(void)
{
    static uint32_t base;
    struct timeval tv;
    uint32_t now;

    gettimeofday(&tv, NULL);
    now = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    if (!base) base = now;
    return now - base;
}

int sys_clock(int *f)
{
    time_t now = time(NULL);
    struct tm tm;

    if (now == (time_t)-1 || !localtime_r(&now, &tm)) return -1;
    f[0] = tm.tm_year + 1900;
    f[1] = tm.tm_mon + 1;
    f[2] = tm.tm_mday;
    f[3] = tm.tm_hour;
    f[4] = tm.tm_min;
    f[5] = tm.tm_sec;
    return 0;
}

void sys_sleep(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

int sys_unlink(const char *path)
{
    return unlink(path);
}

/* A key when one can be read without waiting.  stdin is unbuffered,
 * so what poll() sees is what getchar() gets; at a terminal the keys
 * arrive when the line is entered, as the terminal is line-buffered. */
int sys_inkey(void)
{
    struct pollfd pfd;
    int c;

    if (got_int) return -1;
    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0) return -1;
    c = getchar();
    if (c == EOF) {
        clearerr(stdin);
        return -1;
    }
    return c;
}

/* The PC has no program flash. */
int sys_flash_save(const char *text, int len)
{
    (void)text;
    (void)len;
    return SYS_EIO;
}

int sys_autostart(int on)
{
    (void)on;
    return SYS_EIO;
}

#include "pretend.c"

/* Ready the console and, with a program, type OLD "program" and RUN
 * for the user; the flags for bas_main(), batch mode with a program. */
uint32_t host_start(const char *program)
{
    static char oldcmd[600];

    if (program) {
        snprintf(oldcmd, sizeof oldcmd, "OLD \"%s\"", program);
        queued[nqueued++] = oldcmd;
        queued[nqueued++] = "RUN";
    }
    setvbuf(stdin, NULL, _IONBF, 0);    /* so INKEY$ can poll for a key */
    {
        /* sigaction: signal() may reset to the default after one ^C */
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_int;
        sigaction(SIGINT, &sa, NULL);
    }
    return program ? 1 : 0;
}

#ifndef BAS_VMHOST
int main(int argc, char **argv)
{
    uint32_t memsz = 256 * 1024;
    const char *program = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc)
            memsz = (uint32_t)strtoul(argv[++i], 0, 0) * 1024;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            program = argv[++i];
        else {
            fprintf(stderr, "usage: basic-host [-m kbytes] [-r program.bas]\n");
            return 1;
        }
    }
    return bas_main(calloc(1, memsz), memsz, host_start(program));
}
#endif
