/* armrt.c - the board's BASIC under qemu-arm, for the tests.
 *
 *   basic-arm [-m kbytes] [-r program.bas] [-s]
 *
 * basic.c compiled the way samples/basic11 is, by arm-none-eabi-gcc for
 * a Cortex-M4F with its FPU, -Os and -ffp-contract=off, and run by
 * qemu-arm as a Linux program.  The arithmetic is the FPU's VFP
 * instructions, the stack frames are the board's, setjmp, longjmp and
 * the stack bas_main() runs on are armrt.h's, the board program's own.
 * Only the system calls of bas.h are different: they are hostrt.c's,
 * made with Linux system calls since there is no C library here, and
 * the pins are the same pretend ones.  So the BASIC tests can check
 * that the code for the board prints what basic-host and the image for
 * the virtual machine print.
 *
 * The interpreter runs on a stack of STACK_BYTES, filled with a pattern
 * first; -s reports, at the end, how much of it was used, which is how
 * the stack of samples/basic11 was sized.
 */
#include <stdint.h>

#define STACK_BYTES  (64u * 1024u)
#define STACK_MARGIN 1536u
#define STACK_FILL   0x5afe57acu
#define WORK_MAX     (256u * 1024u)

/* as samples/basic11/main.c has them: one translation unit, so that
 * what the compiler inlines and the frames it makes are the board's */
static uintptr_t stack_floor;
#define BAS_STACK_FLOOR stack_floor
#include "basic.c"
#include "armrt.h"

/* ------------------------------------------------------------------ */
/* Linux, ARM EABI                                                     */

#define NR_exit_group    248
#define NR_read          3
#define NR_write         4
#define NR_open          5
#define NR_close         6
#define NR_unlink        10
#define NR_nanosleep     162
#define NR_poll          168
#define NR_clock_gettime 263

#define O_WRONLY 01
#define O_CREAT  0100
#define O_TRUNC  01000
#define POLLIN   1
#define CLOCK_REALTIME 0

static long arm_sc(long nr, long a, long b, long c)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r7 __asm__("r7") = nr;

    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

/* a system call's answer as the C library would give it: -1 for an error */
static int arm_rc(long r)
{
    return r < 0 && r > -4096 ? -1 : (int)r;
}

/* the compiler may call these for a structure copy or clear */
void *memcpy(void *d, const void *s, unsigned n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;

    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, unsigned n)
{
    uint8_t *dp = d;

    while (n--) *dp++ = (uint8_t)c;
    return d;
}

/* ------------------------------------------------------------------ */
/* The system calls of bas.h                                           */

static uint8_t arm_work[WORK_MAX];
static uint32_t arm_stack[STACK_BYTES / 4];
static int arm_stepping;            /* -s */
static const char *arm_queued[2];
static int arm_nqueued;
static char arm_out[256];
static int arm_nout;

static void arm_flush(void)
{
    if (arm_nout) arm_sc(NR_write, 1, (long)arm_out, arm_nout);
    arm_nout = 0;
}

static void arm_puts(int fd, const char *s)
{
    int n = 0;

    while (s[n]) n++;
    arm_sc(NR_write, fd, (long)s, n);
}

static void arm_putnum(int fd, uint32_t v)
{
    char b[12];
    int i = 11;

    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    arm_puts(fd, b + i);
}

void sys_exit(int code)
{
    uint32_t i;

    arm_flush();
    if (arm_stepping) {
        for (i = 0; i < STACK_BYTES / 4 && arm_stack[i] == STACK_FILL; i++)
            ;
        arm_putnum(2, STACK_BYTES - i * 4);
        arm_puts(2, " bytes of stack\n");
    }
    arm_sc(NR_exit_group, code, 0, 0);
    for (;;)
        ;
}

void sys_putc(int c)
{
    arm_out[arm_nout++] = (char)c;
    if ((c & 0xff) == '\n' || arm_nout == (int)sizeof arm_out) arm_flush();
}

int sys_readline(char *buf, int max, int running)
{
    int n = 0;
    char c;

    if (arm_nqueued) {
        const char *s = arm_queued[0];

        arm_queued[0] = arm_queued[1];
        arm_nqueued--;
        while (*s && n < max - 1) buf[n++] = *s++;
        buf[n] = 0;
        return n;
    }
    arm_flush();
    for (;;) {
        if (arm_sc(NR_read, 0, (long)&c, 1) != 1) {
            if (n == 0) return -1;
            break;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < max - 1) buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

int sys_break(void)
{
    return 0;
}

int sys_open(const char *path, int mode)
{
    return arm_rc(arm_sc(NR_open, (long)path, mode ? O_WRONLY | O_CREAT | O_TRUNC : 0, 0666));
}

int sys_close(int fd)
{
    return arm_rc(arm_sc(NR_close, fd, 0, 0));
}

int sys_read(int fd, void *buf, int len)
{
    return arm_rc(arm_sc(NR_read, fd, (long)buf, len));
}

int sys_write(int fd, const void *buf, int len)
{
    return arm_rc(arm_sc(NR_write, fd, (long)buf, len));
}

static void arm_now(uint32_t *sec, uint32_t *ms)
{
    struct { int32_t sec, nsec; } ts;

    arm_sc(NR_clock_gettime, CLOCK_REALTIME, (long)&ts, 0);
    *sec = (uint32_t)ts.sec;
    *ms = (uint32_t)ts.nsec / 1000000u;
}

/* milliseconds since the first call, as hostrt.c counts them */
uint32_t sys_ticks(void)
{
    static uint32_t base;
    static int started;
    uint32_t s, ms, t;

    arm_now(&s, &ms);
    t = s * 1000u + ms;
    if (!started) {
        base = t;
        started = 1;
    }
    return t - base;
}

/* the civil time of the clock, in UTC */
int sys_clock(int *f)
{
    uint32_t s, ms, days, y = 1970, m = 1;
    static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t leap, len;

    arm_now(&s, &ms);
    days = s / 86400u;
    s %= 86400u;
    for (;;) {
        leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        if (days < 365u + leap) break;
        days -= 365u + leap;
        y++;
    }
    for (;;) {
        len = mdays[m - 1] + (m == 2 && leap);
        if (days < len) break;
        days -= len;
        m++;
    }
    f[0] = (int)y;
    f[1] = (int)m;
    f[2] = (int)days + 1;
    f[3] = (int)(s / 3600u);
    f[4] = (int)(s / 60u % 60u);
    f[5] = (int)(s % 60u);
    return 0;
}

void sys_sleep(uint32_t ms)
{
    struct { int32_t sec, nsec; } ts;

    arm_flush();
    ts.sec = (int32_t)(ms / 1000u);
    ts.nsec = (int32_t)(ms % 1000u * 1000000u);
    arm_sc(NR_nanosleep, (long)&ts, 0, 0);
}

int sys_unlink(const char *path)
{
    return arm_rc(arm_sc(NR_unlink, (long)path, 0, 0));
}

int sys_inkey(void)
{
    struct { int32_t fd; int16_t events, revents; } p;
    char c;

    arm_flush();
    p.fd = 0;
    p.events = POLLIN;
    p.revents = 0;
    if (arm_sc(NR_poll, (long)&p, 1, 0) <= 0) return -1;
    if (arm_sc(NR_read, 0, (long)&c, 1) != 1) return -1;
    return (uint8_t)c;
}

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

/* ------------------------------------------------------------------ */

static int arm_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t arm_number(const char *s)
{
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    return v;
}

__attribute__((used)) static void start(uint32_t *sp)
{
    int argc = (int)sp[0], i;
    char **argv = (char **)(sp + 1);
    static char oldcmd[600];
    uint32_t kib = 256, flags = 0;
    int n;
    const char *p;

    for (i = 1; i < argc; i++) {
        if (arm_streq(argv[i], "-m") && i + 1 < argc) {
            kib = arm_number(argv[++i]);
        } else if (arm_streq(argv[i], "-r") && i + 1 < argc) {
            p = argv[++i];
            n = 0;
            oldcmd[n++] = 'O'; oldcmd[n++] = 'L'; oldcmd[n++] = 'D';
            oldcmd[n++] = ' '; oldcmd[n++] = '"';
            while (*p && n < (int)sizeof oldcmd - 3) oldcmd[n++] = *p++;
            oldcmd[n++] = '"';
            oldcmd[n] = 0;
            arm_queued[arm_nqueued++] = oldcmd;
            arm_queued[arm_nqueued++] = "RUN";
            flags = 1;
        } else if (arm_streq(argv[i], "-s")) {
            arm_stepping = 1;
        } else {
            arm_puts(2, "usage: basic-arm [-m kbytes] [-r program.bas] [-s]\n");
            sys_exit(1);
        }
    }
    if (kib * 1024u > WORK_MAX) kib = WORK_MAX / 1024u;
    for (i = 0; i < (int)(STACK_BYTES / 4); i++) arm_stack[i] = STACK_FILL;
    stack_floor = (uintptr_t)arm_stack + STACK_MARGIN;
    sys_exit(bas_main_on(arm_work, kib * 1024u, flags, arm_stack + STACK_BYTES / 4));
}

/* Linux starts here with argc, then argv, at the stack pointer. */
__asm__(
    ".syntax unified\n"
    ".thumb\n"
    ".section .text._start,\"ax\",%progbits\n"
    ".thumb_func\n"
    ".global _start\n"
    "_start:\n"
    "    mov     r0, sp\n"
    "    bl      start\n"
);
