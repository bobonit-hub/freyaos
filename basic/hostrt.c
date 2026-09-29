/* hostrt.c - run the interpreter natively, for testing.
 *
 *   cc -DBAS_HOST -DBAS_FP11 basic/basic.c basic/hostrt.c -o basic-host
 *   cc -DBAS_HOST basic/basic.c basic/hostrt.c -lm -o basic-float
 *   basic-host [-m kbytes] [-r program.bas]
 *
 * Supplies the system calls of bas.h with stdio, so the same basic.c
 * that goes into the VM image, or into the program for the board, runs
 * on the PC, where a debugger and the sanitizers can reach it.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
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

uint32_t sys_ticks(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

int sys_unlink(const char *path)
{
    return unlink(path);
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
    {
        /* sigaction: signal() may reset to the default after one ^C */
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_int;
        sigaction(SIGINT, &sa, NULL);
    }
    return bas_main(heap, memsz, flags);
}
