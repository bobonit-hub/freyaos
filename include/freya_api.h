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
#define FREYA_ABI_VERSION      1

/*
 * Where a program lives.  This is the one part of the ABI that depends on
 * the board, because it depends on how much RAM there is; the Makefile
 * defines FREYA_BOARD_* for the kernel and for every program it builds, so
 * the two always agree.  The loader checks the header against these values
 * and refuses an image linked for a different region.
 */
#if defined(FREYA_BOARD_BLUEPILL)
#define FREYA_APP_LOAD_ADDR    0x20001800UL     /* 20 KiB of SRAM */
#define FREYA_APP_REGION_SIZE  (8U * 1024U)
#elif defined(FREYA_BOARD_BLACKPILL)
#define FREYA_APP_LOAD_ADDR    0x20010000UL     /* 128 KiB of SRAM */
#define FREYA_APP_REGION_SIZE  (56U * 1024U)
#else
#error "no board selected - define FREYA_BOARD_BLACKPILL or FREYA_BOARD_BLUEPILL"
#endif

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
} freya_app_header_t;

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
} freya_api_t;

#endif /* FREYA_API_H */
