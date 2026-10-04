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
#if defined(FREYA_SETTINGS_ADDR) && \
    ((FREYA_SETTINGS_ADDR + FREYA_SETTINGS_SIZE) > \
     (0x08000000UL + (BOARD_FLASH_KIB) * 1024UL))
#error "system settings extend past the board's flash"
#endif

/* The OS version is the release named in the documentation.  A firmware
 * build does not change it.  The build may override the firmware version;
 * a source build always has one of its own.  Keep both in
 * major.minor.patch form. */
#define FREYA_OS_VERSION        "4.0.0"
#ifndef FREYA_FIRMWARE_VERSION
#define FREYA_FIRMWARE_VERSION  "3.1.1"
#endif
#define FREYA_CODENAME          "Bigfoot"
#define FREYA_BUILD_ID          __DATE__ " " __TIME__

/* --------------------------------------------------------------- misc */
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

/*
 * Interrupt priorities, in the four bits these parts implement (the
 * NVIC helpers shift them up).  The console is highest so that no
 * character is lost and Ctrl-C always arrives; the USB frame of a
 * headset comes next, so a call does not stutter; a program's pin and timer
 * handlers share one level below it, which keeps them from preempting
 * each other; SysTick and PendSV keep the bottom, so a program abort
 * or a thread switch always runs with the thread's exception frame on
 * top of that thread's stack.
 */
#define IRQ_PRIO_CONSOLE   2
#define IRQ_PRIO_USB       3    /* a headset's 1 ms frame (AUDIO=1)    */
#define IRQ_PRIO_HANDLER   14

/* Symbols provided by the linker script (addresses, not objects). */
extern char __data_start[], __data_end[], __bss_start[], __bss_end[];
extern char __heap_start[], __heap_end[], __etext[], __kernel_flash_end[];
extern char __kext_start[], __kext_end[];
extern char __app_ram_start[], __app_ram_end[];
extern char __stack_top[], __stack_limit[], __ram_start[], __ram_end[];
extern char __thread_stack_top[];      /* shell stack: PSP, below the IRQ stack */

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

/* Civil time counted from SysTick.  rtc_set() is what the date command
 * stores, and what file timestamps read.  It is not battery backed.
 * RTC=ds3231 adds a DS3231 on the pins below; the shell copies it into
 * this count at boot and writes it back when the date is set. */
typedef struct {
    uint16_t year;
    uint8_t  mon, day, hour, min, sec;
} rtc_time_t;

void     rtc_set(const rtc_time_t *t);
void     rtc_get(rtc_time_t *t);
/* rtc_set() with the range check and the DS3231 write the date command
 * and the program's rtc_set() both need.  0, or FREYA_ERR_*. */
int      rtc_apply(const rtc_time_t *t);
uint16_t rtc_fat_date(void);
uint16_t rtc_fat_time(void);

#ifdef FREYA_RTC_DS3231
/* SCL and SDA are the only DS3231 pins connected to the MCU. */
#define DS3231_SCL       FREYA_PB(6)
#define DS3231_SDA       FREYA_PB(7)
#define DS3231_ADDR      0x68

/* 0 the time is valid and *t was filled.  FREYA_ERR_ARG the chip
 * answered but the time is not valid (oscillator stopped, or the
 * registers do not decode).  FREYA_ERR_NACK nothing answered. */
int      ds3231_read(rtc_time_t *t);
int      ds3231_write(const rtc_time_t *t);   /* 0, or FREYA_ERR_*       */
int      ds3231_pins_match(void);   /* 1 when bus 1 is the pins above    */
void     ds3231_boot(void);         /* read the chip into the software clock */
#ifdef FREYA_HOST
void     ds3231_test_load(const uint8_t *mem, int n);
void     ds3231_test_fail(int rc);
void     ds3231_test_save(uint8_t *dst, int n);
#endif
#endif

#ifdef FREYA_RTC_INTERNAL
/* The chip's own calendar RTC, built with RTC=internal (src/rtc.c).
 * rtcin_read(): 0 the time is valid and *t was filled, FREYA_ERR_ARG the
 * RTC runs but was never set, FREYA_ERR_IO it does not run. */
int      rtcin_read(rtc_time_t *t);
int      rtcin_write(const rtc_time_t *t);    /* 0, or FREYA_ERR_*       */
void     rtcin_boot(void);          /* read the RTC into the software clock */
#ifdef FREYA_HOST
void     rtcin_test_regs(uint32_t **tr, uint32_t **dr, uint32_t **isr,
                         uint32_t **prer, uint32_t **cr, uint32_t **bdcr);
#endif
#endif

/* --------------------------------------------------------------- uart */
void uart_init(uint32_t baud);
void uart_putc(char c);
void uart_write(const void *buf, int len);
void uart_puts(const char *s);
int  uart_getc(void);                  /* blocking, -1 when aborted     */
int  uart_getc_timeout(uint32_t ms);   /* -1 on timeout                 */
int  uart_rx_ready(void);
int  uart_take_ctrlc(void);            /* 1 if Ctrl-C was waiting       */
void uart_rx_flush(void);
void uart_drain_tx(void);
int  uart_getc_raw_timeout(uint32_t ms); /* bypasses Ctrl-C handling    */
int  uart_getc_nb(void);               /* -1 when the ring is empty     */
int  uart_is_raw(void);
void uart_term_rx_push(uint8_t c);     /* C6 input; selects TLS output  */
int  uart_term_pending(void);          /* TLS-selected output bytes     */
int  uart_term_peek(uint8_t *dst, int max);
void uart_term_drop(int n);
void uart_term_disconnected(void);     /* select UART and discard TLS TX */
void term_pump(void);                  /* STM32 console <-> C6 TLS shell */
int  uart_waiters(void);               /* threads blocked in uart_getc  */
void uart_set_raw(int raw);
void uart_capture_begin(char *buf, int max); /* divert console output */
int  uart_capture_end(void);                 /* bytes stored           */
int  uart_capture_dropped(void);             /* 1 if the buffer filled */

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
size_t  strcspn(const char *s, const char *reject);
size_t  strspn(const char *s, const char *accept);
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
int      gpio_irq_owns_pin(int pin);       /* 1 when that pin has EXTI   */
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
int      timer_is_open(int timer);        /* 1 when that handle is open  */
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

/* ------------------------------------------------------------- 1-Wire */
/*
 * Master only, standard speed, polled, on a pin the caller names.  The
 * line is open-drain GPIO, the same code on both chips.  A bus a
 * program opened is closed when the run ends.  One opened at the
 * console is not, and a program that wants that pin is told it is busy.
 */
int      w1_open(int pin);            /* 0, or FREYA_ERR_*            */
int      w1_close(int pin);
int      w1_reset(int pin);           /* 0 presence, NACK if nobody    */
int      w1_write(int pin, const void *buf, int len);
int      w1_read(int pin, void *buf, int len);
int      w1_search(int pin, void *rom); /* next ROM, then NACK          */
int      w1_pullup(int pin, int on);  /* strong high, for parasite power */
int      w1_crc(const void *buf, int len); /* CRC-8, or FREYA_ERR_ARG   */
int      w1_owns_pin(int pin);        /* 1 when an open bus uses it    */
void     w1_release(void);            /* drop whatever a run left      */

typedef struct {
    int pin;
    int pullup;
} w1_info_t;

int      w1_info(int idx, w1_info_t *info);    /* -1 past the last open */

/* ---------------------------------------------------------------- SPI */
/*
 * SPI1 belongs to the SD card.  A program's master is the buses
 * BOARD_SPI_MAP names, which are a different controller so the card is
 * never that bus.  Chip select is a pin the caller drives.  A bus a
 * program opened is closed when the run ends.  One opened at the console
 * is not, and a program that wants it is told it is busy.
 */
void     sdspi_init(void);
void     sdspi_set_speed(int fast);
uint8_t  sdspi_xfer(uint8_t v);
void     sdspi_write(const uint8_t *buf, uint32_t len);
void     sdspi_read(uint8_t *buf, uint32_t len);
void     sdspi_cs(int low);
void     sdspi_quiesce(void);             /* stop SPI1, release its pins */
#ifdef BOARD_SD_BITBANG
/* SPI1 for the SPI flash alone, on a board that drives its card itself. */
void     flspi_init(void);
void     flspi_set_speed(int fast);
uint8_t  flspi_xfer(uint8_t v);
void     flspi_write(const uint8_t *buf, uint32_t len);
void     flspi_read(uint8_t *buf, uint32_t len);
void     flspi_cs(int low);
#endif
#ifdef BOARD_SPI_FIFO
/* Master, 8 bits, software NSS, divider code br, mode 0..3, started. */
void     spififo_setup(SPI_TypeDef *regs, uint32_t br, int mode);
#endif

int      spi_open(int bus, uint32_t hz, int mode); /* 0, or FREYA_ERR_* */
int      spi_close(int bus);
int      spi_transfer(int bus, const void *tx, void *rx, int len);
int      spi_write(int bus, const void *buf, int len);
int      spi_read(int bus, void *buf, int len);
int      spi_owns_pin(int pin);           /* 1 when an open bus uses it   */
void     spi_release(void);               /* drop whatever a run left     */

typedef struct {
    const char *name;
    int         sck;
    int         miso;
    int         mosi;
    int         open;
    int         mode;
    uint32_t    hz;              /* the divider's rate, 0 when shut       */
} spi_info_t;

int      spi_info(int idx, spi_info_t *info);   /* -1 past the last bus   */

/* ------------------------------------------------------------ network */
int      wifi_on(void);
int      wifi_off(void);
int      wifi_credentials(const char *ssid, const char *password);
int      wifi_connect(void);
int      wifi_disconnect(void);
int      wifi_status(freya_wifi_status_t *status);
int      wifi_scan_start(void);
int      wifi_scan_next(freya_wifi_scan_t *entry);
int      ping_start(const char *host, uint32_t timeout_ms);
int      ping_result(freya_ping_result_t *result);
int      net_socket(int domain, int type, int protocol);
int      net_close(int socket);
int      net_connect(int socket, const freya_net_addr_t *addr);
int      net_tls_connect(int socket, const char *hostname, uint16_t port);
int      net_bind(int socket, const freya_net_addr_t *addr);
int      net_listen(int socket, int backlog);
int      net_accept(int socket, freya_net_addr_t *peer);
int      net_send(int socket, const void *buf, int len);
int      net_recv(int socket, void *buf, int len);
int      net_sendto(int socket, const void *buf, int len,
                    const freya_net_addr_t *to);
int      net_recvfrom(int socket, void *buf, int len,
                      freya_net_addr_t *from);
int      net_poll(uint32_t timeout_ms);
void     net_release(void);
int      net_unsupported(void);          /* compact Blue Pill API stub */

#define HTTP_FLAG_COMPRESSED  0x01U
#define HTTP_FLAG_INSECURE    0x02U
#define HTTP_FLAG_VERBOSE     0x04U
#define HTTP_HEADERS_MAX      400U
typedef struct {
    uint32_t body_length;
    uint16_t status;
    uint16_t header_length;
    char headers[HTTP_HEADERS_MAX];
} freya_http_info_t;
int      net_http_start(uint8_t flags, const char *url, const char *user_agent,
                        const char *basic, const char *data);
int      net_http_info(freya_http_info_t *info);
int      net_http_read(void *buf, int len);
int      net_http_close(void);
int      web_take(freya_web_req_t *req);
int      web_begin(int status, const char *type, uint32_t length);
int      web_body(const void *data, int len);
int      web_end(void);
int      web_read(void *data, int max, uint32_t *left);
int      cmd_curl(int argc, char **argv);

/* ---------------------------------------------------------------- ADC */
/*
 * One polled conversion from an external pin or an internal source.
 * Channel numbering and the ADC register layout belong to the board.
 */
int      adc_read(int source);             /* raw 12-bit value, or error */
int      adc_lookup(int source);           /* hardware channel, or PIN   */

/* ---------------------------------------------------------- heatshrink */
/*
 * LZSS, the heatshrink stream at an 8-bit window and a 4-bit lookahead
 * (see freya_api.h).  lz_compress() and lz_decompress() are the service
 * calls: whole buffers, the state taken from the heap for the call.
 *
 * The stream calls are for the shell, which works a file through in
 * pieces.  lz_open() takes the state from the heap and is NULL when it
 * cannot, or from a handler.  lz_sink() hands the coder input and
 * returns how much it took; 0 means poll first.  lz_poll() collects
 * output into out and returns how much, 0 when there is nothing until
 * more input is sunk or lz_finish() is called.  lz_finish() marks the
 * end of the input and returns 0 when every byte is out, 1 while
 * lz_poll() still has some, or FREYA_ERR_IO.  A board without the code
 * has the two service calls only, and they return FREYA_ERR_UNSUPPORTED.
 */
typedef struct lz_stream lz_stream_t;
int          lz_compress(const void *in, int in_len, void *out, int out_cap);
int          lz_decompress(const void *in, int in_len, void *out, int out_cap);

/* ------------------------------------------------------- Ascon-AEAD128 */
/*
 * NIST SP 800-232.  aead_encrypt() and aead_decrypt() are the service
 * calls (see freya_api.h).  Neither keeps the key, and neither makes
 * one.  A board without the code returns FREYA_ERR_UNSUPPORTED.
 */
int          aead_encrypt(const void *key, const void *nonce,
                          const void *ad, int ad_len,
                          const void *in, int in_len,
                          void *out, int out_cap);
int          aead_decrypt(const void *key, const void *nonce,
                          const void *ad, int ad_len,
                          const void *in, int in_len,
                          void *out, int out_cap);
#if BOARD_COMPRESS
lz_stream_t *lz_open(int decode);
void         lz_close(lz_stream_t *s);
int          lz_sink(lz_stream_t *s, const void *in, int len);
int          lz_poll(lz_stream_t *s, void *out, int cap);
int          lz_finish(lz_stream_t *s);
#endif

/* ---------------------------------------------------- PDP-11, 32-bit */
/* vm_reset() clears the registers and sets Z.  vm_step() runs one
 * instruction.  vm_run() runs up to steps of them.  See freya_api.h. */
int      vm_reset(freya_vm_t *vm);
int      vm_step(freya_vm_t *vm, void *mem, uint32_t size);
int      vm_run(freya_vm_t *vm, void *mem, uint32_t size,
                uint32_t steps, uint32_t *ran);

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
int         sd_powered(void);             /* 1 while the socket has VDD   */
int         sd_power(int on);             /* the rail itself, 0 or 1      */
int         board_power(int domain, int on); /* previous state, or FREYA_ERR_* */
int         sd_read_block(uint32_t lba, uint8_t *buf);
int         sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count);
int         sd_write_block(uint32_t lba, const uint8_t *buf);
const char *sd_type_str(void);

/* ----------------------------------------------------------- USB host */
/*
 * USB=1, on a board whose USB OTG core can be the host (BOARD_USB_OTG):
 * a mass storage stick in the board's USB socket, mounted as FAT at
 * /usb.  src/usbh.c drives the core, polled, at full speed; src/usbmsc.c
 * enumerates the stick and speaks Bulk-Only Transport and SCSI to it;
 * src/usbvol.c is the second FAT volume.
 */
enum {
    USBH_OK      =  0,
    USBH_STALL   = -1,        /* the endpoint answered STALL             */
    USBH_TIMEOUT = -2,        /* no answer, or NAK past the deadline     */
    USBH_ERR     = -3,        /* transaction errors, three in a row      */
    USBH_GONE    = -4,        /* the device was unplugged                */
    USBH_NODEV   = -5,        /* nothing in the socket                   */
    USBH_UNSUP   = -6         /* not a stick Freya can use               */
};

int         board_usb_init(void);         /* clock, pins, PHY supply     */
void        board_usb_off(void);
int         usbh_open(uint32_t wait_ms);  /* power, connect, reset       */
void        usbh_close(void);
int         usbh_connected(void);
int         usbh_control(uint8_t addr, uint8_t mps0, const uint8_t setup[8],
                         void *data, uint16_t *len);
int         usbh_bulk(uint8_t addr, uint8_t ep, uint16_t mps, uint8_t *toggle,
                      void *buf, uint32_t len, uint32_t *done,
                      uint32_t wait_ms);
const char *usbh_err_str(int err);

enum { USB_KIND_NONE, USB_KIND_MSC, USB_KIND_AUDIO, USB_KIND_OTHER };

typedef struct {
    uint8_t  addr;
    uint8_t  mps0;
    uint8_t  kind;           /* USB_KIND_*                              */
    uint8_t  config;         /* bConfigurationValue                     */
    uint16_t vid, pid;
    uint16_t cfg_len;
    uint8_t  cfg[512];       /* a headset's runs past 256 bytes         */
    char     maker[32];
    char     product[32];
} usb_dev_t;

extern usb_dev_t g_usbdev;

int         usbdev_open(uint32_t wait_ms);   /* port, enumerate, configure */
void        usbdev_close(void);
int         usbdev_control(uint8_t type, uint8_t req, uint16_t value,
                           uint16_t index, void *data, uint16_t len,
                           uint16_t *got);
const char *usbdev_name(void);
int         usb_attach(int boot);         /* reports, 0 when in use      */
void        usb_detach(void);

typedef struct {
    uint8_t  present;        /* answering SCSI                          */
    uint8_t  ep_in, ep_out;  /* bulk endpoints, ep_in with bit 7 set    */
    uint8_t  tog_in, tog_out;
    uint8_t  iface;
    uint8_t  lun;
    uint16_t mps_in, mps_out;
    uint32_t blocks;         /* 512 byte blocks                         */
    uint32_t tag;
    char     vendor[9];
    char     product[17];
} usb_msc_t;

extern usb_msc_t g_usb;

int         usbmsc_start(void);
void        usbmsc_stop(void);
int         usbmsc_read_block(uint32_t lba, uint8_t *buf);
int         usbmsc_write_block(uint32_t lba, const uint8_t *buf);

int         usbvol_attach(int boot);      /* reports, 0 when mounted     */
void        usbvol_unmount(void);
int         usbvol_mounted(void);

/* -------------------------------------------------------------- audio */
/* The calls of freya_api_t.  Without AUDIO=1 they answer unsupported. */
int         audio_open(uint32_t rate, int dirs);
int         audio_close(void);
int         audio_read(int16_t *buf, int count);
int         audio_write(const int16_t *buf, int count);
int         audio_status(freya_audio_status_t *st);
int         audio_gain(int dirs, int gain);
void        audio_release(void);          /* the run that opened it ended */

#ifdef FREYA_AUDIO
/* One direction of the stream, as src/uac.c set it up. */
typedef struct {
    uint8_t  ep;             /* endpoint address, 0 when not used       */
    uint8_t  channels;
    uint16_t mps;
    uint32_t rate;           /* the headset's                           */
} audio_stream_t;

/* Called from the USB interrupt once a frame (src/usbh.c). */
int         audio_frame_out(uint8_t *pkt, int max);
void        audio_frame_in(const uint8_t *pkt, int len);
void        audio_frame_error(void);
void        audio_gone(void);

int         usbh_iso_start(uint8_t out_ep, uint16_t out_mps,
                           uint8_t in_ep, uint16_t in_mps);
void        usbh_iso_stop(void);

#define UAC_MAX_ALTS    8
#define UAC_MAX_FREQ    6

/* One alternate setting of an audio streaming interface. */
typedef struct {
    uint8_t  iface, alt;
    uint8_t  ep;             /* the isochronous data endpoint           */
    uint8_t  interval;
    uint8_t  channels, subframe, bits;
    uint8_t  freq_ctl;       /* the endpoint takes SET_CUR frequency    */
    uint16_t mps;
    uint8_t  nfreq;          /* 0: continuous, freq[0] to freq[1]       */
    uint32_t freq[UAC_MAX_FREQ];
} uac_alt_t;

int         uac_parse(const uint8_t *cfg, uint16_t len);
const uac_alt_t *uac_pick(int in, uint32_t rate, uint32_t *dev_rate);
const uac_alt_t *uac_alts(int *n);
int         uac_attach(int boot);
void        uac_stop(void);
void        uac_info(void);
int         uac_stream_start(uint32_t rate, int dirs, audio_stream_t *mic,
                             audio_stream_t *spk);
void        uac_stream_stop(void);
#endif

/* ----------------------------------------------------------- SPI flash */
/* Black Pill SOP-8 NOR on SPI1.  The mount point is /spi<bus>.  On a
 * board without the footprint these report "not present". */
int         spiflash_probe(void);
int         spiflash_mount_fs(void);
int         spiflash_attach(void);
int         spiflash_mounted(void);
const char *spiflash_name(void);
uint32_t    spiflash_bytes(void);
int         spiflash_bus(void);
void        spiflash_boot(void);
int         spiflash_mount_cmd(void);
void        spiflash_unmount(void);
void        spiflash_info(void);
void        spiflash_df(void);
/* Byte operations for the LittleFS block device.  prog only clears bits. */
int         spiflash_bd_read(uint32_t addr, void *dst, uint32_t len);
int         spiflash_bd_prog(uint32_t addr, const void *src, uint32_t len);
int         spiflash_bd_erase(uint32_t addr);
void        spiflash_bd_sync(void);
int         spiflash_bd_blank(void);
#ifdef FREYA_HOST
int         spiflash_test_bind(uint8_t *mem, uint32_t bytes);
int         spiflash_read_block(uint32_t lba, uint8_t *buf);
int         spiflash_write_block(uint32_t lba, const uint8_t *buf);
void        spiflash_sync(void);
#endif

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
    uint32_t ram_end;        /* first RAM byte past the program's own   */
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
/* 1 while the loaded program's RAM runs on into the thread stacks, which
 * a FREYA_APP_F_NOTHREADS program may do.  The shell keeps its scratch
 * there, so it must not touch it then. */
int  app_holds_thread_stacks(void);
int  app_run(int argc, char **argv);
void app_unload(void);
#ifdef FREYA_APP_FLASH_ADDR
/* The installed flash image is addressed as a pseudo-path, so 'load',
 * 'run' and 'stop' need no special case for it. */
#define APP_FLASH_PATH  "@flash"
/* Auto-start slot words; erased flash reads 0xFFFFFFFF (flag off). */
#define FREYA_AUTOSTART_MAGIC  0x31415946UL   /* 'F','Y','A','1' */
#define FREYA_RAMDUMP_MAGIC    0x50444D52UL   /* 'R','M','D','P' */
#define FREYA_SYSLOG_MAGIC     0x474C5953UL   /* 'S','Y','L','G' */
#define FREYA_SCRIPT_MAGIC 0x54524353UL   /* 'S','C','R','T' */
/* Text follows the header and is ended by a NUL.  'length' is the
 * text, not counting that NUL. */
typedef struct {
    uint32_t magic;
    uint32_t length;
} freya_script_header_t;
/* A text kept after the program image in the region, for the boot to
 * hand that program as 'program -e TEXT': the BASIC program basic11
 * saves.  The header is followed by the text and a NUL; 'name' is the
 * image's own, so a text is never handed to another program. */
#define FREYA_TEXT_MAGIC   0x54584554UL   /* 'T','E','X','T' */
typedef struct {
    uint32_t magic;
    uint32_t length;
    char     name[16];
} freya_text_header_t;
const char *app_flash_text(void);   /* the text, or NULL if there is none */
void app_run_requests(void);        /* what a program asked for its end */
int  app_install(const char *path);           /* card image -> flash    */
int  app_flash_erase(void);
const freya_app_header_t *app_flash_header(void);   /* NULL if empty    */
/* 1 and *text set when the region holds a script.  0 when it does not.
 * -1 when a script header is there but the text is not usable. */
int  app_script_find(const char **text, uint32_t *length);
#ifdef FREYA_AUTORUN_TEXT
extern const char autorun_text[];   /* BASIC=file, for 'program -e TEXT' */
#endif
int  app_autostart_enabled(void);
int  app_autostart_set(int enable);           /* 0 = FLASH_OK           */
uint32_t app_log_level_stored(void);          /* 0xFFFFFFFF if erased   */
int  app_log_level_store(uint32_t level);     /* 0 = FLASH_OK           */
int  app_ramdump_enabled(void);
int  app_ramdump_set(int enable);             /* 0 = FLASH_OK           */
int  app_password_enabled(void);              /* 0 when the slot is erased */
void app_password_read(uint8_t *out);         /* FREYA_PASSWORD_LEN bytes  */
int  app_password_set(const uint8_t *pass);   /* NULL clears; 0 = FLASH_OK */

/* Named settings in the two flash copies.  0 on success.  -1 when the
 * name or the length is wrong, or when both copies are corrupt (the
 * checksum words are left as they are).  A set that changes a field
 * rewrites both copies and both checksums. */
int  settings_get(const char *name, void *buf, uint32_t len);
int  settings_set(const char *name, const void *buf, uint32_t len);
int  settings_ok(void);                       /* 1 ok, 0 both copies bad */
uint32_t settings_fw_stored(void);            /* firmware sum word        */
int  settings_block(void *buf, int len);      /* default copy, or FREYA_ERR_ARG */
uint32_t settings_area_size(void);            /* bytes reserved at end of flash */
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
int  app_switch_blocked(void);            /* guard or handler: do not switch */
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

/* ------------------------------------------------------ remote syslog */
#if BOARD_ESP_LINK
/* klog() lines as RFC 3164 datagrams to one IPv4 server, over UDP through
 * the ESP32-C6.  The flag, the address and the port are system settings.
 * An erased port is FREYA_SYSLOG_PORT; an erased address is no server. */
#define FREYA_SYSLOG_PORT  514U
int      syslog_enabled(void);
uint32_t syslog_server(uint16_t *port);         /* 0 when none is set     */
int      syslog_set_enabled(int on);            /* 0 = FLASH_OK, FREYA_ERR_ARG
                                                 * to turn on with no server */
int      syslog_set_server(uint32_t addr, uint16_t port);   /* 0 = FLASH_OK */
int      syslog_parse_ipv4(const char *s, uint32_t *addr);  /* 0 when valid */
void     syslog_send(int level, const rtc_time_t *t, const char *msg, int len);
void     syslog_flush(void);                    /* queued lines, if any    */
/* One ESP_OP_SYSLOG request (address, port, datagram), waited for.
 * FREYA_ERR_AGAIN when the link is carrying another request. */
int      net_syslog_send(const void *req, uint16_t len);
#endif

/* ----------------------------------------------------------- ram dump */
/* Write SRAM to /freya.ram after a BusFault if a card is up and the
 * auto-start slot has the flag on.  ramdump_then_halt() is the kernel
 * BusFault trampoline and does not return. */
void ramdump_write(void);
void ramdump_then_halt(void) __attribute__((noreturn));

/* ---------------------------------------------------- firmware sum */
/* Byte sum of the kernel image and the kernel extension.  The firmware
 * sum word lives in the system settings area, outside both images.
 * status is FW_CKSUM_OK, FW_CKSUM_MISMATCH or FW_CKSUM_BLANK. */
enum { FW_CKSUM_OK = 0, FW_CKSUM_MISMATCH = 1, FW_CKSUM_BLANK = 2 };
typedef struct {
    uint32_t stored;
    uint32_t computed;
    int      status;
} fw_cksum_t;
uint32_t fw_sum_bytes(const uint8_t *p, uint32_t addr, uint32_t len,
                      uint32_t skip_addr, uint32_t skip_len);
void     fw_cksum_read(fw_cksum_t *out);
int      fw_cksum_show(void);   /* prints one line, returns FW_CKSUM_* */

/* ------------------------------------------------------------ threads */
/*
 * Priority threads.  The shell is thread 0 and, while a program runs, it
 * is that program's main thread.  Idle runs only when every other thread
 * is blocked.  Names are unique.  A larger priority runs first.
 */
void     thread_init(void);
void     thread_tick(void);             /* from SysTick                       */
void     thread_yield(void);
int      thread_sleep(uint32_t ms);     /* 0, -1 if stopping, FREYA_ERR_*     */
void     thread_exit(void) __attribute__((noreturn));
int      thread_self(void);
int      thread_is_main(void);
int      thread_create(const char *name, int priority,
                       freya_thread_fn fn, void *arg);
void     thread_list(void);
int      thread_stop_name(const char *name);   /* 0, or FREYA_ERR_*           */
void     thread_run_begin(const char *name);   /* main thread becomes the run */
void     thread_run_end(void);                 /* drop threads the run made   */
void     thread_after_abort(void);             /* longjmp landed on the shell */
void     thread_reconsider(void);              /* guard lifted: switch or stop */
int      thread_preempt_kind(void);            /* PendSV: 0 hold, 1 abort, 2 switch */
uint32_t thread_switch(uint32_t saved_sp);

/* Saved and restored by the PendSV shim around thread_switch(). */
extern uint32_t thread_exc_save;
extern uint32_t thread_exc_restore;

/* -------------------------------------------------------------- shell */
void shell_poll_runtime(void);          /* threads/stop while a program runs */
#ifdef FREYA_LINUX
int  shell_run(void);          /* at end of input, the last status    */
int  shell_source_file(const char *path);     /* returns the status     */
#else
void shell_run(void) __attribute__((noreturn));
#endif
int  shell_exec(const char *line);            /* returns the status     */
int  shell_source_capture(const char *path, const char *method,
                          const char *query, char *buf, int cap,
                          int *out_len);
/* A shell script is ASCII, plus tab and newline.  'len' may be 0. */
int  script_text_ok(const char *text, uint32_t len);
/* A file passed to 'source' has to fit in the heap.  16 KiB is the cap.
 * A script installed in program flash may be larger; that one is read
 * from the flash when it is over the cap or the heap cannot hold it. */
#define FREYA_SCRIPT_FILE_MAX  (16U * 1024U)
void console_banner(void);

/* ------------------------------------------------------------- xmodem */
/* exact < 0 keeps the old rule (strip SUB padding, or keep it).
 * exact >= 0 stores that many bytes and drops the rest of the packet. */
int xmodem_receive_to_file(const char *path, uint32_t *received,
                           int strip_pad, int32_t exact);
int xmodem_send_file(const char *path, uint32_t *sent);

#endif /* FREYA_H */
