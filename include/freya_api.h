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
 *
 * System settings occupy an erase unit that holds nothing else: the last
 * page of the Blue Pill's flash, after the kernel extension, sector 3 of
 * the F4, between the kernel and the program, and the page after the
 * kernel on the Black Pill 2, the STM32U585, the STM32H523 and the
 * STM32H562, and sector
 * 1 of the STM32H723.  Two copies are stored.
 * Each copy begins
 * with FREYA_SETTINGS_MAGIC and ends its named fields with a checksum of
 * every other byte in the copy.  Named fields, in order, are the
 * auto-start flag, the default log level, the ram-dump-on-BusFault flag,
 * the firmware control sum and the terminal password (eight bytes).  An
 * erased password, eight 0xFF bytes, leaves the terminal open.  The
 * remote syslog fields follow the checksum word, in bytes that the copy's
 * checksum has always covered, so a copy written before they existed is
 * still valid and reads them as erased: the on flag, the server's IPv4
 * address and its UDP port.  The
 * firmware sum covers the kernel image and the kernel extension; it lives
 * outside both, so those images do not include it.
 *
 * The area is 1 KiB: one Blue Pill page, so a settings erase never shares
 * a page with the kernel extension.  On the F4 the same 1 KiB is the head
 * of sector 3, a 16 KiB sector the kernel does not reach; erasing it takes
 * nothing else, and the program region can then run from sector 4 to the
 * sector before the kernel extension, which is the last one.
 *
 * Every supported board has at least 128 KiB of internal flash.  The Blue
 * Pill size register often still reads 64; the map runs to the end of
 * that 128 KiB anyway.
 */

/* The WeAct STM32F4 64-pin core board is an STM32F405 with the same
 * memory map; only its pins differ, and those are the kernel's business.
 * Everything that asks for the STM32F405 means it as well. */
#if defined(FREYA_BOARD_WEACT_F405) && !defined(FREYA_BOARD_STM32F405)
#define FREYA_BOARD_STM32F405 1
#endif

#define FREYA_SETTINGS_MAGIC     0x54455346UL   /* 'F','S','E','T' */
#define FREYA_SETTINGS_BLOCK     64U
#define FREYA_SETTINGS_COPIES    2U
#define FREYA_SETTINGS_DATA      (FREYA_SETTINGS_BLOCK * FREYA_SETTINGS_COPIES)
#define FREYA_SET_MARKER_OFF     0U
#define FREYA_SET_AUTOSTART_OFF  4U
#define FREYA_SET_LOGLEVEL_OFF   8U
#define FREYA_SET_RAMDUMP_OFF    12U
#define FREYA_SET_CKSUM_OFF      16U            /* firmware control sum */
#define FREYA_SET_PASSWORD_OFF   20U
#define FREYA_SET_SUM_OFF        28U            /* checksum of this copy */
#define FREYA_SET_SYSLOG_OFF     32U            /* remote syslog on flag */
#define FREYA_SET_SYSLOG_ADDR_OFF 36U           /* server IPv4, host order */
#define FREYA_SET_SYSLOG_PORT_OFF 40U           /* server UDP port */
/* Names the rest of the kernel already uses.  Offsets are within one copy. */
#define FREYA_AUTOSTART_ADDR     FREYA_SETTINGS_ADDR
#define FREYA_AUTOSTART_SIZE     FREYA_SETTINGS_SIZE
#define FREYA_LOGLEVEL_OFF       FREYA_SET_LOGLEVEL_OFF
#define FREYA_RAMDUMP_OFF        FREYA_SET_RAMDUMP_OFF
#define FREYA_CKSUM_OFF          FREYA_SET_CKSUM_OFF
#define FREYA_PASSWORD_OFF       FREYA_SET_PASSWORD_OFF
#define FREYA_PASSWORD_LEN       8U
#if defined(FREYA_BOARD_BLUEPILL)
#define FREYA_APP_LOAD_ADDR      0x20001C00UL   /* 20 KiB of SRAM */
#define FREYA_APP_REGION_SIZE    (7U * 1024U)
/* The two 1 KiB thread stacks follow the window, so a program that starts
 * no threads (FREYA_APP_F_NOTHREADS) may run on into them. */
#define FREYA_APP_NOTHREADS_SIZE (9U * 1024U)
#define FREYA_SETTINGS_SIZE      1024U          /* last page of the 128 KiB */
#define FREYA_SETTINGS_ADDR      (0x08020000UL - FREYA_SETTINGS_SIZE)
#define FREYA_APP_FLASH_ADDR     0x0800C000UL
/* The kernel extension sits above the program and stops at the settings
 * page: threads, the shell's script interpreter, its variables and
 * functions, the SPI master and XMODEM.  Neither the cipher nor the
 * virtual machine is built for this board; there was no room.
 * The program boundary leaves enough pages for that extension to grow.
 * An install does not erase it. */
#define FREYA_APP_FLASH_SIZE     (0x08012000UL - FREYA_APP_FLASH_ADDR)
#elif defined(FREYA_BOARD_BLACKPILL) || defined(FREYA_BOARD_STM32F405)
#define FREYA_APP_LOAD_ADDR      0x20010000UL   /* 128 KiB of SRAM */
#define FREYA_APP_REGION_SIZE    (56U * 1024U)
/* The thread stacks are below the window here, not after it. */
#define FREYA_APP_NOTHREADS_SIZE FREYA_APP_REGION_SIZE
/* The kernel image occupies sectors 0..2 (48 KiB).  System settings are
 * the first 1 KiB of sector 3, which holds nothing else.  The program
 * region starts at sector 4 and runs to the kernel extension, a second
 * image in the last sector, so writing the kernel or the program never
 * erases it.  The F411 has 512 KiB, eight sectors: the program is sectors
 * 4..6 (320 KiB) and the extension sector 7.  The F405 has 1 MiB, twelve
 * sectors: the program is sectors 4..10 (832 KiB) and the extension
 * sector 11. */
#define FREYA_SETTINGS_SIZE      1024U
#define FREYA_SETTINGS_ADDR      0x0800C000UL
#define FREYA_APP_FLASH_ADDR     0x08010000UL
#if defined(FREYA_BOARD_BLACKPILL)
#define FREYA_APP_FLASH_SIZE     (0x08060000UL - FREYA_APP_FLASH_ADDR)
#else
#define FREYA_APP_FLASH_SIZE     (0x080E0000UL - FREYA_APP_FLASH_ADDR)
#endif
#elif defined(FREYA_BOARD_BLACKPILL2)
#define FREYA_APP_LOAD_ADDR      0x2000C000UL   /* 96 KiB of SRAM */
#define FREYA_APP_REGION_SIZE    (40U * 1024U)
/* The thread stacks are below the window here, not after it. */
#define FREYA_APP_NOTHREADS_SIZE FREYA_APP_REGION_SIZE
/* 2 KiB pages.  The kernel image is the first 48 KiB, and system
 * settings are the first 1 KiB of the page after it, which holds nothing
 * else.  The kernel extension follows at 0x08010000 (128 KiB), so both
 * images are in the first 256 KiB, which read without wait states.  The
 * program region is the rest of the 1 MiB. */
#define FREYA_SETTINGS_SIZE      1024U
#define FREYA_SETTINGS_ADDR      0x0800C000UL
#define FREYA_APP_FLASH_ADDR     0x08030000UL
#define FREYA_APP_FLASH_SIZE     (0x08100000UL - FREYA_APP_FLASH_ADDR)
#elif defined(FREYA_BOARD_STM32U585)
#define FREYA_APP_LOAD_ADDR      0x20040000UL   /* 768 KiB of SRAM */
#define FREYA_APP_REGION_SIZE    (504U * 1024U)
/* The thread stacks are below the window here, not after it. */
#define FREYA_APP_NOTHREADS_SIZE FREYA_APP_REGION_SIZE
/* 8 KiB pages.  The kernel image is the first 48 KiB, and system
 * settings are the first 1 KiB of the page after it, which holds nothing
 * else.  The kernel extension follows at 0x08010000 (128 KiB), and the
 * program region is the rest of the 2 MiB, across both banks. */
#define FREYA_SETTINGS_SIZE      1024U
#define FREYA_SETTINGS_ADDR      0x0800C000UL
#define FREYA_APP_FLASH_ADDR     0x08030000UL
#define FREYA_APP_FLASH_SIZE     (0x08200000UL - FREYA_APP_FLASH_ADDR)
#elif defined(FREYA_BOARD_STM32H523)
#define FREYA_APP_LOAD_ADDR      0x20018000UL   /* 272 KiB of SRAM */
#define FREYA_APP_REGION_SIZE    (168U * 1024U)
/* The thread stacks are below the window here, not after it. */
#define FREYA_APP_NOTHREADS_SIZE FREYA_APP_REGION_SIZE
/* 8 KiB sectors.  The STM32U585's map in 512 KiB: the kernel, the
 * settings in the sector after it, the kernel extension at 0x08010000,
 * and the program region to the end of flash, across both banks. */
#define FREYA_SETTINGS_SIZE      1024U
#define FREYA_SETTINGS_ADDR      0x0800C000UL
#define FREYA_APP_FLASH_ADDR     0x08030000UL
#define FREYA_APP_FLASH_SIZE     (0x08080000UL - FREYA_APP_FLASH_ADDR)
#elif defined(FREYA_BOARD_STM32H562)
#define FREYA_APP_LOAD_ADDR      0x20020000UL   /* 640 KiB of SRAM */
#define FREYA_APP_REGION_SIZE    (504U * 1024U)
/* The thread stacks are below the window here, not after it. */
#define FREYA_APP_NOTHREADS_SIZE FREYA_APP_REGION_SIZE
/* The STM32H523's map in 1 MiB: the program region runs on to the end
 * of bank 2. */
#define FREYA_SETTINGS_SIZE      1024U
#define FREYA_SETTINGS_ADDR      0x0800C000UL
#define FREYA_APP_FLASH_ADDR     0x08030000UL
#define FREYA_APP_FLASH_SIZE     (0x08100000UL - FREYA_APP_FLASH_ADDR)
#elif defined(FREYA_BOARD_STM32H723)
#define FREYA_APP_LOAD_ADDR      0x24018000UL   /* 320 KiB of AXI SRAM */
#define FREYA_APP_REGION_SIZE    (216U * 1024U)
/* The thread stacks are below the window here, not after it. */
#define FREYA_APP_NOTHREADS_SIZE FREYA_APP_REGION_SIZE
/* Eight 128 KiB sectors, in the F4's order: the kernel in sector 0, the
 * system settings alone in sector 1, the program region in sectors 2..6
 * and the kernel extension in sector 7. */
#define FREYA_SETTINGS_SIZE      1024U
#define FREYA_SETTINGS_ADDR      0x08020000UL
#define FREYA_APP_FLASH_ADDR     0x08040000UL
#define FREYA_APP_FLASH_SIZE     (0x080E0000UL - FREYA_APP_FLASH_ADDR)
#elif defined(FREYA_LINUX)
/* The shell language as a Linux program (linux/board.h): no program
 * region, no program flash and no system settings. */
#else
#error "no board selected - define FREYA_BOARD_BLACKPILL, FREYA_BOARD_BLACKPILL2, FREYA_BOARD_BLUEPILL, FREYA_BOARD_STM32F405, FREYA_BOARD_WEACT_F405, FREYA_BOARD_STM32U585, FREYA_BOARD_STM32H523, FREYA_BOARD_STM32H562 or FREYA_BOARD_STM32H723"
#endif
#ifndef FREYA_LINUX
#if (FREYA_SETTINGS_ADDR % 128U) || (FREYA_SETTINGS_SIZE % 128U) || \
    (FREYA_APP_FLASH_ADDR % 128U)
#error "system settings and program flash must be 128-byte aligned"
#endif
#if (FREYA_SETTINGS_DATA > FREYA_SETTINGS_SIZE) || \
    ((FREYA_SET_SUM_OFF + 4U) > FREYA_SETTINGS_BLOCK) || \
    ((FREYA_SET_PASSWORD_OFF + FREYA_PASSWORD_LEN) > FREYA_SET_SUM_OFF) || \
    (FREYA_SET_PASSWORD_OFF < (FREYA_SET_CKSUM_OFF + 4U)) || \
    (FREYA_PASSWORD_OFF % 4U) || \
    (FREYA_SET_SYSLOG_OFF < (FREYA_SET_SUM_OFF + 4U)) || \
    ((FREYA_SET_SYSLOG_PORT_OFF + 4U) > FREYA_SETTINGS_BLOCK)
#error "system settings copies do not fit in the reserved flash"
#endif
#endif /* FREYA_LINUX */

/* header flags */
#define FREYA_APP_F_XIP        0x00000001UL   /* stored in program flash */
/* The program starts no threads, and its RAM may run on into the thread
 * stacks: FREYA_APP_NOTHREADS_SIZE instead of FREYA_APP_REGION_SIZE.
 * thread_create() reports FREYA_ERR_UNSUPPORTED to such a program.  A
 * program built with -DFREYA_APP_NOTHREADS sets it (see app_start.c). */
#define FREYA_APP_F_NOTHREADS  0x00000002UL

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
    uint32_t flags;        /* FREYA_APP_F_XIP, FREYA_APP_F_NOTHREADS  */
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

/* ---------------------------------------------------------------- ADC */
/*
 * One synchronous 12-bit conversion.  An external source is a pin;
 * temperature and the internal reference use values outside the packed
 * pin range.  Both internal sources return raw ADC counts, not engineering
 * units, because their calibration constants differ between chips.
 */
#define FREYA_ADC_MAX        4095
#define FREYA_ADC_TEMP       0x100
#define FREYA_ADC_VREF       0x101

/* ---------------------------------------------------------- heatshrink */
/*
 * LZSS as heatshrink writes it, with an 8-bit window and a 4-bit
 * lookahead: the stream of `heatshrink -e -w 8 -l 4` on a host, so a
 * file made there is one decompress() reads, and the other way round.
 * There is no header.  The caller keeps the lengths.  A byte that finds
 * no match costs nine bits, so compress() never needs more room than
 * FREYA_COMPRESS_BOUND(len).  The state is taken from the heap for the
 * duration of a call, which is why a handler is refused.
 */
#define FREYA_COMPRESS_WINDOW_BITS     8
#define FREYA_COMPRESS_LOOKAHEAD_BITS  4
#define FREYA_COMPRESS_BOUND(n)        ((n) + ((n) + 7) / 8)

/* ------------------------------------------------------- Ascon-AEAD128 */
/*
 * NIST SP 800-232.  The key is 16 bytes, the nonce is 16 and the tag
 * is 16, appended to the ciphertext.  Associated data is authenticated
 * and not encrypted; a length of zero needs no pointer.  A plaintext
 * longer than FREYA_AEAD_MAX_LEN is refused rather than split: the tag
 * covers one message, and the next message is a new nonce.  The key is
 * the caller's.  Freya does not generate one; tools/aead does, on the
 * PC.  The STM32F103 build has no cipher and returns
 * FREYA_ERR_UNSUPPORTED.
 */
#define FREYA_AEAD_KEY_LEN     16
#define FREYA_AEAD_NONCE_LEN   16
#define FREYA_AEAD_TAG_LEN     16
#define FREYA_AEAD_MAX_LEN     4096

/*
 * What the pin, timer, PWM, I2C, 1-Wire, SPI, ADC, crypt, compress, aead
 * and interrupt calls return.
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
#define FREYA_ERR_TIMEOUT    -6   /* an I2C, SPI or ADC operation did not
                                   * finish, or a 1-Wire line stayed low */
#define FREYA_ERR_IO         -7   /* a bus error, or the run was asked
                                   * to stop mid-transfer                */
#define FREYA_ERR_AGAIN      -8   /* nonblocking operation is not ready */
#define FREYA_ERR_UNSUPPORTED -9  /* feature is absent on this board    */

/* ------------------------------------------------------------ network */
#define FREYA_NET_SOCKETS       4
#define FREYA_NET_PAYLOAD_MAX   480
#define FREYA_WEB_GET           1
#define FREYA_WEB_HEAD          2
#define FREYA_WEB_POST          3
#define FREYA_WEB_PATH          96
#define FREYA_WEB_QUERY         31
#define FREYA_WEB_TYPE          40
#define FREYA_WEB_CHUNK         400
#define FREYA_WEB_READ_MAX      480         /* one web_read() at most     */
#define FREYA_WEB_BODY_MAX      (1024UL * 1024UL) /* a POST body, bytes   */

typedef struct {
    int32_t method;                     /* FREYA_WEB_GET, _HEAD or _POST   */
    char    path[FREYA_WEB_PATH + 1];   /* URL path, leading slash         */
    char    query[FREYA_WEB_QUERY + 1]; /* raw query, or empty             */
} freya_web_req_t;
#define FREYA_WIFI_SSID_MAX     32
#define FREYA_WIFI_PASS_MAX     63
#define FREYA_AF_INET           2
#define FREYA_SOCK_STREAM       1
#define FREYA_SOCK_DGRAM        2
#define FREYA_IPPROTO_TCP       6
#define FREYA_IPPROTO_UDP       17

enum {
    FREYA_WIFI_OFF = 0,
    FREYA_WIFI_IDLE,
    FREYA_WIFI_CONNECTING,
    FREYA_WIFI_CONNECTED,
    FREYA_WIFI_ERROR
};

typedef struct {
    uint32_t addr;      /* IPv4, first octet in the top byte:
                         * 192.168.1.2 is 0xC0A80102 */
    uint16_t port;      /* host byte order             */
    uint16_t reserved;
} freya_net_addr_t;

typedef struct {
    int32_t  state;     /* FREYA_WIFI_* */
    int32_t  rssi;
    uint32_t ip;
    uint32_t gateway;
    uint32_t netmask;
    char     ssid[FREYA_WIFI_SSID_MAX + 1];
} freya_wifi_status_t;

typedef struct {
    int32_t rssi;
    uint8_t channel;
    uint8_t auth;
    uint8_t reserved[2];
    char    ssid[FREYA_WIFI_SSID_MAX + 1];
} freya_wifi_scan_t;

typedef struct {
    uint32_t addr;
    uint32_t elapsed_ms;
    uint32_t replies;
    uint32_t lost;
} freya_ping_result_t;

/*
 * A pin or timer handler.  It runs in interrupt context, on the same
 * stack as everything else, with 'source' set to the pin or the timer
 * that called it and 'arg' to whatever was registered beside it.
 *
 * What a handler may do is decided by what it can preempt.  Console
 * output, the LED, ticks_ms(), the pin calls, the timer calls,
 * crypt(), aead_encrypt() and aead_decrypt() are all safe.  malloc(),
 * free(), the filesystem, power(),
 * compress(), decompress() and
 * the I2C, SPI, 1-Wire and ADC calls are not - they can be interrupted
 * halfway through their own bookkeeping, or they spin on a bus - so the
 * kernel refuses them from a handler
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

/* A supply the kernel can take away.  FREYA_PWR_SD is the card socket. */
#define FREYA_PWR_SD         1

/* -------------------------------------------------------------- audio */
/*
 * A USB headset (USB Audio Class 1) on the board's USB port, in a kernel
 * built with AUDIO=1.  The program sees mono 16-bit samples at its own
 * rate, 16000 (wideband) or 8000 (narrowband), whatever the headset
 * runs at: a headset at 2, 3 or 6 times that rate is resampled, and a
 * stereo one is mixed down and fed both channels.  Each direction has a
 * ring of FREYA_AUDIO_RING samples (64 ms at 16 kHz), filled and drained
 * by the USB interrupt once a millisecond.  See docs/audio.md.
 */
#define FREYA_AUDIO_MIC      1      /* audio_open(): the microphone      */
#define FREYA_AUDIO_SPK      2      /* audio_open(): the speaker         */
#define FREYA_AUDIO_RATE     16000  /* the default rate                  */
#define FREYA_AUDIO_RING     1024   /* samples in each direction         */
#define FREYA_AUDIO_UNITY    256    /* audio_gain(): x1                  */
#define FREYA_AUDIO_GAIN_MAX 1024   /* audio_gain(): x4, +12 dB          */

typedef struct {
    uint32_t rate;          /* the program's rate, 8000 or 16000        */
    uint32_t mic_rate;      /* the headset's, 0 when not capturing      */
    uint32_t spk_rate;      /* the headset's, 0 when not playing        */
    uint8_t  mic_channels;
    uint8_t  spk_channels;
    uint8_t  dirs;          /* FREYA_AUDIO_* that are open              */
    uint8_t  connected;     /* a headset is attached                    */
    uint16_t mic_avail;     /* samples audio_read() can return now      */
    uint16_t spk_queued;    /* written, not yet played                  */
    uint16_t spk_free;      /* what audio_write() can take now          */
    uint16_t mic_gain;      /* FREYA_AUDIO_UNITY = x1                   */
    uint16_t spk_gain;
    uint32_t frames;        /* 1 ms USB frames since audio_open()       */
    uint32_t underruns;     /* the speaker ran dry and played silence   */
    uint32_t overruns;      /* microphone samples dropped: ring full    */
    uint32_t errors;        /* isochronous packets lost or late         */
    char     name[32];      /* the headset's product string             */
} freya_audio_status_t;

/* ---------------------------------------------------- PDP-11, 32-bit */
/*
 * Eight general registers, as on a PDP-11.  R6 is the stack and R7 the
 * program counter.  Each register is 32 bits, and a memory word is 32
 * bits too; the opcodes and the condition codes are the PDP-11's.
 * Instructions are 32-bit little-endian words whose low 16 bits are the
 * PDP-11 opcode.  A following index or immediate is a whole 32-bit word.
 * The stack and the program counter step by 4.  R0-R5 step by 1 on a
 * byte operand and by 4 on a word.  MOVB into a register sign-extends
 * the byte through it, as on a PDP-11; every other byte instruction
 * leaves the rest of a register alone.
 *
 * vm_step() and vm_run() return 0 when the instruction completed,
 * FREYA_VM_HALT when it executed HALT, FREYA_VM_TRAP for EMT, TRAP, BPT
 * or IOT, FREYA_VM_FAULT when an address is outside mem or a word is
 * not aligned, FREYA_VM_ILLEGAL for an opcode this machine does not
 * have, or FREYA_ERR_ARG.  vm_run() returns FREYA_VM_LIMIT when steps
 * (or FREYA_VM_MAX_STEPS, when steps is 0) run out first.  An illegal
 * opcode leaves R7 on the instruction itself; HALT and a trap leave it
 * on the next one.
 */
#define FREYA_VM_NREGS       8
#define FREYA_VM_SP          6
#define FREYA_VM_PC          7
#define FREYA_VM_C           0x1u
#define FREYA_VM_V           0x2u
#define FREYA_VM_Z           0x4u
#define FREYA_VM_N           0x8u
#define FREYA_VM_HALT        1
#define FREYA_VM_TRAP        2
#define FREYA_VM_FAULT       3
#define FREYA_VM_ILLEGAL     4
#define FREYA_VM_LIMIT       5
#define FREYA_VM_MAX_STEPS   1000000u

typedef struct {
    uint32_t r[FREYA_VM_NREGS];
    uint32_t psw;            /* FREYA_VM_N | Z | V | C                    */
} freya_vm_t;

/*
 * Civil time, as rtc_get() and rtc_set() carry it.  mon is 1..12, day
 * is 1..31 and hour is 0..23.  The year runs from FREYA_RTC_MIN_YEAR,
 * where a FAT timestamp starts, to FREYA_RTC_MAX_YEAR, which is as far
 * as a DS3231 counts.
 */
#define FREYA_RTC_MIN_YEAR   1980
#define FREYA_RTC_MAX_YEAR   2199

typedef struct {
    uint16_t year;
    uint8_t  mon, day, hour, min, sec;
} freya_rtc_t;

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

    /* appended: pins.  The console owns PA2 and PA3.  The card owns
     * PA4..PA7 and PA8, which switches its supply.  Those are refused
     * with FREYA_ERR_PIN and everything else is the program's. */
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
     * id.  None of these may be called from a pin or timer handler.
     * A program marked FREYA_APP_F_NOTHREADS gets FREYA_ERR_UNSUPPORTED
     * from thread_create(). */
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

    /* appended: once XTEA in CTR mode.  The cipher has been removed
     * from every board; the slot keeps its place so the calls after it
     * do not move, and it always returns FREYA_ERR_UNSUPPORTED.  Ascon
     * (aead_encrypt, aead_decrypt) is the cipher a program has. */
    int      (*crypt)(const void *key, const void *nonce, uint32_t off,
                      const void *in, void *out, int len);

    /* appended: a raw console.  With on non-zero, Ctrl-C is no longer
     * the kernel's: it arrives through getc() as 0x03 like any other key,
     * and the program is the only way out of the run until it turns raw
     * mode off again, returns or exits.  The kernel turns it off when the
     * run ends however it ends.  Returns the previous setting. */
    int      (*console_raw)(int on);

    /* appended: board power.  FREYA_PWR_SD is the card socket.  on is
     * 0 or 1.  The call returns the state it found, so that state can
     * be put back, or FREYA_ERR_*.  Off closes every open file,
     * unmounts, releases the SPI pins and drops VDD through the
     * board's switch.  On brings VDD back and waits for the rail; the
     * card stays unidentified until the next mount.  A pin or timer
     * handler is refused.  A kernel built without SD=1 has no card
     * driver and returns FREYA_ERR_UNSUPPORTED. */
    int      (*power)(int domain, int on);

    /* appended: one polled, 12-bit ADC conversion.  source is an
     * ADC-capable pin, FREYA_ADC_TEMP or FREYA_ADC_VREF.  An external
     * pin is put in analog mode and left there. */
    int      (*adc_read)(int source);              /* 0..FREYA_ADC_MAX */

    /* appended: a PDP-11, with its registers and opcodes, on a 32-bit
     * data path.  R6 is the stack pointer and R7 the program counter.
     * A word is 32 bits; a byte is still 8.  The processor status keeps
     * N, Z, V and C in the same bits as a PDP-11.  mem is the machine's
     * whole address space, little-endian.  vm_step() runs one
     * instruction.  vm_run() runs up to steps of them, or
     * FREYA_VM_MAX_STEPS when steps is 0, and writes how many completed
     * to ran when ran is not null. */
    int      (*vm_reset)(freya_vm_t *vm);
    int      (*vm_step)(freya_vm_t *vm, void *mem, uint32_t size);
    int      (*vm_run)(freya_vm_t *vm, void *mem, uint32_t size,
                       uint32_t steps, uint32_t *ran);

    /* appended: ESP32-C6 network coprocessor.  Every operation is
     * nonblocking; FREYA_ERR_AGAIN means call net_poll() and retry.
     * Credentials are persisted by the C6 and are never readable back. */
    int      (*wifi_on)(void);
    int      (*wifi_off)(void);
    int      (*wifi_credentials)(const char *ssid, const char *password);
    int      (*wifi_connect)(void);
    int      (*wifi_disconnect)(void);
    int      (*wifi_status)(freya_wifi_status_t *status);
    int      (*wifi_scan_start)(void);
    int      (*wifi_scan_next)(freya_wifi_scan_t *entry);
    int      (*ping_start)(const char *host, uint32_t timeout_ms);
    int      (*ping_result)(freya_ping_result_t *result);
    int      (*net_socket)(int domain, int type, int protocol);
    int      (*net_close)(int socket);
    int      (*net_connect)(int socket, const freya_net_addr_t *addr);
    int      (*net_bind)(int socket, const freya_net_addr_t *addr);
    int      (*net_listen)(int socket, int backlog);
    int      (*net_accept)(int socket, freya_net_addr_t *peer);
    int      (*net_send)(int socket, const void *buf, int len);
    int      (*net_recv)(int socket, void *buf, int len);
    int      (*net_sendto)(int socket, const void *buf, int len,
                           const freya_net_addr_t *to);
    int      (*net_recvfrom)(int socket, void *buf, int len,
                             freya_net_addr_t *from);
    int      (*net_poll)(uint32_t timeout_ms);

    /* appended: upgrade an unconnected stream socket to a TLS client and
     * connect it.  TLS terminates on the ESP32-C6; this side sends and
     * receives plaintext.  The C6 verifies hostname and certificate and
     * permits TLS 1.3 only. */
    int      (*net_tls_connect)(int socket, const char *hostname,
                                uint16_t port);

    /* appended: HTTPS file service.  The ESP32-C6 accepts one TLS 1.3
     * connection on port 443 and parses the request.  web_take() returns
     * 0 when a GET, HEAD or POST is waiting, or FREYA_ERR_AGAIN when it
     * is not.  web_begin/web_body/web_end send the response the C6
     * writes back.  There is no cleartext listener. */
    int      (*web_take)(freya_web_req_t *req);
    int      (*web_begin)(int status, const char *type, uint32_t length);
    int      (*web_body)(const void *data, int len);
    int      (*web_end)(void);

    /* appended: run a shell script from the filesystem and capture what
     * it prints.  method and query are published as $method and $query
     * (query is at most FREYA_WEB_QUERY characters).  *out_len is the
     * captured byte count.  The return is the script status, 0 when it
     * finished, or a negative FREYA_ERR_* when it was not run.  The
     * interpreter keeps its state in the thread stacks, so a program
     * whose RAM runs on into them gets FREYA_ERR_BUSY. */
    int      (*shell_source_capture)(const char *path, const char *method,
                                     const char *query, char *buf, int cap,
                                     int *out_len);

    /* appended: factory system settings.  settings_block() writes the
     * default binary copy — marker, erased fields, checksum — and
     * returns how many bytes that copy is, or FREYA_ERR_ARG when the
     * buffer is too small.  settings_area_size() is the flash reserved
     * at the end of internal flash for both copies. */
    int      (*settings_block)(void *buf, int len);
    uint32_t (*settings_area_size)(void);

    /* appended: the body of a POST.  After web_take() has reported
     * FREYA_WEB_POST, web_read() copies up to max bytes of the body,
     * at most FREYA_WEB_READ_MAX at a time, and returns how many: 0 once
     * the body has all been read, FREYA_ERR_AGAIN while the next piece
     * is still crossing the link (poll and call again with the same
     * arguments), or a FREYA_ERR_*.  *left, when not null, receives the
     * bytes still to come after this call.  The C6 refuses a body over
     * FREYA_WEB_BODY_MAX before the request reaches web_take().  The
     * response is sent with web_begin() as for a GET; a body that was
     * not read to the end is dropped by the C6. */
    int      (*web_read)(void *data, int max, uint32_t *left);

    /* appended: heatshrink.  compress() writes the LZSS stream of the
     * in_len bytes at in into out and returns its length.  decompress()
     * reads such a stream back and returns how many bytes it produced.
     * in and out may not overlap.  A length of zero returns zero and
     * needs no pointers.  FREYA_ERR_ARG is a bad pointer or length, or
     * an out too small for the result - compress() always fits in
     * FREYA_COMPRESS_BOUND(in_len).  FREYA_ERR_IO is a stream the
     * decoder cannot finish.  FREYA_ERR_BUSY is a heap that cannot hold
     * the state, FREYA_ERR_HANDLER a call from a pin or timer handler,
     * and FREYA_ERR_UNSUPPORTED a board built without the code. */
    int      (*compress)(const void *in, int in_len, void *out, int out_cap);
    int      (*decompress)(const void *in, int in_len, void *out, int out_cap);

    /* appended: Ascon-AEAD128.  aead_encrypt() writes the ciphertext of
     * the in_len bytes at in, then the 16-byte tag, and returns that
     * length.  aead_decrypt() checks the tag and returns the plaintext
     * length; in_len counts the tag.  ad is associated data, or a zero
     * length and a null pointer when there is none.  in and out may not
     * overlap, and neither may ad and out.  A length of zero is a
     * message: encryption returns the tag alone.  FREYA_ERR_ARG is a
     * bad pointer or length, an overlap, or an out too small - the
     * ciphertext always fits in in_len + FREYA_AEAD_TAG_LEN.
     * FREYA_ERR_IO is a tag that does not match, and out is then zeros.
     * FREYA_ERR_UNSUPPORTED is a board built without the code.  The key
     * is not generated here. */
    int      (*aead_encrypt)(const void *key, const void *nonce,
                             const void *ad, int ad_len,
                             const void *in, int in_len,
                             void *out, int out_cap);
    int      (*aead_decrypt)(const void *key, const void *nonce,
                             const void *ad, int ad_len,
                             const void *in, int in_len,
                             void *out, int out_cap);

    /* appended: the civil clock.  Freya counts it from SysTick, so it
     * starts at the epoch below after a reset and the date command, or
     * a program, is what puts the real time into it.  A board built
     * with RTC=ds3231 reads the battery-backed chip into that count at
     * boot, and rtc_set() writes the chip as well as the count, so the
     * time survives the next reset.  Without the chip rtc_set() only
     * moves the count and returns 0 all the same.
     *
     * rtc_get() fills *t and returns 0, or FREYA_ERR_ARG for a null
     * pointer.  Until something sets the clock a board believes it is
     * 2026-01-01 00:00:00.  rtc_set() returns 0, or FREYA_ERR_ARG when
     * the fields are out of range, in which case it changes nothing.
     * The range is FREYA_RTC_MIN_YEAR..FREYA_RTC_MAX_YEAR, mon 1..12,
     * day 1..31, hour 0..23.  A day past the end of its month is taken
     * as it falls and the count carries it into the next one, except
     * on a board with the chip, which refuses it; that board also
     * stores 2000 and later only, so an earlier year is out of range
     * there.  A chip that fails for any other reason still leaves the
     * software count set, since a board built with the driver and no
     * chip on the pins has to be able to set its clock.  rtc_set() in
     * a pin or timer handler is FREYA_ERR_HANDLER: writing the chip
     * needs the I2C bus, which cannot be taken in interrupt context.
     * Both are FREYA_ERR_UNSUPPORTED on a board whose kernel had no
     * room for them, which is the Blue Pill; its clock still keeps
     * file timestamps and still answers the date command. */
    int      (*rtc_get)(freya_rtc_t *t);
    int      (*rtc_set)(const freya_rtc_t *t);

    /* appended: flash that outlives the run.  A running program cannot
     * write flash, so both calls only ask, return 0, and the kernel does
     * it once the run has ended, however it ended, and prints what it
     * did.  The program is unloaded then.
     *
     * flash_text_save() keeps the len bytes at text after the image of
     * the program in flash, and the boot then starts that program as
     * 'program -e TEXT'; basic11 saves its BASIC program this way.  A
     * len of 0 removes the text.  The text is read when the run ends,
     * so it has to stay where it is until then.  FREYA_ERR_UNSUPPORTED
     * is a program that was not started from flash; FREYA_ERR_ARG is a
     * text that is not printable ASCII, tab, CR and LF, or one too long
     * for the room after the image.  On the Blue Pill that room is what
     * is left of the image's last page, under 1 KiB after basic11.
     *
     * autostart_set() turns the auto-start flag on or off, as the
     * shell's autostart command does. */
    int      (*flash_text_save)(const char *text, int len);
    int      (*autostart_set)(int on);

    /* appended: audio on a USB headset.  audio_open() starts the
     * directions in dirs (FREYA_AUDIO_MIC, FREYA_AUDIO_SPK or both) at
     * rate, 8000 or 16000, or 0 for FREYA_AUDIO_RATE.  It returns 0;
     * FREYA_ERR_ARG for another rate or no direction; FREYA_ERR_BUSY when
     * audio is already open; FREYA_ERR_IO when there is no headset or it
     * does not answer; FREYA_ERR_UNSUPPORTED for a kernel without AUDIO=1
     * or a headset with no 16-bit format at a rate it can be resampled
     * from.  The kernel closes it when the run ends.
     *
     * audio_read() copies up to count microphone samples and returns how
     * many; audio_write() queues up to count speaker samples and returns
     * how many it took.  Neither waits: 0 means try again in a
     * millisecond or two.  The speaker starts once 20 ms are queued, and
     * plays silence, counting an underrun, when the queue runs dry; it
     * starts again after the next 20 ms.  Both return FREYA_ERR_IO once
     * the headset is unplugged, FREYA_ERR_ARG when that direction is not
     * open.  Both may be called from a pin or timer handler.
     *
     * audio_status() fills *st.  audio_gain() scales the directions in
     * dirs by gain / FREYA_AUDIO_UNITY, 0 (mute) to FREYA_AUDIO_GAIN_MAX,
     * in software, with saturation; the headset's own volume is left
     * where it was. */
    int      (*audio_open)(uint32_t rate, int dirs);
    int      (*audio_close)(void);
    int      (*audio_read)(int16_t *buf, int count);
    int      (*audio_write)(const int16_t *buf, int count);
    int      (*audio_status)(freya_audio_status_t *st);
    int      (*audio_gain)(int dirs, int gain);

    /* appended: look a host name up through the ESP32-C6's resolver.
     * Nonblocking like every network call: FREYA_ERR_AGAIN until the
     * lookup ends (poll and call again with the same name), then 0 with
     * *addr set, in the byte order of freya_net_addr_t.addr, or
     * FREYA_ERR_IO for a name that has no IPv4 address.  The C6 runs one
     * lookup at a time.  net_tls_connect() looks its host up itself; this
     * is for net_connect() and net_sendto().  The HTTP client library
     * (http/freya_http.h) uses it for http:// URLs. */
    int      (*net_resolve)(const char *host, uint32_t *addr);
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
