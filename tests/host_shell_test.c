/*
 * Freya - host test for the console commands whose help text is a single
 * word, and for help itself.
 *
 * The shell is the one in src/shell.c.  Hardware it would touch (the
 * CPUID and flash-size registers, the card, the clock) is planted or
 * stubbed, so each command can be typed the way it is on the board and
 * its status and output checked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#include "freya.h"
#include "fat.h"

static char s_out[8192];
static int  s_outn;
static int  checks, fails;

static int  s_mounted;
static int  s_installed;
static int  s_erased;
static int  s_unloaded;
static int  s_stop_requested;
static int  s_rebooted;
static int  s_synced;
static int  s_have_exit;

char freya_test_lay[4096];

/* meminfo subtracts these.  One object keeps that subtraction defined.
 * The names are not the C library's __data_start and __bss_start; shell.c
 * is compiled with -D renaming its linker symbols onto these. */
__asm__(
    ".globl freya_test_ram_start\n"
    ".globl freya_test_data_start\n"
    ".globl freya_test_data_end\n"
    ".globl freya_test_bss_start\n"
    ".globl freya_test_bss_end\n"
    ".globl freya_test_heap_start\n"
    ".globl freya_test_stack_limit\n"
    ".globl freya_test_stack_top\n"
    ".globl freya_test_ram_end\n"
    ".globl freya_test_kernel_flash_end\n"
    ".set freya_test_ram_start, freya_test_lay\n"
    ".set freya_test_data_start, freya_test_lay+64\n"
    ".set freya_test_data_end, freya_test_lay+96\n"
    ".set freya_test_bss_start, freya_test_lay+128\n"
    ".set freya_test_bss_end, freya_test_lay+192\n"
    ".set freya_test_heap_start, freya_test_lay+256\n"
    ".set freya_test_stack_limit, freya_test_lay+512\n"
    ".set freya_test_stack_top, freya_test_lay+1536\n"
    ".set freya_test_ram_end, freya_test_lay+2048\n"
    ".set freya_test_kernel_flash_end, freya_test_lay+2056\n"
);

sys_clocks_t g_clocks;
sd_info_t    g_sd;
fat_fs_t     g_fs;
app_state_t  g_app;

static void capture_reset(void)
{
    s_outn = 0;
    s_out[0] = '\0';
}

void uart_putc(char c)
{
    if (s_outn < (int)sizeof s_out - 1)
        s_out[s_outn++] = c;
    s_out[s_outn] = '\0';
}

void uart_puts(const char *s)          { while (*s) uart_putc(*s++); }
void uart_write(const void *buf, int len)
{
    const char *s = buf;
    while (len-- > 0) uart_putc(*s++);
}
int  uart_getc(void)                   { return -1; }
int  uart_getc_timeout(uint32_t ms)    { (void)ms; return -1; }
int  uart_rx_ready(void)               { return 0; }
int  uart_take_ctrlc(void)            { return 0; }
void uart_rx_flush(void)               { }
void uart_drain_tx(void)               { }

int  str_to_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (!s || !*s) return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    if (*s) return -1;
    *out = v;
    return 0;
}

uint32_t sys_uptime_ms(void)           { return 90061000UL; }
void     sys_reboot(void)              { s_rebooted = 1; }
const char *sys_reset_cause_str(void)  { return "power-on"; }
void     sys_delay_ms(uint32_t ms)     { (void)ms; }

void rtc_get(rtc_time_t *t)
{
    t->year = 2026; t->mon = 9; t->day = 22;
    t->hour = 12; t->min = 0; t->sec = 0;
}
void rtc_set(const rtc_time_t *t)      { (void)t; }

char to_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

const char *flash_err_str(int rc)     { (void)rc; return "flash error"; }

void heap_stats(uint32_t *total, uint32_t *used, uint32_t *free_bytes,
                uint32_t *largest, uint32_t *blocks)
{
    *total = 1024; *used = 128; *free_bytes = 896;
    *largest = 896; *blocks = 2;
}
uint32_t stack_used(void)              { return 40; }
uint32_t stack_peak(void)              { return 80; }

static int s_sd_on = 1;

int  sd_powered(void)              { return s_sd_on; }
int  board_power(int domain, int on)
{
    int was;

    if (domain != FREYA_PWR_SD || (on != 0 && on != 1))
        return FREYA_ERR_ARG;
    was = s_sd_on;
    if (on == was) return was;
    s_sd_on = on;
    if (!on) {
        g_sd.initialised = 0;
        g_sd.type = SD_TYPE_NONE;
        s_mounted = 0;
    }
    return was;
}
int  sd_init(void)
{
    s_sd_on = 1;
    g_sd.initialised = 1;
    g_sd.type = SD_TYPE_SDHC;
    g_sd.blocks = 2048;
    return 0;
}
const char *sd_type_str(void)
{
    return g_sd.initialised ? "SDHC" : "no card";
}

int  fat_mount(void)
{
    s_mounted = 1;
    g_fs.mounted = 1;
    g_fs.free_valid = 1;
    g_fs.free_count = 400;
    g_fs.clus_count = 1000;
    g_fs.bytes_per_clus = 4096;
    g_fs.sec_per_clus = 8;
    memcpy(g_fs.label, "FREYA", 6);
    return FAT_OK;
}
int  fat_mounted(void)                 { return s_mounted; }
void fat_unmount(void)                 { s_mounted = 0; }
const char *fat_err_str(int err)       { (void)err; return "error"; }
const char *fat_type_str(void)         { return "FAT32"; }
int  fat_free_clusters(uint32_t *n)    { *n = g_fs.free_count; return FAT_OK; }
int  fat_sync(void)                    { s_synced = 1; return FAT_OK; }
int  fat_opendir(fat_dir_t *d, const char *path)
{
    (void)d; (void)path; return FAT_ERR_INVAL;
}
int  fat_readdir(fat_dir_t *d, fat_dirent_t *e)
{
    (void)d; (void)e; return 1;
}
int  fat_closedir(fat_dir_t *d)        { (void)d; return FAT_OK; }
int  fat_stat(const char *path, fat_dirent_t *e)
{
    (void)path; (void)e; return FAT_ERR_INVAL;
}
int  fat_mkdir(const char *path)       { (void)path; return FAT_ERR_INVAL; }
int  fat_unlink(const char *path)      { (void)path; return FAT_ERR_INVAL; }

const char *fs_cwd(void)               { return "/data"; }
int  fs_abspath(const char *in, char *out, int size)
{
    (void)in; (void)out; (void)size; return -1;
}
int  fs_chdir(const char *path)        { (void)path; return FAT_ERR_INVAL; }
int  fs_rename(const char *a, const char *b)
{
    (void)a; (void)b; return FAT_ERR_INVAL;
}
void fs_close_all(void)                { }

/* One planted script file.  Every other open fails, as before. */
static const char *s_src_path;
static const char *s_src_data;
static int s_src_pos;
static int s_src_open;

static void plant_script(const char *path, const char *data)
{
    s_src_path = path;
    s_src_data = data;
    s_src_pos = 0;
    s_src_open = 0;
}

int  fs_fd_open(const char *path, int flags)
{
    (void)flags;
    if (s_src_path && path && strcmp(path, s_src_path) == 0 && s_src_data) {
        s_src_open = 1;
        s_src_pos = 0;
        return 3;
    }
    return FAT_ERR_NOENT;
}
int  fs_fd_close(int fd)
{
    if (fd == 3) s_src_open = 0;
    return FAT_OK;
}
int  fs_fd_read(int fd, void *buf, int len)
{
    int left, n;

    if (fd != 3 || !s_src_open || !s_src_data || len < 0) return 0;
    left = (int)strlen(s_src_data) - s_src_pos;
    if (left <= 0) return 0;
    n = len < left ? len : left;
    memcpy(buf, s_src_data + s_src_pos, (size_t)n);
    s_src_pos += n;
    return n;
}
int32_t fs_fd_size(int fd)
{
    if (fd != 3 || !s_src_data) return FAT_ERR_INVAL;
    return (int32_t)strlen(s_src_data);
}
int  fs_fd_write(int fd, const void *buf, int len)
{
    (void)fd; (void)buf; (void)len; return len;
}
int  fs_fd_seek(int fd, int32_t off, int whence)
{
    (void)fd; (void)off; (void)whence; return 0;
}

int  log_get_level(void)               { return 3; }
int  log_set_level(int level)          { (void)level; return 0; }
const char *log_level_str(int level)   { (void)level; return "info"; }

int  app_autostart_enabled(void)       { return 0; }
int  app_autostart_set(int enable)     { (void)enable; return 0; }
int  app_ramdump_enabled(void)         { return 0; }
int  app_ramdump_set(int enable)       { (void)enable; return 0; }
const freya_app_header_t *app_flash_header(void)
{
    static freya_app_header_t h;
    return s_installed ? &h : NULL;
}
static const char *s_flash_script;
static int s_flash_script_bad;
int app_script_find(const char **text, uint32_t *length)
{
    if (s_flash_script_bad) return -1;
    if (!s_flash_script) return 0;
    if (text) *text = s_flash_script;
    if (length) *length = (uint32_t)strlen(s_flash_script);
    return 1;
}

static char s_km[8192];
static uint32_t s_km_used;
void *kmalloc(uint32_t size)
{
    uint32_t a;

    if (size == 0) return NULL;
    a = (s_km_used + 7U) & ~7U;
    if (a + size > sizeof s_km) return NULL;
    s_km_used = a + size;
    return s_km + a;
}
void kfree(void *p)                    { (void)p; }
int  app_flash_erase(void)
{
    s_erased = 1;
    s_installed = 0;
    kprintf("ok\r\n");
    return 0;
}
int  app_load(const char *path)        { (void)path; return -1; }
int  app_install(const char *path)     { (void)path; return -1; }
int  app_run(int argc, char **argv)    { (void)argc; (void)argv; return 0; }
void app_request_stop(void)            { s_stop_requested = 1; }
int  app_should_stop(void)             { return s_stop_requested; }
int  uart_getc_nb(void)                { return -1; }
int  uart_is_raw(void)                 { return 0; }
int  uart_waiters(void)                { return 0; }
void thread_list(void)
{
    kprintf("  id  pri  state    name\r\n");
    kprintf("   0    0  running  shell\r\n");
}
int thread_stop_name(const char *name)
{
    if (name && (strcmp(name, "shell") == 0 || strcmp(name, "idle") == 0))
        return FREYA_ERR_BUSY;
    return FREYA_ERR_ARG;
}
void app_unload(void)
{
    s_unloaded = 1;
    g_app.loaded = 0;
    g_app.running = 0;
}
const char *app_stop_reason_str(int reason)
{
    (void)reason;
    return "exited";
}
int  app_last_exit(freya_exit_t *st)
{
    if (!s_have_exit) return -1;
    memset(st, 0, sizeof *st);
    memcpy(st->name, "hello", 6);
    st->status = 0;
    st->reason = FREYA_STOP_EXIT;
    st->run_ms = 15;
    return 0;
}

void led_set(int on)                   { (void)on; }
void led_toggle(void)                  { }

static int s_pin_level;
static int s_pwm_on;

int  gpio_pin_mode(int pin, int mode)  { (void)pin; (void)mode; return 0; }
int  gpio_pin_read(int pin)            { (void)pin; return s_pin_level; }
int  gpio_pin_write(int pin, int value)
{
    (void)pin;
    s_pin_level = value ? 1 : 0;
    return 0;
}
int  gpio_pin_toggle(int pin)          { (void)pin; return 0; }

int  pwm_info(int idx, pwm_info_t *info) { (void)idx; (void)info; return -1; }
int  pwm_lookup(int pin)               { (void)pin; return 0; }
int  pwm_close(int pwm)
{
    (void)pwm;
    if (!s_pwm_on) return -1;
    s_pwm_on = 0;
    return 0;
}
int  pwm_open(int pin, uint32_t hz, uint32_t duty)
{
    (void)pin; (void)hz; (void)duty;
    s_pwm_on = 1;
    return 0;
}
int  adc_read(int source)               { (void)source; return FREYA_ADC_MAX; }

static int s_timer_on;
static int s_timer_running;
static uint32_t s_timer_us;
static int s_timer_flags;
static uint32_t s_timer_count;
static freya_irq_fn s_timer_fn;
static void *s_timer_arg;

int timer_open(uint32_t us, int flags, freya_irq_fn fn, void *arg)
{
    if (us < FREYA_TIMER_MIN_US || us > FREYA_TIMER_MAX_US) return FREYA_ERR_ARG;
    if (flags & ~FREYA_TIMER_ONESHOT) return FREYA_ERR_ARG;
    if (s_timer_on) return FREYA_ERR_BUSY;
    s_timer_on = 1;
    s_timer_running = 0;
    s_timer_us = us;
    s_timer_flags = flags;
    s_timer_count = 0;
    s_timer_fn = fn;
    s_timer_arg = arg;
    return 0;
}
int timer_close(int timer)
{
    if (!s_timer_on || timer != 0) return FREYA_ERR_ARG;
    s_timer_on = 0;
    s_timer_running = 0;
    s_timer_fn = NULL;
    return 0;
}
int timer_start(int timer)
{
    if (!s_timer_on || timer != 0) return FREYA_ERR_ARG;
    s_timer_running = 1;
    return 0;
}
int timer_stop(int timer)
{
    if (!s_timer_on || timer != 0) return FREYA_ERR_ARG;
    s_timer_running = 0;
    return 0;
}
int timer_period(int timer, uint32_t us)
{
    if (!s_timer_on || timer != 0) return FREYA_ERR_ARG;
    if (us < FREYA_TIMER_MIN_US || us > FREYA_TIMER_MAX_US) return FREYA_ERR_ARG;
    s_timer_us = us;
    return 0;
}
uint32_t timer_count(int timer)
{
    if (!s_timer_on || timer != 0) return 0;
    return s_timer_count;
}
int timer_is_open(int timer)
{
    return s_timer_on && timer == 0;
}

static int s_irq_on;
static int s_irq_pin;
static int s_irq_edge;
static uint32_t s_irq_count;
static freya_irq_fn s_irq_fn;
static void *s_irq_arg;

int gpio_irq_attach(int pin, int edge, freya_irq_fn fn, void *arg)
{
    if (!(edge & FREYA_EDGE_BOTH) ||
        (edge & ~(FREYA_EDGE_BOTH | FREYA_EDGE_DEBOUNCE)))
        return FREYA_ERR_ARG;
    s_irq_on = 1;
    s_irq_pin = pin;
    s_irq_edge = edge;
    s_irq_count = 0;
    s_irq_fn = fn;
    s_irq_arg = arg;
    return 0;
}
int gpio_irq_detach(int pin)
{
    if (!s_irq_on || pin != s_irq_pin) return FREYA_ERR_ARG;
    s_irq_on = 0;
    s_irq_fn = NULL;
    return 0;
}
uint32_t gpio_irq_count(int pin)
{
    if (!s_irq_on || pin != s_irq_pin) return 0;
    return s_irq_count;
}

static void fire_timer(uint32_t count)
{
    s_timer_count = count;
    if (s_timer_fn) s_timer_fn(0, s_timer_arg);
}

static void fire_irq(uint32_t count)
{
    s_irq_count = count;
    if (s_irq_fn) s_irq_fn(s_irq_pin, s_irq_arg);
}

int  i2c_info(int idx, i2c_info_t *info) { (void)idx; (void)info; return -1; }
int  i2c_close(int bus)                { (void)bus; return 0; }
int  i2c_write(int bus, int addr, const void *buf, int len)
{
    (void)bus; (void)addr; (void)buf; (void)len; return 0;
}
int  i2c_open(int bus, uint32_t hz)    { (void)bus; (void)hz; return 0; }
int  i2c_transfer(int bus, int addr, const void *tx, int txlen,
                  void *rx, int rxlen)
{
    (void)bus; (void)addr; (void)tx; (void)txlen; (void)rx; (void)rxlen;
    return 0;
}

int  w1_info(int idx, w1_info_t *info) { (void)idx; (void)info; return -1; }
int  w1_open(int pin)                  { (void)pin; return 0; }
int  w1_close(int pin)                 { (void)pin; return 0; }
int  w1_reset(int pin)                 { (void)pin; return 0; }
int  w1_write(int pin, const void *buf, int len)
{
    (void)pin; (void)buf; (void)len; return 0;
}
int  w1_read(int pin, void *buf, int len)
{
    (void)pin; (void)buf; (void)len; return 0;
}
int  w1_search(int pin, void *rom)     { (void)pin; (void)rom; return 0; }
int  w1_pullup(int pin, int on)        { (void)pin; (void)on; return 0; }
int  w1_crc(const void *buf, int len)  { (void)buf; (void)len; return 0; }

int  spi_info(int idx, spi_info_t *info) { (void)idx; (void)info; return -1; }
int  spi_open(int bus, uint32_t hz, int mode)
{
    (void)bus; (void)hz; (void)mode; return 0;
}
int  spi_close(int bus)                { (void)bus; return 0; }
int  spi_transfer(int bus, const void *tx, void *rx, int len)
{
    (void)bus; (void)tx; (void)rx; (void)len; return 0;
}
int  spi_owns_pin(int pin)             { (void)pin; return 0; }

int xmodem_receive_to_file(const char *path, uint32_t *received, int strip)
{
    (void)path; (void)received; (void)strip; return -1;
}

static void plant_mmio(void)
{
    static const uintptr_t pages[] = {
        0xE000E000UL,   /* SCB */
        0xE0042000UL,   /* DBGMCU_IDCODE */
        (uintptr_t)UID_BASE & ~(uintptr_t)0xFFFU
                         /* unique id and flash size */
    };
    unsigned i;

    for (i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        void *p = mmap((void *)pages[i], 0x1000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
    }
    SCB->CPUID =
#if defined(FREYA_BOARD_BLUEPILL)
        0x410FC231UL;
#else
        0x410FC241UL;
#endif
#if defined(FREYA_BOARD_BLUEPILL)
    *(volatile uint32_t *)0xE0042000UL = 0x10006410UL;
#else
    *(volatile uint32_t *)0xE0042000UL = 0x10006411UL;
#endif
    ((volatile uint32_t *)UID_BASE)[0] = 0x11111111UL;
    ((volatile uint32_t *)UID_BASE)[1] = 0x22222222UL;
    ((volatile uint32_t *)UID_BASE)[2] = 0x33333333UL;
    *(volatile uint16_t *)FLASHSIZE_BASE =
#if defined(FREYA_BOARD_BLUEPILL)
        64;
#else
        512;
#endif
}

static int run(const char *cmd)
{
    char line[160];

    snprintf(line, sizeof line, "%s", cmd);
    capture_reset();
    return shell_exec(line);
}

static void pass(const char *what)
{
    checks++;
    printf("  ok    %s\n", what);
}

static void fail(const char *what)
{
    checks++;
    fails++;
    printf("  FAIL  %s\n", what);
    if (s_out[0]) {
        fputs("        --- output ---\n", stdout);
        fputs(s_out, stdout);
        fputs("        --------------\n", stdout);
    }
}

static void expect_rc(const char *what, int got, int want)
{
    if (got == want) pass(what);
    else {
        fail(what);
        printf("        status %d, wanted %d\n", got, want);
    }
}

static void expect_has(const char *what, const char *needle)
{
    if (strstr(s_out, needle)) pass(what);
    else fail(what);
}

static void expect_exact(const char *what, const char *text)
{
    if (strcmp(s_out, text) == 0) pass(what);
    else fail(what);
}

static void expect_lacks(const char *what, const char *needle)
{
    if (!strstr(s_out, needle)) pass(what);
    else fail(what);
}

/* Every listed command is one word: a line of the summary has no space
 * in the name.  Usage lines that still carry arguments are not in this
 * set; those commands print their name from the command table instead. */
static void check_summary_words(void)
{
    static const char *const names[] = {
        "sysinfo", "meminfo", "mount", "pwd", "df", "threads", "status",
        "uninstall", "uptime", "clear", "reboot", "else", "end"
    };
    static const char *const gone[] = {
        "CPU, clocks, reset, card, fs",
        "flash and RAM usage",
        "mount the SD card",
        "print directory",
        "show free space",
        "unload the program",
        "last exit status",
        "erase flash program",
        "time since reset",
        "clear the screen",
        "restart the MCU"
    };
    unsigned i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        char line[32];
        snprintf(line, sizeof line, "  %s\r\n", names[i]);
        if (strstr(s_out, line)) pass(names[i]);
        else fail(names[i]);
    }
    for (i = 0; i < sizeof gone / sizeof gone[0]; i++) {
        if (!strstr(s_out, gone[i])) pass("old phrase absent");
        else fail(gone[i]);
    }
}

int main(void)
{
    static const char *const one_word[] = {
        "sysinfo", "meminfo", "mount", "pwd", "df", "threads", "status",
        "uninstall", "uptime", "clear", "reboot", "else", "end"
    };
    unsigned i;
    int rc;

    plant_mmio();
#if defined(FREYA_BOARD_BLUEPILL)
    g_clocks.sysclk_hz = 72000000;
    g_clocks.hclk_hz = 72000000;
    g_clocks.pclk1_hz = 36000000;
    g_clocks.pclk2_hz = 72000000;
#else
    g_clocks.sysclk_hz = 96000000;
    g_clocks.hclk_hz = 96000000;
    g_clocks.pclk1_hz = 48000000;
    g_clocks.pclk2_hz = 96000000;
#endif
    g_clocks.clock_source = 1;

    printf("help\n");
    rc = run("help");
    expect_rc("help succeeds", rc, 0);
    expect_has("help introduces the list", "Freya commands:\r\n");
    expect_has("help lists stop with its argument", "  stop [thread]\r\n");
    expect_has("help lists sleep", "  sleep <ms>\r\n");
    expect_has("help lists if", "  if <command>\r\n");
    expect_has("help lists loop", "  loop <count>\r\n");
    expect_has("help lists power", "  power [sd [on|off]]\r\n");
    check_summary_words();

    printf("help <command>\n");
    for (i = 0; i < sizeof one_word / sizeof one_word[0]; i++) {
        char cmd[32], want[32];
        snprintf(cmd, sizeof cmd, "help %s", one_word[i]);
        snprintf(want, sizeof want, "%s\r\n", one_word[i]);
        rc = run(cmd);
        expect_rc(cmd, rc, 0);
        expect_exact(cmd, want);
    }
    rc = run("help ls");
    expect_rc("help ls succeeds", rc, 0);
    expect_exact("help ls names the command, then its usage",
                 "ls\r\nls [-l] [path]\r\n");
    rc = run("help stop");
    expect_rc("help stop succeeds", rc, 0);
    expect_exact("help stop names the command, then its usage",
                 "stop\r\nstop [thread]\r\n");
    rc = run("help nosuch");
    expect_rc("help of an unknown command fails", rc, FREYA_EXIT_FAIL);
    expect_has("unknown command is named", "no such command: nosuch");
    rc = run("help w1");
    expect_rc("help w1 succeeds", rc, 0);
    expect_has("help w1 names the command", "w1\r\n");
    expect_has("help w1 shows the ROM search", "search");
    rc = run("w1");
    expect_rc("w1 with no pin succeeds", rc, 0);
    expect_has("w1 asks for a pull-up", "pull the data pin up to 3.3 V");
    rc = run("help spi");
    expect_rc("help spi succeeds", rc, 0);
    expect_has("help spi names the command", "spi\r\n");
    expect_has("help spi shows a transfer", "x <byte>");
    rc = run("spi");
    expect_rc("spi with no bus succeeds", rc, 0);
    expect_has("spi names chip select", "chip select is a pin you drive");
    rc = run("help crypt");
    expect_rc("help crypt succeeds", rc, 0);
    expect_exact("help crypt names the command, then its usage",
                 "crypt\r\ncrypt [<key> <nonce> <hex>]\r\n");
    rc = run("crypt");
    expect_rc("crypt with no arguments succeeds", rc, 0);
    expect_has("crypt names the cipher", "XTEA-CTR");
    rc = run("crypt 000102030405060708090a0b0c0d0e0f "
             "4142434445464748 0000000000000000");
    expect_rc("crypt of the published vector succeeds", rc, 0);
    expect_exact("crypt prints the published ciphertext",
                 "497df3d072612cb5\r\n");
    rc = run("crypt 000102030405060708090A0B0C0D0E0F "
             "4142434445464748 497df3d072612cb5");
    expect_rc("crypt decrypts with the same command", rc, 0);
    expect_exact("crypt restores the zeros", "0000000000000000\r\n");
    rc = run("crypt 00 4142434445464748 00");
    expect_rc("a short key fails", rc, FREYA_EXIT_FAIL);
    expect_has("a short key prints the usage", "usage: crypt");

    printf("commands\n");
    rc = run("status");
    expect_rc("status succeeds", rc, 0);
    expect_has("status reports the last command", "command");
    expect_has("status before any program", "nothing has run since reset");

    rc = run("sysinfo");
    expect_rc("sysinfo succeeds", rc, 0);
#if defined(FREYA_BOARD_BLUEPILL)
    expect_has("sysinfo names the board", "Blue Pill");
    expect_has("sysinfo reads the CPUID", "410fc231");
    expect_has("sysinfo reads the device id", "0x410");
    expect_has("sysinfo reads the flash size", "128 KiB internal");
    expect_has("sysinfo reports the clock", "72000000 Hz");
#else
    expect_has("sysinfo names the board", "Black Pill");
    expect_has("sysinfo reads the CPUID", "410fc241");
    expect_has("sysinfo reads the device id", "0x411");
    expect_has("sysinfo reads the flash size", "512 KiB internal");
    expect_has("sysinfo reports the clock", "96000000 Hz");
#endif
    expect_has("sysinfo reads the unique id", "11111111-22222222-33333333");
    expect_has("sysinfo reports the reset", "power-on");
    expect_has("sysinfo before mount", "not mounted");
    expect_has("sysinfo with nothing loaded", "none loaded");

    rc = run("meminfo");
    expect_rc("meminfo succeeds", rc, 0);
    expect_has("meminfo reports flash", "Flash ");
    expect_has("meminfo reports SRAM", "SRAM  ");
    expect_has("meminfo reports the heap", "system heap");
    expect_has("meminfo with nothing loaded", "empty");

    rc = run("pwd");
    expect_rc("pwd succeeds", rc, 0);
    expect_exact("pwd prints the directory", "/data\r\n");

    rc = run("df");
    expect_rc("df before mount fails", rc, FREYA_EXIT_FAIL);
    expect_has("df asks for mount", "no filesystem mounted");

    rc = run("mount");
    expect_rc("mount succeeds", rc, 0);
    expect_has("mount names the card", "SDHC");
    expect_has("mount names the filesystem", "mounted FAT32");
    expect_has("mount names the volume", "FREYA");
    if (!s_mounted) fail("mount did not mount");
    else pass("mount mounted the card");

    rc = run("sysinfo");
    expect_rc("sysinfo after mount succeeds", rc, 0);
    expect_has("sysinfo sees the card", "SDHC");
    expect_has("sysinfo sees the filesystem", "FAT32");

    rc = run("df");
    expect_rc("df succeeds", rc, 0);
    expect_has("df reports capacity", "capacity");
    expect_has("df reports free space", "free");
    expect_has("df reports used space", "used");

    rc = run("uptime");
    expect_rc("uptime succeeds", rc, 0);
    expect_exact("uptime is time since reset", "up 1 days 01:01:01.000\r\n");

    rc = run("clear");
    expect_rc("clear succeeds", rc, 0);
    expect_exact("clear erases the screen", "\033[2J\033[H");

    rc = run("stop");
    expect_rc("stop with nothing loaded succeeds", rc, 0);
    expect_has("stop says nothing is loaded", "no program is loaded");

    g_app.loaded = 1;
    memcpy(g_app.path, "/hello.bin", 11);
    s_unloaded = 0;
    rc = run("stop");
    expect_rc("stop unloads", rc, 0);
    expect_has("stop names the program", "unloading /hello.bin");
    if (s_unloaded && !g_app.loaded) pass("stop unloaded the program");
    else fail("stop did not unload");

    g_app.running = 1;
    s_stop_requested = 0;
    rc = run("stop");
    expect_rc("stop of a running program succeeds", rc, 0);
    expect_has("stop requests a stop", "stop requested");
    if (s_stop_requested) pass("stop asked the loader to stop");
    else fail("stop did not ask the loader");
    g_app.running = 0;

    rc = run("threads");
    expect_rc("threads succeeds", rc, 0);
    expect_has("threads names the shell", "shell");
    expect_has("threads prints a header", "pri");

    rc = run("stop nosuch");
    expect_rc("stop of an unknown thread fails", rc, FREYA_EXIT_FAIL);
    expect_has("stop names the missing thread", "no thread named nosuch");

    rc = run("stop shell");
    expect_rc("stop of the shell fails", rc, FREYA_EXIT_FAIL);
    expect_has("stop refuses the shell", "shell cannot be stopped");

    rc = run("uninstall");
    expect_rc("uninstall of an empty region succeeds", rc, 0);
    expect_has("uninstall says the region is empty",
               "no program is installed in flash");

    s_installed = 1;
    s_erased = 0;
    rc = run("uninstall");
    expect_rc("uninstall erases", rc, 0);
    expect_has("uninstall finishes", "ok\r\n");
    if (s_erased && !s_installed) pass("uninstall erased the program");
    else fail("uninstall did not erase");

    s_have_exit = 1;
    g_app.runs = 1;
    rc = run("status");
    expect_rc("status after a run succeeds", rc, 0);
    expect_has("status names the program", "hello");
    expect_has("status reports the exit", "exit status");
    expect_has("status reports the run time", "15 ms");

    s_rebooted = 0;
    s_synced = 0;
    rc = run("reboot");
    expect_rc("reboot succeeds", rc, 0);
    expect_has("reboot says it is restarting", "rebooting");
    if (s_synced) pass("reboot synced the filesystem");
    else fail("reboot did not sync");
    if (s_rebooted) pass("reboot restarted the MCU");
    else fail("reboot did not restart");

    printf("scripts\n");
    rc = run("echo a; echo b");
    expect_rc("two commands succeed", rc, 0);
    expect_exact("semicolon separates commands", "a\r\nb\r\n");

    rc = run("echo \"a;b\"");
    expect_rc("quoted semicolon succeeds", rc, 0);
    expect_exact("quotes hide a semicolon", "a;b\r\n");

    rc = run("nosuch");
    expect_rc("unknown command is 127", rc, FREYA_EXIT_NOTFOUND);
    expect_has("unknown command is named", "command not found");

    rc = run("sleep");
    expect_rc("sleep without a time fails", rc, FREYA_EXIT_FAIL);
    expect_has("sleep usage", "usage: sleep <ms>");
    rc = run("sleep x");
    expect_rc("sleep with a word fails", rc, FREYA_EXIT_FAIL);
    rc = run("sleep 0");
    expect_rc("sleep 0 succeeds", rc, 0);
    expect_exact("sleep 0 prints nothing", "");
    rc = run("sleep 20");
    expect_rc("sleep succeeds", rc, 0);
    expect_exact("sleep prints nothing", "");

    rc = run("if");
    expect_rc("bare if fails", rc, FREYA_EXIT_FAIL);
    expect_exact("bare if is usage", "usage: if <command>\r\n");
    rc = run("loop");
    expect_rc("bare loop fails", rc, FREYA_EXIT_FAIL);
    expect_exact("bare loop is usage", "usage: loop <count>\r\n");
    rc = run("else");
    expect_rc("bare else fails", rc, FREYA_EXIT_FAIL);
    expect_exact("bare else is unexpected", "unexpected else\r\n");
    rc = run("echo hi; end");
    expect_rc("end at the top fails", rc, FREYA_EXIT_FAIL);
    expect_exact("end at the top runs nothing", "unexpected end\r\n");
    rc = run("if echo hi");
    expect_rc("unclosed if fails", rc, FREYA_EXIT_FAIL);
    expect_exact("unclosed if runs nothing", "missing end\r\n");
    rc = run("loop 1000001; end");
    expect_rc("huge loop fails", rc, FREYA_EXIT_FAIL);
    expect_exact("huge loop is refused", "loop: count too large\r\n");

    rc = run("if echo hi; echo THEN; else; echo ELSE; end");
    expect_rc("if of a success succeeds", rc, 0);
    expect_exact("if takes the then branch", "hi\r\nTHEN\r\n");

    rc = run("if help nosuch; echo THEN; else; echo ELSE; end");
    expect_rc("if of a failure takes else", rc, 0);
    expect_has("else branch ran", "ELSE\r\n");
    expect_lacks("then branch did not run", "THEN\r\n");

    rc = run("if help nosuch; echo THEN; end");
    expect_rc("if without else keeps the failure", rc, FREYA_EXIT_FAIL);
    expect_lacks("skipped then prints nothing", "THEN\r\n");

    rc = run("help nosuch");
    rc = run("loop 0; echo no; end");
    expect_rc("loop 0 leaves the status", rc, FREYA_EXIT_FAIL);
    expect_exact("loop 0 runs nothing", "");

    rc = run("loop $?; echo $?; end");
    expect_rc("loop of $? runs once", rc, 0);
    expect_exact("$? is the status before the loop", "1\r\n");

    rc = run("loop 2; echo tick; end");
    expect_rc("loop succeeds", rc, 0);
    expect_exact("loop repeats the body", "tick\r\ntick\r\n");

    rc = run("loop 2; sleep 5; echo s; end");
    expect_rc("sleep in a loop succeeds", rc, 0);
    expect_exact("sleep does not hide the body", "s\r\ns\r\n");

    rc = run("if echo hi\necho THEN\nelse\necho ELSE\nend");
    expect_rc("multi-line if succeeds", rc, 0);
    expect_exact("newlines separate like semicolons", "hi\r\nTHEN\r\n");

    rc = run("loop 2; if help nosuch; echo T; else; echo E; end; end");
    expect_rc("loop of if succeeds", rc, 0);
    expect_exact("else ran on each pass",
                 "help: no such command: nosuch\r\nE\r\n"
                 "help: no such command: nosuch\r\nE\r\n");
    expect_lacks("then did not run inside the loop", "\r\nT\r\n");

    rc = run("if echo a; if help nosuch; echo innerT; else; echo innerE; end; "
             "else; echo outerE; end");
    expect_rc("nested if succeeds", rc, 0);
    expect_has("inner else ran", "innerE\r\n");
    expect_lacks("inner then did not run", "innerT\r\n");
    expect_lacks("outer else did not run", "outerE\r\n");

    rc = run("if echo; if echo; if echo; if echo; if echo; if echo; "
             "if echo; if echo; if echo; end; end; end; end; end; end; "
             "end; end; end");
    expect_rc("nine nested blocks fail", rc, FREYA_EXIT_FAIL);
    expect_exact("nesting has a limit", "too many nested blocks\r\n");

    printf("source\n");
    rc = run("echo hi # there");
    expect_rc("comment after a command succeeds", rc, 0);
    expect_exact("a hash after a space is a comment", "hi\r\n");
    rc = run("echo \"hi # there\"");
    expect_rc("hash inside quotes succeeds", rc, 0);
    expect_exact("quotes hide a hash", "hi # there\r\n");
    rc = run("# only a comment\necho z");
    expect_rc("a comment line succeeds", rc, 0);
    expect_exact("a comment line is skipped", "z\r\n");

    rc = run("source");
    expect_rc("source without a path fails", rc, FREYA_EXIT_FAIL);
    expect_exact("source usage", "usage: source <file>|@flash\r\n");
    rc = run("help source");
    expect_rc("help source succeeds", rc, 0);
    expect_exact("help source names the command, then its usage",
                 "source\r\nsource <file>|@flash\r\n");

    fat_unmount();
    rc = run("source /t.sh");
    expect_rc("source before mount fails", rc, FREYA_EXIT_FAIL);
    expect_has("source asks for mount", "no filesystem mounted");
    rc = run("mount");
    expect_rc("mount after source succeeds", rc, 0);

    plant_script(NULL, NULL);
    rc = run("source /missing.sh");
    expect_rc("source of a missing file fails", rc, FREYA_EXIT_FAIL);
    expect_has("source names the missing file", "source: /missing.sh:");

    plant_script("/t.sh", "echo fromfile\n");
    rc = run("source /t.sh");
    expect_rc("source of a file succeeds", rc, 0);
    expect_exact("source runs the file", "fromfile\r\n");

    plant_script("/t.sh", "echo a; echo b\n");
    rc = run("source /t.sh");
    expect_rc("source of two commands succeeds", rc, 0);
    expect_exact("source splits on a semicolon", "a\r\nb\r\n");

    plant_script("/t.sh", "# setup\nif echo hi\necho THEN\nelse\necho ELSE\nend\n");
    rc = run("source /t.sh");
    expect_rc("source of a multi-line if succeeds", rc, 0);
    expect_exact("source runs the then branch", "hi\r\nTHEN\r\n");

    plant_script("/t.sh", "loop 2\necho tick\nend\n");
    rc = run("source /t.sh");
    expect_rc("source of a loop succeeds", rc, 0);
    expect_exact("source repeats the loop", "tick\r\ntick\r\n");

    plant_script("/t.sh", "echo hi\x01\n");
    rc = run("source /t.sh");
    expect_rc("source of a binary file fails", rc, FREYA_EXIT_FAIL);
    expect_exact("source refuses a binary file",
                 "source: /t.sh: not a shell script\r\n");

    {
        static char big[FREYA_SCRIPT_FILE_MAX + 2];
        memset(big, 'a', FREYA_SCRIPT_FILE_MAX + 1);
        big[FREYA_SCRIPT_FILE_MAX + 1] = '\0';
        plant_script("/t.sh", big);
        rc = run("source /t.sh");
        expect_rc("source of a long file fails", rc, FREYA_EXIT_FAIL);
        expect_exact("source refuses a long file", "source: script too long\r\n");
    }

    {
        static char line[200];
        memset(line, 'b', sizeof line - 1);
        line[sizeof line - 1] = '\0';
        plant_script("/t.sh", line);
        rc = run("source /t.sh");
        expect_rc("source of a long line fails", rc, FREYA_EXIT_FAIL);
        expect_exact("source refuses a long line", "line too long\r\n");
    }

    plant_script("/t.sh", "help nosuch\n");
    rc = run("source /t.sh");
    expect_rc("source returns the script status", rc, FREYA_EXIT_FAIL);
    expect_has("source ran the failing command", "no such command");
    rc = run("echo $?");
    expect_exact("source leaves $? set", "1\r\n");

    plant_script("/t.sh", "source /t.sh\n");
    rc = run("source /t.sh");
    expect_rc("source of itself fails", rc, FREYA_EXIT_FAIL);
    expect_exact("source refuses deep nesting",
                 "source: scripts nest too deeply\r\n");

    rc = run("source @flash");
    expect_rc("source of an empty flash region fails", rc, FREYA_EXIT_FAIL);
    expect_exact("source says the flash is empty",
                 "source: no script in flash\r\n");

    s_installed = 1;
    rc = run("source @flash");
    expect_rc("source of a flash program fails", rc, FREYA_EXIT_FAIL);
    expect_exact("source points at runflash",
                 "source: @flash is a program - 'runflash'\r\n");
    s_installed = 0;

    s_flash_script_bad = 1;
    rc = run("source @flash");
    expect_rc("source of a damaged script fails", rc, FREYA_EXIT_FAIL);
    expect_exact("source names a damaged script",
                 "source: script in flash is damaged\r\n");
    s_flash_script_bad = 0;

    s_flash_script = "echo fromflash\n";
    fat_unmount();
    rc = run("source @flash");
    expect_rc("source from flash needs no card", rc, 0);
    expect_exact("source runs the flash script", "fromflash\r\n");
    rc = run("mount");
    expect_rc("mount after flash source succeeds", rc, 0);

    s_flash_script = "echo flashside\n";
    plant_script("/t.sh", "source @flash\n");
    rc = run("source /t.sh");
    expect_rc("a file script can source flash", rc, 0);
    expect_exact("the flash script ran from the file", "flashside\r\n");
    s_flash_script = NULL;

    rc = run("power");
    expect_rc("power succeeds", rc, 0);
    expect_has("power reports the socket", "sd on\r\n");
    expect_has("power prints its usage", "usage: power [sd [on|off]]");

    rc = run("power sd");
    expect_rc("power sd succeeds", rc, 0);
    expect_exact("power sd names the state", "sd on\r\n");

    rc = run("power sd off");
    expect_rc("power sd off succeeds", rc, 0);
    expect_exact("power sd off says so", "sd off\r\n");
    rc = run("sysinfo");
    expect_has("sysinfo reports the socket off", "power off\r\n");
    expect_has("power off unmounts", "not mounted");

    rc = run("power sd on");
    expect_rc("power sd on succeeds", rc, 0);
    expect_exact("power sd on says so", "sd on\r\n");
    rc = run("df");
    expect_rc("power on does not mount", rc, FREYA_EXIT_FAIL);

    rc = run("power led on");
    expect_rc("power of an unknown domain fails", rc, FREYA_EXIT_FAIL);
    expect_has("power prints the usage", "usage: power [sd [on|off]]");

    rc = run("mount");
    expect_rc("mount after power on succeeds", rc, 0);

    printf("variables\n");
    rc = run("set");
    expect_rc("set with no name succeeds", rc, 0);
    expect_exact("set lists nothing yet", "no variables\r\n");

    rc = run("set n 1 + 2 * 3");
    expect_rc("set of an integer succeeds", rc, 0);
    expect_exact("set prints nothing", "");
    rc = run("echo $n");
    expect_rc("echo of a variable succeeds", rc, 0);
    expect_exact("integer arithmetic binds * tighter", "7\r\n");

    rc = run("set n $n + 1");
    expect_rc("set from a variable succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("the variable updated", "8\r\n");

    rc = run("set q 7 / 2");
    expect_rc("integer division succeeds", rc, 0);
    rc = run("echo $q");
    expect_exact("integer division truncates", "3\r\n");
    rc = run("set q 7 % 2");
    rc = run("echo $q");
    expect_exact("remainder is an integer", "1\r\n");

    rc = run("set b 0xF0 & 0x3C");
    rc = run("echo $b");
    expect_exact("bitwise and", "48\r\n");
    rc = run("set b 1 << 4");
    rc = run("echo $b");
    expect_exact("left shift", "16\r\n");
    rc = run("set b ~0");
    rc = run("echo $b");
    expect_exact("bitwise not", "-1\r\n");

    rc = run("set x 1 / 2");
    rc = run("set x 7.5 / 2");
    expect_rc("float division succeeds", rc, 0);
    rc = run("echo $x");
    expect_exact("float division", "3.75\r\n");

    rc = run("set s \"hello\" + \" \" + \"there\"");
    expect_rc("string concatenation succeeds", rc, 0);
    rc = run("echo $s");
    expect_exact("strings join with +", "hello there\r\n");
    rc = run("set s \"%d %s\" $n $s");
    rc = run("echo $s");
    expect_exact("a format takes the following values", "8 hello there\r\n");

    rc = run("if $n == 8; echo yes; else; echo no; end");
    expect_rc("if of == succeeds", rc, 0);
    expect_exact("== is true", "yes\r\n");
    rc = run("if $n /= 8; echo yes; else; echo no; end");
    expect_rc("if of /= succeeds", rc, 0);
    expect_exact("/= is false", "no\r\n");
    rc = run("if $s == \"8 hello there\"; echo yes; else; echo no; end");
    expect_exact("strings compare with ==", "yes\r\n");
    rc = run("if 1 == 1.0; echo yes; else; echo no; end");
    expect_exact("an integer matches the same float", "yes\r\n");

    rc = run("loop $n; echo .; end");
    expect_rc("loop of a variable succeeds", rc, 0);
    expect_exact("the count is the variable", ".\r\n.\r\n.\r\n.\r\n.\r\n.\r\n.\r\n.\r\n");

    rc = run("set");
    expect_has("set lists the integer", "n = 8\r\n");
    expect_has("set lists the float", "x = 3.75\r\n");
    expect_has("set lists the string", "s = \"8 hello there\"\r\n");

    rc = run("unset s");
    expect_rc("unset succeeds", rc, 0);
    rc = run("echo $s");
    expect_rc("a removed variable fails", rc, FREYA_EXIT_FAIL);
    expect_has("the name is gone", "no such variable");

    rc = run("if $n > 2; echo hi; else; echo lo; end");
    expect_rc("if of > succeeds", rc, 0);
    expect_exact("> is true", "hi\r\n");
    rc = run("if $n < 2; echo hi; else; echo lo; end");
    expect_rc("if of < succeeds", rc, 0);
    expect_exact("< is false", "lo\r\n");
    rc = run("if 1.5 > 1; echo y; else; echo n; end");
    expect_exact("a float compares with an integer", "y\r\n");
    rc = run("if $n >< 8; echo y; else; echo n; end");
    expect_exact(">< is false when the numbers match", "n\r\n");
    rc = run("if $n >< 1; echo y; else; echo n; end");
    expect_exact(">< is true when the numbers differ", "y\r\n");
    rc = run("if \"a\" > \"b\"; echo y; else; echo n; end");
    expect_rc("ordering a string fails", rc, 0);
    expect_has("ordering wants a number", "not a number");

    rc = run("break");
    expect_rc("break outside a loop fails", rc, FREYA_EXIT_FAIL);
    expect_exact("break outside a loop runs nothing", "unexpected break\r\n");
    rc = run("loop 1; break 1; end");
    expect_rc("break with an argument fails", rc, FREYA_EXIT_FAIL);
    expect_exact("break takes nothing", "usage: break\r\n");

    rc = run("set k 0; loop 4; set k $k + 1; if $k < 3; echo $k; else; break; end; end");
    expect_rc("break in else succeeds", rc, 0);
    expect_exact("break leaves the loop", "1\r\n2\r\n");

    rc = run("loop 2; loop 2; break; end; echo x; end");
    expect_rc("break leaves only the inner loop", rc, 0);
    expect_exact("the outer loop continues", "x\r\nx\r\n");

    rc = run("set n 1 / 0");
    expect_rc("division by zero fails", rc, FREYA_EXIT_FAIL);
    expect_has("division by zero is named", "division by zero");
    rc = run("set n 1.5 & 1");
    expect_rc("a float bit operation fails", rc, FREYA_EXIT_FAIL);
    expect_has("bit operations want an integer", "not an integer");

    printf("functions\n");
    rc = run("fn");
    expect_rc("fn with no name succeeds", rc, 0);
    expect_exact("fn lists nothing yet", "no functions\r\n");

    rc = run("return 1");
    expect_rc("return outside a function fails", rc, FREYA_EXIT_FAIL);
    expect_exact("return outside a function runs nothing", "unexpected return\r\n");

    rc = run("fn add; return $1 + $2; end");
    expect_rc("fn definition succeeds", rc, 0);
    rc = run("set n add(2, 3)");
    expect_rc("a call in an expression succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("the function returned the sum", "5\r\n");

    rc = run("fn id; return $1; end");
    rc = run("set n add(id(4), 1)");
    expect_rc("a nested call succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("the outer call saw the inner value", "5\r\n");

    rc = run("fn narg; return $0; end");
    rc = run("set n narg()");
    rc = run("echo $n");
    expect_exact("a call with no arguments has count 0", "0\r\n");
    rc = run("set n narg(7, 8, 9)");
    rc = run("echo $n");
    expect_exact("$0 is how many arguments were passed", "3\r\n");

    rc = run("fn hi; return \"x\" + $1; end");
    rc = run("set s hi(\"y\")");
    rc = run("echo $s");
    expect_exact("a string argument is a value", "xy\r\n");

    rc = run("set n add(1)");
    expect_rc("a missing argument fails", rc, FREYA_EXIT_FAIL);
    expect_has("the missing argument is named", "no such argument");

    rc = run("if add(1, 2) == 3; echo yes; else; echo no; end");
    expect_rc("if of a call succeeds", rc, 0);
    expect_exact("the call is the condition value", "yes\r\n");

    rc = run("fn narg; end");
    rc = run("set n narg()");
    rc = run("echo $n");
    expect_exact("no return leaves 0", "0\r\n");

    rc = run("fn add; return $1; end");
    rc = run("set n add(9, 1)");
    rc = run("echo $n");
    expect_exact("defining the same name replaces the body", "9\r\n");

    rc = run("fn hi; echo $1; return $1; end");
    rc = run("set n hi(4)");
    expect_exact("the body ran, then returned", "4\r\n");

    rc = run("fn");
    expect_has("fn lists a function", "add\r\n");
    expect_has("fn lists another function", "id\r\n");

    rc = run("set n $1");
    expect_rc("an argument outside a call fails", rc, FREYA_EXIT_FAIL);
    expect_has("the argument is not there", "no such argument");

    rc = run("set n nosuch(1)");
    expect_rc("an unknown function fails", rc, FREYA_EXIT_FAIL);
    expect_has("the unknown function is named", "no such function: nosuch");

    rc = run("fn if; return 1; end");
    expect_rc("a reserved name fails", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses a reserved name", "bad name");

    {
        char line[160];
        int a;
        strcpy(line, "set n narg(");
        for (a = 0; a < 33; a++) {
            if (a) strcat(line, ",");
            strcat(line, "1");
        }
        strcat(line, ")");
        rc = run(line);
        expect_rc("33 arguments fails", rc, FREYA_EXIT_FAIL);
        expect_has("32 is the limit", "too many arguments");
    }

    rc = run("set n get(\"PB0\")");
    expect_rc("get of a pin succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("get reads the pin", "0\r\n");
    rc = run("set n set(\"PB5\", 1)");
    expect_rc("set of a pin succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("set returns the level read back", "1\r\n");
    rc = run("set n get(\"PB5\")");
    rc = run("echo $n");
    expect_exact("get sees the level that was written", "1\r\n");

    rc = run("set n adc(\"temp\")");
    expect_rc("adc of the temperature source succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("adc returns the raw count", "4095\r\n");
    rc = run("set n adc(\"nope\")");
    expect_rc("adc of a bad name fails", rc, FREYA_EXIT_FAIL);
    expect_has("adc names a bad pin", "not a pin");

    rc = run("set n pwm(\"PB6\", 1000, 25)");
    expect_rc("pwm start succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("pwm returns the rate", "1000\r\n");
    rc = run("set n pwm(\"PB6\", 1000, 7.5)");
    expect_rc("pwm accepts a fractional duty", rc, 0);
    rc = run("set n pwm(\"PB6\")");
    expect_rc("pwm stop succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("pwm stop returns 0", "0\r\n");
    rc = run("set n pwm(\"PB6\")");
    expect_rc("pwm stop of an idle channel fails", rc, FREYA_EXIT_FAIL);
    expect_has("pwm says the channel is idle", "is not running");

    printf("conversions\n");
    rc = run("set n int(\"42\")");
    expect_rc("int of a decimal string succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("int parses decimal text", "42\r\n");
    rc = run("set n int(\"-42\")");
    rc = run("echo $n");
    expect_exact("int keeps a leading minus", "-42\r\n");
    rc = run("set n int(1.9)");
    rc = run("echo $n");
    expect_exact("int truncates a float toward zero", "1\r\n");
    rc = run("set n int(-1.9)");
    rc = run("echo $n");
    expect_exact("int truncates a negative float toward zero", "-1\r\n");
    rc = run("set n int(\"1.9\")");
    rc = run("echo $n");
    expect_exact("int of float text truncates", "1\r\n");
    rc = run("set n int(\"0x10\")");
    rc = run("echo $n");
    expect_exact("int parses a 0x string", "16\r\n");
    rc = run("set n int(\"0xFFFFFFFF\")");
    rc = run("echo $n");
    expect_exact("int keeps the high bit of a hex string", "-1\r\n");
    rc = run("set n 0xFFFFFFFF");
    rc = run("echo $n");
    expect_exact("a hex literal keeps all 32 bits", "-1\r\n");
    rc = run("set n int(\"-2147483648\")");
    rc = run("echo $n");
    expect_exact("int accepts the most negative integer", "-2147483648\r\n");

    rc = run("set x float(\"1.5\")");
    expect_rc("float of text succeeds", rc, 0);
    rc = run("echo $x");
    expect_exact("float parses decimal text", "1.5\r\n");
    rc = run("set x float(2)");
    rc = run("echo $x");
    expect_exact("float widens an integer", "2\r\n");
    rc = run("set x float(\"-2.5\")");
    rc = run("echo $x");
    expect_exact("float keeps a leading minus", "-2.5\r\n");
    rc = run("set x float(\"0x10\")");
    rc = run("echo $x");
    expect_exact("float of a hex string is that integer", "16\r\n");

    rc = run("set s str(255)");
    rc = run("echo $s");
    expect_exact("str renders an integer", "255\r\n");
    rc = run("set s str(1.5)");
    rc = run("echo $s");
    expect_exact("str renders a float", "1.5\r\n");
    rc = run("set s str(\"ab\")");
    rc = run("echo $s");
    expect_exact("str leaves a string as it is", "ab\r\n");

    rc = run("set s hex(255)");
    rc = run("echo $s");
    expect_exact("hex renders lowercase digits", "ff\r\n");
    rc = run("set n hex(\"ff\")");
    rc = run("echo $n");
    expect_exact("hex parses digits", "255\r\n");
    rc = run("set n hex(\"0xFF\")");
    rc = run("echo $n");
    expect_exact("hex accepts a 0x prefix", "255\r\n");
    rc = run("set s hex(-1)");
    rc = run("echo $s");
    expect_exact("hex of a negative is the 32-bit pattern", "ffffffff\r\n");
    rc = run("set n hex($s)");
    rc = run("echo $n");
    expect_exact("hex of that text is the same integer", "-1\r\n");
    rc = run("set s hex(255.9)");
    rc = run("echo $s");
    expect_exact("hex truncates a float first", "ff\r\n");
    rc = run("set n int(float(hex(\"10\")))");
    rc = run("echo $n");
    expect_exact("int, float and hex compose", "16\r\n");

    rc = run("set n int(\"zz\")");
    expect_rc("int of junk fails", rc, FREYA_EXIT_FAIL);
    expect_has("junk is not a number", "not a number");
    rc = run("set n int(\"9999999999\")");
    expect_rc("an integer past 32 bits fails", rc, FREYA_EXIT_FAIL);
    expect_has("the overflow is named", "integer overflow");
    rc = run("set n int()");
    expect_rc("int with no argument fails", rc, FREYA_EXIT_FAIL);
    expect_has("int wants one value", "bad expression");
    rc = run("set n hex(\"0x100000000\")");
    expect_rc("hex past 32 bits fails", rc, FREYA_EXIT_FAIL);
    expect_has("hex overflow is named", "integer overflow");

    printf("random\n");
    rc = run("set n srand(1)");
    expect_rc("srand succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("srand returns 0", "0\r\n");
    rc = run("set n rand()");
    expect_rc("rand succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("the first value after seed 1", "16838\r\n");
    rc = run("set n rand()");
    rc = run("echo $n");
    expect_exact("the second value after seed 1", "5758\r\n");
    rc = run("set n rand()");
    rc = run("echo $n");
    expect_exact("the third value after seed 1", "10113\r\n");
    rc = run("set n srand(1)");
    rc = run("set n rand()");
    rc = run("echo $n");
    expect_exact("the same seed repeats", "16838\r\n");
    rc = run("set n srand(0)");
    rc = run("set n rand()");
    rc = run("echo $n");
    expect_exact("seed 0 yields 0", "0\r\n");
    rc = run("set n srand(-1)");
    rc = run("set n rand()");
    rc = run("echo $n");
    expect_exact("a negative seed is the bit pattern", "15929\r\n");
    rc = run("set n rand(1)");
    expect_rc("rand takes no argument", rc, FREYA_EXIT_FAIL);
    expect_has("rand wants nothing", "bad expression");
    rc = run("set n srand()");
    expect_rc("srand needs a seed", rc, FREYA_EXIT_FAIL);
    expect_has("srand wants an integer", "bad expression");
    rc = run("set n srand(1.5)");
    expect_rc("srand refuses a float", rc, FREYA_EXIT_FAIL);
    printf("trigonometry\n");
    rc = run("set x pi()");
    expect_rc("pi() succeeds", rc, 0);
    rc = run("echo $x");
    expect_exact("pi() prints to four places", "3.1416\r\n");
    rc = run("set x pi");
    expect_rc("pi needs parentheses", rc, FREYA_EXIT_FAIL);
    expect_has("bare pi is not a value", "bad expression");
    rc = run("set x sin(0)");
    rc = run("echo $x");
    expect_exact("sin of 0 is 0", "0\r\n");
    rc = run("set x sin(pi() / 2)");
    rc = run("echo $x");
    expect_exact("sin of pi/2 is 1", "1\r\n");
    rc = run("set x cos(0)");
    rc = run("echo $x");
    expect_exact("cos of 0 is 1", "1\r\n");
    rc = run("set x cos(pi())");
    rc = run("echo $x");
    expect_exact("cos of pi is -1", "-1\r\n");
    rc = run("set x sin(1)");
    rc = run("echo $x");
    expect_exact("sin of 1 radian", "0.8415\r\n");
    rc = run("set x cos(1)");
    rc = run("echo $x");
    expect_exact("cos of 1 radian", "0.5403\r\n");
    rc = run("set x sin(-pi() / 2)");
    rc = run("echo $x");
    expect_exact("sin is odd", "-1\r\n");
    rc = run("set x sin(\"a\")");
    expect_rc("sin of a string fails", rc, FREYA_EXIT_FAIL);
    expect_has("sin wants a number", "bad expression");
    rc = run("set x sin()");
    expect_rc("sin needs an angle", rc, FREYA_EXIT_FAIL);
    rc = run("set x pi(1)");
    expect_rc("pi takes no argument", rc, FREYA_EXIT_FAIL);
    expect_has("pi wants nothing", "bad expression");
    rc = run("fn sin; return 1; end");
    expect_rc("sin cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses sin", "bad name");

    rc = run("fn rand; return 1; end");
    expect_rc("rand cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses rand", "bad name");

    rc = run("fn get; return 1; end");
    expect_rc("a built-in name cannot be defined", rc, FREYA_EXIT_FAIL);
    rc = run("fn int; return 1; end");
    expect_rc("int cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses int", "bad name");

    printf("datetime\n");
    rc = run("set n now()");
    expect_rc("now succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("now is the stubbed clock", "1790078400\r\n");
    rc = run("set s date()");
    rc = run("echo $s");
    expect_exact("date formats the clock", "2026-09-22 12:00:00\r\n");
    rc = run("set n year()");
    rc = run("echo $n");
    expect_exact("year reads the clock", "2026\r\n");
    rc = run("set n month()");
    rc = run("echo $n");
    expect_exact("month reads the clock", "9\r\n");
    rc = run("set n day()");
    rc = run("echo $n");
    expect_exact("day reads the clock", "22\r\n");
    rc = run("set n hour()");
    rc = run("echo $n");
    expect_exact("hour reads the clock", "12\r\n");
    rc = run("set n minute()");
    rc = run("echo $n");
    expect_exact("minute reads the clock", "0\r\n");
    rc = run("set n second()");
    rc = run("echo $n");
    expect_exact("second reads the clock", "0\r\n");
    rc = run("set n time(1970, 1, 1, 0, 0, 0)");
    rc = run("echo $n");
    expect_exact("the epoch is zero", "0\r\n");
    rc = run("set n time(2026, 1, 1, 0, 0, 0)");
    rc = run("echo $n");
    expect_exact("2026-01-01 is the boot instant", "1767225600\r\n");
    rc = run("set s date(0)");
    rc = run("echo $s");
    expect_exact("date formats the epoch", "1970-01-01 00:00:00\r\n");
    rc = run("set n time(2024, 2, 29, 0, 0, 0)");
    rc = run("set s day($n)");
    rc = run("echo $s");
    expect_exact("a leap day is the 29th", "29\r\n");
    rc = run("set s month($n)");
    rc = run("echo $s");
    expect_exact("a leap day stays in February", "2\r\n");
    rc = run("set s time(2024, 3, 1, 0, 0, 0) - $n");
    rc = run("echo $s");
    expect_exact("March follows the leap day", "86400\r\n");
    rc = run("set n time(2038, 1, 19, 3, 14, 7)");
    rc = run("echo $n");
    expect_exact("the last signed instant", "2147483647\r\n");
    rc = run("set n time(2038, 1, 19, 3, 14, 8)");
    expect_rc("the next second does not fit", rc, FREYA_EXIT_FAIL);
    expect_has("past 2038 is overflow", "integer overflow");
    rc = run("set n time(2023, 2, 29, 0, 0, 0)");
    expect_rc("a non-leap February 29 fails", rc, FREYA_EXIT_FAIL);
    expect_has("a missing day is a bad date", "bad expression");
    rc = run("set n time(1969, 12, 31, 23, 59, 59)");
    expect_rc("a year before 1970 fails", rc, FREYA_EXIT_FAIL);
    expect_has("before the epoch is a bad date", "bad expression");
    rc = run("set s date(-1)");
    expect_rc("a negative count fails", rc, FREYA_EXIT_FAIL);
    expect_has("a negative count is overflow", "integer overflow");
    rc = run("set n now(1)");
    expect_rc("now takes no argument", rc, FREYA_EXIT_FAIL);
    expect_has("now wants nothing", "bad expression");
    rc = run("set n time(2026, 1, 1, 0, 0)");
    expect_rc("time wants six fields", rc, FREYA_EXIT_FAIL);
    rc = run("set n year(1.5)");
    expect_rc("a field refuses a float", rc, FREYA_EXIT_FAIL);
    expect_has("a field wants an integer", "bad expression");
    rc = run("fn now; return 1; end");
    expect_rc("now cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses now", "bad name");
    rc = run("fn date; return 1; end");
    expect_rc("date cannot be defined", rc, FREYA_EXIT_FAIL);

    printf("timers and interrupts\n");
    rc = run("set n ticks()");
    expect_rc("ticks succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("ticks is milliseconds since boot", "90061000\r\n");
    rc = run("fn ticks; return 1; end");
    expect_rc("ticks cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses ticks", "bad name");

    rc = run("fn hi; echo tick; return $1; end");
    expect_rc("a timer function can be defined", rc, 0);
    rc = run("set n timer(1000, 0, \"hi\")");
    expect_rc("timer arms a periodic timer", rc, 0);
    rc = run("echo $n");
    expect_exact("timer returns the handle", "0\r\n");
    if (s_timer_us == 1000) pass("the period was passed through");
    else fail("the period was passed through");
    if (s_timer_flags == 0) pass("the flags were periodic");
    else fail("the flags were periodic");
    if (s_timer_running) pass("the timer was started");
    else fail("the timer was started");
    fire_timer(4);
    rc = run("set n wait(50)");
    expect_rc("wait delivers the timer function", rc, 0);
    expect_has("the timer function ran", "tick\r\n");
    rc = run("echo $n");
    expect_exact("wait returns 0 when an event arrived", "0\r\n");
    rc = run("set n tcount(0)");
    rc = run("echo $n");
    expect_exact("tcount reads the expiries", "4\r\n");
    rc = run("set n tperiod(0, 2000)");
    expect_rc("tperiod changes the period", rc, 0);
    if (s_timer_us == 2000) pass("the new period was stored");
    else fail("the new period was stored");
    rc = run("set n tstop(0)");
    expect_rc("tstop stops the timer", rc, 0);
    if (!s_timer_running) pass("the timer is stopped");
    else fail("the timer is stopped");
    rc = run("set n tstart(0)");
    expect_rc("tstart starts it again", rc, 0);
    if (s_timer_running) pass("the timer is running");
    else fail("the timer is running");
    rc = run("set n tclose(0)");
    expect_rc("tclose closes the timer", rc, 0);
    rc = run("set n tcount(0)");
    expect_rc("a closed timer has no count", rc, FREYA_EXIT_FAIL);
    expect_has("a closed timer is out of range", "out of range");

    rc = run("set n timer(250, 1)");
    expect_rc("a oneshot timer opens", rc, 0);
    if (s_timer_flags == FREYA_TIMER_ONESHOT) pass("oneshot is the flag bit");
    else fail("oneshot is the flag bit");
    rc = run("set n tclose(0)");
    rc = run("set n timer(1)");
    expect_rc("a period below the minimum fails", rc, FREYA_EXIT_FAIL);
    expect_has("a short period is out of range", "out of range");
    rc = run("set n wait(20)");
    expect_rc("wait with nothing pending succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("a timeout is -1", "-1\r\n");

    rc = run("fn id; echo edge; return $1; end");
    rc = run("set n irq(\"PB0\", 2, \"id\")");
    expect_rc("irq arms a falling edge", rc, 0);
    if (s_irq_edge == FREYA_EDGE_FALLING) pass("the edge was falling");
    else fail("the edge was falling");
    fire_irq(2);
    rc = run("set n wait(50)");
    expect_rc("wait delivers the pin function", rc, 0);
    expect_has("the pin function ran", "edge\r\n");
    rc = run("set n irq(\"PB0\")");
    rc = run("echo $n");
    expect_exact("irq with one argument is the edge count", "2\r\n");
    rc = run("set n irq(\"PB0\", 0)");
    expect_rc("irq with edge 0 detaches", rc, 0);
    if (!s_irq_on) pass("the line is free");
    else fail("the line is free");
    rc = run("set n irq(\"nope\", 1)");
    expect_rc("irq of a bad pin fails", rc, FREYA_EXIT_FAIL);
    expect_has("irq names a bad pin", "not a pin");
    rc = run("fn irq; return 1; end");
    expect_rc("irq cannot be defined", rc, FREYA_EXIT_FAIL);

    rc = run("fn a; break; end");
    expect_rc("break in a function fails", rc, FREYA_EXIT_FAIL);
    expect_exact("break does not cross a function", "unexpected break\r\n");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
