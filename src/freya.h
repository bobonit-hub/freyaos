/*
 * Freya - kernel wide declarations.
 */
#ifndef FREYA_H
#define FREYA_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "board.h"
#include "freya_api.h"

#define FREYA_VERSION   "1.0"
#define FREYA_BUILD_ID  __DATE__ " " __TIME__

/* --------------------------------------------------------------- misc */
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

/* Symbols provided by the linker script (addresses, not objects). */
extern char __data_start[], __data_end[], __bss_start[], __bss_end[];
extern char __heap_start[], __heap_end[], __etext[], __kernel_flash_end[];
extern char __app_ram_start[], __app_ram_end[];
extern char __stack_top[], __stack_limit[], __ram_start[], __ram_end[];

/* ------------------------------------------------------------- system */
typedef struct {
    uint32_t sysclk_hz;
    uint32_t hclk_hz;
    uint32_t pclk1_hz;
    uint32_t pclk2_hz;
    uint8_t  clock_source;       /* 0 = HSI+PLL, 1 = HSE+PLL            */
    uint8_t  reset_cause;
} sys_clocks_t;

enum {
    RESET_UNKNOWN = 0, RESET_POWER_ON, RESET_PIN, RESET_SOFTWARE,
    RESET_IWDG, RESET_WWDG, RESET_LOWPOWER, RESET_BROWNOUT
};

extern sys_clocks_t g_clocks;

void        sys_init(void);
uint32_t    sys_ticks(void);
void        sys_delay_ms(uint32_t ms);
void        sys_delay_us(uint32_t us);
uint32_t    sys_uptime_ms(void);
void        sys_reboot(void);
const char *sys_reset_cause_str(void);
void        led_init(void);
void        led_set(int on);
void        led_toggle(void);

/* Software real time clock (no battery backed RTC on the board). */
typedef struct {
    uint16_t year;
    uint8_t  mon, day, hour, min, sec;
} rtc_time_t;

void     rtc_set(const rtc_time_t *t);
void     rtc_get(rtc_time_t *t);
uint16_t rtc_fat_date(void);
uint16_t rtc_fat_time(void);

/* --------------------------------------------------------------- uart */
void uart_init(uint32_t baud);
void uart_putc(char c);
void uart_write(const void *buf, int len);
void uart_puts(const char *s);
int  uart_getc(void);                  /* blocking, -1 when aborted     */
int  uart_getc_timeout(uint32_t ms);   /* -1 on timeout                 */
int  uart_rx_ready(void);
void uart_rx_flush(void);
void uart_drain_tx(void);
int  uart_getc_raw_timeout(uint32_t ms); /* bypasses Ctrl-C handling    */
void uart_set_raw(int raw);

/* ------------------------------------------------------------- printf */
int  kprintf(const char *fmt, ...);
int  ksnprintf(char *out, int size, const char *fmt, ...);
int  kvfprintf(void (*emit)(void *, char), void *arg, const char *fmt, va_list ap);
void kput_size(uint64_t bytes);        /* "12.3 KiB" style              */

/* ------------------------------------------------------------ strings */
void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
int     strcasecmp(const char *a, const char *b);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strcat(char *dst, const char *src);
int     str_to_u32(const char *s, uint32_t *out);
char    to_upper(char c);
char    to_lower(char c);

/* --------------------------------------------------------------- heap */
void     heap_init(void);
void    *kmalloc(uint32_t size);
void     kfree(void *p);
void     heap_stats(uint32_t *total, uint32_t *used, uint32_t *free_bytes,
                    uint32_t *largest, uint32_t *blocks);
uint32_t stack_used(void);
uint32_t stack_peak(void);

/* ---------------------------------------------------------------- SPI */
void     spi_init(void);
void     spi_set_speed(int fast);
uint8_t  spi_xfer(uint8_t v);
void     spi_write(const uint8_t *buf, uint32_t len);
void     spi_read(uint8_t *buf, uint32_t len);
void     spi_cs(int low);

/* ------------------------------------------------------------ SD card */
enum { SD_TYPE_NONE = 0, SD_TYPE_MMC, SD_TYPE_SD1, SD_TYPE_SD2, SD_TYPE_SDHC };

typedef struct {
    uint8_t  type;
    uint8_t  initialised;
    uint32_t blocks;         /* 512 byte blocks                        */
    uint8_t  cid[16];
    uint8_t  csd[16];
} sd_info_t;

extern sd_info_t g_sd;

int         sd_init(void);
int         sd_read_block(uint32_t lba, uint8_t *buf);
int         sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count);
int         sd_write_block(uint32_t lba, const uint8_t *buf);
const char *sd_type_str(void);

/* ----------------------------------------------------- internal flash */
/*
 * Present only on a board that reserves part of its internal flash for a
 * program image.  The driver lives in boards/<board>/flash.c because the
 * erase granularity and the programming width are the chip's, not Freya's.
 */
#ifdef FREYA_APP_FLASH_ADDR

enum {
    FLASH_OK            =  0,
    FLASH_ERR_RANGE     = -1,   /* outside a writable flash region        */
    FLASH_ERR_ALIGN     = -2,
    FLASH_ERR_LOCKED    = -3,   /* flash_begin() was not called          */
    FLASH_ERR_BUSY      = -4,   /* a program occupies the scratch region */
    FLASH_ERR_PROG      = -5,
    FLASH_ERR_PROTECTED = -6,   /* option bytes protect the page         */
    FLASH_ERR_VERIFY    = -7,
    FLASH_ERR_TIMEOUT   = -8
};

int         flash_begin(void);   /* copy the RAM routines, unlock        */
void        flash_end(void);     /* lock again, barrier for fetch        */
int         flash_erase(uint32_t addr, uint32_t len);
int         flash_program(uint32_t addr, const void *src, uint32_t len);
uint32_t    flash_page_size(void);
const char *flash_err_str(int rc);

#endif /* FREYA_APP_FLASH_ADDR */

/* ---------------------------------------------------------- user apps */
typedef uint32_t freya_jmpbuf[10];
int  freya_setjmp(freya_jmpbuf buf) __attribute__((returns_twice));
void freya_longjmp(freya_jmpbuf buf, int value) __attribute__((noreturn));

enum {
    APP_STOP_NONE = 0,
    APP_STOP_EXIT,
    APP_STOP_CTRLC,
    APP_STOP_HARDFAULT,
    APP_STOP_MEMFAULT,
    APP_STOP_BUSFAULT,
    APP_STOP_USAGEFAULT
};

typedef struct {
    int      loaded;
    int      running;
    char     path[64];
    char     name[20];
    uint32_t image_size;
    uint32_t bss_start;
    uint32_t bss_size;
    uint32_t entry;
    uint32_t load_addr;
    uint32_t flags;          /* FREYA_APP_F_XIP: runs from flash        */
    uint32_t data_src;       /* .data initialiser, XIP only             */
    uint32_t data_start;
    uint32_t data_end;
    int      last_exit_code;
    int      last_stop_reason;
    uint32_t last_run_ms;
} app_state_t;

extern app_state_t g_app;

int  app_load(const char *path);
int  app_run(int argc, char **argv);
void app_unload(void);
#ifdef FREYA_APP_FLASH_ADDR
/* The installed flash image is addressed as a pseudo-path, so 'load',
 * 'run' and 'stop' need no special case for it. */
#define APP_FLASH_PATH  "@flash"
/* Auto-start slot words; erased flash reads 0xFFFFFFFF (flag off). */
#define FREYA_AUTOSTART_MAGIC  0x31415946UL   /* 'F','Y','A','1' */
#define FREYA_RAMDUMP_MAGIC    0x50444D52UL   /* 'R','M','D','P' */
int  app_install(const char *path);           /* card image -> flash    */
int  app_flash_erase(void);
const freya_app_header_t *app_flash_header(void);   /* NULL if empty    */
int  app_autostart_enabled(void);
int  app_autostart_set(int enable);           /* 0 = FLASH_OK           */
uint32_t app_log_level_stored(void);          /* 0xFFFFFFFF if erased   */
int  app_log_level_store(uint32_t level);     /* 0 = FLASH_OK           */
int  app_ramdump_enabled(void);
int  app_ramdump_set(int enable);             /* 0 = FLASH_OK           */
#endif
void app_request_stop(void);
void app_guard_enter(void);
void app_guard_leave(void);
int  app_should_stop(void);
const char *app_stop_reason_str(int reason);
const freya_api_t *app_api(void);

/* --------------------------------------------------------- filesystem */
const char *fs_cwd(void);
int  fs_abspath(const char *in, char *out, int size);
int  fs_chdir(const char *path);
int  fs_fd_open(const char *path, int flags);   /* used by shell + apps */
int  fs_fd_close(int fd);
int  fs_fd_read(int fd, void *buf, int len);
int  fs_fd_write(int fd, const void *buf, int len);
int  fs_fd_seek(int fd, int32_t off, int whence);
int32_t fs_fd_tell(int fd);
int32_t fs_fd_size(int fd);
int  fs_dd_open(const char *path);
int  fs_dd_read(int dd, freya_stat_t *st);
int  fs_dd_close(int dd);
int  fs_rename(const char *old_path, const char *new_path);
void fs_close_all(void);

/* -------------------------------------------------------------- logging */
void        log_init(void);
void        klog(int level, const char *fmt, ...);
int         log_get_level(void);
int         log_set_level(int level);          /* persists when flash allows */
const char *log_level_str(int level);

/* ----------------------------------------------------------- ram dump */
/* Blue Pill: write SRAM to /freya.ram after a BusFault if a card is up.
 * Other boards: no-ops.  ramdump_then_halt() is the kernel BusFault
 * trampoline and does not return. */
void ramdump_write(void);
void ramdump_then_halt(void) __attribute__((noreturn));

/* -------------------------------------------------------------- shell */
void shell_run(void) __attribute__((noreturn));
int  shell_exec(char *line);
void console_banner(void);

/* ------------------------------------------------------------- xmodem */
int xmodem_receive_to_file(const char *path, uint32_t *received, int strip_pad);

#endif /* FREYA_H */
