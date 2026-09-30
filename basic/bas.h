/* bas.h - types and the host interface of the BASIC interpreter.
 *
 * basic.c is built two ways from the same source:
 *
 *   BAS_HOST   the interpreter compiled natively on the PC for the
 *              tests, which supply the sys_* calls with stdio.
 *   neither    a Freya program, compiled with arm-none-eabi-gcc for the
 *              board itself.  Freestanding, though the compiler's
 *              <stdint.h> is there; the program that includes basic.c
 *              supplies setjmp, longjmp and the sys_* calls.
 *
 * The numbers are the C float of fpnat.c either way, so the host build
 * computes what the FPU of a Cortex-M4F computes.
 */
#ifndef BAS_H
#define BAS_H

#include <stdint.h>
typedef uintptr_t uptr;

#if defined(BAS_HOST)
#include <stddef.h>
#include <setjmp.h>
#else
#define NULL ((void *)0)

/* Supplied by the program: r4-r11, sp, lr and the callee-saved half
 * of the FPU, s16-s31. */
typedef uint32_t jmp_buf[26];
int  setjmp(jmp_buf b) __attribute__((returns_twice));
void longjmp(jmp_buf b, int val) __attribute__((noreturn));
#endif

static inline int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* The host: stdio on the PC, the Freya API on the board. */
void     sys_exit(int code);
void     sys_putc(int c);
int      sys_readline(char *buf, int max);  /* length, or -1 at end/break */
int      sys_break(void);                   /* non-zero once Ctrl-C was seen */
int      sys_open(const char *path, int mode); /* 0 read, 1 write; fd or -1 */
int      sys_close(int fd);
int      sys_read(int fd, void *buf, int len);
int      sys_write(int fd, const void *buf, int len);
uint32_t sys_ticks(void);                   /* milliseconds since start */
int      sys_unlink(const char *path);
/* The civil clock: year, month, day, hour, minute and second in
 * f[0..5].  0 when they were filled, -1 when this host has no clock a
 * program may read. */
int      sys_clock(int *f);
/* Called in short pieces, so that Ctrl-C is still noticed. */
void     sys_sleep(uint32_t ms);

#endif
