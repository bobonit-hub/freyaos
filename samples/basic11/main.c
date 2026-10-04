/*
 * basic11 - BASIC-11 compiled for the board itself.
 *
 *     run basic11 [-m KIB] [PROGRAM.BAS | -e TEXT]
 *
 * The interpreter in basic/basic.c, the one that also runs on the PDP-11
 * virtual machine, built with arm-none-eabi-gcc for the Cortex-M4F, the
 * Cortex-M33 and the Cortex-M3.  Its numbers are single-precision floats
 * (basic/fpnat.c): the FPU's on the Black Pill, the STM32F405, the Black
 * Pill 2, the STM32U585, the STM32H523, the STM32H562 and the STM32H723,
 * and on the Blue
 * Pill, whose Cortex-M3 has no floating point, the soft-float helpers of
 * src/softfp.c, which the Makefile links into every program there.
 *
 * The BASIC program, its variables and its strings live in KIB kilobytes
 * taken from the kernel heap: what -m asks for, or as much as the kernel
 * will give up to 32 KiB.  The Blue Pill has no such heap.  There the
 * interpreter is built small (BAS_SMALL in basic.c) and its workspace is
 * a fixed part of this program's own RAM window, and -m is refused.
 *
 * With PROGRAM.BAS the program is loaded and run and the run ends when it
 * does, with status 1 after an error.  -e TEXT does the same with the
 * program's lines given as the text itself, one per line; it is how a
 * kernel built with BASIC=file starts the program it carries.  Otherwise
 * the interpreter takes commands from the console until BYE.
 *
 * basic.c wants its system calls and a setjmp; they are below, written
 * against the Freya API.  Ctrl-C is taken raw so that it stops the BASIC
 * program and returns to READY rather than ending this one.
 */
#include "freya_api.h"

#define BAS_BANNER "BASIC-11 for Freya"
#ifndef __ARM_FP
#define BAS_SMALL
/*
 * The Blue Pill.  This program starts no threads and owns the whole of
 * its 9 KiB window (NOTHREADS and WHOLE_WINDOW in the Makefile).  Its
 * variables are at the bottom.  The program stack is the shell's, the
 * 2560 bytes directly above the window, and the top STACK_EXTRA bytes of
 * the window are left to it to grow into; the workspace is what lies
 * between.  The interpreter stops nesting STACK_MARGIN bytes short of
 * the bottom of that, which leaves room for the deepest calls below an
 * expression and the kernel's under them.
 */
#define WINDOW_END   (FREYA_APP_LOAD_ADDR + FREYA_APP_NOTHREADS_SIZE)
#define STACK_EXTRA  1024u
#define STACK_MARGIN 384u
#define BAS_STACK_FLOOR (WINDOW_END - STACK_EXTRA + STACK_MARGIN)
#endif
#include "../../basic/basic.c"

#ifdef BAS_SMALL
extern char __bss_used__[], __bss_end__[];
#else
#define DEFAULT_KIB  32u
#define MIN_KIB      12u        /* 8 KiB of workspace plus the scratch area */
#endif

static const freya_api_t *g;
static int s_break;             /* Ctrl-C seen, not yet reported */
static const char *queued[2];   /* OLD "name" and RUN, typed for the user */
static int nqueued;
static const char *text;        /* -e: the lines still to be typed */
static int scripted;            /* PROGRAM.BAS or -e: end when it does */

/*
 * setjmp and longjmp for the interpreter's error exit.  r4-r11, sp and
 * lr, and s16-s31 when the code is hard-float, because the compiler may
 * keep a value in the callee-saved half of the FPU across a call.  The
 * Blue Pill's Cortex-M3 has no FPU and is built soft-float, so there
 * the core registers are all of it.  Plain Thumb-2 and VFP, as the
 * kernel's own src/setjmp.s is.
 */
#ifdef __ARM_FP
#define VFP_SAVE    "    vstmia  r0!, {s16-s31}\n"
#define VFP_RESTORE "    vldmia  r0!, {s16-s31}\n"
#else
#define VFP_SAVE    ""
#define VFP_RESTORE ""
#endif
__asm__(
    ".syntax unified\n"
    ".thumb\n"
    ".section .text.setjmp,\"ax\",%progbits\n"
    ".thumb_func\n"
    ".global setjmp\n"
    ".type setjmp, %function\n"
    "setjmp:\n"                             /* r0 = buffer */
    "    mov     r2, sp\n"
    "    stmia   r0!, {r2, r4-r11, lr}\n"
    VFP_SAVE
    "    movs    r0, #0\n"
    "    bx      lr\n"
    ".size setjmp, . - setjmp\n"
    ".section .text.longjmp,\"ax\",%progbits\n"
    ".thumb_func\n"
    ".global longjmp\n"
    ".type longjmp, %function\n"
    "longjmp:\n"                            /* r0 = buffer, r1 = value */
    "    ldmia   r0!, {r2, r4-r11, lr}\n"
    VFP_RESTORE
    "    mov     sp, r2\n"
    "    movs    r0, r1\n"
    "    it      eq\n"
    "    moveq   r0, #1\n"                  /* setjmp must never return 0 twice */
    "    bx      lr\n"
    ".size longjmp, . - longjmp\n"
);

/* The keys typed while a program runs.  Ctrl-C among them stops it;
 * the rest wait here for INKEY$, or for the next INPUT, and the
 * oldest is dropped when there is no room. */
#define NKEYBUF 16
static uint8_t keybuf[NKEYBUF];
static int keyhead, keycount;

static void key_put(int c)
{
    if (keycount == NKEYBUF) {
        keyhead = (keyhead + 1) % NKEYBUF;
        keycount--;
    }
    keybuf[(keyhead + keycount++) % NKEYBUF] = (uint8_t)c;
}

static int key_get(void)
{
    int c;

    if (!keycount) return -1;
    c = keybuf[keyhead];
    keyhead = (keyhead + 1) % NKEYBUF;
    keycount--;
    return c;
}

/* Everything the console has: Ctrl-C is the break, the rest is kept. */
static void poll_break(void)
{
    while (g->kbhit()) {
        int c = g->getc_timeout(0);

        if (c < 0) break;
        if (c == 0x03) s_break = 1;
        else key_put(c);
    }
}

/* ------------------------------------------------------------------ */
/* The system calls of bas.h                                          */

void sys_exit(int code)
{
    g->exit(code);
    for (;;)
        ;
}

void sys_putc(int c)
{
    if ((c & 0xff) == '\n') g->putc('\r');
    g->putc((char)c);
}

/* A line with its own echo and erase; the newline is not stored. */
int sys_readline(char *buf, int max)
{
    int n = 0;

    if (max <= 0) return -1;
    if (text && *text) {
        /* the next line of -e; one too long for the buffer is cut */
        while (*text && *text != '\n') {
            if (*text != '\r' && n < max - 1) buf[n++] = *text;
            text++;
        }
        if (*text) text++;
        buf[n] = 0;
        return n;
    }
    if (nqueued) {
        const char *s = queued[0];

        queued[0] = queued[1];
        nqueued--;
        while (*s && n < max - 1) buf[n++] = *s++;
        buf[n] = 0;
        return n;
    }
    /* Typed for the user and run: the next command line is the end of
     * the input, which ends batch mode.  An INPUT while the program
     * runs still reads the console. */
    if (scripted && !running) return -1;
    for (;;) {
        int c = key_get();

        if (c < 0) c = g->getc();
        if (c < 0) return -1;
        if (c == '\r' || c == '\n') break;
        if (c == 0x03) {
            g->puts("^C\r\n");
            s_break = 1;
            n = 0;
            break;
        }
        if (c == 0x08 || c == 0x7f) {
            if (n) {
                n--;
                g->puts("\b \b");
            }
            continue;
        }
        if (c >= ' ' && n < max - 1) {
            buf[n++] = (char)c;
            g->putc((char)c);
        }
    }
    g->puts("\r\n");
    buf[n] = 0;
    return n;
}

int sys_break(void)
{
    int r;

    poll_break();
    r = s_break;
    s_break = 0;
    return r;
}

int sys_open(const char *path, int mode)
{
    return g->open(path, mode ? FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC
                              : FREYA_O_RDONLY);
}

int sys_close(int fd)
{
    return g->close(fd);
}

int sys_read(int fd, void *buf, int len)
{
    return g->read(fd, buf, len);
}

int sys_write(int fd, const void *buf, int len)
{
    return g->write(fd, buf, len);
}

uint32_t sys_ticks(void)
{
    return g->ticks_ms();
}

int sys_unlink(const char *path)
{
    return g->unlink(path);
}

/* The clock a kernel older than these calls does not have, and the one
 * the Blue Pill had no room for, both answer that there is no date. */
int sys_clock(int *f)
{
    freya_rtc_t t;

    if (!FREYA_API_HAS(g, rtc_get) || g->rtc_get(&t) != 0) return -1;
    f[0] = (int)t.year;
    f[1] = t.mon;
    f[2] = t.day;
    f[3] = t.hour;
    f[4] = t.min;
    f[5] = t.sec;
    return 0;
}

void sys_sleep(uint32_t ms)
{
    g->delay_ms(ms);
}

int sys_inkey(void)
{
    poll_break();
    return key_get();
}

/* The kernel writes both when this program has ended; FSAVE ends it.
 * A kernel older than these calls cannot. */
int sys_flash_save(const char *text, int len)
{
    int rc;

    if (!FREYA_API_HAS(g, flash_text_save)) return SYS_EIO;
    rc = g->flash_text_save(text, len);
    if (rc == FREYA_ERR_ARG) return SYS_EARG;
    return rc < 0 ? SYS_EIO : 0;
}

int sys_autostart(int on)
{
    if (!FREYA_API_HAS(g, autostart_set)) return SYS_EIO;
    return g->autostart_set(on) < 0 ? SYS_EIO : 0;
}

/* The pins, on the same calls the shell's pin, pwm and adc commands
 * make.  The first three FREYA_ERR_* codes are the SYS_E* codes;
 * everything else the API can answer, a timeout, a bus error, a call
 * a kernel this old does not have, is a device error to BASIC. */
static int pin_rc(int rc)
{
    if (rc >= 0 || rc == FREYA_ERR_PIN || rc == FREYA_ERR_BUSY || rc == FREYA_ERR_ARG)
        return rc;
    return SYS_EIO;
}

int sys_pin_mode(int pin, int mode)
{
    if (!FREYA_API_HAS(g, pin_toggle)) return SYS_EIO;
    return pin_rc(g->pin_mode(pin, mode));
}

int sys_pin_read(int pin)
{
    if (!FREYA_API_HAS(g, pin_toggle)) return SYS_EIO;
    return pin_rc(g->pin_read(pin));
}

int sys_pin_write(int pin, int level)
{
    if (!FREYA_API_HAS(g, pin_toggle)) return SYS_EIO;
    return pin_rc(g->pin_write(pin, level));
}

int sys_pin_toggle(int pin)
{
    if (!FREYA_API_HAS(g, pin_toggle)) return SYS_EIO;
    return pin_rc(g->pin_toggle(pin));
}

/* pwm_open() hands out a handle, and the same one again for a pin that
 * is already open, so the handles of the channels this program started
 * are kept by pin for PWM(P$) to close with.  The kernel closes them
 * all when the run ends. */
#define NPWM 8

static struct {
    int pin, handle;
} pwms[NPWM];
static int npwm;

int sys_pwm(int pin, uint32_t hz, uint32_t duty)
{
    int i, rc;

    if (!FREYA_API_HAS(g, pwm_freq)) return SYS_EIO;
    for (i = 0; i < npwm && pwms[i].pin != pin; i++)
        ;
    if (hz == 0) {
        if (i == npwm) return SYS_EARG;             /* not running */
        rc = g->pwm_close(pwms[i].handle);
        pwms[i] = pwms[--npwm];
        return pin_rc(rc);
    }
    rc = g->pwm_open(pin, hz, duty);
    if (rc < 0) return pin_rc(rc);
    if (i == npwm) {
        if (npwm == NPWM) {
            g->pwm_close(rc);
            return SYS_EBUSY;
        }
        pwms[npwm].pin = pin;
        pwms[npwm++].handle = rc;
    }
    return 0;
}

int sys_adc(int source)
{
    if (!FREYA_API_HAS(g, adc_read)) return SYS_EIO;
    return pin_rc(g->adc_read(source));
}

/* ------------------------------------------------------------------ */

#ifndef BAS_SMALL
static int parse_kib(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 1000000u) return -1;
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}
#endif

#ifdef BAS_SMALL
#define USAGE "usage: basic11 [PROGRAM.BAS | -e TEXT]\r\n"
#else
#define USAGE "usage: basic11 [-m KIB] [PROGRAM.BAS | -e TEXT]\r\n"
#endif

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *program = 0;
    uint32_t flags = 0;
#ifdef BAS_SMALL
    uint32_t work_bytes;
#else
    uint32_t kib = 0;
#endif
    uint8_t *heap = 0;
    static char oldcmd[80];
    int i;

    g = api;
    if (!FREYA_API_HAS(api, console_raw)) {
        api->puts("basic11: this kernel has no raw console\r\n");
        return FREYA_EXIT_FAIL;
    }
    for (i = 1; i < argc; i++) {
#ifndef BAS_SMALL
        if (argv[i][0] == '-' && argv[i][1] == 'm' && argv[i][2] == 0 && i + 1 < argc) {
            if (parse_kib(argv[++i], &kib) || kib < MIN_KIB) {
                api->printf("basic11: -m needs a size in KiB, %u or more\r\n", MIN_KIB);
                return FREYA_EXIT_USAGE;
            }
            continue;
        }
#endif
        if (argv[i][0] == '-' && argv[i][1] == 'e' && argv[i][2] == 0 &&
            i + 1 < argc && !program && !text) {
            text = argv[++i];
        } else if (argv[i][0] == '-' || program || text) {
            api->puts(USAGE);
            return FREYA_EXIT_USAGE;
        } else {
            program = argv[i];
        }
    }

#ifdef BAS_SMALL
    /* Everything from the end of the variables to the stack's room.  A
     * program image copied to RAM would sit at the top of the window,
     * so check that the linker really gave this program all of it. */
    if ((uintptr_t)__bss_end__ != WINDOW_END) {
        api->puts("basic11: not linked to own the program window\r\n");
        return FREYA_EXIT_FAIL;
    }
    heap = (uint8_t *)(((uintptr_t)__bss_used__ + 7u) & ~(uintptr_t)7u);
    work_bytes = (uint32_t)(WINDOW_END - STACK_EXTRA - (uintptr_t)heap);
#else
    /* the workspace: what was asked for, or the most the kernel has */
    if (kib) {
        heap = api->malloc(kib * 1024u);
    } else {
        for (kib = DEFAULT_KIB; kib >= MIN_KIB && !heap; kib -= 4u)
            heap = api->malloc(kib * 1024u);
    }
    if (!heap) {
        api->printf("basic11: no %u KiB of memory for the workspace\r\n", kib);
        return FREYA_EXIT_FAIL;
    }
#endif

    if (text) {
        /* the lines of the text, then RUN */
        queued[nqueued++] = "RUN";
        flags |= 1;                     /* batch: no banner, exit at the end */
        scripted = 1;
    } else if (program) {
        /* the first two lines of input are OLD "name" and RUN */
        const char *p = program;
        int n = 0;

        oldcmd[n++] = 'O'; oldcmd[n++] = 'L'; oldcmd[n++] = 'D';
        oldcmd[n++] = ' '; oldcmd[n++] = '"';
        while (*p && n < (int)sizeof oldcmd - 3) oldcmd[n++] = *p++;
        oldcmd[n++] = '"';
        oldcmd[n] = 0;
        queued[nqueued++] = oldcmd;
        queued[nqueued++] = "RUN";
        flags |= 1;                     /* batch: no banner, exit at the end */
        scripted = 1;
    }

    /* The interpreter leaves through sys_exit(), and the kernel frees
     * the workspace and turns raw mode off when the run ends. */
    api->console_raw(1);
#ifdef BAS_SMALL
    return bas_main(heap, work_bytes, flags);
#else
    return bas_main(heap, kib * 1024u, flags);
#endif
}
