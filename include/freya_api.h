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
 * before that region holds the auto-start flag and reserved padding.
 */
#if defined(FREYA_BOARD_BLUEPILL)
#define FREYA_APP_LOAD_ADDR    0x20001800UL     /* 20 KiB of SRAM */
#define FREYA_APP_REGION_SIZE  (8U * 1024U)
#define FREYA_AUTOSTART_ALIGN  128U
#define FREYA_AUTOSTART_ADDR   0x08009C00UL     /* page 39, 128-byte aligned */
#define FREYA_AUTOSTART_SIZE   FREYA_AUTOSTART_ALIGN
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
    void     (*exit)(int code);      /* never returns                     */

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
} freya_api_t;

#endif /* FREYA_API_H */
