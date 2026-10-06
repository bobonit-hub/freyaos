/* bas.h - types and the host interface of the BASIC interpreter.
 *
 * basic.c is built three ways from the same source:
 *
 *   BAS_HOST   the interpreter compiled natively on the PC for the
 *              tests, which supply the sys_* calls with stdio.
 *   BAS_VM     the image for the Freya virtual machine, compiled with
 *              cproc and QBE.  No headers at all: the types are below,
 *              and setjmp, longjmp and the sys_* calls are vmrt.s,
 *              each call a TRAP that the program running the machine
 *              serves.  Its numbers are BAS_SOFTFLOAT's, integers.
 *   neither    a Freya program, compiled with arm-none-eabi-gcc for the
 *              board itself.  Freestanding, though the compiler's
 *              <stdint.h> is there; the program that includes basic.c
 *              supplies setjmp, longjmp and the sys_* calls.
 *
 * The numbers are IEEE single-precision floats every way, rounded the
 * same, so each build computes what the FPU of a Cortex-M4F computes.
 */
#ifndef BAS_H
#define BAS_H

#if defined(BAS_VM)
/* cproc for the Freya VM is ILP32 with an unsigned char. */
typedef unsigned int   uint32_t;
typedef signed int     int32_t;
typedef unsigned short uint16_t;
typedef signed short   int16_t;
typedef unsigned char  uint8_t;
typedef signed char    int8_t;
typedef uint32_t       uptr;
#define NULL ((void *)0)

/* vmrt.s: the frame pointer, the stack pointer and the return address */
typedef uint32_t jmp_buf[3];
int  setjmp(jmp_buf b);
void longjmp(jmp_buf b, int val);
#else
#include <stdint.h>
typedef uintptr_t uptr;
#endif

#if defined(BAS_HOST)
#include <stddef.h>
#include <setjmp.h>
#elif !defined(BAS_VM)
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
/* A line from the terminal, the newline dropped: its length, or -1 at
 * the end of the input.  running says whether a stored program is
 * executing, an INPUT, or the interpreter waits for a command. */
int      sys_readline(char *buf, int max, int running);
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
/* The next key typed, or -1 when none is waiting; never waits. */
int      sys_inkey(void);
/* Flash the host writes once the interpreter has ended: the program's
 * text, len bytes, to run at the next boot, and the auto-start flag.
 * 0 when the host has taken note; SYS_EARG for a text too long for the
 * room there, SYS_EIO when this host cannot. */
int      sys_flash_save(const char *text, int len);
int      sys_autostart(int on);

/* The pins, behind PIN, PWM and ADC.  A pin is its port and its number
 * in one integer, port * 16 + number, as FREYA_PIN() packs one; the
 * interpreter turns "PB0" into it.  A mode is one of SYS_PIN_*, in the
 * order the Freya API numbers them.  Each call answers with the value
 * asked for, or with one of the SYS_E* codes, which are the first
 * FREYA_ERR_* codes of the API; a host that has no such hardware
 * answers SYS_EIO. */
#define SYS_PIN_IN          0
#define SYS_PIN_IN_PULLUP   1
#define SYS_PIN_IN_PULLDOWN 2
#define SYS_PIN_OUT         3
#define SYS_PIN_OUT_OD      4
#define SYS_PIN_ANALOG      5
#define SYS_ADC_TEMP        0x100   /* the internal sources of sys_adc() */
#define SYS_ADC_VREF        0x101
#define SYS_PWM_FULL        10000u  /* a duty cycle is in ten-thousandths */
#define SYS_EPIN   -1               /* no such pin, or one the kernel keeps */
#define SYS_EBUSY  -2               /* the pin, or its timer, is taken */
#define SYS_EARG   -3               /* out of range */
#define SYS_EIO    -4               /* did not finish, or no such device here */
int      sys_pin_mode(int pin, int mode);
int      sys_pin_read(int pin);             /* 0 or 1 */
int      sys_pin_write(int pin, int level);
int      sys_pin_toggle(int pin);
/* Start a channel at hz with that duty, or with hz 0 stop the one on
 * the pin; 0 when done. */
int      sys_pwm(int pin, uint32_t hz, uint32_t duty);
int      sys_adc(int source);               /* a pin or SYS_ADC_*; 0..4095 */

#endif
