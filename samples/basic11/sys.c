/*
 * sys.c - the system calls of basic/bas.h on the Freya API.
 *
 * For samples/basic11, which compiles basic.c for the board and calls
 * these directly, and samples/basic11vm, which runs basic.c compiled
 * for the virtual machine and calls them for the image's TRAPs: one
 * copy, so a BASIC program sees the same console, files, clock and
 * pins under either.  Included by both, after setting g and the rest
 * of the state below that app_main() fills in.
 *
 * Ctrl-C is taken raw so that it stops the BASIC program and returns
 * to READY rather than ending the program running it.
 */

static const freya_api_t *g;
static int s_break;             /* Ctrl-C seen, not yet reported */
static const char *queued[2];   /* OLD "name" and RUN, typed for the user */
static int nqueued;
static const char *text;        /* -e: the lines still to be typed */
static int scripted;            /* PROGRAM.BAS or -e: end when it does */

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
int sys_readline(char *buf, int max, int running)
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

/* A chip that pulls an input only refuses a pull-up on an open-drain
 * pin with FREYA_ERR_UNSUPPORTED, which is a device error here, as a
 * kernel older than the call is. */
int sys_pin_pull(int pin, int pull)
{
    if (!FREYA_API_HAS(g, pin_pull_get)) return SYS_EIO;
    return pin_rc(g->pin_pull(pin, pull));
}

int sys_pin_pull_get(int pin)
{
    if (!FREYA_API_HAS(g, pin_pull_get)) return SYS_EIO;
    return pin_rc(g->pin_pull_get(pin));
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
