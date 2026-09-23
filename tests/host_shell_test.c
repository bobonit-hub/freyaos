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

int  sd_init(void)
{
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

int  gpio_pin_mode(int pin, int mode)  { (void)pin; (void)mode; return 0; }
int  gpio_pin_read(int pin)            { (void)pin; return 0; }
int  gpio_pin_write(int pin, int value){ (void)pin; (void)value; return 0; }
int  gpio_pin_toggle(int pin)          { (void)pin; return 0; }

int  pwm_info(int idx, pwm_info_t *info) { (void)idx; (void)info; return -1; }
int  pwm_lookup(int pin)               { (void)pin; return -1; }
int  pwm_close(int pwm)                { (void)pwm; return 0; }
int  pwm_open(int pin, uint32_t hz, uint32_t duty)
{
    (void)pin; (void)hz; (void)duty; return -1;
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

int xmodem_receive_to_file(const char *path, uint32_t *received, int strip)
{
    (void)path; (void)received; (void)strip; return -1;
}

static void plant_mmio(void)
{
    static const uintptr_t pages[] = {
        0xE000E000UL,   /* SCB */
        0xE0042000UL,   /* DBGMCU_IDCODE */
        0x1FFF7000UL    /* unique id and flash size */
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
    SCB->CPUID = 0x410FC241UL;
    *(volatile uint32_t *)0xE0042000UL = 0x10006411UL;
    ((volatile uint32_t *)0x1FFF7A10UL)[0] = 0x11111111UL;
    ((volatile uint32_t *)0x1FFF7A10UL)[1] = 0x22222222UL;
    ((volatile uint32_t *)0x1FFF7A10UL)[2] = 0x33333333UL;
    *(volatile uint16_t *)0x1FFF7A22UL = 512;
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
    g_clocks.sysclk_hz = 96000000;
    g_clocks.hclk_hz = 96000000;
    g_clocks.pclk1_hz = 48000000;
    g_clocks.pclk2_hz = 96000000;
    g_clocks.clock_source = 1;

    printf("help\n");
    rc = run("help");
    expect_rc("help succeeds", rc, 0);
    expect_has("help introduces the list", "Freya commands:\r\n");
    expect_has("help lists stop with its argument", "  stop [thread]\r\n");
    expect_has("help lists sleep", "  sleep <ms>\r\n");
    expect_has("help lists if", "  if <command>\r\n");
    expect_has("help lists loop", "  loop <count>\r\n");
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

    printf("commands\n");
    rc = run("status");
    expect_rc("status succeeds", rc, 0);
    expect_has("status reports the last command", "command");
    expect_has("status before any program", "nothing has run since reset");

    rc = run("sysinfo");
    expect_rc("sysinfo succeeds", rc, 0);
    expect_has("sysinfo names the board", "Black Pill");
    expect_has("sysinfo reads the CPUID", "410fc241");
    expect_has("sysinfo reads the device id", "0x411");
    expect_has("sysinfo reads the unique id", "11111111-22222222-33333333");
    expect_has("sysinfo reads the flash size", "512 KiB internal");
    expect_has("sysinfo reports the clock", "96000000 Hz");
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

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
