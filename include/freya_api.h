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
 * One program runs at a time.  It may start threads; they share that run
 * and are gone when the run ends.  Pressing Ctrl-C on the console, calling
 * api->exit(), or returning from app_main() hands control back to the
 * Freya shell and stops every thread of the run.
 */
#ifndef FREYA_API_H
#define FREYA_API_H

#include <stdint.h>

#define FREYA_APP_MAGIC        0x41595246UL   /* 'F','R','Y','A' */
#define FREYA_ABI_VERSION      3

/*
 * ABI 1 described only RAM images.  ABI 2 appends four fields for a
 * program that executes from internal flash, and because the v1 header is
 * a byte for byte prefix of the v2 header the loader still accepts a v1
 * RAM image - every hello.bin already sitting on a card keeps working.
 * ABI 3 appends the location and size of a relocation table.  An installed
 * program is copied to RAM before it runs; each table entry names a word
 * containing a program address that must move with it.
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
 * program is stored there and copied to RAM before it executes when code
 * and writable state fit together; larger programs retain XIP execution.
 * A 128-byte aligned slot immediately before that region holds the
 * auto-start flag (first word), the default log level (second word) and
 * the ram-dump-on-BusFault flag (third word).
 *
 * Every supported board has at least 128 KiB of internal flash.  The Blue
 * Pill size register often still reads 64; the program region runs to the
 * end of that 128 KiB anyway.
 */
#if defined(FREYA_BOARD_BLUEPILL)
#define FREYA_APP_LOAD_ADDR    0x20001800UL     /* 20 KiB of SRAM */
#define FREYA_APP_REGION_SIZE  (8U * 1024U)
#define FREYA_AUTOSTART_ALIGN  128U
#define FREYA_AUTOSTART_ADDR   0x0800C000UL     /* page 48, 128-byte aligned */
#define FREYA_AUTOSTART_SIZE   FREYA_AUTOSTART_ALIGN
#define FREYA_LOGLEVEL_OFF     4U               /* second word of that slot */
#define FREYA_RAMDUMP_OFF      8U               /* third word of that slot  */
#define FREYA_APP_FLASH_ADDR   (FREYA_AUTOSTART_ADDR + FREYA_AUTOSTART_SIZE)
/* The last 10 KiB of the 128 KiB holds the kernel extension (threads,
 * the shell's script interpreter, the SPI master and the cipher), so
 * an install does not erase it. */
#define FREYA_APP_FLASH_SIZE   (0x0801D800UL - FREYA_APP_FLASH_ADDR)
#if (FREYA_AUTOSTART_ADDR % FREYA_AUTOSTART_ALIGN) || \
    (FREYA_AUTOSTART_SIZE % FREYA_AUTOSTART_ALIGN) || \
    (FREYA_APP_FLASH_ADDR % FREYA_AUTOSTART_ALIGN)
#error "Blue Pill auto-start slot and program flash must be 128-byte aligned"
#endif
#elif defined(FREYA_BOARD_BLACKPILL)
#define FREYA_APP_LOAD_ADDR    0x20010000UL     /* 128 KiB of SRAM */
#define FREYA_APP_REGION_SIZE  (56U * 1024U)
/* Kernel image occupies sectors 0..2 (48 KiB).  Sector 3 is unused except
 * for the 128-byte auto-start slot at its end, so an autostart erase never
 * shares a sector with the kernel or with the program.  Sector 4 is the
 * program.  The thread scheduler is a second image at the start of sector 5,
 * so writing the kernel does not erase it. */
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
#define FREYA_APP_F_XIP        0x00000001UL   /* stored in program flash */

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
    /* ------------------------------------- appended in ABI 3 ------- */
    uint32_t reloc_offset; /* image offset of uint32_t pointer offsets */
    uint32_t reloc_count;  /* number of entries in that table          */
} freya_app_header_t;

/* Size of the ABI 1 header, which is a prefix of the one above.  The
 * loader reads this much first so that it never mistakes the first bytes
 * of an old image's .text for the appended fields. */
#define FREYA_APP_HDR_V1_SIZE  48
#define FREYA_APP_HDR_V2_SIZE  64

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

/* --------------------------------------------------------------- I2C */
/*
 * Speeds the master will run at, or a little slower when a microsecond
 * is too coarse to hit the one that was asked for.  Below the floor a
 * transfer is all timeout; above the ceiling is Fast-mode Plus, which
 * these pins are not set up for.
 * A transfer longer than FREYA_I2C_MAX_LEN is refused rather than split,
 * because 255 is a whole SMBus block and the usual EEPROM page.
 */
#define FREYA_I2C_MIN_HZ     10000UL
#define FREYA_I2C_MAX_HZ     400000UL
#define FREYA_I2C_MAX_LEN    255

/* ------------------------------------------------------------- 1-Wire */
/*
 * Standard speed on one open-drain pin.  A program names the pin; up
 * to FREYA_W1_BUSES of them may be open at once.  A transfer longer
 * than FREYA_W1_MAX_LEN is refused rather than split.  A ROM is eight
 * bytes, family code first.  There is no overdrive: those slots are
 * shorter than a one-microsecond delay can promise.
 */
#define FREYA_W1_BUSES       4
#define FREYA_W1_ROM_LEN     8
#define FREYA_W1_MAX_LEN     64

/* ---------------------------------------------------------------- SPI */
/*
 * Master, 8-bit, MSB first.  Mode is CPOL and CPHA: mode 0 is the usual
 * one, clock idle low and sampled on the rising edge.  The clock is the
 * fastest power-of-two division of the bus clock that does not exceed
 * the rate asked for, so a program gets that rate or a slower one.
 * FREYA_SPI_MIN_HZ is the slowest tap on the faster board; a slower
 * board asked for it runs slower still.  FREYA_SPI_MAX_HZ is the fastest
 * tap on that same board; a slower board asked for it runs at its own
 * PCLK/2.  A transfer longer than FREYA_SPI_MAX_LEN is refused rather
 * than split.  Chip select is not part of the bus: the program drives
 * that pin itself.
 */
#define FREYA_SPI_MODE0      0
#define FREYA_SPI_MODE1      1
#define FREYA_SPI_MODE2      2
#define FREYA_SPI_MODE3      3
#define FREYA_SPI_MIN_HZ     187500UL
#define FREYA_SPI_MAX_HZ     24000000UL
#define FREYA_SPI_MAX_LEN    4096

/* --------------------------------------------------------------- XTEA */
/*
 * 32 rounds, a 16-byte key and an 8-byte block, used in CTR mode.  A
 * block is two big-endian words, which is the order the bytes have in
 * hex.  The nonce is the counter at byte 0, as a big-endian 64-bit
 * number, and each following block adds one.  A length past
 * FREYA_CRYPT_MAX_LEN is refused rather than split; the next piece is
 * the same call with off advanced by what was already done.  The same
 * call encrypts and decrypts.
 */
#define FREYA_CRYPT_ROUNDS     32
#define FREYA_CRYPT_KEY_LEN    16
#define FREYA_CRYPT_NONCE_LEN  8
#define FREYA_CRYPT_BLOCK      8
#define FREYA_CRYPT_MAX_LEN    4096

/*
 * What the pin, timer, PWM, I2C, 1-Wire, SPI, crypt and interrupt calls
 * return.
 * Anything else they hand back is the value asked for: a pin level, a
 * handle, a count.
 */
#define FREYA_ERR_PIN        -1   /* no such pin, one the kernel owns, or
                                   * one with no PWM channel behind it   */
#define FREYA_ERR_BUSY       -2   /* that line is taken, every timer is,
                                   * or the timer runs at another rate   */
#define FREYA_ERR_ARG        -3   /* mode, edge, period, frequency,
                                   * duty cycle, bus or length out of
                                   * range                               */
#define FREYA_ERR_HANDLER    -4   /* not allowed from a handler          */
#define FREYA_ERR_NACK       -5   /* an I2C address or byte was not
                                   * acknowledged, or no 1-Wire device
                                   * pulled the line down                */
#define FREYA_ERR_TIMEOUT    -6   /* an I2C or SPI transfer did not
                                   * finish, or a 1-Wire line stayed low */
#define FREYA_ERR_IO         -7   /* a bus error, or the run was asked
                                   * to stop mid-transfer                */

/*
 * A pin or timer handler.  It runs in interrupt context, on the same
 * stack as everything else, with 'source' set to the pin or the timer
 * that called it and 'arg' to whatever was registered beside it.
 *
 * What a handler may do is decided by what it can preempt.  Console
 * output, the LED, ticks_ms(), the pin calls, the timer calls and
 * crypt() are all safe.  malloc(), free(), the filesystem and the I2C,
 * SPI and 1-Wire calls are not - they can be interrupted halfway
 * through their own bookkeeping, or they spin on a bus - so the kernel
 * refuses them from a handler
 * instead of letting a program corrupt the heap or the card.  A handler
 * that faults, or one that never returns, is killed and ends the run
 * the way a fault in the program would; it
 * does not take Freya down with it.
 *
 * A handler is optional.  Attached as NULL, the interrupt is still
 * counted, and the program reads pin_irq_count() / timer_count() or
 * sleeps in irq_wait() from thread mode, where it may do anything.
 */
typedef void (*freya_irq_fn)(int source, void *arg);

/* ----------------------------------------------------------- threads */
/*
 * A thread is a name, a priority and a function.  The name is not
 * optional: the console stops a thread by that name, so an empty one is
 * refused.  Priorities are small integers, FREYA_PRIO_MIN up to
 * FREYA_PRIO_MAX, and a larger number runs ahead of a smaller one.
 * Equal priorities take turns.
 *
 * The program's own app_main() is a thread too, at FREYA_PRIO_NORMAL,
 * for as long as the run lasts.  Threads a program creates die with the
 * run — when app_main() returns, exit() is called, or the run is stopped.
 * Each of those has FREYA_THREAD_STACK bytes of stack.  The call is
 * refused with FREYA_ERR_BUSY when the board has no room for another one.
 */
#define FREYA_THREAD_NAME_MAX  16
#define FREYA_THREAD_STACK     1024U
#define FREYA_PRIO_MIN         0
#define FREYA_PRIO_MAX         7
#define FREYA_PRIO_NORMAL      1

typedef void (*freya_thread_fn)(void *arg);

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

    /* appended: I2C master.  'bus' is 1 for the first bus the board
     * lists.  Addresses are 7-bit.  A length of zero writes the
     * address and stops, which is a scan.  i2c_transfer() with both
     * buffers is the write-then-read of a device register: the repeated
     * start between them is the kernel's, so nothing else can own the
     * bus in the middle.  Opening an open bus sets a new speed. */
    int      (*i2c_open)(int bus, uint32_t hz);       /* 0, or FREYA_ERR_* */
    int      (*i2c_close)(int bus);
    int      (*i2c_write)(int bus, int addr, const void *buf, int len);
    int      (*i2c_read)(int bus, int addr, void *buf, int len);
    int      (*i2c_transfer)(int bus, int addr, const void *tx, int txlen,
                             void *rx, int rxlen);

    /* appended: 1-Wire master, standard speed, on a pin the program
     * names.  The line is open drain and needs a pull-up to 3.3 V.
     * w1_reset() is the presence pulse: 0 when a device answers,
     * FREYA_ERR_NACK when the line rose and nobody pulled it down.
     * w1_search() writes the next 8-byte ROM and returns 0, then
     * FREYA_ERR_NACK when the walk is finished; the call after that
     * starts over.  w1_pullup() drives the pin high so a
     * parasite-powered device can draw current, and the next reset,
     * read, write or search releases it.  w1_crc() is the CRC-8 over
     * len bytes; a ROM or a scratchpad that includes its own CRC byte
     * comes out 0.  Opening a pin this side already has open does
     * nothing.  A bus a program opened is closed when the run ends. */
    int      (*w1_open)(int pin);
    int      (*w1_close)(int pin);
    int      (*w1_reset)(int pin);
    int      (*w1_write)(int pin, const void *buf, int len);
    int      (*w1_read)(int pin, void *buf, int len);
    int      (*w1_search)(int pin, void *rom);
    int      (*w1_pullup)(int pin, int on);
    int      (*w1_crc)(const void *buf, int len);

    /* appended: threads.  thread_create() returns a thread id, or a
     * FREYA_ERR_* .  thread_exit() does not return; called from the
     * program's main thread it ends the run the way return would.
     * thread_sleep() returns 0, -1 if the run was asked to stop, or
     * FREYA_ERR_HANDLER from a handler.  thread_self() is the caller's
     * id.  None of these may be called from a pin or timer handler. */
    int      (*thread_create)(const char *name, int priority,
                              freya_thread_fn fn, void *arg);
    void     (*thread_exit)(void);
    void     (*thread_yield)(void);
    int      (*thread_sleep)(uint32_t ms);
    int      (*thread_self)(void);

    /* appended: SPI master, 8-bit, MSB first.  'bus' is 1 for the first
     * bus the board lists.  mode is FREYA_SPI_MODE0..3.  The clock is
     * the fastest power-of-two division of the bus clock that does not
     * exceed hz, so the bus runs at that rate or slower.  A transfer
     * shifts one byte out for each byte in; spi_read() clocks out 0xFF
     * and spi_write() discards what came back.  Chip select is a pin
     * the program drives around the call.  Opening an open bus, by the
     * same side, programs a new speed and mode.  A bus a program opened
     * is closed when the run ends. */
    int      (*spi_open)(int bus, uint32_t hz, int mode); /* 0, or FREYA_ERR_* */
    int      (*spi_close)(int bus);
    int      (*spi_transfer)(int bus, const void *tx, void *rx, int len);
    int      (*spi_write)(int bus, const void *buf, int len);
    int      (*spi_read)(int bus, void *buf, int len);

    /* appended: XTEA in CTR mode.  The key is FREYA_CRYPT_KEY_LEN
     * bytes and the nonce is FREYA_CRYPT_NONCE_LEN.  off is the index
     * of the first byte of this piece, so a longer message is split by
     * the caller and the counter does not restart.  The same call
     * encrypts and decrypts.  in and out may be the same buffer.
     * Nothing is kept between calls, so a handler may use this. */
    int      (*crypt)(const void *key, const void *nonce, uint32_t off,
                      const void *in, void *out, int len);

    /* appended: a raw console.  With on non-zero, Ctrl-C is no longer
     * the kernel's: it arrives through getc() as 0x03 like any other key,
     * and the program is the only way out of the run until it turns raw
     * mode off again, returns or exits.  The kernel turns it off when the
     * run ends however it ends.  Returns the previous setting. */
    int      (*console_raw)(int on);
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
