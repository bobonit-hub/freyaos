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
void vol_use(int dev)                  { (void)dev; }
int  spiflash_mounted(void)            { return 0; }
void spiflash_df(void)                 { }
void spiflash_info(void)               { }
int  spiflash_mount_cmd(void)          { return -1; }

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
void fs_close_all(void);

/* One planted script file on fd 3.  Three more files live in RAM so the
 * shell's open/read/write calls can be checked without a card image. */
static const char *s_src_path;
static const char *s_src_data;
static int s_src_pos;
static int s_src_open;

#define RAM_N   3
#define RAM_CAP 160

typedef struct {
    int used, open, flags;
    char path[40];
    char data[RAM_CAP];
    int len, pos;
} ram_file_t;

static ram_file_t s_ram[RAM_N];

static void plant_script(const char *path, const char *data)
{
    s_src_path = path;
    s_src_data = data;
    s_src_pos = 0;
    s_src_open = 0;
}

static int ram_lookup(const char *path)
{
    int i;

    if (!path) return -1;
    for (i = 0; i < RAM_N; i++)
        if (s_ram[i].used && strcmp(s_ram[i].path, path) == 0) return i;
    return -1;
}

int  fs_fd_open(const char *path, int flags)
{
    int i;

    if (s_src_path && path && strcmp(path, s_src_path) == 0 && s_src_data) {
        s_src_open = 1;
        s_src_pos = 0;
        return 3;
    }
    i = ram_lookup(path);
    if (i < 0) {
        if (!(flags & FREYA_O_CREATE)) return FAT_ERR_NOENT;
        for (i = 0; i < RAM_N; i++)
            if (!s_ram[i].used) break;
        if (i == RAM_N) return FAT_ERR_NOSPC;
        memset(&s_ram[i], 0, sizeof s_ram[i]);
        snprintf(s_ram[i].path, sizeof s_ram[i].path, "%s", path ? path : "");
        s_ram[i].used = 1;
    }
    if (s_ram[i].open) return FAT_ERR_INVAL;
    if (flags & FREYA_O_TRUNC) s_ram[i].len = 0;
    s_ram[i].flags = flags;
    s_ram[i].pos = (flags & FREYA_O_APPEND) ? s_ram[i].len : 0;
    s_ram[i].open = 1;
    return i;
}
int  fs_fd_close(int fd)
{
    if (fd == 3) {
        s_src_open = 0;
        return FAT_OK;
    }
    if (fd < 0 || fd >= RAM_N || !s_ram[fd].open) return FAT_ERR_INVAL;
    s_ram[fd].open = 0;
    return FAT_OK;
}
int  fs_fd_read(int fd, void *buf, int len)
{
    int left, n;

    if (fd == 3) {
        if (!s_src_open || !s_src_data || len < 0) return 0;
        left = (int)strlen(s_src_data) - s_src_pos;
        if (left <= 0) return 0;
        n = len < left ? len : left;
        memcpy(buf, s_src_data + s_src_pos, (size_t)n);
        s_src_pos += n;
        return n;
    }
    if (fd < 0 || fd >= RAM_N || !s_ram[fd].open || len < 0) return FAT_ERR_INVAL;
    left = s_ram[fd].len - s_ram[fd].pos;
    if (left <= 0) return 0;
    n = len < left ? len : left;
    memcpy(buf, s_ram[fd].data + s_ram[fd].pos, (size_t)n);
    s_ram[fd].pos += n;
    return n;
}
int32_t fs_fd_size(int fd)
{
    if (fd == 3 && s_src_data) return (int32_t)strlen(s_src_data);
    if (fd < 0 || fd >= RAM_N || !s_ram[fd].open) return FAT_ERR_INVAL;
    return s_ram[fd].len;
}
int  fs_fd_write(int fd, const void *buf, int len)
{
    if (fd < 0 || fd >= RAM_N || !s_ram[fd].open || len < 0) return FAT_ERR_INVAL;
    if (s_ram[fd].flags & FREYA_O_APPEND) s_ram[fd].pos = s_ram[fd].len;
    if (s_ram[fd].pos > RAM_CAP || len > RAM_CAP - s_ram[fd].pos)
        return FAT_ERR_NOSPC;
    memcpy(s_ram[fd].data + s_ram[fd].pos, buf, (size_t)len);
    s_ram[fd].pos += len;
    if (s_ram[fd].pos > s_ram[fd].len) s_ram[fd].len = s_ram[fd].pos;
    return len;
}
int  fs_fd_seek(int fd, int32_t off, int whence)
{
    int32_t base, next;

    if (fd == 3 && s_src_data) {
        if (whence == FREYA_SEEK_SET) base = 0;
        else if (whence == FREYA_SEEK_CUR) base = s_src_pos;
        else if (whence == FREYA_SEEK_END) base = (int32_t)strlen(s_src_data);
        else return FAT_ERR_INVAL;
        if (base + off < 0) return FAT_ERR_INVAL;
        s_src_pos = (int)(base + off);
        return 0;
    }
    if (fd < 0 || fd >= RAM_N || !s_ram[fd].open) return FAT_ERR_INVAL;
    if (whence == FREYA_SEEK_SET) base = 0;
    else if (whence == FREYA_SEEK_CUR) base = s_ram[fd].pos;
    else if (whence == FREYA_SEEK_END) base = s_ram[fd].len;
    else return FAT_ERR_INVAL;
    next = base + off;
    if (next < 0) return FAT_ERR_INVAL;
    s_ram[fd].pos = (int)next;
    return 0;
}
int32_t fs_fd_tell(int fd)
{
    if (fd == 3 && s_src_open) return s_src_pos;
    if (fd < 0 || fd >= RAM_N || !s_ram[fd].open) return FAT_ERR_INVAL;
    return s_ram[fd].pos;
}
void fs_close_all(void)
{
    int i;

    s_src_open = 0;
    for (i = 0; i < RAM_N; i++) s_ram[i].open = 0;
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

/* First fit, so the shell can free an array or a dict and use the
 * bytes again.  The kernel heap does the same on the board. */
static char s_km[8192];
static int s_km_ready;

typedef struct {
    uint32_t size;
    uint32_t free;
} host_blk_t;

static void km_init(void)
{
    host_blk_t *b = (host_blk_t *)s_km;
    b->size = (uint32_t)sizeof s_km;
    b->free = 1;
    s_km_ready = 1;
}

void *kmalloc(uint32_t size)
{
    uint8_t *p;
    uint32_t need;

    if (!s_km_ready) km_init();
    if (size == 0) return NULL;
    need = ((size + 7U) & ~7U) + (uint32_t)sizeof(host_blk_t);
    p = (uint8_t *)s_km;
    while (p + sizeof(host_blk_t) <= (uint8_t *)s_km + sizeof s_km) {
        host_blk_t *b = (host_blk_t *)p;
        if (b->size < sizeof(host_blk_t) ||
            p + b->size > (uint8_t *)s_km + sizeof s_km)
            return NULL;
        if (b->free && b->size >= need) {
            if (b->size >= need + sizeof(host_blk_t) + 8U) {
                host_blk_t *n = (host_blk_t *)(p + need);
                n->size = b->size - need;
                n->free = 1;
                b->size = need;
            }
            b->free = 0;
            return p + sizeof(host_blk_t);
        }
        p += b->size;
    }
    return NULL;
}

void kfree(void *ptr)
{
    uint8_t *p;
    if (!ptr) return;
    ((host_blk_t *)((uint8_t *)ptr - sizeof(host_blk_t)))->free = 1;
    p = (uint8_t *)s_km;
    while (p + sizeof(host_blk_t) <= (uint8_t *)s_km + sizeof s_km) {
        host_blk_t *c = (host_blk_t *)p;
        host_blk_t *n;
        if (c->size < sizeof(host_blk_t) ||
            p + c->size > (uint8_t *)s_km + sizeof s_km)
            break;
        n = (host_blk_t *)(p + c->size);
        if (c->free && (uint8_t *)n + sizeof(host_blk_t) <=
            (uint8_t *)s_km + sizeof s_km && n->free &&
            (uint8_t *)n + n->size <= (uint8_t *)s_km + sizeof s_km) {
            c->size += n->size;
            continue;
        }
        p += c->size;
    }
}
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

int xmodem_receive_to_file(const char *path, uint32_t *received, int strip,
                           int32_t exact)
{
    (void)path; (void)received; (void)strip; (void)exact; return -1;
}

int xmodem_send_file(const char *path, uint32_t *sent)
{
    (void)path; (void)sent; return -1;
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

extern int shell_test_console_call_only;

static int run_console(const char *cmd)
{
    int rc;

    shell_test_console_call_only = 1;
    rc = run(cmd);
    shell_test_console_call_only = 0;
    return rc;
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

/* Commands with no arguments are still shown as calls. */
static void check_summary_words(void)
{
    static const char *const names[] = {
        "sysinfo", "meminfo", "mount", "pwd", "df", "threads", "status",
        "uninstall", "uptime", "clear", "reboot"
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
        snprintf(line, sizeof line, "  %s()\r\n", names[i]);
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
        "uninstall", "uptime", "clear", "reboot"
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
    expect_has("help introduces call syntax",
               "Freya commands (use function syntax):\r\n");
    expect_has("help lists stop as a call", "  stop([\"thread\"])\r\n");
    expect_has("help lists sleep as a call", "  sleep(ms)\r\n");
    expect_has("help separates shell syntax", "Shell syntax: set, fn");
    expect_has("help lists power as a call", "  power([\"sd\"");
    check_summary_words();

    printf("help <command>\n");
    for (i = 0; i < sizeof one_word / sizeof one_word[0]; i++) {
        char cmd[32], want[32];
        snprintf(cmd, sizeof cmd, "help %s", one_word[i]);
        snprintf(want, sizeof want, "%s()\r\n", one_word[i]);
        rc = run(cmd);
        expect_rc(cmd, rc, 0);
        expect_exact(cmd, want);
    }
    rc = run("help ls");
    expect_rc("help ls succeeds", rc, 0);
    expect_exact("help ls shows call syntax",
                 "ls([\"-l\"] [, \"path\"])\r\n");
    rc = run("help stop");
    expect_rc("help stop succeeds", rc, 0);
    expect_exact("help stop shows call syntax",
                 "stop([\"thread\"])\r\n");
    rc = run("help nosuch");
    expect_rc("help of an unknown command fails", rc, FREYA_EXIT_FAIL);
    expect_has("unknown command is named", "no such command: nosuch");
    rc = run("help w1");
    expect_rc("help w1 succeeds", rc, 0);
    expect_has("help w1 names the command", "w1(");
    expect_has("help w1 shows the ROM search", "search");
    rc = run("w1");
    expect_rc("w1 with no pin succeeds", rc, 0);
    expect_has("w1 asks for a pull-up", "pull the data pin up to 3.3 V");
    rc = run("help spi");
    expect_rc("help spi succeeds", rc, 0);
    expect_has("help spi names the command", "spi(");
    expect_has("help spi shows a transfer", "\"x\"");
    rc = run("spi");
    expect_rc("spi with no bus succeeds", rc, 0);
    expect_has("spi names chip select", "chip select is a pin you drive");
    rc = run("help crypt");
    expect_rc("help crypt succeeds", rc, 0);
    expect_exact("help crypt shows call syntax",
                 "crypt([\"key\", \"nonce\", \"hex\"])\r\n");
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

    printf("expressions\n");
    rc = run("3+2");
    expect_rc("an expression succeeds", rc, 0);
    expect_exact("3+2 prints 5", "5\r\n");
    rc = run("(2+2)%10");
    expect_rc("a grouped expression succeeds", rc, 0);
    expect_exact("(2+2)%10 prints 4", "4\r\n");
    rc = run("\"Sun\"");
    expect_rc("a string expression succeeds", rc, 0);
    expect_exact("a string prints quoted", "\"Sun\"\r\n");
    rc = run("true");
    expect_rc("a bool expression succeeds", rc, 0);
    expect_exact("true prints true", "true\r\n");
    rc = run("65b");
    expect_rc("a byte expression succeeds", rc, 0);
    expect_exact("a byte prints with b", "65b\r\n");
    rc = run("help()");
    expect_rc("help() succeeds", rc, 0);
    expect_has("help() lists function commands", "use function syntax");
    expect_lacks("help() prints no extra value", "none");
    rc = run("sysinfo()");
    expect_rc("sysinfo() succeeds", rc, 0);
    expect_has("sysinfo() runs the command", "CPU");
    rc = run("meminfo()");
    expect_rc("meminfo() succeeds", rc, 0);
    expect_has("meminfo() runs the command", "Flash ");
    rc = run("help(\"meminfo\")");
    expect_rc("help(\"meminfo\") succeeds", rc, 0);
    expect_exact("command arguments are expression values", "meminfo()\r\n");
    rc = run_console("meminfo");
    expect_rc("legacy console command syntax fails", rc, FREYA_EXIT_FAIL);
    expect_exact("legacy syntax points to the call", "meminfo: use meminfo(...)\r\n");
    rc = run_console("meminfo()");
    expect_rc("function console command syntax succeeds", rc, 0);
    expect_has("the console call runs meminfo", "Flash ");
    rc = run("nosuch()");
    expect_rc("an unknown call fails", rc, FREYA_EXIT_FAIL);
    expect_has("an unknown call is not a function", "no such function");

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
    expect_exact("help source shows call syntax",
                 "source(\"file\"|\"@flash\")\r\n");

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

    printf("arrays\n");
    rc = run("set a array(10, 20, 30)");
    expect_rc("array() succeeds", rc, 0);
    rc = run("echo $a");
    expect_exact("an array prints its elements", "[10, 20, 30]\r\n");
    rc = run("echo $a[1]");
    expect_exact("an index reads one element", "20\r\n");
    rc = run("set n len($a)");
    rc = run("echo $n");
    expect_exact("len is the element count", "3\r\n");
    rc = run("set a[3] 40");
    expect_rc("writing the next index appends", rc, 0);
    rc = run("echo $a[3]");
    expect_exact("the appended element is there", "40\r\n");
    rc = run("set a[5] 60");
    rc = run("echo $a");
    expect_exact("a gap is filled with zeros", "[10, 20, 30, 40, 0, 60]\r\n");
    rc = run("set a[0] 1b");
    expect_rc("an element of another type fails", rc, FREYA_EXIT_FAIL);
    expect_has("the array keeps its type", "type mismatch");
    rc = run("set a[8] 1");
    expect_rc("past the last slot fails", rc, FREYA_EXIT_FAIL);
    expect_has("the array has a fixed ceiling", "out of range");
    rc = run("set b $a");
    rc = run("set a[0] 1");
    rc = run("echo $b[0]");
    expect_exact("assigning an array copies it", "10\r\n");
    rc = run("set n min($a)");
    rc = run("echo $n");
    expect_exact("min is the least element", "0\r\n");
    rc = run("set n max($a)");
    rc = run("echo $n");
    expect_exact("max is the greatest element", "60\r\n");
    rc = run("set b sort($a)");
    expect_rc("sort succeeds", rc, 0);
    rc = run("echo $b");
    expect_exact("sort returns ascending order", "[0, 1, 20, 30, 40, 60]\r\n");
    rc = run("echo $a");
    expect_exact("sort leaves the original array", "[1, 20, 30, 40, 0, 60]\r\n");
    rc = run("unset a");
    rc = run("unset b");

    rc = run("set a array(1.5, 0.25, 2.0)");
    rc = run("set n min($a)");
    rc = run("echo $n");
    expect_exact("min orders floats", "0.25\r\n");
    rc = run("set b sort($a)");
    rc = run("echo $b");
    expect_exact("sort orders floats", "[0.25, 1.5, 2]\r\n");
    rc = run("unset a");
    rc = run("unset b");

    rc = run("set a array(\"c\", \"a\", \"b\")");
    rc = run("set s min($a)");
    rc = run("echo $s");
    expect_exact("min orders strings", "a\r\n");
    rc = run("set b sort($a)");
    rc = run("set");
    expect_has("sort orders strings", "b = [\"a\", \"b\", \"c\"]\r\n");
    rc = run("unset a");
    rc = run("unset b");
    rc = run("unset s");

    rc = run("set a array(3b, 1b, 2b)");
    rc = run("set b sort($a)");
    rc = run("echo $b");
    expect_exact("sort orders bytes", "[1b, 2b, 3b]\r\n");
    rc = run("set n max($a)");
    rc = run("echo $n");
    expect_exact("max of bytes is the greatest", "3\r\n");
    rc = run("unset a");
    rc = run("unset b");

    rc = run("set a array()");
    rc = run("set n min($a)");
    expect_rc("min of an empty array fails", rc, FREYA_EXIT_FAIL);
    expect_has("an empty array has no least element", "empty array");
    rc = run("set b sort($a)");
    rc = run("echo $b");
    expect_exact("sort of an empty array is empty", "[]\r\n");
    rc = run("set n len($a)");
    rc = run("echo $n");
    expect_exact("len of an empty array is 0", "0\r\n");
    rc = run("unset a");
    rc = run("unset b");

    rc = run("set d dict(\"b\", 2, \"a\", 1)");
    rc = run("set n min($d)");
    expect_rc("min refuses a dict", rc, FREYA_EXIT_FAIL);
    expect_has("min wants an array", "bad expression");
    rc = run("unset d");

    rc = run("fn sort; return 1; end");
    expect_rc("sort is a reserved name", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses sort", "bad name");

    rc = run("set d dict(\"b\", 2, \"a\", 1)");
    expect_rc("dict() succeeds", rc, 0);
    rc = run("set n len($d)");
    rc = run("echo $n");
    expect_exact("len is the pair count", "2\r\n");
    rc = run("set");
    expect_has("dict keys are kept sorted", "d = {\"a\": 1, \"b\": 2}\r\n");
    rc = run("echo $d[\"b\"]");
    expect_exact("a key reads its value", "2\r\n");
    rc = run("set d[\"c\"] 3");
    rc = run("echo $d[\"a\"]");
    expect_exact("insert keeps the earlier key", "1\r\n");
    rc = run("set");
    expect_has("the new key lands in order", "d = {\"a\": 1, \"b\": 2, \"c\": 3}\r\n");
    rc = run("set d[\"b\"] 9");
    rc = run("echo $d[\"b\"]");
    expect_exact("a known key is replaced", "9\r\n");
    rc = run("set n len($d)");
    rc = run("echo $n");
    expect_exact("replacing a key leaves the count", "3\r\n");
    rc = run("set e dict()");
    rc = run("set n len($e)");
    rc = run("echo $n");
    expect_exact("len of an empty dict is 0", "0\r\n");
    rc = run("unset e");
    rc = run("set n len(1)");
    expect_rc("len of a number fails", rc, FREYA_EXIT_FAIL);
    expect_has("len wants an array or a dict", "bad expression");
    rc = run("set d[1] 4");
    expect_rc("a key of another type fails", rc, FREYA_EXIT_FAIL);
    expect_has("dict keys stay one type", "type mismatch");
    rc = run("set d[\"a\"] \"x\"");
    expect_rc("a value of another type fails", rc, FREYA_EXIT_FAIL);
    expect_has("dict values stay one type", "type mismatch");
    rc = run("echo $d[\"z\"]");
    expect_rc("a missing key fails", rc, FREYA_EXIT_FAIL);
    expect_has("the missing key is named", "no such key");
    rc = run("set m[\"x\"] 7");
    expect_rc("a string index starts a dict", rc, 0);
    rc = run("echo $m[\"x\"]");
    expect_exact("the new dict holds the value", "7\r\n");
    rc = run("unset m");
    rc = run("set p[0] 4");
    rc = run("echo $p");
    expect_exact("an integer index starts an array", "[4]\r\n");
    rc = run("unset p");
    rc = run("if $d == dict(\"c\", 3, \"a\", 1, \"b\", 9); echo yes; else; echo no; end");
    expect_exact("dicts compare by key and value", "yes\r\n");
    rc = run("unset d");
    rc = run("set n 8");

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

    rc = run("fn id; if $1 /= 0; return 1, 2.5, \"ok\"; else; return 0, 0, \"no\"; end; end");
    expect_rc("a function may return several values", rc, 0);
    rc = run("set a, b, c id(1)");
    expect_rc("set stores each returned value", rc, 0);
    rc = run("echo $a");
    expect_exact("the first value is an integer", "1\r\n");
    rc = run("echo $b");
    expect_exact("the second value is a float", "2.5\r\n");
    rc = run("echo $c");
    expect_exact("the third value is a string", "ok\r\n");
    rc = run("set a, b, c id(0)");
    rc = run("echo $c");
    expect_exact("return in the other branch is used", "no\r\n");
    rc = run("set n id(1)");
    expect_rc("one name takes the first value", rc, 0);
    rc = run("echo $n");
    expect_exact("the later values are left", "1\r\n");
    rc = run("set a, b id(1)");
    expect_rc("fewer names than values succeeds", rc, 0);
    rc = run("echo $b");
    expect_exact("the second name took the float", "2.5\r\n");

    rc = run("fn add; return 7, 8; end");
    rc = run("fn narg; return 1, add(); end");
    rc = run("set a, b, c narg()");
    expect_rc("a trailing call is handed on", rc, 0);
    rc = run("echo $a");
    expect_exact("the value before the call is kept", "1\r\n");
    rc = run("echo $c");
    expect_exact("the call's second value is handed on", "8\r\n");

    rc = run("fn hi; loop 3; return 4, \"x\"; end; end");
    rc = run("set a, b hi()");
    rc = run("echo $b");
    expect_exact("return leaves from inside a loop", "x\r\n");

    rc = run("set a, b, c, d id(1)");
    expect_rc("fewer values than names fails", rc, FREYA_EXIT_FAIL);
    expect_has("a short return is named", "too few values");
    rc = run("set a, b 1");
    expect_rc("several names need a call", rc, FREYA_EXIT_FAIL);
    expect_has("a single value is not enough", "too few values");

    {
        char line[160];
        int a;
        strcpy(line, "fn hi; return ");
        for (a = 0; a < 32; a++) {
            if (a) strcat(line, ",");
            strcat(line, "1");
        }
        strcat(line, "; end");
        rc = run(line);
        expect_rc("32 values can be defined", rc, 0);
        rc = run("set n hi()");
        expect_rc("32 values can be returned", rc, 0);
        strcpy(line, "fn hi; return ");
        for (a = 0; a < 33; a++) {
            if (a) strcat(line, ",");
            strcat(line, "1");
        }
        strcat(line, "; end");
        rc = run(line);
        expect_rc("33 values can be defined", rc, 0);
        rc = run("set n hi()");
        expect_rc("33 values fails", rc, FREYA_EXIT_FAIL);
        expect_has("32 values is the limit", "too many values");
    }

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

    printf("byte and empty\n");
    rc = run("set b 65b");
    expect_rc("a byte literal succeeds", rc, 0);
    rc = run("echo $b");
    expect_exact("a byte prints as decimal", "65\r\n");
    rc = run("set b byte(0x41)");
    rc = run("echo $b");
    expect_exact("byte() of a hex integer", "65\r\n");
    rc = run("set b 0b");
    rc = run("echo $b");
    expect_exact("byte zero", "0\r\n");
    rc = run("set b 256b");
    expect_rc("a byte past 255 fails", rc, FREYA_EXIT_FAIL);
    expect_has("the byte range is named", "integer overflow");
    rc = run("set b byte(255)");
    expect_rc("byte() succeeds", rc, 0);
    rc = run("echo $b");
    expect_exact("byte() keeps 255", "255\r\n");
    rc = run("set b byte(1.9)");
    rc = run("echo $b");
    expect_exact("byte() truncates toward zero", "1\r\n");
    rc = run("set b byte(\"0x10\")");
    rc = run("echo $b");
    expect_exact("byte() parses a hex string", "16\r\n");
    rc = run("set b byte(256)");
    expect_rc("byte() past 255 fails", rc, FREYA_EXIT_FAIL);
    expect_has("byte() overflow is named", "integer overflow");
    rc = run("set b byte(-1)");
    expect_rc("byte() of a negative fails", rc, FREYA_EXIT_FAIL);
    rc = run("set n int(65b)");
    rc = run("echo $n");
    expect_exact("int widens a byte", "65\r\n");
    rc = run("set n 1b + 2b");
    rc = run("echo $n");
    expect_exact("bytes add as integers", "3\r\n");
    rc = run("set n byte(0xF0) & byte(0x0F)");
    rc = run("echo $n");
    expect_exact("bytes have bitwise and", "0\r\n");
    rc = run("set n ~0b");
    rc = run("echo $n");
    expect_exact("bitwise not of a byte is an integer", "-1\r\n");
    rc = run("if 1b == 1; echo yes; else; echo no; end");
    expect_exact("a byte matches the same integer", "yes\r\n");
    rc = run("if 1b == 1.0; echo yes; else; echo no; end");
    expect_exact("a byte matches the same float", "yes\r\n");
    rc = run("if 1b < 2; echo yes; else; echo no; end");
    expect_exact("a byte orders with an integer", "yes\r\n");
    rc = run("set s \"%d\" 65b");
    rc = run("echo $s");
    expect_exact("a format accepts a byte", "65\r\n");
    rc = run("set s hex(255b)");
    rc = run("echo $s");
    expect_exact("hex of a byte", "ff\r\n");
    rc = run("set b byte(255)");
    rc = run("set");
    expect_has("set lists the byte", "b = 255b\r\n");

    rc = run("unset q");
    rc = run("set e empty");
    expect_rc("the empty literal succeeds", rc, 0);
    rc = run("echo $e");
    expect_exact("empty prints its name", "empty\r\n");
    rc = run("set e empty()");
    rc = run("echo $e");
    expect_exact("empty() is the same value", "empty\r\n");
    rc = run("if empty == empty; echo yes; else; echo no; end");
    expect_exact("empty matches empty", "yes\r\n");
    rc = run("if empty == 0; echo yes; else; echo no; end");
    expect_exact("empty does not match zero", "no\r\n");
    rc = run("if empty == \"\"; echo yes; else; echo no; end");
    expect_exact("empty does not match an empty string", "no\r\n");
    rc = run("set s str(empty)");
    rc = run("echo $s");
    expect_exact("str of empty is the name", "empty\r\n");
    rc = run("set s \"x\" + empty");
    rc = run("echo $s");
    expect_exact("a string joins empty as text", "xempty\r\n");
    rc = run("set n int(empty)");
    expect_rc("int of empty fails", rc, FREYA_EXIT_FAIL);
    expect_has("empty is not a number", "not a number");
    rc = run("set n empty + 1");
    expect_rc("adding to empty fails", rc, FREYA_EXIT_FAIL);
    expect_has("empty is not arithmetic", "bad expression");
    rc = run("if empty > 0; echo y; else; echo n; end");
    expect_rc("ordering empty fails", rc, 0);
    expect_has("ordering wants a number", "not a number");
    rc = run("set");
    expect_has("set lists empty", "e = empty\r\n");
    rc = run("fn id; return 1b, empty; end");
    rc = run("set b, e id()");
    rc = run("echo $b");
    expect_exact("a call can return a byte", "1\r\n");
    rc = run("echo $e");
    expect_exact("a call can return empty", "empty\r\n");

    printf("bool and none\n");
    rc = run("set b true");
    expect_rc("the true literal succeeds", rc, 0);
    rc = run("echo $b");
    expect_exact("true prints its name", "true\r\n");
    rc = run("set b false()");
    rc = run("echo $b");
    expect_exact("false() is the false bool", "false\r\n");
    rc = run("if true; echo yes; else; echo no; end");
    expect_exact("if true takes the first branch", "yes\r\n");
    rc = run("if false; echo yes; else; echo no; end");
    expect_exact("if false takes the else", "no\r\n");
    rc = run("if true == false; echo yes; else; echo no; end");
    expect_exact("true does not match false", "no\r\n");
    rc = run("if true == 1; echo yes; else; echo no; end");
    expect_exact("true does not match the integer 1", "no\r\n");
    rc = run("if false == 0; echo yes; else; echo no; end");
    expect_exact("false does not match zero", "no\r\n");
    rc = run("set b bool(0)");
    rc = run("echo $b");
    expect_exact("bool of zero is false", "false\r\n");
    rc = run("set b bool(2)");
    rc = run("echo $b");
    expect_exact("bool of a nonzero integer is true", "true\r\n");
    rc = run("set b bool(0.0)");
    rc = run("echo $b");
    expect_exact("bool of zero float is false", "false\r\n");
    rc = run("set b bool(\"true\")");
    rc = run("echo $b");
    expect_exact("bool of the text true", "true\r\n");
    rc = run("set b bool(\"no\")");
    expect_rc("bool of other text fails", rc, FREYA_EXIT_FAIL);
    expect_has("other text is not a bool", "not a number");
    rc = run("set n int(true)");
    rc = run("echo $n");
    expect_exact("int of true is 1", "1\r\n");
    rc = run("set n int(false)");
    rc = run("echo $n");
    expect_exact("int of false is 0", "0\r\n");
    rc = run("set n true + 1");
    expect_rc("adding to true fails", rc, FREYA_EXIT_FAIL);
    expect_has("a bool is not arithmetic", "bad expression");
    rc = run("if true > false; echo y; else; echo n; end");
    expect_rc("ordering a bool fails", rc, 0);
    expect_has("ordering wants a number", "not a number");
    rc = run("set s \"%s\" true");
    rc = run("echo $s");
    expect_exact("a format prints a bool as text", "true\r\n");
    rc = run("set a array(true, false)");
    rc = run("set a[3] true");
    rc = run("echo $a");
    expect_exact("a bool gap is false", "[true, false, false, true]\r\n");
    rc = run("set n min($a)");
    rc = run("echo $n");
    expect_exact("min of bools is false", "false\r\n");
    rc = run("unset a");
    rc = run("set b true");
    rc = run("set");
    expect_has("set lists a bool", "b = true\r\n");

    rc = run("unset b");
    rc = run("set e none");
    expect_rc("the none literal succeeds", rc, 0);
    rc = run("echo $e");
    expect_exact("none prints its name", "none\r\n");
    rc = run("set e none()");
    rc = run("echo $e");
    expect_exact("none() is the same value", "none\r\n");
    rc = run("if none == none; echo yes; else; echo no; end");
    expect_exact("none matches none", "yes\r\n");
    rc = run("if none == empty; echo yes; else; echo no; end");
    expect_exact("none does not match empty", "no\r\n");
    rc = run("if none == 0; echo yes; else; echo no; end");
    expect_exact("none does not match zero", "no\r\n");
    rc = run("set s str(none)");
    rc = run("echo $s");
    expect_exact("str of none is the name", "none\r\n");
    rc = run("set s \"x\" + none");
    rc = run("echo $s");
    expect_exact("a string joins none as text", "xnone\r\n");
    rc = run("set n int(none)");
    expect_rc("int of none fails", rc, FREYA_EXIT_FAIL);
    expect_has("none is not a number", "not a number");
    rc = run("set n bool(none)");
    expect_rc("bool of none fails", rc, FREYA_EXIT_FAIL);
    expect_has("none is not a bool", "not a number");
    rc = run("set n none + 1");
    expect_rc("adding to none fails", rc, FREYA_EXIT_FAIL);
    expect_has("none is not arithmetic", "bad expression");
    rc = run("fn id; return true, none; end");
    rc = run("set b, e id()");
    rc = run("echo $b");
    expect_exact("a call can return a bool", "true\r\n");
    rc = run("echo $e");
    expect_exact("a call can return none", "none\r\n");
    rc = run("set");
    expect_has("set lists none", "e = none\r\n");
    rc = run("fn bool; return 1; end");
    expect_rc("bool is a reserved name", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses bool", "bad name");
    rc = run("fn none; return 1; end");
    expect_rc("none is a reserved name", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses none", "bad name");

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

    printf("files\n");
    fat_unmount();
    rc = run("set x open(\"/n.txt\")");
    expect_rc("open before mount fails", rc, FREYA_EXIT_FAIL);
    expect_has("open asks for mount", "no filesystem mounted");
    fat_mount();

    rc = run("set x open(\"/missing.txt\")");
    expect_rc("open of a missing file fails", rc, FREYA_EXIT_FAIL);
    expect_has("open names the missing file", "open: /missing.txt:");
    rc = run("set x open(\"/n.txt\", \"z\")");
    expect_rc("a bad mode fails", rc, FREYA_EXIT_FAIL);
    expect_has("a bad mode is a bad expression", "bad expression");

    rc = run("set x open(\"/n.txt\", \"w\")");
    expect_rc("open for write succeeds", rc, 0);
    rc = run("echo $x");
    expect_exact("open returns a handle", "0\r\n");
    rc = run("set n write($x, \"hi\", 10b)");
    expect_rc("write of a string and a newline succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("write counts the bytes", "3\r\n");
    rc = run("set n close($x)");
    expect_rc("close succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("close returns 0", "0\r\n");

    rc = run("set x open(\"/n.txt\")");
    expect_rc("open for read succeeds", rc, 0);
    rc = run("set s read($x)");
    expect_rc("read of a line succeeds", rc, 0);
    rc = run("echo $s");
    expect_exact("read drops the newline", "hi\r\n");
    rc = run("set s read($x)");
    expect_rc("read at the end succeeds", rc, 0);
    rc = run("echo $s");
    expect_exact("the end of the file is empty", "empty\r\n");
    rc = run("set n close($x)");

    rc = run("set x open(\"/n.txt\", \"w\")");
    rc = run("set n write($x, \"ab\", \"cd\")");
    rc = run("echo $n");
    expect_exact("write joins its values", "4\r\n");
    rc = run("set n seek($x, \"set\", 1)");
    expect_rc("seek set moves to an offset", rc, 0);
    rc = run("echo $n");
    expect_exact("seek returns the new position", "1\r\n");
    rc = run("set s read($x, 2)");
    rc = run("echo $s");
    expect_exact("read of a count returns those characters", "bc\r\n");
    rc = run("set n seek($x)");
    rc = run("echo $n");
    expect_exact("seek with no offset is the position", "3\r\n");
    rc = run("set n seek($x, \"end\")");
    rc = run("echo $n");
    expect_exact("seek end is the length", "4\r\n");
    s_synced = 0;
    rc = run("set n flush($x)");
    expect_rc("flush succeeds", rc, 0);
    if (s_synced) pass("flush syncs the card");
    else fail("flush syncs the card");
    rc = run("set n close($x)");

    rc = run("set x open(\"/num.txt\", \"w\")");
    rc = run("set n write($x, \" 42\")");
    rc = run("set n seek($x, \"set\", 0)");
    rc = run("set n read($x, \"*n\")");
    expect_rc("read of a number succeeds", rc, 0);
    rc = run("echo $n");
    expect_exact("read *n skips spaces", "42\r\n");
    rc = run("set n close($x)");
    rc = run("set x open(\"/num.txt\", \"w\")");
    rc = run("set n write($x, \"1.5\")");
    rc = run("set n seek($x, \"set\", 0)");
    rc = run("set x read($x, \"*n\")");
    rc = run("echo $x");
    expect_exact("read *n of a decimal is a float", "1.5\r\n");

    rc = run("set x open(\"/n.txt\", \"w\")");
    rc = run("set n write($x, \"ab\")");
    rc = run("set n close($x)");
    rc = run("set x open(\"/n.txt\", \"a\")");
    rc = run("set n write($x, \"c\")");
    rc = run("set n close($x)");
    rc = run("set x open(\"/n.txt\", \"rb\")");
    rc = run("set s read($x, \"*a\")");
    rc = run("echo $s");
    expect_exact("append and *a keep the bytes", "abc\r\n");
    rc = run("set n close($x)");

    rc = run("set x open(\"/n.txt\", \"w\")");
    rc = run("set n write($x, 65)");
    rc = run("set n seek($x, \"set\", 0)");
    rc = run("set s read($x, \"*a\")");
    rc = run("echo $s");
    expect_exact("an integer is written as text", "65\r\n");
    rc = run("set n close($x)");
    rc = run("set x open(\"/raw.txt\", \"w\")");
    rc = run("set n write($x, 65b)");
    rc = run("set n seek($x, \"set\", 0)");
    rc = run("set s read($x, \"*a\")");
    rc = run("echo $s");
    expect_exact("a byte is written raw", "A\r\n");
    rc = run("set n close($x)");

    rc = run("set x open(\"/n.txt\", \"w\")");
    rc = run("set n write($x, \"0123456789abcdef\", \"0123456789abcdef\")");
    rc = run("set n seek($x, \"set\", 0)");
    rc = run("set s read($x, \"*a\")");
    expect_rc("read *a of a long file fails", rc, FREYA_EXIT_FAIL);
    expect_has("a long read is too long", "string too long");
    rc = run("set s read($x, 31)");
    rc = run("echo $s");
    expect_exact("a failed read leaves the position",
                 "0123456789abcdef0123456789abcde\r\n");
    rc = run("set n close($x)");

    rc = run("set x open(\"/n.txt\", \"w\")");
    rc = run("set n write($x, \"hi\", 10b)");
    rc = run("set n seek($x, \"set\", 0)");
    rc = run("set s read($x, \"*L\")");
    expect_rc("read *L succeeds", rc, 0);
    rc = run("if $s /= \"hi\"; echo kept; else; echo stripped; end");
    expect_exact("read *L keeps the newline", "kept\r\n");
    rc = run("set n close($x)");

    rc = run("set n read($x)");
    expect_rc("read of a closed handle fails", rc, FREYA_EXIT_FAIL);
    rc = run("fn open; return 1; end");
    expect_rc("open cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses open", "bad name");

    printf("script threads\n");
    rc = run("fn add; echo a1; yield; echo a2; end; "
             "fn id; echo b1; yield; echo b2; end; "
             "set x spawn(\"add\", 1); set y spawn(\"id\", 1); yield");
    expect_rc("two threads run", rc, 0);
    expect_exact("yield gives each thread a turn",
                 "a1\r\nb1\r\na2\r\nb2\r\n");

    rc = run("fn add; echo a; sleep 10; echo b; end; "
             "set n spawn(\"add\", 1); echo mid; sleep 10; echo end");
    expect_rc("a thread sleeps", rc, 0);
    expect_exact("sleep runs the thread when its wait is over",
                 "a\r\nmid\r\nb\r\nend\r\n");

    rc = run("fn add; echo a; sleep 5; echo b; end; "
             "set n spawn(\"add\", 0); set k join($n); echo done; echo $k");
    expect_rc("join waits", rc, 0);
    expect_exact("join returns when the thread has finished",
                 "a\r\nb\r\ndone\r\n0\r\n");

    rc = run("fn add; echo a; sleep 50; echo b; end; "
             "set n spawn(\"add\", 1); stop add; echo after");
    expect_rc("stop of a script thread succeeds", rc, 0);
    expect_exact("stop ends the thread before it wakes",
                 "a\r\nstopped add\r\nafter\r\n");

    rc = run("fn add; sleep 1000; end; set n spawn(\"add\", 1); threads; stop add");
    expect_rc("threads lists a script thread", rc, 0);
    expect_has("the script thread is asleep", "sleep    add");

    rc = run("fn add; echo lo; end; fn id; echo hi; end; "
             "set n spawn(\"add\", 1) + spawn(\"id\", 2)");
    expect_rc("a higher priority runs first", rc, 0);
    expect_exact("priority 2 runs ahead of priority 1", "hi\r\nlo\r\n");

    rc = run("fn add; set n 0; loop 3; set n $n + 1; end; echo $n; end; "
             "set n spawn(\"add\", 1)");
    expect_rc("a thread runs a loop", rc, 0);
    expect_exact("the loop counted in the thread", "3\r\n");

    rc = run("fn add; if 1 == 1; echo yes; else; echo no; end; end; "
             "set n spawn(\"add\", 1)");
    expect_rc("a thread runs if", rc, 0);
    expect_exact("the thread took the first branch", "yes\r\n");

    rc = run("fn add; loop 4; echo x; break; end; echo z; end; "
             "set n spawn(\"add\", 1)");
    expect_rc("a thread breaks a loop", rc, 0);
    expect_exact("break leaves the loop", "x\r\nz\r\n");

    rc = run("fn add; echo a; return 1; echo b; end; set n spawn(\"add\", 1)");
    expect_rc("return ends a thread", rc, 0);
    expect_exact("the thread stops at return", "a\r\n");

    rc = run("fn add; sleep 100; end; fn id; sleep 100; end; "
             "fn narg; sleep 100; end; "
             "set x spawn(\"add\", 1); set y spawn(\"id\", 1); "
             "set z spawn(\"narg\", 1)");
    expect_rc("a third thread is refused", rc, FREYA_EXIT_FAIL);
    expect_has("only two script threads fit", "too many threads");
    rc = run("stop add; stop id");
    expect_rc("the two threads stop", rc, 0);

    rc = run("fn add; sleep 100; end; "
             "set x spawn(\"add\", 1); set y spawn(\"add\", 1)");
    expect_rc("a second thread of the same name fails", rc, FREYA_EXIT_FAIL);
    expect_has("the name is in use", "that name is in use");
    rc = run("stop add");

    rc = run("set n spawn(\"nope\", 1)");
    expect_rc("spawn of a missing function fails", rc, FREYA_EXIT_FAIL);
    expect_has("spawn names the missing function", "no such function");
    rc = run("set n spawn(\"add\", 8)");
    expect_rc("a priority past 7 fails", rc, FREYA_EXIT_FAIL);
    expect_has("a bad priority is refused", "bad expression");
    rc = run("fn spawn; return 1; end");
    expect_rc("spawn cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses spawn", "bad name");

    rc = run("fn add; set n join($n); end; set n spawn(\"add\", 1)");
    expect_rc("a thread cannot join itself", rc, FREYA_EXIT_FAIL);
    expect_has("join refuses the running thread", "cannot join itself");

    rc = run("fn add; sleep 1000; end; set n spawn(\"add\", 1); run");
    expect_rc("run is refused while a thread is alive", rc, FREYA_EXIT_FAIL);
    expect_has("run names the thread", "a thread is running");
    rc = run("stop add");
    expect_rc("the last thread stops", rc, 0);

    rc = run("help yield");
    expect_rc("help yield succeeds", rc, 0);
    expect_exact("help yield shows call syntax", "yield()\r\n");

    printf("collection ownership\n");
    run("unset a");
    run("unset b");
    run("unset c");
    run("unset d");
    run("unset e");
    run("unset f");
    run("unset k");
    run("unset n");
    run("unset p");
    run("unset q");
    run("unset s");
    run("unset t");
    run("unset x");
    run("unset y");
    run("unset z");

    rc = run("set a array(1)");
    expect_rc("ownership test array succeeds", rc, 0);
    for (i = 0; i < 8; i++) {
        rc = run("set s $a + \"\"");
        expect_rc("collection concatenation releases its temporary", rc, 0);
    }
    for (i = 0; i < 8; i++) {
        rc = run("set s \"%s\" $a");
        expect_rc("collection formatting releases its argument", rc, 0);
    }

    rc = run("fn add; return $a + 1; end");
    expect_rc("failing collection function is defined", rc, 0);
    for (i = 0; i < 8; i++) {
        rc = run("set n add()");
        expect_rc("failed return releases its partial value", rc, FREYA_EXIT_FAIL);
    }

    rc = run("fn add; return array(1); end");
    expect_rc("collection function is defined", rc, 0);
    rc = run("fn id; return add(), 2; end");
    expect_rc("multi-return collection function is defined", rc, 0);
    for (i = 0; i < 8; i++) {
        rc = run("set b, c id()");
        expect_rc("non-final call return keeps valid ownership", rc, 0);
    }
    rc = run("echo $b");
    expect_exact("non-final call returned its collection", "[1]\r\n");

    rc = run("fn add; return $a; end");
    expect_rc("interrupt collection function is defined", rc, 0);
    rc = run("set n timer(1000, 0, \"add\")");
    expect_rc("ownership test timer starts", rc, 0);
    for (i = 1; i <= 8; i++) {
        fire_timer(i);
        rc = run("set n wait(50)");
        expect_rc("interrupt return value is released", rc, 0);
    }
    rc = run("set n tclose(0)");
    expect_rc("ownership test timer closes", rc, 0);

    rc = run("set p[8] 1");
    expect_rc("failed first indexed write fails", rc, FREYA_EXIT_FAIL);
    rc = run("unset p");
    expect_rc("failed first indexed write leaves no variable", rc, FREYA_EXIT_FAIL);

    run("unset a");
    run("unset b");
    run("unset c");
    run("unset n");
    run("unset s");

    printf("patterns\n");
    run("unset x");
    run("unset f");
    run("unset t");
    run("unset k");
    run("unset y");
    run("unset z");
    rc = run("set s match(\"abc-12\", \"%a+\")");
    expect_rc("match of letters succeeds", rc, 0);
    rc = run("echo $s");
    expect_exact("match returns the letters", "abc\r\n");
    rc = run("set s match(\"abc-12\", \"%d+\")");
    rc = run("echo $s");
    expect_exact("match returns the digits", "12\r\n");
    rc = run("set s match(\"abc\", \"%d+\")");
    rc = run("echo $s");
    expect_exact("a failed match is none", "none\r\n");
    rc = run("set a, b match(\"abc-12\", \"(%a+)%-(%d+)\")");
    expect_rc("match of two captures succeeds", rc, 0);
    rc = run("echo $a");
    expect_exact("the first capture is the letters", "abc\r\n");
    rc = run("echo $b");
    expect_exact("the second capture is the digits", "12\r\n");
    rc = run("set n match(\"ab\", \"a()\")");
    rc = run("echo $n");
    expect_exact("an empty capture is the position", "2\r\n");
    rc = run("set s match(\"<a>b>\", \"<.->\")");
    rc = run("echo $s");
    expect_exact("the short repeat stops early", "<a>\r\n");
    rc = run("set s match(\"<a>b>\", \"<.*>\")");
    rc = run("echo $s");
    expect_exact("the long repeat runs on", "<a>b>\r\n");
    rc = run("set s match(\"abc\", \"^b\")");
    rc = run("echo $s");
    expect_exact("a caret misses past the start", "none\r\n");
    rc = run("set s match(\"abc\", \"c$\")");
    rc = run("echo $s");
    expect_exact("a dollar matches the end", "c\r\n");
    rc = run("set s match(\"abcabc\", \"a\", 2)");
    rc = run("echo $s");
    expect_exact("match starts at the index", "a\r\n");
    rc = run("set s match(\"(a(b)c)\", \"%b()\")");
    rc = run("echo $s");
    expect_exact("a balanced pair takes the outer", "(a(b)c)\r\n");
    rc = run("set s match(\"ab12\", \"%D+\")");
    rc = run("echo $s");
    expect_exact("an uppercase class is the complement", "ab\r\n");
    rc = run("set s match(\"ab12\", \"[0-9]+\")");
    rc = run("echo $s");
    expect_exact("a range matches the digits", "12\r\n");
    rc = run("set a, b find(\"abc-12\", \"%d+\")");
    rc = run("echo $a");
    expect_exact("find returns where the match starts", "5\r\n");
    rc = run("echo $b");
    expect_exact("find returns where the match ends", "6\r\n");
    rc = run("set a, b find(\"a%d\", \"%d\", 1, true)");
    rc = run("echo $a");
    expect_exact("a plain find starts at the percent", "2\r\n");
    rc = run("echo $b");
    expect_exact("a plain find ends at the d", "3\r\n");
    rc = run("set s, n gsub(\"a1b2\", \"%d\", \"x\")");
    expect_rc("gsub succeeds", rc, 0);
    rc = run("echo $s");
    expect_exact("gsub replaces each digit", "axbx\r\n");
    rc = run("echo $n");
    expect_exact("gsub counts the replacements", "2\r\n");
    rc = run("set s gsub(\"ab\", \"(.)\", \"[%1]\")");
    rc = run("echo $s");
    expect_exact("a replacement keeps the capture", "[a][b]\r\n");
    rc = run("set s gsub(\"a\", \"a\", \"%%\")");
    rc = run("echo $s");
    expect_exact("a doubled percent is one percent", "%\r\n");
    rc = run("set s match(\"a\", \"[\")");
    expect_rc("a broken pattern fails", rc, FREYA_EXIT_FAIL);
    expect_has("a broken pattern is named", "bad pattern");
    rc = run("set s gsub(\"abcdefghijklmnop\", \".\", \"xy\")");
    expect_rc("a long replacement fails", rc, FREYA_EXIT_FAIL);
    expect_has("a long replacement is too long", "string too long");
    rc = run("fn match; return 1; end");
    expect_rc("match cannot be defined", rc, FREYA_EXIT_FAIL);
    expect_has("fn refuses match", "bad name");
    rc = run("if match(\"ab12\", \"%d+\") /= none; echo yes; else; echo no; end");
    expect_exact("a match is a condition value", "yes\r\n");
    rc = run("set s match(\"aa\", \"(a)%1\")");
    rc = run("echo $s");
    expect_exact("a pattern can repeat a capture", "a\r\n");
    rc = run("set s match(\"abc\", \"b\", -2)");
    rc = run("echo $s");
    expect_exact("a negative index counts from the end", "b\r\n");
    rc = run("set s, n gsub(\"a1b2\", \"%d\", \"x\", 1)");
    rc = run("echo $s");
    expect_exact("gsub stops after the given count", "axb2\r\n");
    rc = run("echo $n");
    expect_exact("gsub reports the limited count", "1\r\n");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
