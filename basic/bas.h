/* bas.h - types and the host interface of the BASIC interpreter.
 *
 * basic.c is built three ways from the same source:
 *
 *   BAS_VM     the image for the PDP-11 virtual machine, with cproc and
 *              QBE.  Freestanding: no libc, no headers.  cproc for the
 *              Freya VM is ILP32 with an unsigned char, so the widths
 *              below are the only ones this program uses.  rt.s has
 *              setjmp and the system calls.
 *   BAS_HOST   the same interpreter compiled natively on the PC for the
 *              tests, which supply the sys_* calls with stdio.
 *   neither    a Freya program, compiled with arm-none-eabi-gcc for the
 *              board itself.  Freestanding too, but the compiler's
 *              <stdint.h> is there; the program that includes basic.c
 *              supplies setjmp, longjmp and the sys_* calls.
 *
 * BAS_FP11 picks the FP11 arithmetic of fp11.c for the numbers; without
 * it they are the C float of fpnat.c.  The VM image always uses the
 * FP11, since that machine has no floating point of its own.
 */
#ifndef BAS_H
#define BAS_H

#if defined(BAS_HOST)
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
typedef uintptr_t uptr;
#elif defined(BAS_VM)
typedef unsigned int   uint32_t;
typedef signed int     int32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef signed char    int8_t;
typedef uint32_t       uptr;
#define NULL ((void *)0)

/* rt.s */
typedef uint32_t jmp_buf[3];
int  setjmp(jmp_buf b);
void longjmp(jmp_buf b, int val);
#else
#include <stdint.h>
typedef uintptr_t uptr;
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

/* The host: TRAP n on the VM, stdio on the PC, the Freya API on the
 * board.  The arguments are in R0-R2 and the result in R0 on the VM. */
void     sys_exit(int code);
void     sys_putc(int c);
int      sys_readline(char *buf, int max);  /* length, or -1 at end/break */
int      sys_break(void);                   /* non-zero once Ctrl-C was seen */
int      sys_open(const char *path, int mode); /* 0 read, 1 write; fd or -1 */
int      sys_close(int fd);
int      sys_read(int fd, void *buf, int len);
int      sys_write(int fd, const void *buf, int len);
uint32_t sys_ticks(void);                   /* milliseconds */
int      sys_unlink(const char *path);

#endif
