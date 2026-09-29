/* bas.h - types and the host interface of the BASIC image.
 *
 * The image is freestanding: no libc, no headers.  cproc for the
 * Freya VM is ILP32 with an unsigned char, so the widths below are
 * the only ones this program uses.  Built with -DBAS_HOST the same
 * sources compile with a host C compiler for the tests, which then
 * supply the sys_* calls themselves.
 */
#ifndef BAS_H
#define BAS_H

#ifdef BAS_HOST
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#else
typedef unsigned int   uint32_t;
typedef signed int     int32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef signed char    int8_t;
#define NULL ((void *)0)

/* rt.s */
typedef uint32_t jmp_buf[3];
int  setjmp(jmp_buf b);
void longjmp(jmp_buf b, int val);
#endif

/* rt.s: TRAP n with the arguments in R0-R2 and the result in R0. */
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
