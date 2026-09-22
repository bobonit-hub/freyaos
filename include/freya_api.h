/*
 * Freya - user program ABI.
 *
 * This header is shared between the Freya kernel and the programs that
 * are loaded from the SD card into RAM.  A program is a raw binary image
 * that starts with a freya_app_header_t and is linked to run from the
 * user program region (see FREYA_APP_LOAD_ADDR).
 *
 * The entry point has the signature
 *
 *     int app_main(const freya_api_t *api, int argc, char **argv);
 *
 * The kernel is single tasking: while the program runs it owns the CPU.
 * Pressing Ctrl-C on the console, calling api->exit(), or returning from
 * app_main() hands control back to the Freya shell.
 */
#ifndef FREYA_API_H
#define FREYA_API_H

#include <stdint.h>

#define FREYA_APP_MAGIC        0x41595246UL   /* 'F','R','Y','A' */
#define FREYA_ABI_VERSION      2

/*
 * ABI 1 described only RAM images.  ABI 2 appends four fields for a
 * program that executes from internal flash, and because the v1 header is
 * a byte for byte prefix of the v2 header the loader still accepts a v1
 * RAM image - every hello.bin already sitting on a card keeps working.
 */
#define FREYA_ABI_MIN_VERSION  1

/*
 * Where a program lives.  This is the one part of the ABI that depends on
 * the board, because it depends on how much RAM there is; the Makefile
 * defines FREYA_BOARD_* for the kernel and for every program it builds, so
 * the two always agree.  The loader checks the header against these values
 * and refuses an image linked for a different region.
 *
 * A board that reserves part of its internal flash for a program image
 * also defines FREYA_APP_FLASH_ADDR and FREYA_APP_FLASH_SIZE.  Such a
 * program executes in place from flash and spends the RAM region on its
 * .data and .bss alone, which is what makes a ~25 KiB program possible on a
 * board whose whole SRAM is 20 KiB.  A 128-byte aligned slot immediately
 * before that region holds the auto-start flag (first word), the default
 * log level (second word) and the ram-dump-on-BusFault flag (third word).
 */
#if defined(FREYA_BOARD_BLUEPILL)
#define FREYA_APP_LOAD_ADDR    0x20001800UL     /* 20 KiB of SRAM */
#define FREYA_APP_REGION_SIZE  (8U * 1024U)
#define FREYA_AUTOSTART_ALIGN  128U
#define FREYA_AUTOSTART_ADDR   0x0800B000UL     /* page 44, 128-byte aligned */
#define FREYA_AUTOSTART_SIZE   FREYA_AUTOSTART_ALIGN
#define FREYA_LOGLEVEL_OFF     4U               /* second word of that slot */
#define FREYA_RAMDUMP_OFF      8U               /* third word of that slot  */
#define FREYA_APP_FLASH_ADDR   (FREYA_AUTOSTART_ADDR + FREYA_AUTOSTART_SIZE)
#define FREYA_APP_FLASH_SIZE   (0x08010000UL - FREYA_APP_FLASH_ADDR)
#if (FREYA_AUTOSTART_ADDR % FREYA_AUTOSTART_ALIGN) || \
    (FREYA_AUTOSTART_SIZE % FREYA_AUTOSTART_ALIGN) || \
    (FREYA_APP_FLASH_ADDR % FREYA_AUTOSTART_ALIGN)
#error "Blue Pill auto-start slot and program flash must be 128-byte aligned"
#endif
#elif defined(FREYA_BOARD_BLACKPILL)
#define FREYA_APP_LOAD_ADDR    0x20010000UL     /* 128 KiB of SRAM */
#define FREYA_APP_REGION_SIZE  (56U * 1024U)
/* Kernel occupies sectors 0..2 (48 KiB).  Sector 3 is unused except for
 * the 128-byte auto-start slot at its end, so an autostart erase never
 * shares a 64 KiB sector with a program image.  Sector 4 is the program. */
#define FREYA_AUTOSTART_ALIGN  128U
#define FREYA_AUTOSTART_ADDR   0x0800FF80UL     /* last 128 B of sector 3 */
#define FREYA_AUTOSTART_SIZE   FREYA_AUTOSTART_ALIGN
#define FREYA_LOGLEVEL_OFF     4U
#define FREYA_RAMDUMP_OFF      8U
#define FREYA_APP_FLASH_ADDR   (FREYA_AUTOSTART_ADDR + FREYA_AUTOSTART_SIZE)
#define FREYA_APP_FLASH_SIZE   (0x08020000UL - FREYA_APP_FLASH_ADDR)
#if (FREYA_AUTOSTART_ADDR % FREYA_AUTOSTART_ALIGN) || \
    (FREYA_AUTOSTART_SIZE % FREYA_AUTOSTART_ALIGN) || \
    (FREYA_APP_FLASH_ADDR % FREYA_AUTOSTART_ALIGN)
#error "Black Pill auto-start slot and program flash must be 128-byte aligned"
#endif
#else
#error "no board selected - define FREYA_BOARD_BLACKPILL or FREYA_BOARD_BLUEPILL"
#endif

/* header flags */
#define FREYA_APP_F_XIP        0x00000001UL   /* executes from flash     */

/* Header located at offset 0 of the program image. */
typedef struct {
    uint32_t magic;        /* FREYA_APP_MAGIC                         */
    uint32_t abi_version;  /* FREYA_ABI_VERSION                       */
    uint32_t load_addr;    /* address the image was linked for        */
    uint32_t entry;        /* absolute address of app_main (thumb)    */
    uint32_t image_size;   /* bytes of the file image (text + data)   */
    uint32_t bss_start;    /* zero initialised region, absolute       */
    uint32_t bss_end;
    uint32_t stack_need;   /* bytes of stack the program requires     */
    char     name[16];     /* informational, NUL padded               */
    /* ------------------------------------- appended in ABI 2 ------- */
    uint32_t flags;        /* FREYA_APP_F_XIP                         */
    uint32_t data_src;     /* flash address of the .data initialiser  */
    uint32_t data_start;   /* RAM destination of .data, absolute      */
    uint32_t data_end;
} freya_app_header_t;

/* Size of the ABI 1 header, which is a prefix of the one above.  The
 * loader reads this much first so that it never mistakes the first bytes
 * of an old image's .text for the appended fields. */
#define FREYA_APP_HDR_V1_SIZE  48

/*
 * How a run ended.  The kernel decides this, not the program: a program
 * either returns from app_main() or calls api->exit(), and everything else
 * is the console or a fault.
 */
enum {
    FREYA_STOP_NONE = 0,      /* app_main() returned                   */
    FREYA_STOP_EXIT,          /* the program called api->exit()        */
    FREYA_STOP_CTRLC,         /* stopped from the console              */
    FREYA_STOP_HARDFAULT,
    FREYA_STOP_MEMFAULT,
    FREYA_STOP_BUSFAULT,
    FREYA_STOP_USAGEFAULT
};

/*
 * Exit status.
 *
 * A status is a byte, so the kernel keeps the low eight bits of whatever
 * the program asked for: exit(-1) reads back as 255, the way waitpid()
 * would report it.  A run the kernel ended itself reports 128 + the reason
 * instead, which puts Ctrl-C at 130 - the number a POSIX shell gives a
 * program killed by SIGINT, because FREYA_STOP_CTRLC and SIGINT are both
 * 2.  A program is free to exit with 130 of its own accord; the reason is
 * recorded separately, and 'status' at the console prints both.
 */
#define FREYA_EXIT_OK        0      /* the only status that means success */
#define FREYA_EXIT_FAIL      1      /* the program failed                 */
#define FREYA_EXIT_USAGE     2      /* it was called wrongly              */
#define FREYA_EXIT_NOEXEC    126    /* the image was there but refused    */
#define FREYA_EXIT_NOTFOUND  127    /* there was nothing to run           */
#define FREYA_EXIT_KILLED    128    /* + FREYA_STOP_*: the kernel ended it */
#define FREYA_EXIT_STOPPED   (FREYA_EXIT_KILLED + FREYA_STOP_CTRLC)
#define FREYA_EXIT_MAX       255

/* The status a run reports, from the reason it ended and the code the
 * program asked for.  The kernel applies this; a program can use it to
 * read a status it was handed. */
static inline int freya_exit_status(int reason, int code)
{
    if (reason <= FREYA_STOP_EXIT || reason > FREYA_STOP_USAGEFAULT)
        return code & FREYA_EXIT_MAX;
    return FREYA_EXIT_KILLED + reason;
}

/* What became of the last program that ran.  Filled in by last_exit(). */
typedef struct {
    int32_t  status;      /* 0 .. FREYA_EXIT_MAX                       */
    int32_t  reason;      /* FREYA_STOP_*                              */
    uint32_t run_ms;      /* how long the run lasted                   */
    char     name[20];    /* the program's header name, NUL terminated */
} freya_exit_t;

/* open() flags */
#define FREYA_O_RDONLY  0x01
#define FREYA_O_WRONLY  0x02
#define FREYA_O_RDWR    0x03
#define FREYA_O_CREATE  0x04
#define FREYA_O_TRUNC   0x08
#define FREYA_O_APPEND  0x10

/* seek() whence */
#define FREYA_SEEK_SET  0
#define FREYA_SEEK_CUR  1
#define FREYA_SEEK_END  2

/* directory entry returned by readdir() */
typedef struct {
    char     name[64];
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  pad[3];
} freya_stat_t;

/* log() levels: a message is written when 0 < level <= the current level. */
#define FREYA_LOG_OFF    0
#define FREYA_LOG_ERROR  1
#define FREYA_LOG_WARN   2
#define FREYA_LOG_INFO   3
#define FREYA_LOG_DEBUG  4

#define FREYA_LOG_PATH      "/freya.log"
#define FREYA_LOG_OLD_PATH  "/freya.log.old"
#define FREYA_LOG_MAX_SIZE  (1024U * 1024U)     /* rotate at 1 MiB */

/* ------------------------------------------------- pins and interrupts */
/*
 * A pin is its port and its number in one integer, so that a program can
 * write FREYA_PB(0) and the kernel can hand the same number back to a
 * handler as the source of the interrupt.
 */
#define FREYA_PIN(port, n)   ((((port) & 0x0F) << 4) | ((n) & 0x0F))
#define FREYA_PIN_PORT(pin)  (((pin) >> 4) & 0x0F)
#define FREYA_PIN_NUM(pin)   ((pin) & 0x0F)

#define FREYA_PA(n)          FREYA_PIN(0, n)
#define FREYA_PB(n)          FREYA_PIN(1, n)
#define FREYA_PC(n)          FREYA_PIN(2, n)

/* pin_mode() */
#define FREYA_PIN_IN         0    /* input, floating                     */
#define FREYA_PIN_IN_PULLUP  1
#define FREYA_PIN_IN_PULLDOWN 2
#define FREYA_PIN_OUT        3    /* push-pull output                    */
#define FREYA_PIN_OUT_OD     4    /* open drain output                   */
#define FREYA_PIN_ANALOG     5    /* input buffer off                    */

/* pin_irq_attach() edges.  A switch bounces for a few milliseconds, which
 * is a few dozen edges; FREYA_EDGE_DEBOUNCE takes the first of them and
 * ignores the rest for FREYA_DEBOUNCE_MS.  A program that wants another
 * interval leaves the flag off and times the edges itself. */
#define FREYA_EDGE_RISING    1
#define FREYA_EDGE_FALLING   2
#define FREYA_EDGE_BOTH      (FREYA_EDGE_RISING | FREYA_EDGE_FALLING)
#define FREYA_EDGE_DEBOUNCE  4
#define FREYA_DEBOUNCE_MS    20

/* timer_open() flags */
#define FREYA_TIMER_ONESHOT  0x01   /* fire once, then stop itself       */

/* What a timer period may be: below the floor the kernel would spend the
 * run inside its own dispatch, and above the ceiling a 16-bit prescaler
 * and a 16-bit reload cannot reach. */
#define FREYA_TIMER_MIN_US   10UL
#define FREYA_TIMER_MAX_US   40000000UL

/* ---------------------------------------------------------------- PWM */
/*
 * A duty cycle is a fraction of the period in ten-thousandths, so 5000 is
 * half and FREYA_PWM_FULL is a pin held high for the whole period.  The
 * frequency floor is what a 16-bit prescaler and a 16-bit reload can
 * still divide down to; the ceiling is where a period stops having
 * enough counts left to set a duty cycle with.
 */
#define FREYA_PWM_FULL       10000UL
#define FREYA_PWM_MIN_HZ     1UL
#define FREYA_PWM_MAX_HZ     1000000UL

/*
 * What the pin, timer, PWM and interrupt calls return.  Anything else
 * they hand back is the value asked for: a pin level, a handle, a count.
 */
#define FREYA_ERR_PIN        -1   /* no such pin, one the kernel owns, or
                                   * one with no PWM channel behind it   */
#define FREYA_ERR_BUSY       -2   /* that line is taken, every timer is,
                                   * or the timer runs at another rate   */
#define FREYA_ERR_ARG        -3   /* mode, edge, period, frequency or
                                   * duty cycle out of range             */
#define FREYA_ERR_HANDLER    -4   /* not allowed from a handler          */

/*
 * A pin or timer handler.  It runs in interrupt context, on the same
 * stack as everything else, with 'source' set to the pin or the timer
 * that called it and 'arg' to whatever was registered beside it.
 *
 * What a handler may do is decided by what it can preempt.  Console
 * output, the LED, ticks_ms(), the pin calls and the timer calls are all
 * safe.  malloc(), free() and the filesystem are not - they can be
 * interrupted halfway through their own bookkeeping - so the kernel
 * refuses them from a handler instead of letting a program corrupt the
 * heap or the card.  A handler that faults, or one that never returns,
 * is killed and ends the run the way a fault in the program would; it
 * does not take Freya down with it.
 *
 * A handler is optional.  Attached as NULL, the interrupt is still
 * counted, and the program reads pin_irq_count() / timer_count() or
 * sleeps in irq_wait() from thread mode, where it may do anything.
 */
typedef void (*freya_irq_fn)(int source, void *arg);

/*
 * Service table handed to the program.  Fields are only ever appended,
 * and 'size' lets a program check what the running kernel provides.
 */
typedef struct freya_api {
    uint32_t size;
    uint32_t version;

    /* console */
    void     (*putc)(char c);
    void     (*puts)(const char *s);
    int      (*printf)(const char *fmt, ...);
    int      (*getc)(void);                    /* blocking, -1 if aborted  */
    int      (*getc_timeout)(uint32_t ms);     /* -1 on timeout            */
    int      (*kbhit)(void);

    /* memory */
    void    *(*malloc)(uint32_t size);
    void     (*free)(void *p);

    /* time */
    uint32_t (*ticks_ms)(void);
    void     (*delay_ms)(uint32_t ms);

    /* flow control */
    int      (*should_stop)(void);   /* non-zero once Ctrl-C was pressed  */
    void     (*yield)(void);         /* aborts the program if stopped     */
    void     (*exit)(int code);      /* never returns; code -> a status   */

    /* filesystem */
    int      (*open)(const char *path, int flags);
    int      (*close)(int fd);
    int      (*read)(int fd, void *buf, int len);
    int      (*write)(int fd, const void *buf, int len);
    int      (*seek)(int fd, int32_t off, int whence);
    int32_t  (*tell)(int fd);
    int32_t  (*fsize)(int fd);
    int      (*unlink)(const char *path);
    int      (*mkdir)(const char *path);
    int      (*opendir)(const char *path);
    int      (*readdir)(int dd, freya_stat_t *st);
    int      (*closedir)(int dd);

    /* raw board access */
    void     (*led)(int on);
    uint32_t (*cpu_hz)(void);

    /* appended: directory-entry rename (no data copy) */
    int      (*rename)(const char *old_path, const char *new_path);

    /* appended: file log on the card (datetime + message, 1 MiB rotate) */
    void     (*log)(int level, const char *fmt, ...);
    int      (*get_log_level)(void);
    int      (*set_log_level)(int level);

    /* appended: the exit status of the run before this one */
    int         (*last_exit)(freya_exit_t *st);   /* -1 if nothing ran   */
    const char *(*exit_reason_str)(int reason);

    /* appended: pins.  The console and the card own PA2..PA7; those are
     * refused with FREYA_ERR_PIN and everything else is the program's. */
    int      (*pin_mode)(int pin, int mode);      /* FREYA_PIN_*         */
    int      (*pin_read)(int pin);                /* 0 or 1              */
    int      (*pin_write)(int pin, int value);
    int      (*pin_toggle)(int pin);

    /* appended: pin interrupts.  One handler per pin number across the
     * ports, because the hardware gives PA0, PB0 and PC0 one line. */
    int      (*pin_irq_attach)(int pin, int edge, freya_irq_fn fn, void *arg);
    int      (*pin_irq_detach)(int pin);
    uint32_t (*pin_irq_count)(int pin);           /* edges since attach  */

    /* appended: hardware timers, microseconds, one interrupt per period */
    int      (*timer_open)(uint32_t period_us, int flags,
                           freya_irq_fn fn, void *arg);   /* -> a handle */
    int      (*timer_close)(int timer);
    int      (*timer_start)(int timer);
    int      (*timer_stop)(int timer);
    int      (*timer_period)(int timer, uint32_t period_us);
    uint32_t (*timer_count)(int timer);           /* expiries so far     */

    /* appended: what both of them did, and how to wait for the next one */
    uint32_t (*irq_count)(void);        /* pin and timer events this run */
    int      (*irq_wait)(uint32_t ms);  /* 0 when one arrived, -1 if not */

    /* appended: PWM on the pins a timer channel reaches.  The timers are
     * the three timer_open() hands out, so one that drives pins is not
     * one a program can also take an interrupt from, and every channel
     * of a timer shares its frequency.  A channel comes up running. */
    int      (*pwm_open)(int pin, uint32_t freq_hz, uint32_t duty);
    int      (*pwm_close)(int pwm);
    int      (*pwm_duty)(int pwm, uint32_t duty);     /* 0..FREYA_PWM_FULL */
    int      (*pwm_pulse_us)(int pwm, uint32_t us);   /* the high time     */
    int      (*pwm_freq)(int pwm, uint32_t freq_hz);  /* the whole timer   */
} freya_api_t;

/*
 * Whether the running kernel's table reaches past a given call, which is
 * how a program written against an older kernel uses one that was
 * appended later: FREYA_API_HAS(api, last_exit).
 */
#define FREYA_API_HAS(api, field)                            \
    ((api)->size >= __builtin_offsetof(freya_api_t, field) +  \
                    sizeof((api)->field))

#endif /* FREYA_API_H */
