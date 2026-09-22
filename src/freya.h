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

/* Every board Freya runs on has at least this much internal flash.  A
 * smaller figure in the size register is the register being wrong. */
#if !defined(BOARD_FLASH_KIB) || (BOARD_FLASH_KIB < 128)
#error "supported boards have at least 128 KiB of internal flash"
#endif
#if defined(FREYA_APP_FLASH_ADDR) && \
    ((FREYA_APP_FLASH_ADDR + FREYA_APP_FLASH_SIZE) > \
     (0x08000000UL + (BOARD_FLASH_KIB) * 1024UL))
#error "program flash region extends past the board's flash"
#endif

#define FREYA_VERSION   "1.0.1"
#define FREYA_CODENAME  "Chupacabra"
#define FREYA_BUILD_ID  __DATE__ " " __TIME__

/* --------------------------------------------------------------- misc */
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

/*
 * Interrupt priorities, in the four bits these parts implement (the
 * NVIC helpers shift them up).  The console is highest so that no
 * character is lost and Ctrl-C always arrives; a program's pin and timer
 * handlers share one level below it, which keeps them from preempting
 * each other; SysTick and PendSV keep the bottom, so a program abort
 * always runs with the thread's exception frame on top of the stack.
 */
#define IRQ_PRIO_CONSOLE   2
#define IRQ_PRIO_HANDLER   14

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
void kput_hms(uint32_t y, uint32_t mo, uint32_t d,
              uint32_t h, uint32_t mi, int sec);  /* sec < 0 omits seconds */

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

/* ------------------------------------------------- pins and interrupts */
/*
 * Pins a program may drive, and interrupts it may take on them.  The
 * register layout belongs to the chip, so boards/<board>/board.c does the
 * configuring and src/gpio.c owns the sixteen EXTI lines, which are the
 * same on every STM32: line n is pin n of one port at a time.
 */
int      gpio_pin_mode(int pin, int mode);       /* FREYA_PIN_*          */
int      gpio_pin_read(int pin);
int      gpio_pin_write(int pin, int value);
int      gpio_pin_toggle(int pin);
int      gpio_irq_attach(int pin, int edge, freya_irq_fn fn, void *arg);
int      gpio_irq_detach(int pin);
uint32_t gpio_irq_count(int pin);
void     gpio_irq_release(void);          /* drop whatever a run left     */

/* ------------------------------------------------------------- timers */
/*
 * The general purpose timers a program may claim, one periodic (or one
 * shot) interrupt each.  The hardware is the board's list; everything
 * about turning a period in microseconds into a prescaler and a reload
 * is the same on both.
 */
int      timer_open(uint32_t period_us, int flags, freya_irq_fn fn, void *arg);
int      timer_close(int timer);
int      timer_start(int timer);
int      timer_stop(int timer);
int      timer_period(int timer, uint32_t period_us);
uint32_t timer_count(int timer);
void     timer_release(void);             /* drop whatever a run left     */
uint32_t timer_clock_hz(void);
const char *timer_name(int timer);        /* "TIM2", for the console      */

/* Lent to src/pwm.c, which drives the compare channels of these same
 * timers: a borrowed one is not handed out by timer_open(). */
TIM_TypeDef *timer_take(int timer);       /* NULL when it is spoken for   */
void         timer_give(int timer);

/* ---------------------------------------------------------------- PWM */
/*
 * Square waves on the pins the board's timer channels reach, from 1 Hz
 * to 1 MHz, with a duty cycle in ten-thousandths of the period.  A
 * handle is the channel; the pins are BOARD_PWM_MAP, and the channels of
 * one timer share its frequency because they share its counter.
 */
int      pwm_open(int pin, uint32_t freq_hz, uint32_t duty);
int      pwm_close(int pwm);
int      pwm_duty(int pwm, uint32_t duty);
int      pwm_pulse_us(int pwm, uint32_t us);
int      pwm_freq(int pwm, uint32_t freq_hz);
int      pwm_lookup(int pin);             /* the channel a pin is         */
void     pwm_release(void);               /* drop whatever a run left     */

/* One of the board's channels, for the 'pwm' command to list. */
typedef struct {
    int         pin;
    const char *timer;
    int         ch;
    int         open;
    uint32_t    freq_hz;
    uint32_t    duty;
} pwm_info_t;

int      pwm_info(int idx, pwm_info_t *info);   /* -1 past the last one   */
int      pwm_pin_busy(int pin);           /* 1 when that pin is driving   */

/* ---------------------------------------------------------------- I2C */
/*
 * Master only, polled, on the buses BOARD_I2C_MAP names.  The pins are
 * driven as open-drain GPIO, the same code on both chips.  A bus a
 * program opened is closed when the run ends.  One opened at the console
 * is not, and a program that wants it is told it is busy.
 */
int      i2c_open(int bus, uint32_t hz);  /* 0, or FREYA_ERR_*            */
int      i2c_close(int bus);
int      i2c_write(int bus, int addr, const void *buf, int len);
int      i2c_read(int bus, int addr, void *buf, int len);
int      i2c_transfer(int bus, int addr, const void *tx, int txlen,
                      void *rx, int rxlen);
int      i2c_owns_pin(int pin);           /* 1 when an open bus uses it   */
void     i2c_release(void);               /* drop whatever a run left     */

typedef struct {
    const char *name;
    int         scl;
    int         sda;
    int         open;
    uint32_t    hz;
} i2c_info_t;

int      i2c_info(int idx, i2c_info_t *info);   /* -1 past the last bus   */

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

/* How a run ended, and the status it reports, are both ABI: see
 * FREYA_STOP_* and freya_exit_status() in freya_api.h. */

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
    uint32_t flags;          /* FREYA_APP_F_XIP: installed in flash     */
    uint32_t data_src;       /* relocated .data initialiser, XIP only   */
    uint32_t data_start;
    uint32_t data_end;
    /* The last run, which outlives the image: these survive an unload so
     * that 'status' can still say what happened. */
    int      last_status;    /* freya_exit_status(), 0 .. 255           */
    int      last_stop_reason;
    uint32_t last_run_ms;
    uint32_t runs;           /* runs since reset; 0 means nothing ran   */
    char     last_name[20];  /* which program that was                  */
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

/*
 * A pin or timer handler is the program's code running in interrupt
 * context, so it is contained the same way the program itself is:
 * app_handler_call() runs it inside a jump buffer, and app_handler_kill()
 * - used by the console when Ctrl-C finds one that is not finishing, and
 * by the fault handler when one crashes - makes it resume in a trampoline
 * that unwinds back into the interrupt that called it.
 */
extern volatile uint32_t g_irq_events;    /* handler events this run      */
int  app_handler_call(freya_irq_fn fn, int source, void *arg);
int  app_in_handler(void);                /* interrupt context, not thread */
int  app_handler_kill(uint32_t *frame);   /* 1 if this frame was redirected */
const char *app_stop_reason_str(int reason);
int  app_last_exit(freya_exit_t *st);         /* -1 if nothing ran      */
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
/* Write SRAM to /freya.ram after a BusFault if a card is up and the
 * auto-start slot has the flag on.  ramdump_then_halt() is the kernel
 * BusFault trampoline and does not return. */
void ramdump_write(void);
void ramdump_then_halt(void) __attribute__((noreturn));

/* -------------------------------------------------------------- shell */
void shell_run(void) __attribute__((noreturn));
int  shell_exec(char *line);                  /* returns the status     */
void console_banner(void);

/* ------------------------------------------------------------- xmodem */
int xmodem_receive_to_file(const char *path, uint32_t *received, int strip_pad);

#endif /* FREYA_H */
