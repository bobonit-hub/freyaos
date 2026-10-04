/* hostrt.c - run the interpreter natively, for testing.
 *
 *   cc -DBAS_HOST basic/basic.c basic/hostrt.c -lm -o basic-host
 *   basic-host [-m kbytes] [-r program.bas]
 *
 * Supplies the system calls of bas.h with stdio, so the same basic.c
 * that goes into the program for the board runs on the PC, where a
 * debugger and the sanitizers can reach it.
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

int sys_readline(char *buf, int max)
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

/* The pins of a pretend board, so that PIN, PWM and ADC can be tested
 * where there is no hardware: ports A, B and C of 16 pins, every one
 * an input reading 0 until it is driven, PA2 and PA3 kept back as the
 * console's are on the boards, and an input with a pull-up or a
 * pull-down reads what the pull makes it until it is driven.  A PWM
 * channel is any pin of ports A
 * and B.  The ADC reads PA0-PA7, PB0, PB1 and PC0-PC5, and answers
 * 2048 from a pin, 1000 from TEMP and 1500 from VREF. */
#define HOST_PORTS 3

static struct {
    uint8_t mode, level, pwm;
} pins[HOST_PORTS * 16];

static int pin_ok(int pin)
{
    if (pin < 0 || pin >= HOST_PORTS * 16) return 0;
    return pin != 2 && pin != 3;
}

int sys_pin_mode(int pin, int mode)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    if (mode < SYS_PIN_IN || mode > SYS_PIN_ANALOG) return SYS_EARG;
    pins[pin].mode = (uint8_t)mode;
    /* an input nothing drives follows its pull */
    if (mode == SYS_PIN_IN_PULLUP) pins[pin].level = 1;
    if (mode == SYS_PIN_IN_PULLDOWN) pins[pin].level = 0;
    return 0;
}

int sys_pin_read(int pin)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    return pins[pin].level;
}

int sys_pin_write(int pin, int level)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    pins[pin].level = level ? 1 : 0;
    return 0;
}

int sys_pin_toggle(int pin)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    pins[pin].level ^= 1;
    return 0;
}

int sys_pwm(int pin, uint32_t hz, uint32_t duty)
{
    if (!pin_ok(pin) || pin >= 32) return SYS_EPIN;
    if (hz == 0) {
        if (!pins[pin].pwm) return SYS_EARG;
        pins[pin].pwm = 0;
        pins[pin].mode = SYS_PIN_IN;
        return 0;
    }
    if (hz > 1000000 || duty > SYS_PWM_FULL) return SYS_EARG;
    pins[pin].pwm = 1;
    return 0;
}

int sys_adc(int source)
{
    if (source == SYS_ADC_TEMP) return 1000;
    if (source == SYS_ADC_VREF) return 1500;
    if (!pin_ok(source)) return SYS_EPIN;
    if (!(source <= 7 || source == 16 || source == 17 ||
          (source >= 32 && source <= 37)))
        return SYS_EPIN;
    pins[source].mode = SYS_PIN_ANALOG;
    return 2048;
}

int main(int argc, char **argv)
{
    uint32_t memsz = 256 * 1024, flags = 0;
    uint8_t *heap;
    static char oldcmd[600];
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc)
            memsz = (uint32_t)strtoul(argv[++i], 0, 0) * 1024;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            snprintf(oldcmd, sizeof oldcmd, "OLD \"%s\"", argv[++i]);
            queued[nqueued++] = oldcmd;
            queued[nqueued++] = "RUN";
            flags = 1;
        } else {
            fprintf(stderr, "usage: basic-host [-m kbytes] [-r program.bas]\n");
            return 1;
        }
    }
    heap = calloc(1, memsz);
    setvbuf(stdin, NULL, _IONBF, 0);    /* so INKEY$ can poll for a key */
    {
        /* sigaction: signal() may reset to the default after one ^C */
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_int;
        sigaction(SIGINT, &sa, NULL);
    }
    return bas_main(heap, memsz, flags);
}
