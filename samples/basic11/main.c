/*
 * basic11 - BASIC-11 compiled for the board itself.
 *
 *     run basic11 [-m KIB] [PROGRAM.BAS]
 *
 * The interpreter in basic/basic.c, the one that also runs on the PDP-11
 * virtual machine, built with arm-none-eabi-gcc for the Cortex-M4F and the
 * Cortex-M33.  Its numbers are the FPU's single-precision floats
 * (basic/fpnat.c), so this program is for the Black Pill, the STM32F405,
 * the Black Pill 2, the STM32U585 and the STM32H523 and is
 * not built for the Blue Pill, whose Cortex-M3 has no floating point.
 *
 * The BASIC program, its variables and its strings live in KIB kilobytes
 * taken from the kernel heap: what -m asks for, or as much as the kernel
 * will give up to 32 KiB.  With PROGRAM.BAS the program is loaded and
 * run and the run ends when it does, with status 1 after an error;
 * otherwise the interpreter takes commands from the console until BYE.
 *
 * basic.c wants its system calls and a setjmp; they are below, written
 * against the Freya API.  Ctrl-C is taken raw so that it stops the BASIC
 * program and returns to READY rather than ending this one.
 */
#include "freya_api.h"

#define BAS_BANNER "BASIC-11 for Freya"
#include "../../basic/basic.c"

#define DEFAULT_KIB  32u
#define MIN_KIB      12u        /* 8 KiB of workspace plus the scratch area */

static const freya_api_t *g;
static int s_break;             /* Ctrl-C seen, not yet reported */
static const char *queued[2];   /* OLD "name" and RUN, typed for the user */
static int nqueued;

/*
 * setjmp and longjmp for the interpreter's error exit.  r4-r11, sp and
 * lr, and s16-s31 because the code is hard-float and the compiler may
 * keep a value in the callee-saved half of the FPU across a call.
 * Plain Thumb-2 and VFP, as the kernel's own src/setjmp.s is.
 */
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
    "    vstmia  r0!, {s16-s31}\n"
    "    movs    r0, #0\n"
    "    bx      lr\n"
    ".size setjmp, . - setjmp\n"
    ".section .text.longjmp,\"ax\",%progbits\n"
    ".thumb_func\n"
    ".global longjmp\n"
    ".type longjmp, %function\n"
    "longjmp:\n"                            /* r0 = buffer, r1 = value */
    "    ldmia   r0!, {r2, r4-r11, lr}\n"
    "    vldmia  r0!, {s16-s31}\n"
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
    if (nqueued) {
        const char *s = queued[0];

        queued[0] = queued[1];
        nqueued--;
        while (*s && n < max - 1) buf[n++] = *s++;
        buf[n] = 0;
        return n;
    }
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

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *program = 0;
    uint32_t kib = 0, flags = 0;
    uint8_t *heap = 0;
    static char oldcmd[80];
    int i;

    g = api;
    if (!FREYA_API_HAS(api, console_raw)) {
        api->puts("basic11: this kernel has no raw console\r\n");
        return FREYA_EXIT_FAIL;
    }
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'm' && argv[i][2] == 0 && i + 1 < argc) {
            if (parse_kib(argv[++i], &kib) || kib < MIN_KIB) {
                api->printf("basic11: -m needs a size in KiB, %u or more\r\n", MIN_KIB);
                return FREYA_EXIT_USAGE;
            }
        } else if (argv[i][0] == '-' || program) {
            api->puts("usage: basic11 [-m KIB] [PROGRAM.BAS]\r\n");
            return FREYA_EXIT_USAGE;
        } else {
            program = argv[i];
        }
    }

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

    if (program) {
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
    }

    /* The interpreter leaves through sys_exit(), and the kernel frees
     * the workspace and turns raw mode off when the run ends. */
    api->console_raw(1);
    return bas_main(heap, kib * 1024u, flags);
}
