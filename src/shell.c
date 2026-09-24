/*
 * Freya - console shell on USART2.
 *
 * Line editing with backspace, Ctrl-U, Ctrl-C and a small command
 * history on the cursor keys, plus the built-in command set.  Every
 * command leaves an exit status behind, which '$?' and 'status' read.
 * ';' separates commands on one line.  'if'/'else'/'end' and 'loop'
 * group them; a block that is still open is finished on later lines.
 * 'set' keeps a few named values — integer, float or string — and
 * '$name' expands one the way '$?' expands the status.
 * int(), float(), str() and hex() convert between those and hex text.
 * rand() and srand() are the ANSI C 1989 example generator.
 * sin(), cos() and pi() are single-precision.
 * open(), read(), write(), close(), seek() and flush() are the file calls.
 * match(), find() and gsub() are Lua patterns: % classes, captures.
 * now(), date(), time() and the calendar fields read the software clock.
 * timer() and irq() arm a hardware timer or a pin edge.  wait() runs the
 * script function named for that source, in thread mode, then returns.
 * 'fn' defines a function of up to 32 arguments.  'return' leaves it
 * from anywhere in the body with 1 to 32 values, each an integer, a
 * float or a string.
 * 'source' runs the same language from a file or from program flash.
 */
#include "freya.h"
#include "fat.h"

/* The 48 KiB kernel image has no room left for the script interpreter.
 * The extension is a separate image; a call across the two is an
 * ordinary branch.  These stay out of line so they are not copied
 * back into the main image. */
#define KEXT __attribute__((noinline, section(".text.kext_script")))

#define LINE_MAX    160
#define MAX_ARGS    16
#define HIST_DEPTH  8
#define SOURCE_NEST 3           /* shell_exec frames, including this one */

static char s_hist[HIST_DEPTH][LINE_MAX];
static int  s_hist_count;
static int  s_hist_pos;
static int  s_status;               /* status of the last command, '$?' */
static char s_poll_line[LINE_MAX];  /* a command typed during a run       */
static int  s_poll_len;
static int  s_script_stop;          /* Ctrl-C while a script or sleep runs */
static int  s_exec_depth;           /* shell_exec frames currently active  */

/* Script threads share this interpreter.  Declarations; the scheduler
 * is with the script runner. */
static int  KEXT sh_pump(void);
static void KEXT sh_stop_all(void);
static int  KEXT sh_alive(void);
static int  KEXT sh_stop_named(const char *name);
static void KEXT sh_list(void);
static int  KEXT sh_getc(void);
static void KEXT s_now_add(uint32_t ms);
static uint32_t KEXT sh_soon(void); /* ms until the next wake, or 0 */

/* ------------------------------------------------------- line editing */
static void erase_line(int len)
{
    for (int i = 0; i < len; i++) uart_puts("\b \b");
}

static void hist_push(const char *line)
{
    if (!line[0]) return;
    if (s_hist_count && strcmp(s_hist[(s_hist_count - 1) % HIST_DEPTH], line) == 0)
        return;
    strncpy(s_hist[s_hist_count % HIST_DEPTH], line, LINE_MAX - 1);
    s_hist[s_hist_count % HIST_DEPTH][LINE_MAX - 1] = '\0';
    s_hist_count++;
}

static const char *hist_get(int back)
{
    int idx;

    if (back <= 0 || back > s_hist_count || back > HIST_DEPTH) return NULL;
    idx = (s_hist_count - back) % HIST_DEPTH;
    return s_hist[idx];
}

/* Returns the line length, or -1 when the line was cancelled. */
static int readline(char *buf, int max)
{
    int len = 0;

    s_hist_pos = 0;
    for (;;) {
        int c = sh_getc();

        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            uart_puts("\r\n");
            buf[len] = '\0';
            return len;
        }
        if (c == 0x03) {                    /* Ctrl-C */
            sh_stop_all();
            if (!s_script_stop) uart_puts("^C\r\n");
            s_script_stop = 0;
            buf[0] = '\0';
            return -1;
        }
        if (c == 0x15) {                    /* Ctrl-U */
            erase_line(len);
            len = 0;
            continue;
        }
        if (c == 0x08 || c == 0x7F) {       /* backspace */
            if (len > 0) { len--; uart_puts("\b \b"); }
            continue;
        }
        if (c == 0x1B) {                    /* escape sequence */
            int a = uart_getc_timeout(50);
            int b = uart_getc_timeout(50);
            if (a == '[' && (b == 'A' || b == 'B')) {
                const char *h;
                int want = (b == 'A') ? s_hist_pos + 1 : s_hist_pos - 1;

                if (want < 0) want = 0;
                h = hist_get(want);
                if (want == 0) {
                    erase_line(len);
                    len = 0;
                    s_hist_pos = 0;
                } else if (h) {
                    erase_line(len);
                    len = (int)strlen(h);
                    if (len > max - 1) len = max - 1;
                    memcpy(buf, h, (size_t)len);
                    uart_write(buf, len);
                    s_hist_pos = want;
                }
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;

        if (len < max - 1) {
            buf[len++] = (char)c;
            uart_putc((char)c);
        }
    }
}

/* ------------------------------------------------------------ helpers */
static int split_args(char *line, char **argv, int max)
{
    int argc = 0;

    while (*line && argc < max) {
        while (*line == ' ' || *line == '\t') *line++ = '\0';
        if (!*line) break;

        if (*line == '"') {
            line++;
            argv[argc++] = line;
            while (*line && *line != '"') line++;
        } else {
            argv[argc++] = line;
            while (*line && *line != ' ' && *line != '\t') line++;
        }
        if (*line) *line++ = '\0';
    }
    return argc;
}

/*
 * A command's own result becomes '$?'.  Commands report failure as -1,
 * which is a status of 1; 'run' reports the program's exit status, which
 * passes through as it is.
 */
static int status_of(int rc)
{
    if (rc < 0) return FREYA_EXIT_FAIL;
    return rc & FREYA_EXIT_MAX;
}

/* Text of one variable.  0 on success, -1 when that name is not set.
 * Defined with the variable store; a call from here is into the
 * extension, same as any other branch across the two images. */
static int KEXT var_copy(const char *name, int nlen, char *out, int size);
static int KEXT var_elem_copy(const char *name, int nlen, const char **pp,
                              char *out, int size);

/* Text of function argument idx.  0 is the count.  -1 when this is
 * not a call, or that argument was not passed. */
static int KEXT arg_copy(int idx, char *out, int size);

static int name_char(char c, int first)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
        return 1;
    return !first && c >= '0' && c <= '9';
}

/*
 * '$?' becomes the status of the command before this one.  '$name'
 * becomes that variable, rendered as text.  Both are expanded when
 * the command runs, so each pass of a loop sees the values the
 * previous command left.  -1 leaves a message and sets nothing.
 */
static int expand_status(const char *in, char *out, int size)
{
    int o = 0;

    while (*in && o < size - 1) {
        if (in[0] == '$' && in[1] == '?') {
            char num[12];
            int n = ksnprintf(num, (int)sizeof(num), "%d", s_status);

            if (n > (int)sizeof(num) - 1) n = (int)sizeof(num) - 1;
            for (int i = 0; i < n && o < size - 1; i++) out[o++] = num[i];
            in += 2;
            continue;
        }
        if (in[0] == '$' && in[1] >= '0' && in[1] <= '9') {
            char tmp[40];
            int idx = 0, k = 0;

            while (in[1 + k] >= '0' && in[1 + k] <= '9') {
                idx = idx * 10 + (in[1 + k] - '0');
                k++;
                if (k > 2 || idx > 32) {
                    kprintf("bad name\r\n");
                    s_status = FREYA_EXIT_FAIL;
                    out[0] = '\0';
                    return -1;
                }
            }
            if (arg_copy(idx, tmp, (int)sizeof tmp) != 0) {
                kprintf("no such argument\r\n");
                s_status = FREYA_EXIT_FAIL;
                out[0] = '\0';
                return -1;
            }
            for (int i = 0; tmp[i] && o < size - 1; i++) out[o++] = tmp[i];
            in += 1 + k;
            continue;
        }
        if (in[0] == '$' && name_char(in[1], 1)) {
            char tmp[40];
            int n = 0;

            while (name_char(in[1 + n], 0)) n++;
            if (n >= 8) {
                kprintf("bad name\r\n");
                s_status = FREYA_EXIT_FAIL;
                out[0] = '\0';
                return -1;
            }
            if (in[1 + n] == '[') {
                const char *p = in + 1 + n;
                int ev = var_elem_copy(in + 1, n, &p, tmp, (int)sizeof tmp);
                if (ev != 0) {
                    if (ev < 0 && ev != -1)
                        kprintf("no such variable\r\n");
                    s_status = FREYA_EXIT_FAIL;
                    out[0] = '\0';
                    return -1;
                }
                in = p;
            } else {
                if (var_copy(in + 1, n, tmp, (int)sizeof tmp) != 0) {
                    kprintf("no such variable\r\n");
                    s_status = FREYA_EXIT_FAIL;
                    out[0] = '\0';
                    return -1;
                }
                in += 1 + n;
            }
            for (int i = 0; tmp[i] && o < size - 1; i++) out[o++] = tmp[i];
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
    return 0;
}

static void print_fat_time(uint16_t date, uint16_t time)
{
    kput_hms(1980 + (date >> 9), (date >> 5) & 0x0F, date & 0x1F,
             (time >> 11) & 0x1F, (time >> 5) & 0x3F, -1);
}

static void attr_string(uint8_t attr, char *out)
{
    out[0] = (attr & FAT_ATTR_DIR)    ? 'd' : '-';
    out[1] = (attr & FAT_ATTR_RDONLY) ? 'r' : 'w';
    out[2] = (attr & FAT_ATTR_HIDDEN) ? 'h' : '-';
    out[3] = (attr & FAT_ATTR_SYSTEM) ? 's' : '-';
    out[4] = (attr & FAT_ATTR_ARCHIVE)? 'a' : '-';
    out[5] = '\0';
}

static const char *onoff(int v) { return v ? "on" : "off"; }

static int need_fs(void)
{
    if (fat_mounted()) return 1;
    kprintf("no filesystem mounted - run 'mount'\r\n");
    return 0;
}

static int busy_running(const char *who)
{
    if (!g_app.running) return 0;
    kprintf("%s: a program is running - stop it first\r\n", who);
    return 1;
}

static int usage(const char *s)
{
    kprintf("usage: %s\r\n", s);
    return -1;
}

static int fs_fail(const char *cmd, const char *path, int rc)
{
    if (path) kprintf("%s: %s: %s\r\n", cmd, path, fat_err_str(rc));
    else      kprintf("%s: %s\r\n", cmd, fat_err_str(rc));
    return -1;
}

static uint32_t ratio(uint32_t n, uint32_t d, uint32_t scale)
{
    if (!d) return 0;
    if (n >= d) return scale;
    if (n <= 0xFFFFFFFFu / scale) return n * scale / d;
    if (d < scale) return n ? scale : 0;
    return n / (d / scale);
}

static void print_bar(uint32_t used, uint32_t total)
{
    uint32_t filled = ratio(used, total, 20);

    uart_putc('[');
    for (uint32_t i = 0; i < 20; i++) uart_putc(i < filled ? '#' : '.');
    kprintf("] %u%%", ratio(used, total, 100));
}

static void mem_at(const char *k, uint32_t n, uint32_t addr)
{
    kprintf("  %-14s: %6u B  at 0x%08x\r\n", k, n, addr);
}

static void bar_nl(uint32_t used, uint32_t total)
{
    kprintf("     ");
    print_bar(used, total);
    kprintf("\r\n");
}

static void inf(const char *k)
{
    kprintf("  %-11s: ", k);
}

/* The installed flash image is named by a pseudo-path rather than a file,
 * so 'load' and 'run' take it without needing a mounted card. */
#ifdef FREYA_APP_FLASH_ADDR
#define PROG_ARG  "<file>|" APP_FLASH_PATH
#else
#define PROG_ARG  "<file>"
#endif

static int is_flash_path(const char *p)
{
#ifdef FREYA_APP_FLASH_ADDR
    return strcmp(p, APP_FLASH_PATH) == 0;
#else
    (void)p;
    return 0;
#endif
}

/* KiB of internal flash.  The size register is a floor of BOARD_FLASH_KIB:
 * a Blue Pill often still reads 64, and every supported board has at least
 * 128.  A larger report (the Black Pill's 512) is kept. */
static uint32_t mcu_flash_kib(void)
{
    uint32_t kib = *(volatile uint16_t *)FLASHSIZE_BASE;

    if (kib < BOARD_FLASH_KIB) kib = BOARD_FLASH_KIB;
    return kib;
}

/* ----------------------------------------------------------- commands */
static int cmd_help(int argc, char **argv);
static int cmd_script(int argc, char **argv);
static int KEXT cmd_unset(int argc, char **argv);

static int cmd_sysinfo(int argc, char **argv)
{
    uint32_t idcode = DBGMCU_IDCODE;
    uint32_t fl_kb = mcu_flash_kib();
    const uint32_t *uid = (const uint32_t *)UID_BASE;
    uint32_t up = sys_uptime_ms();
    rtc_time_t t;

    (void)argc; (void)argv;
    rtc_get(&t);

    kprintf("Freya %s \"%s\"  (built %s)\r\n",
            FREYA_VERSION, FREYA_CODENAME, FREYA_BUILD_ID);
    inf("board");      kprintf("%s\r\n", BOARD_NAME);
    inf("core");       kprintf("%s, CPUID 0x%08x\r\n", BOARD_CORE, SCB->CPUID);
    inf("device id");  kprintf("0x%03x  rev 0x%04x\r\n",
            idcode & 0xFFF, (idcode >> 16) & 0xFFFF);
    inf("unique id");  kprintf("%08x-%08x-%08x\r\n", uid[0], uid[1], uid[2]);
    inf("flash");      kprintf("%u KiB internal\r\n", fl_kb);
    inf("clock src");  kprintf("%s -> PLL\r\n",
            g_clocks.clock_source ? BOARD_HSE_NAME
                                  : BOARD_HSI_NAME " (crystal not found)");
    inf("sysclk");     kprintf("%u Hz   AHB %u Hz\r\n", g_clocks.sysclk_hz, g_clocks.hclk_hz);
    inf("apb1/apb2");  kprintf("%u Hz / %u Hz\r\n", g_clocks.pclk1_hz, g_clocks.pclk2_hz);
    inf("console");    kprintf("%s\r\n", BOARD_CONSOLE_NAME);
    inf("reset by");   kprintf("%s\r\n", sys_reset_cause_str());
    inf("uptime");     kprintf("%u.%03u s\r\n", up / 1000, up % 1000);
    inf("date/time");  kput_hms(t.year, t.mon, t.day, t.hour, t.min, t.sec);
    kprintf("\r\n");
    inf("log level");  kprintf("%s (%d)\r\n",
            log_level_str(log_get_level()), log_get_level());
#ifdef FREYA_APP_FLASH_ADDR
    inf("auto-start"); kprintf("%s\r\n", onoff(app_autostart_enabled()));
    inf("ram dump");   kprintf("%s\r\n", onoff(app_ramdump_enabled()));
#endif

    inf("sd card");
    if (!sd_powered()) {
        kprintf("power off\r\n");
    } else {
        kprintf("%s", sd_type_str());
        if (g_sd.initialised) {
            kprintf(", ");
            kput_size((uint64_t)g_sd.blocks * 512ULL);
            kprintf(" (%u blocks)", g_sd.blocks);
        }
        kprintf("\r\n");
    }

    if (fat_mounted()) {
        kprintf("  filesystem : %s", fat_type_str());
        if (g_fs.label[0]) kprintf(" \"%s\"", g_fs.label);
        kprintf(", cluster ");
        kput_size(g_fs.bytes_per_clus);
        kprintf(", %u clusters\r\n", g_fs.clus_count);
    } else {
        kprintf("  filesystem : not mounted\r\n");
    }

    if (g_app.loaded) {
        kprintf("  program    : %s (%s), entry 0x%08x\r\n",
                g_app.path, g_app.name[0] ? g_app.name : "unnamed", g_app.entry);
    } else {
        kprintf("  program    : none loaded\r\n");
    }
    return 0;
}

static int cmd_meminfo(int argc, char **argv)
{
    uint32_t data_sz = (uint32_t)((uint8_t *)__data_end - (uint8_t *)__data_start);
    uint32_t bss_sz  = (uint32_t)((uint8_t *)__bss_end  - (uint8_t *)__bss_start);
    uint32_t flash_used = (uint32_t)((uint8_t *)__kernel_flash_end - (uint8_t *)0x08000000UL);
    uint32_t flash_total = mcu_flash_kib() * 1024UL;
    uint32_t heap_total, heap_used, heap_free, heap_big, heap_blocks;
    uint32_t stack_total = (uint32_t)((uint8_t *)__stack_top - (uint8_t *)__stack_limit);
    uint32_t app_total = FREYA_APP_REGION_SIZE;

    (void)argc; (void)argv;
    heap_stats(&heap_total, &heap_used, &heap_free, &heap_big, &heap_blocks);

    kprintf("Flash 0x08000000 .. 0x%08x\r\n", 0x08000000UL + flash_total);
    kprintf("  kernel image   : ");
    kput_size(flash_used);
    kprintf(" of ");
    kput_size(flash_total);
    kprintf("   ");
    print_bar(flash_used, flash_total);
    kprintf("\r\n");

#ifdef FREYA_APP_FLASH_ADDR
    {
        const freya_app_header_t *h = app_flash_header();
        char nm[sizeof(h->name) + 1];

        kprintf("  program flash  : %6u B  at 0x%08x\r\n",
                (unsigned)FREYA_APP_FLASH_SIZE, (unsigned)FREYA_APP_FLASH_ADDR);
        if (h) {
            memcpy(nm, h->name, sizeof(h->name));   /* may not be NUL padded */
            nm[sizeof(h->name)] = '\0';
            kprintf("     \"%s\": image %u B, .data %u B, .bss %u B\r\n",
                    nm, h->image_size,
                    h->data_end - h->data_start, h->bss_end - h->bss_start);
            kprintf("     ");
            print_bar(h->image_size, FREYA_APP_FLASH_SIZE);
            kprintf("\r\n");
        } else {
            const char *script = NULL;
            uint32_t slen = 0;

            if (app_script_find(&script, &slen) > 0) {
                kprintf("     shell script, %u B\r\n", slen);
                kprintf("     ");
                print_bar((uint32_t)sizeof(freya_script_header_t) + slen + 1U,
                          FREYA_APP_FLASH_SIZE);
                kprintf("\r\n");
            } else {
                kprintf("     empty - 'install <file>', or flash one in with the kernel\r\n");
            }
        }
        kprintf("  auto-start     : %s  at 0x%08x  (%u B)\r\n",
                onoff(app_autostart_enabled()),
                (unsigned)FREYA_AUTOSTART_ADDR,
                (unsigned)FREYA_AUTOSTART_SIZE);
        kprintf("  log level      : %s (%d)  stored at 0x%08x\r\n",
                log_level_str(log_get_level()), log_get_level(),
                (unsigned)(FREYA_AUTOSTART_ADDR + FREYA_LOGLEVEL_OFF));
        kprintf("  ram dump       : %s  stored at 0x%08x\r\n",
                onoff(app_ramdump_enabled()),
                (unsigned)(FREYA_AUTOSTART_ADDR + FREYA_RAMDUMP_OFF));
    }
#endif

    kprintf("SRAM  0x%08x .. 0x%08x  (%u KiB)\r\n",
            (uint32_t)(uintptr_t)__ram_start, (uint32_t)(uintptr_t)__ram_end,
            (uint32_t)(__ram_end - __ram_start) / 1024U);
    mem_at(".data", data_sz, (uint32_t)(uintptr_t)__data_start);
    mem_at(".bss", bss_sz, (uint32_t)(uintptr_t)__bss_start);
    mem_at("system heap", heap_total, (uint32_t)(uintptr_t)__heap_start);
    kprintf("     used %u B, free %u B, largest free block %u B, %u blocks\r\n",
            heap_used, heap_free, heap_big, heap_blocks);
    bar_nl(heap_used, heap_total);

    mem_at("program region", app_total, (uint32_t)FREYA_APP_LOAD_ADDR);
    if (g_app.loaded && (g_app.flags & FREYA_APP_F_XIP)) {
        uint32_t data_sz2 = g_app.data_end - g_app.data_start;

        if (g_app.load_addr >= FREYA_APP_LOAD_ADDR &&
            g_app.load_addr < FREYA_APP_LOAD_ADDR + FREYA_APP_REGION_SIZE) {
            kprintf("     %s (copied from flash): image %u B, data %u B + "
                    "bss %u B\r\n", g_app.name[0] ? g_app.name : g_app.path,
                    g_app.image_size, data_sz2, g_app.bss_size);
            bar_nl(g_app.image_size + data_sz2 + g_app.bss_size, app_total);
        } else {
            kprintf("     %s (runs from flash): data %u B + bss %u B\r\n",
                    g_app.name[0] ? g_app.name : g_app.path, data_sz2,
                    g_app.bss_size);
            bar_nl(data_sz2 + g_app.bss_size, app_total);
        }
    } else if (g_app.loaded) {
        kprintf("     %s: image %u B + bss %u B\r\n",
                g_app.name[0] ? g_app.name : g_app.path, g_app.image_size, g_app.bss_size);
        bar_nl(g_app.image_size + g_app.bss_size, app_total);
    } else {
        kprintf("     empty\r\n");
    }

    mem_at("main stack", stack_total, (uint32_t)(uintptr_t)__stack_limit);
    kprintf("     in use now %u B, peak since boot %u B\r\n", stack_used(), stack_peak());
    bar_nl(stack_peak(), stack_total);
    return 0;
}

static int cmd_mount(int argc, char **argv)
{
    int rc;

    (void)argc; (void)argv;
    fs_close_all();                     /* nothing may survive a remount */
    kprintf("initialising SD card ... ");
    if (sd_init() != 0) {
        kprintf("failed (no card, or wiring/level problem)\r\n");
        return -1;
    }
    kprintf("%s\r\n", sd_type_str());

    rc = fat_mount();
    if (rc != FAT_OK) return fs_fail("mount", NULL, rc);
    kprintf("mounted %s", fat_type_str());
    if (g_fs.label[0]) kprintf(" \"%s\"", g_fs.label);
    kprintf(" on /\r\n");
    return 0;
}

/* --------------------------------------------------------------- power */
/* Its own section, so the Black Pill linker can keep it out of the
 * 48 KiB image.  The Blue Pill image still has room and leaves it there. */
#define POWER_USAGE  "power [sd [on|off]]"
#define POWER_TEXT __attribute__((noinline, section(".text.cmd_power")))

static int POWER_TEXT cmd_power(int argc, char **argv)
{
    int on, rc;

    if (argc == 1) {
        kprintf("sd %s\r\n", sd_powered() ? "on" : "off");
        kprintf("usage: %s\r\n", POWER_USAGE);
        return 0;
    }
    if (strcmp(argv[1], "sd") != 0) return usage(POWER_USAGE);
    if (argc == 2) {
        kprintf("sd %s\r\n", sd_powered() ? "on" : "off");
        return 0;
    }
    if (argc != 3) return usage(POWER_USAGE);
    if (strcmp(argv[2], "on") == 0) on = 1;
    else if (strcmp(argv[2], "off") == 0) on = 0;
    else return usage(POWER_USAGE);

    rc = board_power(FREYA_PWR_SD, on);
    if (rc < 0) {
        kprintf("power: refused\r\n");
        return -1;
    }
    kprintf("sd %s\r\n", on ? "on" : "off");
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    char path[FAT_MAX_PATH];
    fat_dir_t dir;
    fat_dirent_t e;
    int long_fmt = 0;
    const char *target = NULL;
    uint32_t files = 0, dirs = 0, bytes = 0;
    int rc, col = 0;

    if (!need_fs()) return -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) long_fmt = 1;
        else target = argv[i];
    }
    if (fs_abspath(target ? target : ".", path, sizeof(path)) != 0)
        return fs_fail("ls", target ? target : ".", FAT_ERR_INVAL);
    rc = fat_opendir(&dir, path);
    if (rc != FAT_OK) return fs_fail("ls", path, rc);

    if (long_fmt) kprintf("%s:\r\n", path);

    for (;;) {
        rc = fat_readdir(&dir, &e);
        if (rc == 1) break;
        if (rc < 0) { fs_fail("ls", NULL, rc); break; }
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;

        if (e.attr & FAT_ATTR_DIR) dirs++;
        else { files++; bytes += e.size; }

        if (long_fmt) {
            char attr[6];
            attr_string(e.attr, attr);
            kprintf("  %s ", attr);
            if (e.attr & FAT_ATTR_DIR) kprintf("%10s", "<DIR>");
            else                       kprintf("%10u", e.size);
            kprintf("  ");
            print_fat_time(e.wdate, e.wtime);
            kprintf("  %s\r\n", e.name);
        } else {
            int len = (int)strlen(e.name) + ((e.attr & FAT_ATTR_DIR) ? 1 : 0);
            kprintf("%s%s", e.name, (e.attr & FAT_ATTR_DIR) ? "/" : "");
            col += len;
            if (col > 56) { kprintf("\r\n"); col = 0; }
            else { for (int i = len; i < 20; i++) uart_putc(' '); col += 20 - len; }
        }
    }
    fat_closedir(&dir);
    if (!long_fmt && col) kprintf("\r\n");

    kprintf("  %u file%s, %u director%s, ", files, files == 1 ? "" : "s",
            dirs, dirs == 1 ? "y" : "ies");
    kput_size(bytes);
    kprintf(" total\r\n");
    return 0;
}

static int cmd_cd(int argc, char **argv)
{
    int rc;

    if (!need_fs()) return -1;
    rc = fs_chdir(argc > 1 ? argv[1] : "/");
    if (rc != FAT_OK) return fs_fail("cd", argc > 1 ? argv[1] : "/", rc);
    return 0;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s\r\n", fs_cwd());
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    char path[FAT_MAX_PATH];
    int rc;

    if (!need_fs()) return -1;
    if (argc < 2) return usage("mkdir <directory>");

    for (int i = 1; i < argc; i++) {
        if (fs_abspath(argv[i], path, sizeof(path)) != 0)
            return fs_fail("mkdir", argv[i], FAT_ERR_INVAL);
        rc = fat_mkdir(path);
        if (rc != FAT_OK) return fs_fail("mkdir", argv[i], rc);
        kprintf("created %s\r\n", path);
    }
    return 0;
}

static int remove_recursive(const char *path, int depth)
{
    fat_dirent_t e;
    int rc;

    if (depth > 8) return FAT_ERR_INVAL;

    rc = fat_stat(path, &e);
    if (rc != FAT_OK) return rc;

    if (e.attr & FAT_ATTR_DIR) {
        for (;;) {
            fat_dir_t dir;
            char child[FAT_MAX_PATH];
            int found = 0;

            rc = fat_opendir(&dir, path);
            if (rc != FAT_OK) return rc;

            /* One entry per pass: removing invalidates the scan position. */
            for (;;) {
                fat_dirent_t c;
                rc = fat_readdir(&dir, &c);
                if (rc != 0) break;
                if (strcmp(c.name, ".") == 0 || strcmp(c.name, "..") == 0) continue;
                ksnprintf(child, sizeof(child), "%s%s%s", path,
                          path[strlen(path) - 1] == '/' ? "" : "/", c.name);
                found = 1;
                break;
            }
            fat_closedir(&dir);
            if (!found) break;

            rc = remove_recursive(child, depth + 1);
            if (rc != FAT_OK) return rc;
        }
    }
    return fat_unlink(path);
}

static int cmd_rm(int argc, char **argv)
{
    char path[FAT_MAX_PATH];
    int recursive = 0, count = 0, rc;

    if (!need_fs()) return -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0) {
            recursive = 1;
            continue;
        }
        if (fs_abspath(argv[i], path, sizeof(path)) != 0)
            return fs_fail("rm", argv[i], FAT_ERR_INVAL);
        if (strcmp(path, "/") == 0) {
            kprintf("rm: refusing to remove the root directory\r\n");
            return -1;
        }
        rc = recursive ? remove_recursive(path, 0) : fat_unlink(path);
        if (rc != FAT_OK) {
            kprintf("rm: %s: %s%s\r\n", argv[i], fat_err_str(rc),
                    rc == FAT_ERR_NOTEMPTY ? " (use rm -r)" : "");
            return -1;
        }
        kprintf("removed %s\r\n", path);
        count++;
    }
    if (!count) return usage("rm [-r] <file|directory>");
    return 0;
}

static int cmd_rename(int argc, char **argv)
{
    int rc;

    if (!need_fs()) return -1;
    if (argc != 3) return usage("rename <old> <new>");

    rc = fs_rename(argv[1], argv[2]);
    if (rc != FAT_OK) return fs_fail(argv[0], NULL, rc);
    kprintf("renamed %s -> %s\r\n", argv[1], argv[2]);
    return 0;
}

static int cmd_download(int argc, char **argv)
{
    uint32_t got = 0;
    int strip = 1;
    int have_exact = 0;
    int32_t exact = -1;
    const char *name = NULL;
    int rc;

    if (!need_fs()) return -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) strip = 0;
        else if (strcmp(argv[i], "--size") == 0) {
            uint32_t n = 0;
            if (i + 1 >= argc || str_to_u32(argv[++i], &n) != 0)
                return usage("download <file> [--raw] [--size <bytes>]");
            have_exact = 1;
            exact = (int32_t)n;
        } else name = argv[i];
    }
    if (!name) {
        kprintf("  receives an XMODEM / XMODEM-1K stream, e.g. 'sx -k file'\r\n");
        return usage("download <file> [--raw] [--size <bytes>]");
    }

    kprintf("Ready to receive '%s' over XMODEM.\r\n"
            "Start the transfer on the host now (Ctrl-X twice on the host to abort).\r\n",
            name);

    rc = xmodem_receive_to_file(name, &got, strip, have_exact ? exact : -1);
    if (rc == 0) {
        kprintf("\r\nreceived ");
        kput_size(got);
        kprintf(" (%u bytes) into %s\r\n", got, name);
        return 0;
    }

    kprintf("\r\ndownload failed: ");
    switch (rc) {
    case -2: uart_puts("timed out waiting for the sender\r\n"); break;
    case -3: uart_puts("cancelled by the sender\r\n"); break;
    case -4: uart_puts("too many bad packets\r\n"); break;
    case -5: uart_puts("packet sequence error\r\n"); break;
    case -6: uart_puts("cannot write to the card\r\n"); break;
    default: kprintf("error %d\r\n", rc); break;
    }
    return -1;
}

static int cmd_upload(int argc, char **argv)
{
    char path[FAT_MAX_PATH];
    fat_dirent_t e;
    uint32_t got = 0;
    int rc;

    if (!need_fs()) return -1;
    if (argc != 2) {
        kprintf("  sends a file as an XMODEM / XMODEM-1K stream\r\n");
        return usage("upload <file>");
    }
    if (fs_abspath(argv[1], path, sizeof(path)) != 0)
        return fs_fail("upload", argv[1], FAT_ERR_INVAL);
    rc = fat_stat(path, &e);
    if (rc != FAT_OK) return fs_fail("upload", argv[1], rc);
    if (e.attr & FAT_ATTR_DIR) {
        kprintf("upload: %s: is a directory\r\n", argv[1]);
        return -1;
    }

    kprintf("Ready to send '%s' (%u bytes) over XMODEM.\r\n"
            "Start the receiver on the host now.\r\n",
            argv[1], e.size);

    rc = xmodem_send_file(path, &got);
    if (rc == 0) {
        kprintf("\r\nsent ");
        kput_size(got);
        kprintf(" (%u bytes) from %s\r\n", got, argv[1]);
        return 0;
    }

    kprintf("\r\nupload failed: ");
    switch (rc) {
    case -2: uart_puts("timed out waiting for the receiver\r\n"); break;
    case -3: uart_puts("cancelled by the receiver\r\n"); break;
    case -4: uart_puts("too many bad packets\r\n"); break;
    case -6: uart_puts("cannot read the card\r\n"); break;
    default: kprintf("error %d\r\n", rc); break;
    }
    return -1;
}

static int cmd_cat(int argc, char **argv)
{
    uint8_t buf[128];
    int fd, n;

    if (!need_fs()) return -1;
    if (argc < 2) return usage("cat <file>");

    fd = fs_fd_open(argv[1], FREYA_O_RDONLY);
    if (fd < 0) return fs_fail("cat", argv[1], fd);

    while ((n = fs_fd_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') uart_putc('\r');
            uart_putc((char)buf[i]);
        }
    }
    fs_fd_close(fd);
    kprintf("\r\n");
    return 0;
}

static int cmd_write(int argc, char **argv)
{
    int fd, rc = 0;

    if (!need_fs()) return -1;
    if (argc < 3) return usage("write <file> <text...>");

    fd = fs_fd_open(argv[1], FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_APPEND);
    if (fd < 0) return fs_fail("write", argv[1], fd);

    for (int i = 2; i < argc; i++) {
        int len = (int)strlen(argv[i]);
        if (fs_fd_write(fd, argv[i], len) != len) { rc = -1; break; }
        if (fs_fd_write(fd, (i + 1 < argc) ? " " : "\r\n", (i + 1 < argc) ? 1 : 2) < 0) {
            rc = -1;
            break;
        }
    }
    fs_fd_close(fd);
    if (rc) kprintf("write: I/O error\r\n");
    return rc;
}

static int cmd_hexdump(int argc, char **argv)
{
    uint8_t buf[16];
    uint32_t off = 0, limit = 256;
    int fd, n;

    if (!need_fs()) return -1;
    if (argc < 2) return usage("hexdump <file> [offset] [length]");
    if (argc > 2) str_to_u32(argv[2], &off);
    if (argc > 3) str_to_u32(argv[3], &limit);

    fd = fs_fd_open(argv[1], FREYA_O_RDONLY);
    if (fd < 0) return fs_fail("hexdump", argv[1], fd);
    fs_fd_seek(fd, (int32_t)off, FREYA_SEEK_SET);

    while (limit > 0 && (n = fs_fd_read(fd, buf, (int)MIN(16U, limit))) > 0) {
        kprintf("%08x  ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n) kprintf("%02x ", buf[i]);
            else       kprintf("   ");
            if (i == 7) uart_putc(' ');
        }
        kprintf(" |");
        for (int i = 0; i < n; i++)
            uart_putc((buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.');
        kprintf("|\r\n");
        off += (uint32_t)n;
        limit -= (uint32_t)n;
    }
    fs_fd_close(fd);
    return 0;
}

static int write_mem_file(const char *who, const char *name,
                          const uint8_t *src, uint32_t size)
{
    uint32_t off;
    int fd, n;

    fd = fs_fd_open(name, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd < 0) return fs_fail(who, name, fd);

    kprintf("writing %u B to %s\r\n", size, name);
    off = 0;
    while (off < size) {
        uint32_t chunk = MIN(512U, size - off);

        if (uart_rx_ready() && uart_getc_timeout(0) == 0x03) {
            uart_rx_flush();
            fs_fd_close(fd);
            kprintf("%s: cancelled\r\n", who);
            return -1;
        }
        n = fs_fd_write(fd, src + off, (int)chunk);
        if (n != (int)chunk) {
            fs_fd_close(fd);
            return fs_fail(who, NULL, n < 0 ? n : FAT_ERR_IO);
        }
        off += chunk;
    }

    n = fs_fd_close(fd);
    if (n != FAT_OK) return fs_fail(who, NULL, n);
    kprintf("wrote %u B\r\n", size);
    return 0;
}

static int cmd_flashdump(int argc, char **argv)
{
    const char *name = "/freya.flash";
    uint32_t size;

    if (!need_fs()) return -1;
    if (argc > 2) return usage("flashdump [file]");
    if (argc == 2) name = argv[1];

    size = mcu_flash_kib() << 10;
    return write_mem_file("flashdump", name, (const uint8_t *)0x08000000UL, size);
}

static int cmd_df(int argc, char **argv)
{
    uint32_t free_clus = 0;
    uint32_t total_clus;

    (void)argc; (void)argv;
    if (!need_fs()) return -1;

    if (g_fs.free_valid) {
        free_clus = g_fs.free_count;        /* maintained since the last scan */
    } else {
        kprintf("scanning the allocation table ...\r\n");
        if (fat_free_clusters(&free_clus) != FAT_OK) {
            kprintf("df: I/O error\r\n");
            return -1;
        }
    }
    total_clus = g_fs.clus_count;

    kprintf("  filesystem : %s", fat_type_str());
    if (g_fs.label[0]) kprintf(" \"%s\"", g_fs.label);
    kprintf("\r\n  capacity   : ");
    kput_size((uint64_t)total_clus * g_fs.bytes_per_clus);
    kprintf("\r\n  free       : ");
    kput_size((uint64_t)free_clus * g_fs.bytes_per_clus);
    kprintf("\r\n  used       : ");
    kput_size((uint64_t)(total_clus - free_clus) * g_fs.bytes_per_clus);
    kprintf("\r\n  cluster    : ");
    kput_size(g_fs.bytes_per_clus);
    kprintf(" (%u sectors)\r\n     ", g_fs.sec_per_clus);
    print_bar(total_clus - free_clus, total_clus);
    kprintf("\r\n");
    return 0;
}

static int cmd_load(int argc, char **argv)
{
    if (argc < 2) return usage("load " PROG_ARG);
    if (!is_flash_path(argv[1]) && !need_fs()) return -1;
    if (g_app.loaded) app_unload();

    if (app_load(argv[1]) != 0) return -1;

    kprintf("loaded %s", g_app.path);
    if (g_app.name[0]) kprintf(" (\"%s\")", g_app.name);
    kprintf(": ");
    kput_size(g_app.image_size);
    kprintf(" image + %u B bss at 0x%08x, entry 0x%08x\r\n",
            g_app.bss_size, g_app.load_addr, g_app.entry);
    return 0;
}

#ifdef FREYA_APP_FLASH_ADDR
static int cmd_install(int argc, char **argv)
{
    if (argc < 2) return usage("install <file>");
    if (!need_fs()) return -1;
    return app_install(argv[1]);
}

static int cmd_uninstall(int argc, char **argv)
{
    (void)argc; (void)argv;

    {
        const char *script = NULL;

        if (!app_flash_header() && app_script_find(&script, NULL) == 0) {
            kprintf("no program is installed in flash\r\n");
            return 0;
        }
    }
    return app_flash_erase();
}

/* The reverse of 'install': write the installed image back to the card
 * so it can be copied, archived, or installed onto another board. */
static int cmd_saveflash(int argc, char **argv)
{
    const freya_app_header_t *h;
    char def[32];
    const char *name;

    if (argc > 2) return usage("saveflash [file]");
    if (!need_fs()) return -1;

    h = app_flash_header();
    if (!h) {
        const char *script = NULL;
        uint32_t slen = 0;

        if (app_script_find(&script, &slen) > 0) {
            name = (argc == 2) ? argv[1] : "/script.sh";
            return write_mem_file("saveflash", name,
                                  (const uint8_t *)script, slen);
        }
        kprintf("saveflash: no program is installed in flash\r\n");
        return -1;
    }

    if (argc == 2) {
        name = argv[1];
    } else {
        char nm[sizeof(h->name) + 1];
        int i, j = 0;

        memcpy(nm, h->name, sizeof(h->name));
        nm[sizeof(h->name)] = '\0';
        def[0] = '/';
        for (i = 0; nm[i] && j < (int)sizeof(def) - 12; i++) {
            char c = nm[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-')
                def[++j] = c;
        }
        if (j == 0) {
            memcpy(def + 1, "program", 7);
            j = 7;
        }
        memcpy(def + 1 + j, ".xip.bin", 9);
        name = def;
    }

    return write_mem_file("saveflash", name,
                          (const uint8_t *)(uintptr_t)FREYA_APP_FLASH_ADDR,
                          h->image_size);
}
#endif

static int cmd_run(int argc, char **argv)
{
    char *app_argv[MAX_ARGS];
    int app_argc = 0;
    int ret;

    if (sh_alive()) {
        kprintf("run: a thread is running\r\n");
        return -1;
    }
    if (argc > 1) {
        fat_dirent_t e;
        char path[FAT_MAX_PATH];
        int is_file = 0;

        if (is_flash_path(argv[1]))
            is_file = 1;                        /* app_load() dispatches */
        else if (fs_abspath(argv[1], path, sizeof(path)) == 0 &&
                 fat_stat(path, &e) == FAT_OK && !(e.attr & FAT_ATTR_DIR))
            is_file = 1;

        if (is_file) {
            if (g_app.loaded) app_unload();
            if (app_load(argv[1]) != 0) return FREYA_EXIT_NOEXEC;
        } else if (!g_app.loaded) {
            kprintf("run: %s: %s\r\n", argv[1], "no such program");
            return FREYA_EXIT_NOTFOUND;
        }
        for (int i = 1; i < argc && app_argc < MAX_ARGS; i++)
            app_argv[app_argc++] = argv[i];
    } else {
        if (!g_app.loaded) {
#ifdef FREYA_APP_FLASH_ADDR
            kprintf("run: no program loaded - 'load <file>', 'run <file>' or 'runflash'\r\n");
#else
            kprintf("run: no program loaded - use 'load <file>' or 'run <file>'\r\n");
#endif
            return FREYA_EXIT_NOTFOUND;
        }
        app_argv[app_argc++] = g_app.path;
    }

    kprintf("--- %s starting (Ctrl-C stops it) ---\r\n",
            g_app.name[0] ? g_app.name : g_app.path);
    uart_drain_tx();

    ret = app_run(app_argc, app_argv);

    kprintf("\r\n--- %s %s, exit status %d, %u ms ---\r\n",
            g_app.name[0] ? g_app.name : g_app.path,
            app_stop_reason_str(g_app.last_stop_reason), ret, g_app.last_run_ms);
    return ret;
}

#ifdef FREYA_APP_FLASH_ADDR
/* 'run @flash' with the path filled in, so a program that was packed into
 * the module - or installed from the card - can be started by name. */
static int cmd_runflash(int argc, char **argv)
{
    char *run_argv[MAX_ARGS];

    if (argc > MAX_ARGS - 1) {
        kprintf("runflash: too many arguments\r\n");
        return -1;
    }
    run_argv[0] = "run";
    run_argv[1] = APP_FLASH_PATH;
    for (int i = 1; i < argc; i++)
        run_argv[i + 1] = argv[i];
    return cmd_run(argc + 1, run_argv);
}

static int parse_onoff(const char *s, int *out)
{
    if (strcmp(s, "on") == 0) { *out = 1; return 0; }
    if (strcmp(s, "off") == 0) { *out = 0; return 0; }
    return -1;
}

static int cmd_slot_flag(int argc, char **argv, const char *label, uint32_t addr,
                         int (*get)(void), int (*set)(int), int warn_empty)
{
    int rc, enable;

    if (argc < 2) {
        kprintf("%s is %s (flag at 0x%08x)\r\n", label, onoff(get()), (unsigned)addr);
        kprintf("usage: %s on|off\r\n", argv[0]);
        return 0;
    }
    if (parse_onoff(argv[1], &enable) != 0) {
        kprintf("usage: %s on|off\r\n", argv[0]);
        return -1;
    }
    if (busy_running(argv[0])) return -1;
    if (enable == get()) {
        kprintf("%s is already %s\r\n", label, onoff(enable));
        return 0;
    }

    kprintf("%s: console input is dropped while flash is busy\r\n", argv[0]);
    uart_drain_tx();
    rc = set(enable);
    uart_rx_flush();
    if (rc != FLASH_OK) {
        kprintf("%s: %s\r\n", argv[0], flash_err_str(rc));
        return -1;
    }
    kprintf("%s %s\r\n", label, onoff(enable));
    if (warn_empty && enable && !app_flash_header())
        kprintf("(no program is installed in flash yet - 'install <file>')\r\n");
    return 0;
}

static int cmd_autostart(int argc, char **argv)
{
    return cmd_slot_flag(argc, argv, "auto-start", FREYA_AUTOSTART_ADDR,
                         app_autostart_enabled, app_autostart_set, 1);
}

static int cmd_ramdump(int argc, char **argv)
{
    return cmd_slot_flag(argc, argv, "ram dump",
                         FREYA_AUTOSTART_ADDR + FREYA_RAMDUMP_OFF,
                         app_ramdump_enabled, app_ramdump_set, 0);
}
#endif

static int parse_log_level(const char *s, int *out)
{
    uint32_t v;

    if (str_to_u32(s, &v) == 0) {
        if (v > (uint32_t)FREYA_LOG_DEBUG) return -1;
        *out = (int)v;
        return 0;
    }
    if (strcasecmp(s, "off") == 0) *out = FREYA_LOG_OFF;
    else if (strcasecmp(s, "error") == 0 || strcasecmp(s, "err") == 0)
        *out = FREYA_LOG_ERROR;
    else if (strcasecmp(s, "warn") == 0 || strcasecmp(s, "warning") == 0)
        *out = FREYA_LOG_WARN;
    else if (strcasecmp(s, "info") == 0) *out = FREYA_LOG_INFO;
    else if (strcasecmp(s, "debug") == 0) *out = FREYA_LOG_DEBUG;
    else return -1;
    return 0;
}

static int cmd_loglevel(int argc, char **argv)
{
    int level, rc;

    if (argc < 2) {
        kprintf("log level is %s (%d)\r\n",
                log_level_str(log_get_level()), log_get_level());
#ifdef FREYA_APP_FLASH_ADDR
        kprintf("stored at 0x%08x (second word of the auto-start slot)\r\n",
                (unsigned)(FREYA_AUTOSTART_ADDR + FREYA_LOGLEVEL_OFF));
#else
        kprintf("(this board has no auto-start slot; the level is RAM only)\r\n");
#endif
        kprintf("usage: loglevel off|error|warn|info|debug | 0..4\r\n");
        return 0;
    }
    if (parse_log_level(argv[1], &level) != 0)
        return usage("loglevel off|error|warn|info|debug | 0..4");
    if (busy_running("loglevel")) return -1;
#ifdef FREYA_APP_FLASH_ADDR
    kprintf("loglevel: console input is dropped while flash is busy\r\n");
    uart_drain_tx();
#endif
    rc = log_set_level(level);
#ifdef FREYA_APP_FLASH_ADDR
    uart_rx_flush();
    if (rc != FLASH_OK) {
        kprintf("loglevel: %s\r\n", flash_err_str(rc));
        return -1;
    }
#else
    if (rc != 0) {
        kprintf("loglevel: failed (%d)\r\n", rc);
        return -1;
    }
#endif
    kprintf("log level %s (%d)\r\n", log_level_str(level), level);
    return 0;
}

static int cmd_threads(int argc, char **argv)
{
    if (argc > 1) return usage("threads");
    thread_list();
    sh_list();
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    int rc;

    if (argc > 2) return usage("stop [thread]");

    if (argc == 2) {
        if (sh_stop_named(argv[1])) {
            kprintf("stopped %s\r\n", argv[1]);
            return 0;
        }
        rc = thread_stop_name(argv[1]);
        if (rc == FREYA_ERR_BUSY) {
            kprintf("stop: %s cannot be stopped\r\n", argv[1]);
            return -1;
        }
        if (rc != 0) {
            kprintf("stop: no thread named %s\r\n", argv[1]);
            return -1;
        }
        if (g_app.running && app_should_stop())
            kprintf("stop requested\r\n");
        else
            kprintf("stopped %s\r\n", argv[1]);
        return 0;
    }

    if (g_app.running) {
        app_request_stop();
        kprintf("stop requested\r\n");
        return 0;
    }
    if (!g_app.loaded) {
        kprintf("no program is loaded\r\n");
        kprintf("(a running program is stopped with Ctrl-C)\r\n");
        return 0;
    }

    kprintf("unloading %s", g_app.path);
    if (g_app.runs)
        kprintf(" (last run: %s, exit status %d)",
                app_stop_reason_str(g_app.last_stop_reason), g_app.last_status);
    kprintf("\r\n");
    app_unload();
    return 0;
}

/*
 * The exit status of the last command and of the last program, which the
 * shell keeps even after the image has been unloaded or replaced.  'run'
 * prints the same status on its own closing line; this is how to ask for
 * it again later, and '$?' is the number on its own.
 */
static int cmd_status(int argc, char **argv)
{
    freya_exit_t st;

    if (argc > 1) return usage("status");

    inf("command");
    kprintf("%d\r\n", s_status);
    if (app_last_exit(&st) != 0) {
        inf("program");
        kprintf("nothing has run since reset\r\n");
        return 0;
    }

    inf("program");
    kprintf("%s\r\n", st.name[0] ? st.name : "unnamed");
    inf("ended by");
    kprintf("%s\r\n", app_stop_reason_str(st.reason));
    inf("exit status");
    if (st.reason > FREYA_STOP_EXIT)
        kprintf("%d  (%d + reason %d, the kernel ended the run)\r\n",
                st.status, FREYA_EXIT_KILLED, st.reason);
    else
        kprintf("%d\r\n", st.status);
    inf("run time");
    kprintf("%u ms\r\n", st.run_ms);
    inf("runs");
    kprintf("%u since reset\r\n", g_app.runs);
    return 0;
}

static int cmd_date(int argc, char **argv)
{
    rtc_time_t t;

    if (argc >= 3) {
        uint32_t y, mo, d, h, mi, s = 0;
        char buf[16];
        const char *p = argv[1];
        int n = 0;

        /* YYYY-MM-DD HH:MM[:SS] */
        for (int i = 0; i < 3; i++) {
            uint32_t *dst = (i == 0) ? &y : (i == 1) ? &mo : &d;
            n = 0;
            while (*p && *p != '-' && n < 15) buf[n++] = *p++;
            buf[n] = '\0';
            if (*p) p++;
            if (str_to_u32(buf, dst) != 0) { kprintf("date: bad date\r\n"); return -1; }
        }
        p = argv[2];
        for (int i = 0; i < 3; i++) {
            uint32_t *dst = (i == 0) ? &h : (i == 1) ? &mi : &s;
            n = 0;
            while (*p && *p != ':' && n < 15) buf[n++] = *p++;
            buf[n] = '\0';
            if (!n) { *dst = 0; break; }
            if (str_to_u32(buf, dst) != 0) { kprintf("date: bad time\r\n"); return -1; }
            if (*p) p++;
            else if (i < 2) { s = 0; break; }
        }
        if (y < 1980 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) {
            kprintf("date: value out of range\r\n");
            return -1;
        }
        t.year = (uint16_t)y; t.mon = (uint8_t)mo; t.day = (uint8_t)d;
        t.hour = (uint8_t)h;  t.min = (uint8_t)mi; t.sec = (uint8_t)s;
        rtc_set(&t);
    }

    rtc_get(&t);
    kput_hms(t.year, t.mon, t.day, t.hour, t.min, t.sec);
    kprintf("\r\n");
    if (argc < 3) kprintf("(set it with: date YYYY-MM-DD HH:MM:SS)\r\n");
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    uint32_t ms = sys_uptime_ms();

    (void)argc; (void)argv;
    kprintf("up %u days %02u:%02u:%02u.%03u\r\n",
            ms / 86400000UL, (ms / 3600000UL) % 24,
            (ms / 60000UL) % 60, (ms / 1000UL) % 60, ms % 1000);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (fat_mounted()) fat_sync();
    kprintf("rebooting ...\r\n");
    sys_reboot();
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("\033[2J\033[H");
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        kprintf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
    kprintf("\r\n");
    return 0;
}

/* Printable ASCII, plus the whitespace a script is written with.
 * A NUL does not belong in the text; the caller supplies the length. */
int script_text_ok(const char *text, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c != '\t' && c != '\n' && c != '\r' && (c < 0x20 || c > 0x7E))
            return 0;
    }
    return 1;
}

/* Read a script into the heap.  The caller frees *out.  0 on success. */
static int KEXT load_script_file(const char *path, char **out)
{
    int fd, n, got = 0;
    int32_t size;
    char *buf;

    *out = NULL;
    fd = fs_fd_open(path, FREYA_O_RDONLY);
    if (fd < 0) return fs_fail("source", path, fd);
    size = fs_fd_size(fd);
    if (size < 0) {
        fs_fd_close(fd);
        return fs_fail("source", path, size);
    }
    if ((uint32_t)size > FREYA_SCRIPT_FILE_MAX) {
        fs_fd_close(fd);
        kprintf("source: script too long\r\n");
        return -1;
    }
    buf = kmalloc((uint32_t)size + 1U);
    if (!buf) {
        fs_fd_close(fd);
        kprintf("source: out of memory\r\n");
        return -1;
    }
    while (got < size) {
        n = fs_fd_read(fd, buf + got, (int)(size - got));
        if (n < 0) {
            kfree(buf);
            fs_fd_close(fd);
            return fs_fail("source", path, n);
        }
        if (n == 0) break;
        got += n;
    }
    fs_fd_close(fd);
    if (got != size) {
        kfree(buf);
        kprintf("source: read error\r\n");
        return -1;
    }
    if (!script_text_ok(buf, (uint32_t)got)) {
        kfree(buf);
        kprintf("source: %s: not a shell script\r\n", path);
        return -1;
    }
    buf[got] = '\0';
    *out = buf;
    return 0;
}

static int KEXT cmd_source(int argc, char **argv)
{
    char *buf = NULL;
    const char *text = NULL;
    int rc;

    if (argc != 2) return usage("source " PROG_ARG);
    if (busy_running("source")) return -1;
    if (s_exec_depth >= SOURCE_NEST) {
        kprintf("source: scripts nest too deeply\r\n");
        return -1;
    }

#ifdef FREYA_APP_FLASH_ADDR
    if (is_flash_path(argv[1])) {
        uint32_t n = 0;
        int found = app_script_find(&text, &n);

        if (found <= 0) {
            if (app_flash_header())
                kprintf("source: %s is a program - 'runflash'\r\n", APP_FLASH_PATH);
            else if (found < 0)
                kprintf("source: script in flash is damaged\r\n");
            else
                kprintf("source: no script in flash\r\n");
            return -1;
        }
        /* A short script is copied so a later 'install' can erase the
         * flash it was stored in.  A longer one is read from the flash. */
        if (n <= FREYA_SCRIPT_FILE_MAX && (buf = kmalloc(n + 1U)) != NULL) {
            memcpy(buf, text, n);
            buf[n] = '\0';
            rc = shell_exec(buf);
            kfree(buf);
            return rc;
        }
        return shell_exec(text);
    }
#endif
    if (!need_fs()) return -1;
    if (load_script_file(argv[1], &buf) != 0) return -1;
    rc = shell_exec(buf);
    kfree(buf);
    return rc;
}

/* 1 once Ctrl-C has been noticed.  The first time prints '^C' and
 * remembers it, so a loop unwinds without printing it again. */
static int KEXT script_interrupted(void)
{
    if (s_script_stop) return 1;
    if (!uart_take_ctrlc()) return 0;
    kprintf("^C\r\n");
    s_script_stop = 1;
    s_status = FREYA_EXIT_FAIL;
    sh_stop_all();
    return 1;
}

/* Busy wait, in short slices, so Ctrl-C can land between them.  The
 * console interrupt still queues the key while this thread spins. */
#define SLEEP_SLICE_MS  20

static int KEXT cmd_yield(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return usage("yield");
    return sh_pump() < 0 ? -1 : 0;
}

static int KEXT cmd_sleep(int argc, char **argv)
{
    uint32_t ms, left;

    if (argc != 2 || str_to_u32(argv[1], &ms) != 0)
        return usage("sleep <ms>");
    left = ms;
    while (left) {
        uint32_t step = left, soon;
        int pr;

        if (script_interrupted()) return -1;
        pr = sh_pump();
        if (pr < 0) return -1;
        if (pr > 0) continue;
        soon = sh_soon();
        if (soon && soon < step) step = soon;
        if (!step) step = 1;
        if (step > SLEEP_SLICE_MS) step = SLEEP_SLICE_MS;
        if (step > left) step = left;
        sys_delay_ms(step);
        s_now_add(step);
        left -= step;
    }
    if (sh_pump() < 0) return -1;
    return 0;
}

static int cmd_led(int argc, char **argv)
{
    if (argc < 2) return usage("led on|off|blink");
    if (strcmp(argv[1], "on") == 0) led_set(1);
    else if (strcmp(argv[1], "off") == 0) led_set(0);
    else if (strcmp(argv[1], "blink") == 0) {
        for (int i = 0; i < 10; i++) { led_toggle(); sys_delay_ms(100); }
        led_set(0);
    } else return usage("led on|off|blink");
    return 0;
}

/* ------------------------------------------------------- pins and PWM */
/*
 * The same pins and the same channels a program gets, driven by hand.
 * Nothing here is a second implementation: the commands call src/gpio.c
 * and src/pwm.c exactly as the service table does, which is also why a
 * pin Freya keeps is refused at the prompt for the same reason.
 */
#define PIN_USAGE  "pin <pin> [in|up|down|out|od|analog] [0|1|toggle]"
#define PWM_USAGE  "pwm [<pin> <hz> <duty%> | <pin> off]"
#define ADC_USAGE  "adc <pin|temp|vref>"

/* The modes in the order the ABI numbers them, FREYA_PIN_IN first. */
static const char *const s_pin_modes[] = {
    "in", "up", "down", "out", "od", "analog"
};

/* "PB0", "pb0" and "B0" are the same pin; -1 is not a pin at all. */
static int parse_pin(const char *s)
{
    uint32_t n;
    int port;

    if (*s == 'P' || *s == 'p') s++;
    port = to_upper(*s) - 'A';
    if (port < 0 || port >= BOARD_PIN_PORTS) return -1;
    if (!s[1] || str_to_u32(s + 1, &n) != 0 || n > 15) return -1;
    return FREYA_PIN(port, (int)n);
}

static void put_pin(int pin)
{
    kprintf("P%c%d", 'A' + FREYA_PIN_PORT(pin), FREYA_PIN_NUM(pin));
}

/* The pin and PWM calls answer with the same few numbers. */
static int pin_fail(const char *cmd, int rc)
{
    const char *why;

    switch (rc) {
    case FREYA_ERR_PIN:  why = "not a pin Freya hands out"; break;
    case FREYA_ERR_BUSY:
        if (strcmp(cmd, "i2c") == 0 || strcmp(cmd, "spi") == 0)
            why = "that bus or its pins are taken";
        else if (strcmp(cmd, "w1") == 0)
            why = "that pin is taken, or every bus is open";
        else if (strcmp(cmd, "adc") == 0)
            why = "that pin is taken";
        else if (strcmp(cmd, "irq") == 0)
            why = "that interrupt line is taken";
        else if (strcmp(cmd, "timer") == 0)
            why = "every timer is taken";
        else why = "that timer is taken";
        break;
    case FREYA_ERR_ARG:
        if (strcmp(cmd, "i2c") == 0)
            why = "speed, address or length out of range";
        else if (strcmp(cmd, "spi") == 0)
            why = "bus, speed, mode or length out of range";
        else if (strcmp(cmd, "w1") == 0)
            why = "not open, or the length is out of range";
        else why = "out of range";
        break;
    case FREYA_ERR_NACK:    why = "no answer"; break;
    case FREYA_ERR_TIMEOUT:
        why = (strcmp(cmd, "w1") == 0) ? "the line stayed low" : "timed out";
        break;
    case FREYA_ERR_IO:      why = "bus error"; break;
    default:                why = "refused"; break;
    }
    kprintf("%s: %s\r\n", cmd, why);
    return -1;
}

/* "50" and "7.5" are a duty cycle in percent; the API takes ten
 * thousandths, so hundredths of a percent are as fine as it gets. */
static int parse_duty(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int digits = 0, places = -1;

    for (; *s; s++) {
        if (*s == '.' && places < 0) { places = 0; continue; }
        if (*s < '0' || *s > '9' || v > FREYA_PWM_FULL) return -1;
        if (places < 0 || places < 2) {
            v = v * 10U + (uint32_t)(*s - '0');
            if (places >= 0) places++;
        }
        digits++;
    }
    if (!digits) return -1;
    if (places < 0) places = 0;
    while (places++ < 2) v *= 10U;
    if (v > FREYA_PWM_FULL) return -1;
    *out = v;
    return 0;
}

static void put_duty(uint32_t duty)
{
    kprintf("%u.%02u%%", duty / 100U, duty % 100U);
}

/*
 * A mode, a level, or both, and then what the pin reads either way - a
 * pin driven is still a pin read back, and IDR is what it really is.
 */
static int cmd_pin(int argc, char **argv)
{
    int pin, rc = 0, at = 2;

    if (argc < 2) return usage(PIN_USAGE);
    pin = parse_pin(argv[1]);
    if (pin < 0) return usage(PIN_USAGE);

    if (argc > 2) {
        for (unsigned i = 0; i < ARRAY_SIZE(s_pin_modes); i++)
            if (strcmp(argv[2], s_pin_modes[i]) == 0) {
                rc = gpio_pin_mode(pin, (int)i);
                at = 3;                     /* a level may follow it */
                break;
            }
        /* A level with no mode before it: the pin becomes a push-pull
         * output, which is what 'pin PB5 1' at a prompt means by it. */
        if (at == 2) rc = gpio_pin_mode(pin, FREYA_PIN_OUT);
        if (rc != 0) return pin_fail("pin", rc);
    }
    if (argc > at) {
        if (strcmp(argv[at], "toggle") == 0)  rc = gpio_pin_toggle(pin);
        else if (strcmp(argv[at], "0") == 0)  rc = gpio_pin_write(pin, 0);
        else if (strcmp(argv[at], "1") == 0)  rc = gpio_pin_write(pin, 1);
        else return usage(PIN_USAGE);
        if (rc != 0) return pin_fail("pin", rc);
    }

    rc = gpio_pin_read(pin);
    if (rc < 0) return pin_fail("pin", rc);
    put_pin(pin);
    kprintf(" = %d\r\n", rc);
    return 0;
}

/* With no arguments, the board's channels and what each is doing; with
 * them, one channel started, changed or stopped. */
static int cmd_pwm(int argc, char **argv)
{
    pwm_info_t in;
    uint32_t hz, duty;
    int pin, ch, rc;

    if (argc < 2) {
        for (int i = 0; pwm_info(i, &in) == 0; i++) {
            kprintf("  ");
            put_pin(in.pin);
            kprintf("  %s CH%d  ", in.timer, in.ch);
            if (in.open) {
                kprintf("%u Hz ", in.freq_hz);
                put_duty(in.duty);
            } else {
                kprintf("off");
            }
            kprintf("\r\n");
        }
        kprintf("the channels of one timer share its frequency\r\n"
                "usage: %s\r\n", PWM_USAGE);
        return 0;
    }

    pin = parse_pin(argv[1]);
    ch  = (pin < 0) ? FREYA_ERR_PIN : pwm_lookup(pin);
    if (ch < 0) return pin_fail("pwm", ch);

    if (argc == 3 && strcmp(argv[2], "off") == 0) {
        put_pin(pin);
        if (pwm_close(ch) != 0) {           /* the handle is good, so: */
            kprintf(" is not running\r\n");
            return -1;
        }
        kprintf(" off, and an input again\r\n");
        return 0;
    }
    if (argc < 4 || str_to_u32(argv[2], &hz) != 0 ||
        parse_duty(argv[3], &duty) != 0) return usage(PWM_USAGE);

    rc = pwm_open(pin, hz, duty);
    if (rc < 0) return pin_fail("pwm", rc);

    /* It outlives the command: nothing stops it but 'pwm <pin> off'. */
    pwm_info(rc, &in);
    put_pin(pin);
    kprintf("  %s CH%d  %u Hz ", in.timer, in.ch, in.freq_hz);
    put_duty(in.duty);
    kprintf("\r\n");
    return 0;
}

/* One raw conversion from a pin or one of the chip's internal sources. */
static int cmd_adc(int argc, char **argv)
    __attribute__((section(".text.cmd_adc")));
static int cmd_adc(int argc, char **argv)
{
    int source, value;

    if (argc != 2) return usage(ADC_USAGE);
    if (strcmp(argv[1], "temp") == 0) source = FREYA_ADC_TEMP;
    else if (strcmp(argv[1], "vref") == 0) source = FREYA_ADC_VREF;
    else source = parse_pin(argv[1]);
    if (source < 0) return usage(ADC_USAGE);

    value = adc_read(source);
    if (value < 0) return pin_fail("adc", value);
    kprintf("%s = %d / %d\r\n", argv[1], value, FREYA_ADC_MAX);
    return 0;
}

/* ------------------------------------------------------------ I2C */
#define I2C_USAGE \
    "i2c [<bus> <hz> | <bus> off | <bus> scan | <bus> <addr> [w <byte>...] [r <n>]]"

#define I2C_TX_MAX  32

static int cmd_i2c(int argc, char **argv)
{
    i2c_info_t in;
    uint8_t tx[I2C_TX_MAX], rx[FREYA_I2C_MAX_LEN];
    uint32_t bus, n, len;
    int i, rc, txlen, rlen;

    if (argc < 2) {
        for (i = 0; i2c_info(i, &in) == 0; i++) {
            kprintf("  %d  %s  SCL ", i + 1, in.name);
            put_pin(in.scl);
            kprintf("  SDA ");
            put_pin(in.sda);
            if (in.open) kprintf("  %u Hz\r\n", in.hz);
            else         kprintf("  off\r\n");
        }
        kprintf("pull SCL and SDA up to 3.3 V\r\nusage: %s\r\n", I2C_USAGE);
        return 0;
    }

    if (str_to_u32(argv[1], &bus) != 0 || i2c_info((int)bus - 1, &in) != 0) {
        kprintf("i2c: no such bus\r\n");
        return -1;
    }

    if (argc == 3 && strcmp(argv[2], "off") == 0) {
        if (i2c_close((int)bus) != 0) {
            kprintf("i2c: not open\r\n");
            return -1;
        }
        kprintf("%s off\r\n", in.name);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "scan") == 0) {
        if (!in.open) {
            kprintf("i2c: not open\r\n");
            return -1;
        }
        for (i = 0x08; i <= 0x77; i++) {
            if (uart_rx_ready() && uart_getc_timeout(0) == 0x03) {
                uart_rx_flush();
                kprintf("^C\r\n");
                return -1;
            }
            rc = i2c_write((int)bus, i, NULL, 0);
            if (rc == 0) kprintf("%02x ", i);
            else if (rc != FREYA_ERR_NACK) return pin_fail("i2c", rc);
        }
        kprintf("\r\n");
        return 0;
    }
    if (argc == 3 && str_to_u32(argv[2], &n) == 0 && n >= FREYA_I2C_MIN_HZ) {
        rc = i2c_open((int)bus, n);
        if (rc != 0) return pin_fail("i2c", rc);
        kprintf("%s  %u Hz\r\n", in.name, n);
        return 0;
    }

    if (!in.open) {
        kprintf("i2c: not open\r\n");
        return -1;
    }
    if (argc < 5 || str_to_u32(argv[2], &n) != 0 || n > 0x7F)
        return usage(I2C_USAGE);

    txlen = 0;
    rlen = 0;
    i = 3;
    if (strcmp(argv[i], "w") == 0) {
        for (i++; i < argc && strcmp(argv[i], "r") != 0; i++) {
            if (txlen >= I2C_TX_MAX || str_to_u32(argv[i], &len) != 0 ||
                len > 0xFF) return usage(I2C_USAGE);
            tx[txlen++] = (uint8_t)len;
        }
        if (txlen == 0) return usage(I2C_USAGE);
    }
    if (i < argc && strcmp(argv[i], "r") == 0) {
        if (i + 2 != argc || str_to_u32(argv[i + 1], &len) != 0 ||
            len < 1 || len > FREYA_I2C_MAX_LEN) return usage(I2C_USAGE);
        rlen = (int)len;
        i += 2;
    }
    if (i != argc || (txlen == 0 && rlen == 0)) return usage(I2C_USAGE);

    rc = i2c_transfer((int)bus, (int)n, txlen ? tx : NULL, txlen,
                      rlen ? rx : NULL, rlen);
    if (rc != 0) return pin_fail("i2c", rc);
    if (rlen == 0) {
        kprintf("ok\r\n");
        return 0;
    }
    for (i = 0; i < rlen; i++) {
        if (i && (i % 16) == 0) kprintf("\r\n");
        else if (i) kprintf(" ");
        kprintf("%02x", rx[i]);
    }
    kprintf("\r\n");
    return 0;
}

/* ------------------------------------------------------------ SPI */
/* Chip select is a pin, driven with 'pin' around the shift. */
#define SPI_USAGE \
    "spi [<bus> <hz> [mode] | <bus> off | <bus> x <byte>...]"

static int cmd_spi(int argc, char **argv)
{
    spi_info_t in;
    uint8_t tx[8], rx[8];
    uint32_t bus, n;
    int i, rc, nbyte;

    if (argc < 2) {
        for (i = 0; spi_info(i, &in) == 0; i++) {
            kprintf("  %d  %s  ", i + 1, in.name);
            put_pin(in.sck);
            kprintf(" ");
            put_pin(in.miso);
            kprintf(" ");
            put_pin(in.mosi);
            if (in.open) kprintf("  %u Hz  mode %d\r\n", in.hz, in.mode);
            else         kprintf("  off\r\n");
        }
        kprintf("chip select is a pin you drive\r\nusage: %s\r\n", SPI_USAGE);
        return 0;
    }

    if (str_to_u32(argv[1], &bus) != 0 || spi_info((int)bus - 1, &in) != 0) {
        kprintf("spi: no such bus\r\n");
        return -1;
    }
    if (argc == 3 && strcmp(argv[2], "off") == 0) {
        if (spi_close((int)bus) != 0) {
            kprintf("spi: not open\r\n");
            return -1;
        }
        kprintf("%s off\r\n", in.name);
        return 0;
    }
    if ((argc == 3 || argc == 4) && argv[2][0] != 'x' &&
        str_to_u32(argv[2], &n) == 0) {
        int mode = FREYA_SPI_MODE0;

        if (argc == 4) {
            uint32_t m;
            if (str_to_u32(argv[3], &m) != 0) return usage(SPI_USAGE);
            mode = (int)m;
        }
        rc = spi_open((int)bus, n, mode);
        if (rc != 0) return pin_fail("spi", rc);
        spi_info((int)bus - 1, &in);
        kprintf("%s  %u Hz  mode %d\r\n", in.name, in.hz, in.mode);
        return 0;
    }
    if (!in.open) {
        kprintf("spi: not open\r\n");
        return -1;
    }
    if (argc < 4 || strcmp(argv[2], "x") != 0) return usage(SPI_USAGE);

    nbyte = 0;
    for (i = 3; i < argc; i++) {
        if (nbyte == 8 || str_to_u32(argv[i], &n) != 0 || n > 0xFF)
            return usage(SPI_USAGE);
        tx[nbyte++] = (uint8_t)n;
    }
    rc = spi_transfer((int)bus, tx, rx, nbyte);
    if (rc != 0) return pin_fail("spi", rc);
    for (i = 0; i < nbyte; i++) {
        if (i) kprintf(" ");
        kprintf("%02x", rx[i]);
    }
    kprintf("\r\n");
    return 0;
}

/* ----------------------------------------------------------- 1-Wire */
/* Byte reads, writes and the strong pull-up are the program calls.
 * The prompt opens a pin, checks presence and walks the ROMs. */
#define W1_USAGE  "w1 [<pin> | <pin> off | <pin> reset | <pin> search]"

static int cmd_w1(int argc, char **argv)
{
    w1_info_t in;
    uint8_t rom[FREYA_W1_ROM_LEN];
    int pin, i, n, rc;

    if (argc < 2) {
        for (i = 0; w1_info(i, &in) == 0; i++) {
            kprintf("  ");
            put_pin(in.pin);
            kprintf("\r\n");
        }
        kprintf("pull the data pin up to 3.3 V\r\nusage: %s\r\n", W1_USAGE);
        return 0;
    }

    pin = parse_pin(argv[1]);
    if (pin < 0 || argc > 3) return usage(W1_USAGE);

    if (argc == 2) {
        rc = w1_open(pin);
        if (rc != 0) return pin_fail("w1", rc);
        put_pin(pin);
        kprintf("  1-Wire\r\n");
        return 0;
    }
    if (strcmp(argv[2], "off") == 0) {
        if (w1_close(pin) != 0) {
            kprintf("w1: not open\r\n");
            return -1;
        }
        put_pin(pin);
        kprintf(" off\r\n");
        return 0;
    }
    if (strcmp(argv[2], "reset") == 0) {
        rc = w1_reset(pin);
        if (rc != 0) return pin_fail("w1", rc);
        kprintf("presence\r\n");
        return 0;
    }
    if (strcmp(argv[2], "search") != 0) return usage(W1_USAGE);

    n = 0;
    for (;;) {
        if (uart_rx_ready() && uart_getc_timeout(0) == 0x03) {
            uart_rx_flush();
            kprintf("^C\r\n");
            return -1;
        }
        rc = w1_search(pin, rom);
        if (rc == FREYA_ERR_NACK) break;
        if (rc != 0) return pin_fail("w1", rc);
        for (i = 0; i < FREYA_W1_ROM_LEN; i++) {
            if (i) kprintf(" ");
            kprintf("%02x", rom[i]);
        }
        kprintf("\r\n");
        n++;
    }
    if (!n) {
        kprintf("no device\r\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ crypt */
/* XTEA-CTR.  Key, nonce and data are hex, with no 0x and no spaces in
 * a word.  The same command decrypts.  The command and its helpers share
 * one section so the linker can put them in the kernel extension; the
 * 48 KiB image has no room for them. */
#define CRYPT_USAGE  "crypt [<key> <nonce> <hex>]"
#define CRYPT_CMD_MAX  64
#define CRYPT_TEXT __attribute__((section(".text.cmd_crypt")))

static int CRYPT_TEXT crypt_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* exact > 0 requires that many bytes.  exact == 0 takes up to max. */
static int CRYPT_TEXT
crypt_parse_hex(const char *s, uint8_t *out, int max, int exact)
{
    int n = 0;

    while (s[0]) {
        int hi, lo;

        if (n >= max) return -1;
        hi = crypt_hexval((unsigned char)s[0]);
        lo = s[1] ? crypt_hexval((unsigned char)s[1]) : -1;
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    if (exact && n != exact) return -1;
    return n;
}

static int cmd_crypt(int argc, char **argv)
{
    uint8_t key[FREYA_CRYPT_KEY_LEN];
    uint8_t nonce[FREYA_CRYPT_NONCE_LEN];
    uint8_t buf[CRYPT_CMD_MAX];
    int n, i, rc;

    if (argc == 1) {
        kprintf("XTEA-CTR, 16-byte key, 8-byte nonce\r\nusage: %s\r\n",
                CRYPT_USAGE);
        return 0;
    }
    if (argc != 4) return usage(CRYPT_USAGE);
    if (crypt_parse_hex(argv[1], key, FREYA_CRYPT_KEY_LEN,
                        FREYA_CRYPT_KEY_LEN) < 0 ||
        crypt_parse_hex(argv[2], nonce, FREYA_CRYPT_NONCE_LEN,
                        FREYA_CRYPT_NONCE_LEN) < 0)
        return usage(CRYPT_USAGE);
    n = crypt_parse_hex(argv[3], buf, CRYPT_CMD_MAX, 0);
    if (n <= 0) return usage(CRYPT_USAGE);

    rc = crypt_apply(key, nonce, 0, buf, buf, n);
    if (rc != 0) return -1;
    for (i = 0; i < n; i++) kprintf("%02x", buf[i]);
    kprintf("\r\n");
    return 0;
}

/* ------------------------------------------------------ command table */
typedef struct {
    const char *name;
    int (*fn)(int, char **);
    const char *help;
} command_t;

static const command_t s_cmds[] = {
    { "help",     cmd_help,     "help [command]" },
    { "sysinfo",  cmd_sysinfo,  "sysinfo" },
    { "meminfo",  cmd_meminfo,  "meminfo" },
    { "mount",    cmd_mount,    "mount" },
    { "power",    cmd_power,    POWER_USAGE },
    { "ls",       cmd_ls,       "ls [-l] [path]" },
    { "cd",       cmd_cd,       "cd [path]" },
    { "pwd",      cmd_pwd,      "pwd" },
    { "mkdir",    cmd_mkdir,    "mkdir <dir>..." },
    { "rm",       cmd_rm,       "rm [-r] <path>..." },
    { "rename",   cmd_rename,   "rename <old> <new>" },
    { "download", cmd_download, "download <file> [--raw] [--size <bytes>]" },
    { "upload",   cmd_upload,   "upload <file>" },
    { "cat",      cmd_cat,      "cat <file>" },
    { "write",    cmd_write,    "write <file> <text...>" },
    { "hexdump",  cmd_hexdump,  "hexdump <file> [off] [len]" },
    { "flashdump",cmd_flashdump,"flashdump [file]" },
    { "df",       cmd_df,       "df" },
    { "load",     cmd_load,     "load " PROG_ARG },
    { "run",      cmd_run,      "run [" PROG_ARG "] [args]" },
#ifdef FREYA_APP_FLASH_ADDR
    { "runflash", cmd_runflash, "runflash [args...]" },
#endif
    { "stop",     cmd_stop,     "stop [thread]" },
    { "threads",  cmd_threads,  "threads" },
    { "status",   cmd_status,   "status" },
#ifdef FREYA_APP_FLASH_ADDR
    { "install",  cmd_install,  "install <file>" },
    { "saveflash",cmd_saveflash,"saveflash [file]" },
    { "uninstall",cmd_uninstall,"uninstall" },
    { "autostart",cmd_autostart,"autostart [on|off]" },
    { "ramdump",  cmd_ramdump,  "ramdump [on|off]" },
#endif
    { "date",     cmd_date,     "date [YYYY-MM-DD HH:MM:SS]" },
    { "loglevel", cmd_loglevel, "loglevel [level]" },
    { "uptime",   cmd_uptime,   "uptime" },
    { "led",      cmd_led,      "led on|off|blink" },
    { "pin",      cmd_pin,      PIN_USAGE },
    { "pwm",      cmd_pwm,      PWM_USAGE },
    { "adc",      cmd_adc,      ADC_USAGE },
    { "i2c",      cmd_i2c,      I2C_USAGE },
    { "spi",      cmd_spi,      SPI_USAGE },
    { "w1",       cmd_w1,       W1_USAGE },
    { "crypt",    cmd_crypt,    CRYPT_USAGE },
    { "echo",     cmd_echo,     "echo <text...>" },
    { "sleep",    cmd_sleep,    "sleep <ms>" },
    { "yield",    cmd_yield,    "yield" },
    { "source",   cmd_source,   "source " PROG_ARG },
    { "set",      cmd_script,   "set [<name> [, <name>]... <expr>]" },
    { "unset",    cmd_unset,    "unset <name>" },
    { "fn",       cmd_script,   "fn [<name>]" },
    { "return",   cmd_script,   "return <expr> [, <expr>]..." },
    { "if",       cmd_script,   "if <command>" },
    { "else",     cmd_script,   "else" },
    { "end",      cmd_script,   "end" },
    { "loop",     cmd_script,   "loop <count>" },
    { "break",    cmd_script,   "break" },
    { "clear",    cmd_clear,    "clear" },
    { "reboot",   cmd_reboot,   "reboot" },
};

/* 'if', 'else', 'end', 'loop', 'fn' and 'return' are syntax, handled
 * before the table is searched.  Reaching here means one of those words
 * was itself the command an 'if' ran, which is not what they mean. */
static int cmd_script(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return -1;
}

static int cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++) {
            if (strcmp(s_cmds[i].name, argv[1]) == 0) {
                /* The name is what you type.  A usage line that is not
                 * just that name follows it; the name is never omitted. */
                kprintf("%s\r\n", s_cmds[i].name);
                if (strcmp(s_cmds[i].help, s_cmds[i].name) != 0)
                    kprintf("%s\r\n", s_cmds[i].help);
                return 0;
            }
        }
        kprintf("help: no such command: %s\r\n", argv[1]);
        return -1;
    }

    kprintf("Freya commands:\r\n");
    for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++)
        kprintf("  %s\r\n", s_cmds[i].help);
    kprintf("Ctrl-C stops a program, Ctrl-U clears the line, "
            "cursor keys walk history.\r\n");
    return 0;
}

/* ------------------------------------------------------------- scripts */
/*
 * A script is a list of commands.  ';' and a new line both separate
 * them, except inside quotes.  A '#' at the start of a statement, or
 * after a space, comments out the rest of that statement.  'if' runs
 * one command and then either the lines up to 'else' or the lines up
 * to 'end', depending on whether that command's status was 0.  'loop'
 * repeats the lines up to 'end'.  A block left open at the end of a
 * typed line is finished on the next lines; the prompt changes so that
 * is visible.
 *
 * The whole script is checked before anything runs, so a missing 'end'
 * or an 'else' in the wrong place does not half-run the commands.
 */
#define SCRIPT_MAX   160           /* one line; longer scripts use ';' */
#define SCRIPT_NEST  8
#define LOOP_MAX     1000000u

#define BLK_IF       1
#define BLK_ELSE     2
#define BLK_LOOP     3
#define BLK_FN       4

#define SCR_DONE     0
#define SCR_END      1
#define SCR_ELSE     2
#define SCR_BREAK    3
#define SCR_RETURN   4
#define SCR_ERR     -1

static char s_script[SCRIPT_MAX];   /* lines of a block still being typed */
static int  s_script_len;

/* Next statement into dst.  1 at the end of the text, -1 when a statement
 * does not fit in dst (the message is already printed).  Quotes hide a
 * ';' or a newline, the way they hide a space when the line is split.
 * A '#' at the start of a statement, or after a space, is a comment
 * through the next separator. */
static int KEXT next_stmt(const char **pp, char *dst, int size)
{
    const char *s = *pp;
    int i = 0, q = 0, over = 0;

    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == ';' || *s == '\n' || *s == '\r')
            s++;
        if (!*s) {
            *pp = s;
            return 1;
        }
        if (*s != '#') break;
        while (*s && *s != ';' && *s != '\n' && *s != '\r') s++;
    }

    while (*s) {
        char c = *s;

        if (c == '"') q = !q;
        else if (!q && (c == ';' || c == '\n' || c == '\r')) break;
        else if (!q && c == '#' &&
                 (i == 0 || dst[i - 1] == ' ' || dst[i - 1] == '\t')) {
            while (*s && *s != ';' && *s != '\n' && *s != '\r') s++;
            break;
        }
        if (i < size - 1) dst[i++] = c;
        else over = 1;
        s++;
    }
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) i--;
    dst[i] = '\0';
    *pp = s;
    if (over) {
        kprintf("line too long\r\n");
        s_status = FREYA_EXIT_FAIL;
        return -1;
    }
    return 0;
}

/* 1 if the statement's first word is kw.  *rest is what follows it. */
static int KEXT word_is(const char *stmt, const char *kw, const char **rest)
{
    int n = (int)strlen(kw);

    while (*stmt == ' ' || *stmt == '\t') stmt++;
    if (strncmp(stmt, kw, (size_t)n) != 0) return 0;
    if (stmt[n] != '\0' && stmt[n] != ' ' && stmt[n] != '\t') return 0;
    stmt += n;
    while (*stmt == ' ' || *stmt == '\t') stmt++;
    if (rest) *rest = stmt;
    return 1;
}

/* Decimal count, 0 .. LOOP_MAX.  -1 is not a count, -2 is too large.
 * Overflow is refused rather than wrapped: a long string of digits
 * must not become a small loop. */
static int KEXT parse_count(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int digits = 0;

    while (*s >= '0' && *s <= '9') {
        uint32_t d = (uint32_t)(*s - '0');

        if (v > (LOOP_MAX - d) / 10U) return -2;
        v = v * 10U + d;
        digits++;
        s++;
    }
    if (!digits || *s) return -1;
    *out = v;
    return 0;
}

static int KEXT count_error(int pr)
{
    if (pr < -1) kprintf("loop: count too large\r\n");
    else usage("loop <count>");
    s_status = FREYA_EXIT_FAIL;
    return -1;
}

/* 0 when the text is a finished script, 1 when a block is still open,
 * -1 when the shape is wrong (the message is already printed). */
static int KEXT script_check(const char *text, char *walk)
{
    const char *p = text;
    int8_t stk[SCRIPT_NEST];
    int sp = 0, ns = 1;

    while ((ns = next_stmt(&p, walk, LINE_MAX)) == 0) {
        const char *rest;

        if (word_is(walk, "if", &rest)) {
            if (!*rest) {
                usage("if <command>");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            if (sp >= SCRIPT_NEST) {
                kprintf("too many nested blocks\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            stk[sp++] = BLK_IF;
        } else if (word_is(walk, "loop", &rest)) {
            if (!*rest) return count_error(-1);
            /* '$?' is a count, but not until the loop actually runs. */
            if (!strchr(rest, '$')) {
                uint32_t n;
                int pr = parse_count(rest, &n);

                if (pr != 0) return count_error(pr);
            }
            if (sp >= SCRIPT_NEST) {
                kprintf("too many nested blocks\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            stk[sp++] = BLK_LOOP;
        } else if (word_is(walk, "fn", &rest)) {
            int nlen = 0;

            if (!*rest) continue;
            if (!name_char(*rest, 1)) {
                kprintf("fn: bad name\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            while (name_char(rest[nlen], 0)) nlen++;
            if (nlen >= 8) {
                kprintf("fn: bad name\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            if (rest[nlen]) {
                usage("fn [<name>]");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            if (sp >= SCRIPT_NEST) {
                kprintf("too many nested blocks\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            stk[sp++] = BLK_FN;
        } else if (word_is(walk, "return", &rest)) {
            int i;

            if (!*rest) {
                usage("return <expr> [, <expr>]...");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            for (i = sp - 1; i >= 0; i--)
                if (stk[i] == BLK_FN) break;
            if (i < 0) {
                kprintf("unexpected return\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
        } else if (word_is(walk, "break", &rest)) {
            int i;

            if (*rest) {
                usage("break");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            for (i = sp - 1; i >= 0; i--) {
                if (stk[i] == BLK_FN) {
                    i = -1;
                    break;
                }
                if (stk[i] == BLK_LOOP) break;
            }
            if (i < 0) {
                kprintf("unexpected break\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
        } else if (word_is(walk, "else", &rest)) {
            if (*rest) {
                usage("else");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            if (sp == 0 || stk[sp - 1] != BLK_IF) {
                kprintf("unexpected else\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            stk[sp - 1] = BLK_ELSE;
        } else if (word_is(walk, "end", &rest)) {
            if (*rest) {
                usage("end");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            if (sp == 0) {
                kprintf("unexpected end\r\n");
                s_status = FREYA_EXIT_FAIL;
                return -1;
            }
            sp--;
        }
    }
    if (ns < 0) return -1;
    return sp ? 1 : 0;
}

/* ---------------------------------------------------------- variables */
/*
 * Eight names.  A value is an integer, a float, a byte (0..255),
 * a bool, empty, none, a short string, an auto array, or a dict.
 * The next assignment decides which.  An array's elements are one
 * type, and it grows when an index past the end is written.  A dict's
 * keys are one type and its values are another; the keys are kept
 * sorted so a lookup is a binary search.  Both live in a small heap
 * pool and are freed when the name is unset or replaced.
 * Arithmetic is + - * / ; an integer or a byte also has % & | ^ ~
 * << >>.  A string is concatenated with + and formatted by writing
 * the format and then its arguments: 'set s "%d" $n'.  int(), float(),
 * byte(), bool() and str() convert, and hex() turns an integer into
 * hex text or hex text into an integer.  empty and none are the only
 * values of their types.  true and false are the bool values.
 * == and /= compare.  < > and >< order numbers.
 * 'if' treats a comparison, true, false, or bool(...) as a condition.
 * 'break' leaves the innermost loop.
 */
#define VAR_MAX   8
#define VAR_NAME  8
#define VAR_STR   32
#define COLL_MAX  4
#define ARR_MAX   8
#define DICT_MAX  8

enum { V_NONE = 0, V_INT, V_FLT, V_STR, V_BYTE, V_EMPTY, V_BOOL, V_NIL,
       V_ARR, V_DICT };

typedef struct {
    char name[VAR_NAME];
    uint8_t type;
    union {
        int32_t i;
        float f;
        char s[VAR_STR];
    } u;
} shell_var_t;

typedef struct {
    uint8_t type;
    int32_t i;
    float f;
    char s[VAR_STR];
} val_t;

static shell_var_t s_var[VAR_MAX];
static const char *s_vwho = "set";

static int KEXT vfail(const char *msg)
{
    kprintf("%s: %s\r\n", s_vwho, msg);
    return -1;
}

static void KEXT vskip(const char **pp)
{
    const char *s = *pp;
    while (*s == ' ' || *s == '\t') s++;
    *pp = s;
}

static shell_var_t *KEXT var_find(const char *name, int nlen, int create)
{
    shell_var_t *gap = NULL;
    int i;

    for (i = 0; i < VAR_MAX; i++) {
        if (s_var[i].type == V_NONE) {
            if (!gap) gap = &s_var[i];
            continue;
        }
        if (strncmp(s_var[i].name, name, (size_t)nlen) == 0 &&
            s_var[i].name[nlen] == '\0')
            return &s_var[i];
    }
    if (!create || !gap) return NULL;
    memset(gap, 0, sizeof *gap);
    memcpy(gap->name, name, (size_t)nlen);
    gap->name[nlen] = '\0';
    return gap;
}

static void KEXT ftoa(char *out, int size, float v)
{
    char tmp[24];
    int neg = 0, n, i;
    int32_t ip, frac;

    if (v != v) {
        ksnprintf(out, size, "nan");
        return;
    }
    if (v < 0.f) { neg = 1; v = -v; }
    if (v > 2147483647.f) {
        ksnprintf(out, size, "%sinf", neg ? "-" : "");
        return;
    }
    ip = (int32_t)v;
    frac = (int32_t)((v - (float)ip) * 10000.f + 0.5f);
    if (frac >= 10000) { ip++; frac = 0; }
    if (frac < 0) frac = 0;
    n = ksnprintf(tmp, (int)sizeof tmp, "%s%d.%04d",
                  neg ? "-" : "", (int)ip, (int)frac);
    if (n < 0 || n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
    i = n;
    while (i > 0 && tmp[i - 1] == '0') i--;
    if (i > 0 && tmp[i - 1] == '.') i--;
    tmp[i] = '\0';
    ksnprintf(out, size, "%s", tmp);
}

static int KEXT type_wide(int type)
{
    return type == V_INT || type == V_BYTE;
}

/* Integer, byte, or bool: the payload lives in the int field. */
static int KEXT type_hold_i(int type)
{
    return type_wide(type) || type == V_BOOL;
}

static int KEXT coll_text(int id, char *out, int size);

static void KEXT val_text(const val_t *v, char *out, int size)
{
    if (v->type == V_ARR || v->type == V_DICT) {
        if (coll_text(v->i, out, size) != 0) ksnprintf(out, size, "...");
        return;
    }
    if (type_wide(v->type)) ksnprintf(out, size, "%d", (int)v->i);
    else if (v->type == V_FLT) ftoa(out, size, v->f);
    else if (v->type == V_BOOL) ksnprintf(out, size, v->i ? "true" : "false");
    else if (v->type == V_EMPTY) ksnprintf(out, size, "empty");
    else if (v->type == V_NIL) ksnprintf(out, size, "none");
    else ksnprintf(out, size, "%s", v->s);
}

/*
 * Four collections.  An array is one block of cells, all one type, and
 * it grows when an index past the end is written.  A dict is two
 * parallel blocks, keys kept sorted, so lookup is a binary search.
 * Keys are one type and values are another.  A variable holds the slot
 * number.  Copying a value clones the cells.  Freeing a slot returns
 * its blocks to the heap.
 */
typedef struct {
    uint8_t type;
    union {
        int32_t i;
        float f;
        char s[VAR_STR];
    } u;
} cell_t;

typedef struct {
    uint8_t kind;
    uint8_t et;
    uint8_t kt;
    uint8_t vt;
    uint8_t n;
    cell_t *a;
    cell_t *b;
} coll_t;

static coll_t s_coll[COLL_MAX];

static int KEXT is_coll(int type)
{
    return type == V_ARR || type == V_DICT;
}

static int KEXT scalar_ok(int type)
{
    return type == V_INT || type == V_FLT || type == V_STR ||
           type == V_BYTE || type == V_BOOL || type == V_EMPTY ||
           type == V_NIL;
}

static void KEXT cell_from_val(cell_t *c, const val_t *v)
{
    memset(c, 0, sizeof *c);
    c->type = v->type;
    if (type_hold_i(v->type)) c->u.i = v->i;
    else if (v->type == V_FLT) c->u.f = v->f;
    else if (v->type == V_STR) memcpy(c->u.s, v->s, VAR_STR);
}

static void KEXT cell_to_val(const cell_t *c, val_t *v)
{
    v->type = c->type;
    v->i = 0;
    v->f = 0.f;
    v->s[0] = '\0';
    if (type_hold_i(c->type)) v->i = c->u.i;
    else if (c->type == V_FLT) v->f = c->u.f;
    else if (c->type == V_STR) memcpy(v->s, c->u.s, VAR_STR);
}

static void KEXT cell_zero(cell_t *c, int type)
{
    memset(c, 0, sizeof *c);
    c->type = (uint8_t)type;
}

static int KEXT cell_cmp(const cell_t *a, const cell_t *b)
{
    if (a->type == V_STR) return strcmp(a->u.s, b->u.s);
    if (a->type == V_FLT) return (a->u.f > b->u.f) - (a->u.f < b->u.f);
    if (a->type == V_EMPTY || a->type == V_NIL) return 0;
    return (a->u.i > b->u.i) - (a->u.i < b->u.i);
}

static int KEXT coll_new(int kind)
{
    int i;

    for (i = 0; i < COLL_MAX; i++) {
        if (s_coll[i].kind) continue;
        memset(&s_coll[i], 0, sizeof s_coll[i]);
        s_coll[i].kind = (uint8_t)kind;
        return i;
    }
    return -1;
}

static void KEXT coll_free(int id)
{
    if (id < 0 || id >= COLL_MAX || !s_coll[id].kind) return;
    kfree(s_coll[id].a);
    kfree(s_coll[id].b);
    memset(&s_coll[id], 0, sizeof s_coll[id]);
}

static void KEXT val_drop(val_t *v)
{
    if (!v) return;
    if (is_coll(v->type)) coll_free(v->i);
    v->type = V_NONE;
}

static int KEXT coll_blocks(coll_t *c)
{
    int n = (c->kind == V_ARR) ? ARR_MAX : DICT_MAX;

    if (!c->a) {
        c->a = kmalloc((uint32_t)sizeof(cell_t) * (uint32_t)n);
        if (!c->a) return -1;
        memset(c->a, 0, sizeof(cell_t) * (size_t)n);
    }
    if (c->kind == V_DICT && !c->b) {
        c->b = kmalloc((uint32_t)sizeof(cell_t) * (uint32_t)n);
        if (!c->b) return -1;
        memset(c->b, 0, sizeof(cell_t) * (size_t)n);
    }
    return 0;
}

static int KEXT coll_clone(int id)
{
    coll_t *src;
    int nid, n;

    if (id < 0 || id >= COLL_MAX || !s_coll[id].kind) return -1;
    src = &s_coll[id];
    nid = coll_new(src->kind);
    if (nid < 0) return -1;
    s_coll[nid].et = src->et;
    s_coll[nid].kt = src->kt;
    s_coll[nid].vt = src->vt;
    s_coll[nid].n = src->n;
    if (!src->n) return nid;
    if (coll_blocks(&s_coll[nid]) != 0) {
        coll_free(nid);
        return -1;
    }
    n = src->n;
    memcpy(s_coll[nid].a, src->a, sizeof(cell_t) * (size_t)n);
    if (src->kind == V_DICT)
        memcpy(s_coll[nid].b, src->b, sizeof(cell_t) * (size_t)n);
    return nid;
}

static int KEXT dict_find(const coll_t *c, const cell_t *key, int *pos)
{
    int lo = 0, hi = c->n;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = cell_cmp(&c->a[mid], key);

        if (cmp == 0) { *pos = mid; return 1; }
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    *pos = lo;
    return 0;
}

static int KEXT array_put(coll_t *c, int32_t idx, const cell_t *v)
{
    int i;

    if (!scalar_ok(v->type)) return vfail("type mismatch");
    if (idx < 0 || idx >= ARR_MAX) return vfail("out of range");
    if (c->n == 0) c->et = v->type;
    else if (v->type != c->et) return vfail("type mismatch");
    if (coll_blocks(c) != 0) return vfail("out of memory");
    for (i = (int)c->n; i < idx; i++) cell_zero(&c->a[i], c->et);
    c->a[idx] = *v;
    if (idx >= (int32_t)c->n) c->n = (uint8_t)(idx + 1);
    return 0;
}

static int KEXT dict_put(coll_t *c, const cell_t *key, const cell_t *val)
{
    int pos = 0, i;

    if (!scalar_ok(key->type) || !scalar_ok(val->type))
        return vfail("type mismatch");
    if (key->type == V_FLT && key->u.f != key->u.f) return vfail("bad expression");
    if (c->n == 0) {
        c->kt = key->type;
        c->vt = val->type;
    } else if (key->type != c->kt || val->type != c->vt) {
        return vfail("type mismatch");
    } else if (dict_find(c, key, &pos)) {
        c->b[pos] = *val;
        return 0;
    }
    if (c->n >= DICT_MAX) return vfail("dict too long");
    if (coll_blocks(c) != 0) return vfail("out of memory");
    if (c->n) dict_find(c, key, &pos);
    for (i = (int)c->n; i > pos; i--) {
        c->a[i] = c->a[i - 1];
        c->b[i] = c->b[i - 1];
    }
    c->a[pos] = *key;
    c->b[pos] = *val;
    c->n++;
    return 0;
}

static int KEXT coll_get(int id, const val_t *idx, val_t *out)
{
    coll_t *c;
    cell_t key;
    int pos;

    if (id < 0 || id >= COLL_MAX || !s_coll[id].kind) return vfail("bad expression");
    c = &s_coll[id];
    if (c->kind == V_ARR) {
        if (!type_wide(idx->type)) return vfail("not an integer");
        if (idx->i < 0 || idx->i >= (int32_t)c->n) return vfail("out of range");
        cell_to_val(&c->a[idx->i], out);
        return 0;
    }
    cell_from_val(&key, idx);
    if (!scalar_ok(key.type)) return vfail("type mismatch");
    if (!c->n || key.type != c->kt) return vfail("no such key");
    if (!dict_find(c, &key, &pos)) return vfail("no such key");
    cell_to_val(&c->b[pos], out);
    return 0;
}

static int KEXT cell_text(const cell_t *c, char *out, int size)
{
    val_t v;

    if (c->type == V_BYTE) return ksnprintf(out, size, "%db", (int)c->u.i) >= size;
    if (c->type == V_STR) return ksnprintf(out, size, "\"%s\"", c->u.s) >= size;
    cell_to_val(c, &v);
    val_text(&v, out, size);
    return 0;
}

static int KEXT coll_text(int id, char *out, int size)
{
    coll_t *c;
    int o = 0, i;

    if (size < 3 || id < 0 || id >= COLL_MAX || !s_coll[id].kind) return -1;
    c = &s_coll[id];
    out[o++] = (c->kind == V_ARR) ? '[' : '{';
    for (i = 0; i < (int)c->n; i++) {
        char piece[VAR_STR + 4];
        int n, k;

        if (i) {
            if (o + 2 >= size) return -1;
            out[o++] = ',';
            out[o++] = ' ';
        }
        if (cell_text(&c->a[i], piece, (int)sizeof piece) != 0) return -1;
        n = (int)strlen(piece);
        if (o + n >= size) return -1;
        for (k = 0; k < n; k++) out[o++] = piece[k];
        if (c->kind == V_DICT) {
            if (o + 2 >= size) return -1;
            out[o++] = ':';
            out[o++] = ' ';
            if (cell_text(&c->b[i], piece, (int)sizeof piece) != 0) return -1;
            n = (int)strlen(piece);
            if (o + n >= size) return -1;
            for (k = 0; k < n; k++) out[o++] = piece[k];
        }
    }
    if (o + 1 >= size) return -1;
    out[o++] = (c->kind == V_ARR) ? ']' : '}';
    out[o] = '\0';
    return 0;
}

static int KEXT same_cells(int ia, int ib)
{
    const coll_t *a, *b;
    int i;

    if (ia < 0 || ib < 0 || ia >= COLL_MAX || ib >= COLL_MAX) return 0;
    a = &s_coll[ia];
    b = &s_coll[ib];
    if (a->kind != b->kind || a->n != b->n) return 0;
    if (a->kind == V_ARR && a->n && a->et != b->et) return 0;
    if (a->kind == V_DICT && a->n && (a->kt != b->kt || a->vt != b->vt)) return 0;
    for (i = 0; i < (int)a->n; i++) {
        if (a->a[i].type != b->a[i].type || cell_cmp(&a->a[i], &b->a[i]) != 0)
            return 0;
        if (a->kind == V_DICT &&
            (a->b[i].type != b->b[i].type || cell_cmp(&a->b[i], &b->b[i]) != 0))
            return 0;
    }
    return 1;
}

static int KEXT var_copy(const char *name, int nlen, char *out, int size)
{
    shell_var_t *slot = var_find(name, nlen, 0);
    val_t v;

    if (!slot) return -1;
    if (is_coll(slot->type)) return coll_text(slot->u.i, out, size);
    v.type = slot->type;
    v.i = 0;
    v.f = 0.f;
    v.s[0] = '\0';
    if (type_hold_i(slot->type)) v.i = slot->u.i;
    else if (slot->type == V_FLT) v.f = slot->u.f;
    else if (slot->type == V_STR) memcpy(v.s, slot->u.s, VAR_STR);
    val_text(&v, out, size);
    return 0;
}

static void KEXT var_list(void)
{
    int any = 0, i;

    for (i = 0; i < VAR_MAX; i++) {
        char buf[LINE_MAX];

        if (s_var[i].type == V_NONE) continue;
        any = 1;
        if (s_var[i].type == V_INT)
            kprintf("%s = %d\r\n", s_var[i].name, (int)s_var[i].u.i);
        else if (s_var[i].type == V_BYTE)
            kprintf("%s = %db\r\n", s_var[i].name, (int)s_var[i].u.i);
        else if (s_var[i].type == V_BOOL)
            kprintf("%s = %s\r\n", s_var[i].name,
                    s_var[i].u.i ? "true" : "false");
        else if (s_var[i].type == V_EMPTY)
            kprintf("%s = empty\r\n", s_var[i].name);
        else if (s_var[i].type == V_NIL)
            kprintf("%s = none\r\n", s_var[i].name);
        else if (s_var[i].type == V_FLT) {
            ftoa(buf, (int)sizeof buf, s_var[i].u.f);
            kprintf("%s = %s\r\n", s_var[i].name, buf);
        } else if (is_coll(s_var[i].type)) {
            kprintf("%s = ", s_var[i].name);
            if (coll_text(s_var[i].u.i, buf, (int)sizeof buf) != 0)
                kprintf("...\r\n");
            else
                kprintf("%s\r\n", buf);
        } else
            kprintf("%s = \"%s\"\r\n", s_var[i].name, s_var[i].u.s);
    }
    if (!any) kprintf("no variables\r\n");
}

static int KEXT load_var(const char *name, int nlen, val_t *out)
{
    shell_var_t *slot = var_find(name, nlen, 0);
    char nb[VAR_NAME];

    if (!slot) {
        memcpy(nb, name, (size_t)nlen);
        nb[nlen] = '\0';
        kprintf("%s: no such variable: %s\r\n", s_vwho, nb);
        return -1;
    }
    out->type = slot->type;
    out->i = 0;
    out->f = 0.f;
    out->s[0] = '\0';
    if (is_coll(slot->type)) {
        int id = coll_clone(slot->u.i);
        if (id < 0) return vfail("out of memory");
        out->i = id;
        return 0;
    }
    if (type_hold_i(slot->type)) out->i = slot->u.i;
    else if (slot->type == V_FLT) out->f = slot->u.f;
    else if (slot->type == V_STR) memcpy(out->s, slot->u.s, VAR_STR);
    return 0;
}

static int KEXT add_i(int32_t a, int32_t b, int32_t *o)
{
    if ((b > 0 && a > 2147483647 - b) ||
        (b < 0 && a < (-2147483647 - 1) - b))
        return -1;
    *o = a + b;
    return 0;
}

static int KEXT mul_i(int32_t a, int32_t b, int32_t *o)
{
    const int32_t vmin = (-2147483647 - 1);
    int neg = 0;
    uint32_t ua, ub, r;

    if (a == 0 || b == 0) { *o = 0; return 0; }
    if (a == vmin) {
        if (b != 1) return -1;
        *o = a;
        return 0;
    }
    if (b == vmin) {
        if (a != 1) return -1;
        *o = b;
        return 0;
    }
    if (a < 0) { neg ^= 1; a = -a; }
    if (b < 0) { neg ^= 1; b = -b; }
    ua = (uint32_t)a;
    ub = (uint32_t)b;
    if (ub > 0xFFFFFFFFu / ua) return -1;
    r = ua * ub;
    if (!neg) {
        if (r > 2147483647u) return -1;
        *o = (int32_t)r;
        return 0;
    }
    if (r > 2147483648u) return -1;
    *o = (int32_t)(0u - r);
    return 0;
}

enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR
};

static int KEXT apply_num(int op, val_t *a, const val_t *b)
{
    int flt = (a->type == V_FLT || b->type == V_FLT);
    int32_t ia, ib, ir;
    float fa, fb;

    if (a->type == V_STR || b->type == V_STR ||
        a->type == V_EMPTY || b->type == V_EMPTY ||
        a->type == V_NIL || b->type == V_NIL ||
        a->type == V_BOOL || b->type == V_BOOL ||
        is_coll(a->type) || is_coll(b->type))
        return vfail("bad expression");

    if (op == OP_MOD || op == OP_AND || op == OP_OR || op == OP_XOR ||
        op == OP_SHL || op == OP_SHR) {
        if (!type_wide(a->type) || !type_wide(b->type)) return vfail("not an integer");
        ia = a->i;
        ib = b->i;
        if (op == OP_MOD) {
            if (ib == 0) return vfail("division by zero");
            if (ia == (-2147483647 - 1) && ib == -1) ir = 0;
            else ir = ia % ib;
        } else if (op == OP_AND) ir = ia & ib;
        else if (op == OP_OR) ir = ia | ib;
        else if (op == OP_XOR) ir = ia ^ ib;
        else {
            if (ib < 0 || ib > 31) return vfail("bad shift");
            if (op == OP_SHL) ir = (int32_t)((uint32_t)ia << (uint32_t)ib);
            else ir = (int32_t)((uint32_t)ia >> (uint32_t)ib);
        }
        a->type = V_INT;
        a->i = ir;
        return 0;
    }

    if (flt) {
        fa = type_wide(a->type) ? (float)a->i : a->f;
        fb = type_wide(b->type) ? (float)b->i : b->f;
        if (op == OP_ADD) fa = fa + fb;
        else if (op == OP_SUB) fa = fa - fb;
        else if (op == OP_MUL) fa = fa * fb;
        else {
            if (fb == 0.f) return vfail("division by zero");
            fa = fa / fb;
        }
        a->type = V_FLT;
        a->f = fa;
        return 0;
    }

    ia = a->i;
    ib = b->i;
    if (op == OP_ADD) {
        if (add_i(ia, ib, &ir) != 0) return vfail("integer overflow");
    } else if (op == OP_SUB) {
        if (ib == (-2147483647 - 1)) {
            if (ia >= 0) return vfail("integer overflow");
            ir = (int32_t)((uint32_t)ia + 2147483648u);
        } else if (add_i(ia, -ib, &ir) != 0) {
            return vfail("integer overflow");
        }
    } else if (op == OP_MUL) {
        if (mul_i(ia, ib, &ir) != 0) return vfail("integer overflow");
    } else {
        if (ib == 0) return vfail("division by zero");
        if (ia == (-2147483647 - 1) && ib == -1) return vfail("integer overflow");
        ir = ia / ib;
    }
    a->type = V_INT;
    a->i = ir;
    return 0;
}

static int KEXT apply_add(val_t *a, const val_t *b)
{
    char left[VAR_STR], right[VAR_STR];
    int n, m;

    if (a->type != V_STR && b->type != V_STR)
        return apply_num(OP_ADD, a, b);
    val_text(a, left, (int)sizeof left);
    val_text(b, right, (int)sizeof right);
    n = (int)strlen(left);
    m = (int)strlen(right);
    if (n + m >= VAR_STR) return vfail("string too long");
    val_drop(a);
    memcpy(a->s, left, (size_t)n);
    memcpy(a->s + n, right, (size_t)m + 1);
    a->type = V_STR;
    return 0;
}

/* One conversion in a format string, or concatenation when it has none.
 * '%%' is a literal percent.  A later conversion is left for the next
 * argument. */
static int KEXT format_step(val_t *dst, const val_t *arg)
{
    char out[VAR_STR];
    char piece[VAR_STR];
    const char *f = dst->s;
    int o = 0;

    while (*f) {
        if (*f != '%') {
            if (o >= VAR_STR - 1) return vfail("string too long");
            out[o++] = *f++;
            continue;
        }
        f++;
        if (*f == '%') {
            if (o >= VAR_STR - 1) return vfail("string too long");
            out[o++] = '%';
            f++;
            continue;
        }
        if (*f != 'd' && *f != 'i' && *f != 'u' && *f != 'x' && *f != 'X' &&
            *f != 's' && *f != 'f')
            return vfail("bad format");
        if (*f == 's') val_text(arg, piece, (int)sizeof piece);
        else if (*f == 'f') {
            float fv;
            if (arg->type == V_STR || arg->type == V_EMPTY ||
                arg->type == V_NIL || arg->type == V_BOOL || is_coll(arg->type))
                return vfail("bad format");
            fv = type_wide(arg->type) ? (float)arg->i : arg->f;
            ftoa(piece, (int)sizeof piece, fv);
        } else if (*f == 'd' || *f == 'i') {
            int32_t n;
            if (arg->type == V_STR || arg->type == V_EMPTY ||
                arg->type == V_NIL || arg->type == V_BOOL || is_coll(arg->type))
                return vfail("bad format");
            if (arg->type == V_FLT) {
                if (arg->f > 2147483647.f || arg->f < -2147483648.f)
                    return vfail("integer overflow");
                n = (int32_t)arg->f;
            } else n = arg->i;
            ksnprintf(piece, (int)sizeof piece, "%d", (int)n);
        } else {
            if (!type_wide(arg->type)) return vfail("not an integer");
            ksnprintf(piece, (int)sizeof piece,
                      (*f == 'u') ? "%u" : (*f == 'X') ? "%X" : "%x",
                      (unsigned)(uint32_t)arg->i);
        }
        f++;
        {
            int pn = (int)strlen(piece);
            int i;
            if (o + pn >= VAR_STR) return vfail("string too long");
            for (i = 0; i < pn; i++) out[o++] = piece[i];
        }
        while (*f) {
            if (o >= VAR_STR - 1) return vfail("string too long");
            out[o++] = *f++;
        }
        out[o] = '\0';
        memcpy(dst->s, out, (size_t)o + 1);
        dst->type = V_STR;
        return 0;
    }
    out[o] = '\0';
    memcpy(dst->s, out, (size_t)o + 1);
    dst->type = V_STR;
    {
        val_t extra = *arg;
        return apply_add(dst, &extra);
    }
}

static int KEXT parse_expr(const char **pp, val_t *out);

/* ---------------------------------------------------------- functions */
/*
 * Four functions.  A body is kept on the heap, at most 127 characters,
 * so the static cost is the table.  A call takes 0 to 32 arguments.
 * 'return' leaves from anywhere in the body with 1 to 32 values, each
 * an integer, a float, a byte, a bool, empty, none, or a string.  The arguments of the call in
 * progress sit on the stack, sized to how many were passed.  $0 is
 * that count, $1 .. $32 are the values.  No 'return' leaves the
 * integer 0.  One name in 'set' takes the first value; several names
 * take the first of those values from a call.
 */
#define FN_MAX    4
#define FN_BODY   128
#define FN_ARGS   32
#define FN_NEST   4
#define FN_STACK  1900

typedef struct {
    uint8_t type;
    union {
        int32_t i;
        float f;
        char s[VAR_STR];
    } u;
} fn_arg_t;

typedef struct {
    char name[VAR_NAME];
    char *body;
} shell_fn_t;

/* A script thread and the list of returned values share the worker
 * stacks.  Nothing else uses that memory until a program starts, and a
 * program is refused while a script thread is alive.  The Blue Pill has
 * no room for both in the kernel's own RAM. */
#define SH_MAX    2

typedef struct {
    uint8_t kind;
    uint8_t skip;
    uint8_t outer;
    uint8_t live;
    uint8_t arm;
    uint32_t left;
    const char *restart;
} sh_fr_t;

typedef struct {
    char name[VAR_NAME];
    char body[FN_BODY];
    int8_t priority;
    uint8_t state;
    uint8_t yielded;
    uint32_t wake;
    const char *pos;
    sh_fr_t fr[SCRIPT_NEST];
    int sp;
} sh_thr_t;

typedef struct {
    fn_arg_t retv[FN_ARGS];
    sh_thr_t sh[SH_MAX];
} shell_scratch_t;

#ifdef FREYA_HOST
static uint8_t s_scratch_mem[2048];
#define SCRATCH_BYTES s_scratch_mem
#else
extern uint8_t __worker_stacks[];
#define SCRATCH_BYTES __worker_stacks
#endif

static uint32_t s_scratch_runs = 0xffffffffu;

static shell_scratch_t *shell_scratch(void)
{
    shell_scratch_t *p = (shell_scratch_t *)(void *)SCRATCH_BYTES;

    /* A running program owns these bytes as thread stacks. */
    if (g_app.running) return p;
    if (s_scratch_runs != g_app.runs) {
        memset(p, 0, sizeof *p);
        s_scratch_runs = g_app.runs;
    }
    return p;
}

#define s_fn_retv (shell_scratch()->retv)
#define s_sh      (shell_scratch()->sh)

_Static_assert(sizeof(shell_scratch_t) <= 2048,
               "shell scratch does not fit in the worker stacks");

static shell_fn_t s_fn[FN_MAX];
static fn_arg_t *s_fn_args;
static int s_fn_argc;
static int s_fn_depth;
static int s_fn_stack;
/* Values the call in progress returned.  One list, in the scratch
 * above: a caller that has already produced values copies them aside,
 * counted in s_fn_stack, before it evaluates an expression that may
 * call again. */
static int s_fn_nret;
/* 1 when the expression just parsed was a call and nothing else, so
 * every value it returned is still in s_fn_retv. */
static int s_bare_call;

static void KEXT rets_drop(void)
{
    int i;

    for (i = 0; i < s_fn_nret; i++) {
        if (is_coll(s_fn_retv[i].type)) coll_free(s_fn_retv[i].u.i);
        s_fn_retv[i].type = V_NONE;
    }
    s_fn_nret = 0;
}

static void KEXT end_bare(void)
{
    if (s_bare_call) rets_drop();
    s_bare_call = 0;
}

static void KEXT args_drop(fn_arg_t *args, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (!is_coll(args[i].type)) continue;
        coll_free(args[i].u.i);
        args[i].type = V_NONE;
    }
}

static int KEXT exec_block(const char **pp, int skip, char *walk, char *one);

static int KEXT fn_reserved(const char *s, int n)
{
    static const char *const w[] __attribute__((section(".rodata.kext_script"))) = {
        "if", "else", "end", "loop", "break", "fn", "return",
        "get", "set", "adc", "pwm", "int", "float", "byte", "bool", "str", "hex",
        "true", "false", "empty", "none", "array", "dict", "len", "min", "max", "sort",
        "rand", "srand", "sin", "cos", "pi",
        "now", "date", "time", "year", "month", "day",
        "hour", "minute", "second",
        "ticks", "timer", "tstart", "tstop", "tcount", "tclose", "tperiod",
        "irq", "wait",
        "spawn", "yield", "join",
        "open", "read", "write", "close", "seek", "flush",
        "match", "find", "gsub"
    };
    int i;

    for (i = 0; i < (int)(sizeof w / sizeof w[0]); i++) {
        if ((int)strlen(w[i]) == n && strncmp(s, w[i], (size_t)n) == 0)
            return 1;
    }
    return 0;
}

static shell_fn_t *KEXT fn_slot(const char *name, int nlen, int create)
{
    shell_fn_t *gap = NULL;
    int i;

    for (i = 0; i < FN_MAX; i++) {
        if (!s_fn[i].body) {
            if (!gap) gap = &s_fn[i];
            continue;
        }
        if (strncmp(s_fn[i].name, name, (size_t)nlen) == 0 &&
            s_fn[i].name[nlen] == '\0')
            return &s_fn[i];
    }
    return create ? gap : NULL;
}

static void KEXT fn_list(void)
{
    int any = 0, i;

    for (i = 0; i < FN_MAX; i++) {
        if (!s_fn[i].body) continue;
        any = 1;
        kprintf("%s\r\n", s_fn[i].name);
    }
    if (!any) kprintf("no functions\r\n");
}

/* 0 copied, -2 when the body does not fit, -1 when the block is open. */
static int KEXT fn_slurp(const char **pp, char *dst, int size, char *walk)
{
    int depth = 1, n = 0;

    while (next_stmt(pp, walk, LINE_MAX) == 0) {
        int add;

        if (word_is(walk, "if", NULL) || word_is(walk, "loop", NULL) ||
            word_is(walk, "fn", NULL))
            depth++;
        else if (word_is(walk, "end", NULL)) {
            if (--depth == 0) {
                if (dst) dst[n] = '\0';
                return 0;
            }
        }
        if (!dst) continue;
        add = (int)strlen(walk);
        if ((n ? n + 1 : 0) + add >= size) return -2;
        if (n) dst[n++] = '\n';
        memcpy(dst + n, walk, (size_t)add);
        n += add;
    }
    return -1;
}

static int KEXT fn_define(const char *rest, const char **pp, char *walk, int skip)
{
    char tmp[FN_BODY];
    char name[VAR_NAME];
    int nlen = 0, rc;
    shell_fn_t *slot;
    char *mem;

    s_vwho = "fn";
    if (!name_char(*rest, 1)) return vfail("bad name");
    while (name_char(rest[nlen], 0)) nlen++;
    if (nlen >= VAR_NAME) return vfail("bad name");
    if (rest[nlen]) return usage("fn [<name>]");
    memcpy(name, rest, (size_t)nlen);
    name[nlen] = '\0';
    if (fn_reserved(name, nlen)) return vfail("bad name");

    rc = fn_slurp(pp, skip ? NULL : tmp, FN_BODY, walk);
    if (rc == -2) return vfail("function too long");
    if (rc != 0) {
        kprintf("missing end\r\n");
        s_status = FREYA_EXIT_FAIL;
        return -1;
    }
    if (skip) return 0;

    slot = fn_slot(name, nlen, 1);
    if (!slot) return vfail("too many functions");
    mem = kmalloc((uint32_t)strlen(tmp) + 1U);
    if (!mem) return vfail("out of memory");
    memcpy(mem, tmp, strlen(tmp) + 1U);
    if (slot->body) kfree(slot->body);
    else {
        memcpy(slot->name, name, (size_t)nlen + 1U);
    }
    slot->body = mem;
    return 0;
}

static int KEXT arg_get(int idx, val_t *out)
{
    const fn_arg_t *a;

    out->type = V_INT;
    out->i = 0;
    out->f = 0.f;
    out->s[0] = '\0';
    if (s_fn_depth <= 0 || idx < 0 || idx > s_fn_argc) return -1;
    if (idx == 0) {
        out->i = s_fn_argc;
        return 0;
    }
    a = &s_fn_args[idx - 1];
    out->type = a->type;
    if (is_coll(a->type)) out->i = a->u.i;
    else if (type_hold_i(a->type)) out->i = a->u.i;
    else if (a->type == V_FLT) out->f = a->u.f;
    else if (a->type == V_STR) memcpy(out->s, a->u.s, VAR_STR);
    return 0;
}

static int KEXT arg_load(int idx, val_t *out)
{
    if (arg_get(idx, out) != 0) return vfail("no such argument");
    if (is_coll(out->type)) {
        int id = coll_clone(out->i);
        if (id < 0) return vfail("out of memory");
        out->i = id;
    }
    return 0;
}

static int KEXT arg_copy(int idx, char *out, int size)
{
    val_t v;

    if (arg_get(idx, &v) != 0) return -1;
    val_text(&v, out, size);
    return 0;
}

static void KEXT val_arg(fn_arg_t *a, const val_t *v)
{
    a->type = v->type;
    if (is_coll(v->type) || type_hold_i(v->type)) a->u.i = v->i;
    else if (v->type == V_FLT) a->u.f = v->f;
    else if (v->type == V_STR) memcpy(a->u.s, v->s, VAR_STR);
}

static int KEXT ret_one(val_t *out, const fn_arg_t *a)
{
    out->type = a->type;
    out->i = 0;
    out->f = 0.f;
    out->s[0] = '\0';
    if (is_coll(a->type)) {
        int id = coll_clone(a->u.i);
        if (id < 0) return vfail("out of memory");
        out->i = id;
        return 0;
    }
    if (type_hold_i(a->type)) out->i = a->u.i;
    else if (a->type == V_FLT) out->f = a->u.f;
    else if (a->type == V_STR) memcpy(out->s, a->u.s, VAR_STR);
    return 0;
}

static void KEXT ret_zero(void)
{
    memset(&s_fn_retv[0], 0, sizeof s_fn_retv[0]);
    s_fn_retv[0].type = V_INT;
    s_fn_nret = 1;
}

static int KEXT fn_invoke(const char *body, fn_arg_t *args, int argc, val_t *out)
{
    char walk[LINE_MAX];
    char one[LINE_MAX];
    const char *p = body;
    fn_arg_t *saved_a = s_fn_args;
    int saved_n = s_fn_argc;
    int rc;

    s_fn_args = args;
    s_fn_argc = argc;
    s_fn_depth++;
    rc = exec_block(&p, 0, walk, one);
    s_fn_depth--;
    s_fn_args = saved_a;
    s_fn_argc = saved_n;
    if (rc == SCR_RETURN) return ret_one(out, &s_fn_retv[0]);
    if (rc == SCR_DONE) {
        ret_zero();
        return ret_one(out, &s_fn_retv[0]);
    }
    if (rc == SCR_BREAK) {
        kprintf("unexpected break\r\n");
        s_status = FREYA_EXIT_FAIL;
    }
    return -1;
}

/* base 10 or 16.  A leading sign is kept.  Hex may start with 0x and
 * keeps all 32 bits, so the high bit is the sign.  -1 bad, -2 overflow. */
static int KEXT parse_i32(const char *s, int base, int32_t *o)
{
    int neg = 0, digits = 0;
    uint32_t u = 0;

    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s) {
        int d;

        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return -1;
        if (base == 10) {
            if (u > (2147483647u - (uint32_t)d) / 10u) {
                if (!(neg && u == 214748364u && d == 8 && s[1] == '\0'))
                    return -2;
            }
        } else if (u > (0xFFFFFFFFu - (uint32_t)d) / 16u) {
            return -2;
        }
        u = u * (uint32_t)base + (uint32_t)d;
        digits++;
        s++;
    }
    if (!digits) return -1;
    if (neg) {
        if (u > 2147483648u) return -2;
        *o = (int32_t)(0u - u);
    } else if (base == 16) {
        *o = (int32_t)u;
    } else {
        if (u > 2147483647u) return -2;
        *o = (int32_t)u;
    }
    return 0;
}

static int KEXT parse_f32(const char *s, float *o)
{
    int neg = 0, saw = 0, dot = 0;
    float ip = 0.f, scale = 0.1f;

    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            int d = *s - '0';
            saw = 1;
            if (!dot) ip = ip * 10.f + (float)d;
            else { ip = ip + (float)d * scale; scale *= 0.1f; }
            s++;
        } else if (*s == '.' && !dot) {
            dot = 1;
            s++;
        } else return -1;
    }
    if (!saw) return -1;
    *o = neg ? -ip : ip;
    return 0;
}

static int KEXT hex_prefix(const char *s)
{
    if (*s == '+' || *s == '-') s++;
    return s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

/* 0 fits, -1 not a number, -2 out of int32 range.  Truncates toward 0. */
static int KEXT flt_to_i32(float f, int32_t *o)
{
    if (f != f) return -1;
    if (f >= 2147483648.f || f < -2147483648.f) return -2;
    *o = (int32_t)f;
    return 0;
}

static int KEXT i32_fail(int rc)
{
    if (rc == -2) return vfail("integer overflow");
    if (rc != 0) return vfail("not a number");
    return 0;
}

static int KEXT conv_int(const fn_arg_t *a, val_t *out)
{
    int32_t n;
    int rc;

    if (a->type == V_INT || a->type == V_BYTE || a->type == V_BOOL) {
        out->i = a->type == V_BOOL ? (a->u.i ? 1 : 0) : a->u.i;
        return 1;
    }
    if (a->type == V_EMPTY || a->type == V_NIL || is_coll(a->type))
        return vfail("not a number");
    if (a->type == V_FLT) {
        rc = flt_to_i32(a->u.f, &n);
        if (i32_fail(rc) != 0) return -1;
        out->i = n;
        return 1;
    }
    if (hex_prefix(a->u.s)) rc = parse_i32(a->u.s, 16, &n);
    else if (strchr(a->u.s, '.') != NULL) {
        float f;
        rc = parse_f32(a->u.s, &f);
        if (rc == 0) rc = flt_to_i32(f, &n);
    } else rc = parse_i32(a->u.s, 10, &n);
    if (i32_fail(rc) != 0) return -1;
    out->i = n;
    return 1;
}

static int KEXT conv_float(const fn_arg_t *a, val_t *out)
{
    out->type = V_FLT;
    if (a->type == V_FLT) {
        out->f = a->u.f;
        return 1;
    }
    if (a->type == V_INT || a->type == V_BYTE || a->type == V_BOOL) {
        out->f = (float)(a->type == V_BOOL ? (a->u.i ? 1 : 0) : a->u.i);
        return 1;
    }
    if (a->type == V_EMPTY || a->type == V_NIL || is_coll(a->type))
        return vfail("not a number");
    if (hex_prefix(a->u.s)) {
        int32_t n;
        int rc = parse_i32(a->u.s, 16, &n);
        if (i32_fail(rc) != 0) return -1;
        out->f = (float)n;
        return 1;
    }
    if (parse_f32(a->u.s, &out->f) != 0) return vfail("not a number");
    return 1;
}

static int KEXT conv_str(const fn_arg_t *a, val_t *out)
{
    val_t v;

    v.type = a->type;
    v.i = 0;
    v.f = 0.f;
    v.s[0] = '\0';
    if (is_coll(a->type)) v.i = a->u.i;
    else if (a->type == V_INT || a->type == V_BYTE || a->type == V_BOOL)
        v.i = a->u.i;
    else if (a->type == V_FLT) v.f = a->u.f;
    else if (a->type == V_STR) memcpy(v.s, a->u.s, VAR_STR);
    val_text(&v, out->s, VAR_STR);
    out->type = V_STR;
    return 1;
}

static int KEXT conv_hex(const fn_arg_t *a, val_t *out)
{
    int32_t n;
    int rc;

    if (a->type == V_EMPTY || a->type == V_NIL || is_coll(a->type))
        return vfail("not a number");
    if (a->type == V_BOOL) {
        ksnprintf(out->s, VAR_STR, "%x", a->u.i ? 1u : 0u);
        out->type = V_STR;
        return 1;
    }
    if (a->type == V_STR) {
        rc = parse_i32(a->u.s, 16, &n);
        if (i32_fail(rc) != 0) return -1;
        out->i = n;
        return 1;
    }
    if (a->type == V_FLT) {
        rc = flt_to_i32(a->u.f, &n);
        if (i32_fail(rc) != 0) return -1;
    } else n = a->u.i;
    ksnprintf(out->s, VAR_STR, "%x", (unsigned)(uint32_t)n);
    out->type = V_STR;
    return 1;
}

/* 0..255.  A float is truncated toward zero first. */
static int KEXT conv_byte(const fn_arg_t *a, val_t *out)
{
    val_t n;
    int rc;

    n.type = V_INT;
    n.i = 0;
    rc = conv_int(a, &n);
    if (rc < 0) return -1;
    if (n.i < 0 || n.i > 255) return vfail("integer overflow");
    out->type = V_BYTE;
    out->i = n.i;
    return 1;
}

/* 0 or 1.  A number is false only at zero.  Text is "true" or "false". */
static int KEXT conv_bool(const fn_arg_t *a, val_t *out)
{
    out->type = V_BOOL;
    if (a->type == V_BOOL) {
        out->i = a->u.i ? 1 : 0;
        return 1;
    }
    if (a->type == V_INT || a->type == V_BYTE) {
        out->i = a->u.i ? 1 : 0;
        return 1;
    }
    if (a->type == V_FLT) {
        if (a->u.f != a->u.f) return vfail("not a number");
        out->i = (a->u.f == 0.f) ? 0 : 1;
        return 1;
    }
    if (a->type == V_STR) {
        if (strcmp(a->u.s, "true") == 0) { out->i = 1; return 1; }
        if (strcmp(a->u.s, "false") == 0) { out->i = 0; return 1; }
        return vfail("not a number");
    }
    return vfail("not a number");
}

/* ANSI C 1989 7.10.2.1 example.  The state is 32 bits, unsigned long
 * on this machine, and the result is 0..32767.  The seed starts at 1. */
static uint32_t s_rand_next = 1;

static int32_t KEXT rand_step(void)
{
    s_rand_next = s_rand_next * 1103515245u + 12345u;
    return (int32_t)((s_rand_next / 65536u) % 32768u);
}

/* Single precision.  The angle is reduced to a quadrant of pi/2, then
 * a Taylor polynomial covers that quadrant.  Past about 1e6 the
 * reduction no longer has a meaningful fraction of pi left. */
static float KEXT shell_pi(void)
{
    return 3.14159265358979323846f;
}

static float KEXT poly_sin(float y)
{
    float y2 = y * y;
    return y * (1.f + y2 * (-1.6666666666666666e-1f + y2 *
           (8.333333333333333e-3f + y2 * (-1.984126984126984e-4f + y2 *
           (2.755731922398589e-6f + y2 * -2.505210838544172e-8f)))));
}

static float KEXT poly_cos(float y)
{
    float y2 = y * y;
    return 1.f + y2 * (-0.5f + y2 * (4.166666666666666e-2f + y2 *
           (-1.388888888888889e-3f + y2 * (2.480158730158730e-5f + y2 *
           -2.755731922398589e-7f))));
}

/* 0 and the value, -1 when the angle is not a usable number. */
static int KEXT shell_sincos(float x, int cos, float *o)
{
    const float half_pi = 1.5707963267948966f;
    float fn, y;
    int n, q;

    if (x != x || x > 1.0e6f || x < -1.0e6f) return -1;
    fn = x * 0.6366197723675814f;
    n = (int)(fn >= 0.f ? fn + 0.5f : fn - 0.5f);
    y = x - (float)n * half_pi;
    q = n & 3;
    if (cos) q = (q + 1) & 3;
    if (q == 0) *o = poly_sin(y);
    else if (q == 1) *o = poly_cos(y);
    else if (q == 2) *o = -poly_sin(y);
    else *o = -poly_cos(y);
    return 0;
}

static int KEXT conv_sincos(const fn_arg_t *a, int cos, val_t *out)
{
    float x, y;

    if (a->type == V_FLT) x = a->u.f;
    else if (a->type == V_INT || a->type == V_BYTE) x = (float)a->u.i;
    else return vfail("bad expression");
    if (shell_sincos(x, cos, &y) != 0) return vfail("not a number");
    out->type = V_FLT;
    out->f = y;
    return 1;
}

/* Civil time as seconds since 1970-01-01 00:00:00.  The result has to
 * fit in a signed integer, so the last instant is 2038-01-19 03:14:07.
 * The clock itself is the software RTC; these only convert. */
static int KEXT dt_leap(uint32_t y)
{
    return (y % 4u == 0u && y % 100u != 0u) || (y % 400u == 0u);
}

static int KEXT dt_mdays(uint32_t y, int m)
{
    static const uint8_t md[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (m == 2 && dt_leap(y)) return 29;
    return md[m - 1];
}

/* 0 stored, -1 the fields are not a date, -2 the instant does not fit. */
static int KEXT dt_pack(int y, int mo, int d, int h, int mi, int s, int32_t *out)
{
    uint32_t days = 0, year, secs;
    int m;

    if (mo < 1 || mo > 12 || d < 1 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59 ||
        y < 1970 || d > dt_mdays((uint32_t)y, mo))
        return -1;
    /* Later years are real dates, but they do not fit a signed count. */
    if (y > 2038) return -2;
    for (year = 1970; year < (uint32_t)y; year++)
        days += dt_leap(year) ? 366u : 365u;
    for (m = 1; m < mo; m++)
        days += (uint32_t)dt_mdays((uint32_t)y, m);
    days += (uint32_t)d - 1u;
    if (days > 2147483647u / 86400u) return -2;
    secs = days * 86400u + (uint32_t)h * 3600u +
           (uint32_t)mi * 60u + (uint32_t)s;
    if (secs > 2147483647u) return -2;
    *out = (int32_t)secs;
    return 0;
}

static int KEXT dt_unpack(int32_t secs, int *y, int *mo, int *d,
                          int *h, int *mi, int *s)
{
    uint32_t u, days, rem, year;
    int m;

    if (secs < 0) return -1;
    u = (uint32_t)secs;
    days = u / 86400u;
    rem = u % 86400u;
    *h = (int)(rem / 3600u);
    *mi = (int)((rem % 3600u) / 60u);
    *s = (int)(rem % 60u);
    year = 1970;
    for (;;) {
        uint32_t len = dt_leap(year) ? 366u : 365u;
        if (days < len) break;
        days -= len;
        year++;
    }
    for (m = 1; m <= 12; m++) {
        uint32_t len = (uint32_t)dt_mdays(year, m);
        if (days < len) break;
        days -= len;
    }
    *y = (int)year;
    *mo = m;
    *d = (int)days + 1;
    return 0;
}

static void KEXT dt_clock(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    rtc_time_t t;

    rtc_get(&t);
    *y = (int)t.year;
    *mo = (int)t.mon;
    *d = (int)t.day;
    *h = (int)t.hour;
    *mi = (int)t.min;
    *s = (int)t.sec;
}

static int KEXT dt_format(int y, int mo, int d, int h, int mi, int s, val_t *out)
{
    ksnprintf(out->s, VAR_STR, "%04u-%02u-%02u %02u:%02u:%02u",
              (unsigned)y, (unsigned)mo, (unsigned)d,
              (unsigned)h, (unsigned)mi, (unsigned)s);
    out->type = V_STR;
    return 1;
}

/* year month day hour minute second, or -1. */
static int KEXT dt_which(const char *name, int nlen)
{
    static const char *const n[] = {
        "year", "month", "day", "hour", "minute", "second"
    };
    int i;

    for (i = 0; i < 6; i++) {
        if ((int)strlen(n[i]) == nlen && strncmp(name, n[i], (size_t)nlen) == 0)
            return i;
    }
    return -1;
}

static int KEXT dt_part(int which, fn_arg_t *args, int argc, val_t *out)
{
    int y, mo, d, h, mi, s;
    int part[6];

    if (argc == 0) dt_clock(&y, &mo, &d, &h, &mi, &s);
    else if (argc == 1 && args[0].type == V_INT) {
        if (dt_unpack(args[0].u.i, &y, &mo, &d, &h, &mi, &s) != 0)
            return vfail("integer overflow");
    } else return vfail("bad expression");
    part[0] = y; part[1] = mo; part[2] = d;
    part[3] = h; part[4] = mi; part[5] = s;
    out->i = part[which];
    return 1;
}

static int KEXT dt_builtin(const char *name, int nlen, fn_arg_t *args,
                           int argc, val_t *out)
{
    int which, y, mo, d, h, mi, s, rc;
    int32_t secs;

    which = dt_which(name, nlen);
    if (which >= 0) return dt_part(which, args, argc, out);

    if (nlen == 3 && strncmp(name, "now", 3) == 0) {
        if (argc != 0) return vfail("bad expression");
        dt_clock(&y, &mo, &d, &h, &mi, &s);
        rc = dt_pack(y, mo, d, h, mi, s, &secs);
        if (rc != 0) return vfail("integer overflow");
        out->i = secs;
        return 1;
    }
    if (nlen == 4 && strncmp(name, "date", 4) == 0) {
        if (argc == 0) dt_clock(&y, &mo, &d, &h, &mi, &s);
        else if (argc == 1 && args[0].type == V_INT) {
            if (dt_unpack(args[0].u.i, &y, &mo, &d, &h, &mi, &s) != 0)
                return vfail("integer overflow");
        } else return vfail("bad expression");
        return dt_format(y, mo, d, h, mi, s, out);
    }
    if (nlen == 4 && strncmp(name, "time", 4) == 0) {
        int i;
        int f[6];

        if (argc != 6) return vfail("bad expression");
        for (i = 0; i < 6; i++) {
            if (args[i].type != V_INT) return vfail("bad expression");
            f[i] = args[i].u.i;
        }
        rc = dt_pack(f[0], f[1], f[2], f[3], f[4], f[5], &secs);
        if (rc == -2) return vfail("integer overflow");
        if (rc != 0) return vfail("bad expression");
        out->i = secs;
        return 1;
    }
    return 0;
}

/* A script function cannot run in interrupt context: the interpreter is
 * one thread of static state.  The handler below only marks the slot.
 * wait() calls the named function afterwards, where a script may do
 * anything it may do between commands. */
#define SHELL_BIND_MAX  8
enum { BIND_NONE = 0, BIND_TIMER = 1, BIND_PIN = 2 };

typedef struct {
    uint8_t kind;
    volatile uint8_t pending;
    int source;
    char name[VAR_NAME];
} shell_bind_t;

static shell_bind_t s_bind[SHELL_BIND_MAX];

static void KEXT shell_irq_kick(int source, void *arg)
{
    shell_bind_t *b = arg;

    (void)source;
    if (b) b->pending = 1;
}

static void KEXT bind_clear(shell_bind_t *b)
{
    b->kind = BIND_NONE;
    b->pending = 0;
    b->source = 0;
    b->name[0] = '\0';
}

static void KEXT bind_drop(int kind, int source)
{
    int i;

    for (i = 0; i < SHELL_BIND_MAX; i++)
        if (s_bind[i].kind == (uint8_t)kind && s_bind[i].source == source)
            bind_clear(&s_bind[i]);
}

/* The slot for this source, or a free one.  NULL when the table is full
 * or the name is not a function name. */
static shell_bind_t *KEXT bind_take(int kind, int source, const char *name)
{
    shell_bind_t *same = NULL, *gap = NULL;
    int i, n;

    n = (int)strlen(name);
    if (n <= 0 || n >= VAR_NAME) return NULL;
    for (i = 0; i < SHELL_BIND_MAX; i++) {
        if (s_bind[i].kind == (uint8_t)kind && s_bind[i].source == source)
            same = &s_bind[i];
        else if (!s_bind[i].kind && !gap)
            gap = &s_bind[i];
    }
    if (!same) same = gap;
    if (!same) return NULL;
    same->kind = (uint8_t)kind;
    same->source = source;
    same->pending = 0;
    memcpy(same->name, name, (size_t)n + 1U);
    return same;
}

/* Call every function whose source has fired since the last wait.
 * 1 when at least one ran, 0 when none were pending, -1 on error. */
static int KEXT shell_irq_run(void)
{
    int i, any = 0;

    for (i = 0; i < SHELL_BIND_MAX; i++) {
        shell_bind_t *b = &s_bind[i];
        shell_fn_t *slot;
        fn_arg_t arg;
        val_t out;
        uint32_t n;

        memset(&out, 0, sizeof out);
        if (!b->kind || !b->pending) continue;
        b->pending = 0;
        any = 1;
        slot = fn_slot(b->name, (int)strlen(b->name), 0);
        if (!slot) {
            kprintf("%s: no such function: %s\r\n",
                    b->kind == BIND_TIMER ? "timer" : "irq", b->name);
            return -1;
        }
        n = (b->kind == BIND_TIMER) ? timer_count(b->source)
                                    : gpio_irq_count(b->source);
        if (n > 2147483647u) return vfail("integer overflow");
        arg.type = V_INT;
        arg.u.i = (int32_t)n;
        if (fn_invoke(slot->body, &arg, 1, &out) != 0) {
            val_drop(&out);
            rets_drop();
            s_bare_call = 0;
            return -1;
        }
        val_drop(&out);
        rets_drop();
        s_bare_call = 0;
    }
    return any;
}

/* 0 an event was delivered, -1 timed out or Ctrl-C.  Zero waits until
 * one of those, the same rule as irq_wait(). */
static int KEXT shell_wait(uint32_t ms, int *hit)
{
    uint32_t done = 0;

    for (;;) {
        int ran;

        if (script_interrupted()) return -1;
        if (sh_pump() < 0) return -1;
        ran = shell_irq_run();
        if (ran < 0) return -1;
        if (ran > 0) { *hit = 1; return 0; }
        if (ms && done >= ms) { *hit = 0; return 0; }
        {
            uint32_t step = SLEEP_SLICE_MS;

            if (ms && ms - done < step) step = ms - done;
            sys_delay_ms(step);
            s_now_add(step);
            done += step;
        }
    }
}

static int KEXT named_fn(const char *s)
{
    int n = (int)strlen(s);

    if (n <= 0 || n >= VAR_NAME || !fn_slot(s, n, 0)) {
        kprintf("%s: no such function: %s\r\n", s_vwho, s);
        return -1;
    }
    return 0;
}

static int KEXT timer_builtin(fn_arg_t *args, int argc, val_t *out)
{
    shell_bind_t *b = NULL;
    uint32_t us;
    int flags = 0, handle, rc;

    if (argc < 1 || argc > 3) return vfail("bad expression");
    if (args[0].type != V_INT || args[0].u.i <= 0) return vfail("bad expression");
    us = (uint32_t)args[0].u.i;
    if (argc >= 2) {
        if (args[1].type != V_INT || (args[1].u.i != 0 && args[1].u.i != 1))
            return vfail("bad expression");
        flags = (int)args[1].u.i;
    }
    if (argc == 3) {
        if (args[2].type != V_STR) return vfail("bad expression");
        if (named_fn(args[2].u.s) != 0) return -1;
        b = bind_take(BIND_TIMER, -1, args[2].u.s);
        if (!b) return vfail("too many interrupts");
    }
    handle = timer_open(us, flags, b ? shell_irq_kick : NULL, b);
    if (handle < 0) {
        if (b) bind_clear(b);
        return pin_fail("timer", handle);
    }
    if (b) b->source = handle;
    rc = timer_start(handle);
    if (rc != 0) {
        timer_close(handle);
        if (b) bind_drop(BIND_TIMER, handle);
        return pin_fail("timer", rc);
    }
    out->i = handle;
    return 1;
}

static int KEXT timer_handle(fn_arg_t *args, int argc, int which, val_t *out)
{
    int handle, rc;

    if (which == 0) {                       /* tperiod(handle, us) */
        uint32_t us;

        if (argc != 2 || args[0].type != V_INT || args[1].type != V_INT ||
            args[1].u.i <= 0)
            return vfail("bad expression");
        us = (uint32_t)args[1].u.i;
        rc = timer_period((int)args[0].u.i, us);
        if (rc != 0) return pin_fail("timer", rc);
        return 1;
    }
    if (argc != 1 || args[0].type != V_INT) return vfail("bad expression");
    handle = (int)args[0].u.i;
    if (which == 1) {                       /* tcount */
        uint32_t n;

        if (!timer_is_open(handle)) return pin_fail("timer", FREYA_ERR_ARG);
        n = timer_count(handle);
        if (n > 2147483647u) return vfail("integer overflow");
        out->i = (int32_t)n;
        return 1;
    }
    if (which == 2) rc = timer_start(handle);
    else if (which == 3) rc = timer_stop(handle);
    else {
        rc = timer_close(handle);
        if (rc == 0) bind_drop(BIND_TIMER, handle);
    }
    if (rc != 0) return pin_fail("timer", rc);
    return 1;
}

static int KEXT irq_builtin(fn_arg_t *args, int argc, val_t *out)
{
    shell_bind_t *b = NULL;
    int pin, edge, rc;

    if (argc < 1 || argc > 3 || args[0].type != V_STR) return vfail("bad expression");
    pin = parse_pin(args[0].u.s);
    if (pin < 0) return pin_fail("irq", FREYA_ERR_PIN);
    if (argc == 1) {
        uint32_t n = gpio_irq_count(pin);
        if (n > 2147483647u) return vfail("integer overflow");
        out->i = (int32_t)n;
        return 1;
    }
    if (args[1].type != V_INT) return vfail("bad expression");
    edge = (int)args[1].u.i;
    if (argc == 2 && edge == 0) {
        rc = gpio_irq_detach(pin);
        if (rc != 0) return pin_fail("irq", rc);
        bind_drop(BIND_PIN, pin);
        return 1;
    }
    if (argc == 3) {
        if (args[2].type != V_STR) return vfail("bad expression");
        if (named_fn(args[2].u.s) != 0) return -1;
        b = bind_take(BIND_PIN, pin, args[2].u.s);
        if (!b) return vfail("too many interrupts");
    } else {
        bind_drop(BIND_PIN, pin);
    }
    rc = gpio_irq_attach(pin, edge, b ? shell_irq_kick : NULL, b);
    if (rc != 0) {
        if (b) bind_clear(b);
        return pin_fail("irq", rc);
    }
    return 1;
}

/* 1 handled, 0 not a built-in, -1 error (already printed).
 * int/float/str/hex convert.  rand() and srand(seed) are the ANSI C
 * generator.  sin and cos take one number, and pi() is the constant.
 * now() is the clock as seconds since 1970.  date() is that clock as
 * text, and time() builds the seconds from six calendar fields.
 * year, month, day, hour, minute and second read one field.
 * get(pin) reads a pin.  set(pin, 0|1) drives it and reads it back.
 * adc(pin|temp|vref) is one raw count.  pwm(pin, hz, duty)
 * starts a channel and returns the rate; pwm(pin) stops it and returns 0.
 * ticks() is milliseconds since boot.  timer() arms a hardware timer,
 * irq() arms a pin edge, and wait() runs the script function either one
 * named, then returns 0.  A timeout returns -1. */
static void KEXT cell_from_arg(cell_t *c, const fn_arg_t *a)
{
    memset(c, 0, sizeof *c);
    c->type = a->type;
    if (type_hold_i(a->type)) c->u.i = a->u.i;
    else if (a->type == V_FLT) c->u.f = a->u.f;
    else if (a->type == V_STR) memcpy(c->u.s, a->u.s, VAR_STR);
}

/* Least or greatest element.  The order is the one dict keys use. */
static int KEXT array_extreme(fn_arg_t *args, int argc, int want_max, val_t *out)
{
    coll_t *c;
    int best, i;

    if (argc != 1 || args[0].type != V_ARR) return vfail("bad expression");
    c = &s_coll[args[0].u.i];
    if (!c->n) return vfail("empty array");
    best = 0;
    for (i = 1; i < (int)c->n; i++) {
        int cmp = cell_cmp(&c->a[i], &c->a[best]);
        if (want_max ? cmp > 0 : cmp < 0) best = i;
    }
    cell_to_val(&c->a[best], out);
    return 1;
}

/* The argument is already a copy.  Sort it and hand that copy back. */
static int KEXT array_sort(fn_arg_t *args, int argc, val_t *out)
{
    coll_t *c;
    int i, id;

    if (argc != 1 || args[0].type != V_ARR) return vfail("bad expression");
    id = (int)args[0].u.i;
    c = &s_coll[id];
    for (i = 1; i < (int)c->n; i++) {
        cell_t key = c->a[i];
        int j = i;

        while (j > 0 && cell_cmp(&c->a[j - 1], &key) > 0) {
            c->a[j] = c->a[j - 1];
            j--;
        }
        c->a[j] = key;
    }
    args[0].type = V_NONE;
    out->type = V_ARR;
    out->i = id;
    return 1;
}

static int KEXT coll_builtin(const char *name, int nlen, fn_arg_t *args,
                             int argc, val_t *out)
{
    int id, i;

    if (nlen == 3 && strncmp(name, "len", 3) == 0) {
        if (argc != 1 || !is_coll(args[0].type)) return vfail("bad expression");
        out->type = V_INT;
        out->i = s_coll[args[0].u.i].n;
        return 1;
    }
    if (nlen == 5 && strncmp(name, "array", 5) == 0) {
        if (argc > ARR_MAX) return vfail("array too long");
        id = coll_new(V_ARR);
        if (id < 0) return vfail("out of memory");
        for (i = 0; i < argc; i++) {
            cell_t e;
            cell_from_arg(&e, &args[i]);
            if (array_put(&s_coll[id], i, &e) != 0) {
                coll_free(id);
                return -1;
            }
        }
        out->type = V_ARR;
        out->i = id;
        return 1;
    }
    if (nlen == 4 && strncmp(name, "dict", 4) == 0) {
        if (argc & 1) return vfail("bad expression");
        id = coll_new(V_DICT);
        if (id < 0) return vfail("out of memory");
        for (i = 0; i < argc; i += 2) {
            cell_t k, v;
            cell_from_arg(&k, &args[i]);
            cell_from_arg(&v, &args[i + 1]);
            if (dict_put(&s_coll[id], &k, &v) != 0) {
                coll_free(id);
                return -1;
            }
        }
        out->type = V_DICT;
        out->i = id;
        return 1;
    }
    if (nlen == 3 && strncmp(name, "min", 3) == 0)
        return array_extreme(args, argc, 0, out);
    if (nlen == 3 && strncmp(name, "max", 3) == 0)
        return array_extreme(args, argc, 1, out);
    if (nlen == 4 && strncmp(name, "sort", 4) == 0)
        return array_sort(args, argc, out);
    return 0;
}

/* Lua's io library, as functions.  A handle is the integer open()
 * returned.  read() at the end of a file returns empty, which is this
 * language's nil.  A string result is at most 31 characters. */
static int KEXT file_fd(const fn_arg_t *a, int *fd)
{
    if (!a || a->type != V_INT || a->u.i < 0) return vfail("bad expression");
    *fd = (int)a->u.i;
    return 0;
}

static int KEXT file_mode(const char *m, int *flags)
{
    char buf[4];
    int n = 0;

    while (m[n] && n < (int)sizeof buf) {
        buf[n] = m[n];
        n++;
    }
    if (m[n]) return -1;
    if (n > 0 && buf[n - 1] == 'b') n--;
    buf[n] = '\0';
    if (strcmp(buf, "r") == 0)       *flags = FREYA_O_RDONLY;
    else if (strcmp(buf, "w") == 0)  *flags = FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC;
    else if (strcmp(buf, "a") == 0)  *flags = FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_APPEND;
    else if (strcmp(buf, "r+") == 0) *flags = FREYA_O_RDWR;
    else if (strcmp(buf, "w+") == 0) *flags = FREYA_O_RDWR | FREYA_O_CREATE | FREYA_O_TRUNC;
    else if (strcmp(buf, "a+") == 0) *flags = FREYA_O_RDWR | FREYA_O_CREATE | FREYA_O_APPEND;
    else return -1;
    return 0;
}

static int KEXT file_back(int fd, int32_t pos)
{
    if (fs_fd_seek(fd, pos, FREYA_SEEK_SET) != 0)
        return fs_fail("read", NULL, FAT_ERR_IO);
    return 0;
}

static int KEXT file_byte(int fd, int *ch)
{
    unsigned char c;
    int n = fs_fd_read(fd, &c, 1);

    if (n < 0) return fs_fail("read", NULL, n);
    *ch = (n == 0) ? -1 : (int)c;
    return 0;
}

static int KEXT file_unget(int fd)
{
    if (fs_fd_seek(fd, -1, FREYA_SEEK_CUR) != 0)
        return fs_fail("read", NULL, FAT_ERR_IO);
    return 0;
}

/* A line.  '\n' ends it, and a '\r' just before that is dropped.
 * keep stores the '\n'.  Nothing left is empty. */
static int KEXT file_line(int fd, int keep, val_t *out)
{
    int32_t start = fs_fd_tell(fd);
    char buf[VAR_STR];
    int n = 0, ch, saw = 0;

    if (start < 0) return fs_fail("read", NULL, start);
    for (;;) {
        if (file_byte(fd, &ch) != 0) return -1;
        if (ch < 0) break;
        saw = 1;
        if (ch == '\r') {
            int32_t at = fs_fd_tell(fd);
            int n2;

            if (at < 0) return fs_fail("read", NULL, at);
            if (file_byte(fd, &n2) != 0) return -1;
            if (n2 == '\n') ch = '\n';
            else if (n2 >= 0 && file_back(fd, at) != 0) return -1;
        }
        if (ch == '\n') {
            if (keep) {
                if (n >= VAR_STR - 1) {
                    if (file_back(fd, start) != 0) return -1;
                    return vfail("string too long");
                }
                buf[n++] = '\n';
            }
            break;
        }
        if (n >= VAR_STR - 1) {
            if (file_back(fd, start) != 0) return -1;
            return vfail("string too long");
        }
        buf[n++] = (char)ch;
    }
    if (!saw) {
        out->type = V_EMPTY;
        return 1;
    }
    buf[n] = '\0';
    out->type = V_STR;
    memcpy(out->s, buf, (size_t)n + 1U);
    return 1;
}

static int KEXT file_all(int fd, val_t *out)
{
    int32_t start = fs_fd_tell(fd);
    char buf[VAR_STR];
    int n = 0, ch;

    if (start < 0) return fs_fail("read", NULL, start);
    for (;;) {
        if (file_byte(fd, &ch) != 0) return -1;
        if (ch < 0) break;
        if (n >= VAR_STR - 1) {
            if (file_back(fd, start) != 0) return -1;
            return vfail("string too long");
        }
        buf[n++] = (char)ch;
    }
    buf[n] = '\0';
    out->type = V_STR;
    memcpy(out->s, buf, (size_t)n + 1U);
    return 1;
}

static int KEXT file_num(const char *s, val_t *out)
{
    int neg = 0, dot = 0, digits = 0;
    int32_t ip = 0, frac = 0, scale = 1;

    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }
    while (*s) {
        if (*s == '.' && !dot) {
            dot = 1;
            s++;
            continue;
        }
        if (*s < '0' || *s > '9') return -1;
        digits++;
        if (!dot) {
            int d = *s - '0';
            if (ip > (2147483647 - d) / 10) return -2;
            ip = ip * 10 + d;
        } else if (scale <= 100000000) {
            frac = frac * 10 + (*s - '0');
            scale *= 10;
        }
        s++;
    }
    if (!digits) return -1;
    if (dot) {
        float f = (float)ip + (float)frac / (float)scale;
        out->type = V_FLT;
        out->f = neg ? -f : f;
        return 0;
    }
    out->type = V_INT;
    out->i = neg ? -ip : ip;
    return 0;
}

static int KEXT file_read_num(int fd, val_t *out)
{
    char buf[VAR_STR];
    int n = 0, ch, saw = 0, rc;
    int32_t mark;

    for (;;) {
        if (file_byte(fd, &ch) != 0) return -1;
        if (ch < 0) {
            out->type = V_EMPTY;
            return 1;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            if (file_unget(fd) != 0) return -1;
            break;
        }
    }
    mark = fs_fd_tell(fd);
    if (mark < 0) return fs_fail("read", NULL, mark);
    if (file_byte(fd, &ch) != 0) return -1;
    if (ch < 0) {
        out->type = V_EMPTY;
        return 1;
    }
    if (ch == '+' || ch == '-') {
        buf[n++] = (char)ch;
        if (file_byte(fd, &ch) != 0) return -1;
    }
    while (ch >= '0' && ch <= '9') {
        saw = 1;
        if (n >= VAR_STR - 1) {
            if (file_back(fd, mark) != 0) return -1;
            return vfail("string too long");
        }
        buf[n++] = (char)ch;
        if (file_byte(fd, &ch) != 0) return -1;
    }
    if (ch == '.') {
        if (n >= VAR_STR - 1) {
            if (file_back(fd, mark) != 0) return -1;
            return vfail("string too long");
        }
        buf[n++] = '.';
        if (file_byte(fd, &ch) != 0) return -1;
        while (ch >= '0' && ch <= '9') {
            saw = 1;
            if (n >= VAR_STR - 1) {
                if (file_back(fd, mark) != 0) return -1;
                return vfail("string too long");
            }
            buf[n++] = (char)ch;
            if (file_byte(fd, &ch) != 0) return -1;
        }
    }
    if (ch >= 0 && file_unget(fd) != 0) return -1;
    if (!saw) {
        if (file_back(fd, mark) != 0) return -1;
        out->type = V_EMPTY;
        return 1;
    }
    buf[n] = '\0';
    rc = file_num(buf, out);
    if (rc == -2) return vfail("integer overflow");
    if (rc != 0) {
        if (file_back(fd, mark) != 0) return -1;
        out->type = V_EMPTY;
        return 1;
    }
    return 1;
}

static int KEXT file_read_n(int fd, int n, val_t *out)
{
    char buf[VAR_STR];
    int got;

    if (n == 0) {
        int32_t pos = fs_fd_tell(fd);
        int32_t sz = fs_fd_size(fd);

        if (pos < 0) return fs_fail("read", NULL, pos);
        if (sz >= 0 && pos >= sz) {
            out->type = V_EMPTY;
            return 1;
        }
        out->type = V_STR;
        out->s[0] = '\0';
        return 1;
    }
    got = fs_fd_read(fd, buf, n);
    if (got < 0) return fs_fail("read", NULL, got);
    if (got == 0) {
        out->type = V_EMPTY;
        return 1;
    }
    buf[got] = '\0';
    out->type = V_STR;
    memcpy(out->s, buf, (size_t)got + 1U);
    return 1;
}

static int KEXT file_open(fn_arg_t *args, int argc, val_t *out)
{
    int flags = FREYA_O_RDONLY, fd;

    if (!need_fs()) return -1;
    if (argc < 1 || argc > 2 || args[0].type != V_STR || args[0].u.s[0] == '\0')
        return vfail("bad expression");
    if (argc == 2) {
        if (args[1].type != V_STR || file_mode(args[1].u.s, &flags) != 0)
            return vfail("bad expression");
    }
    fd = fs_fd_open(args[0].u.s, flags);
    if (fd < 0) return fs_fail("open", args[0].u.s, fd);
    out->type = V_INT;
    out->i = fd;
    return 1;
}

static int KEXT file_read(fn_arg_t *args, int argc, val_t *out)
{
    int fd;

    if (!need_fs()) return -1;
    if (argc < 1 || argc > 2 || file_fd(&args[0], &fd) != 0) {
        if (argc < 1 || argc > 2) return vfail("bad expression");
        return -1;
    }
    if (argc == 1) return file_line(fd, 0, out);
    if (args[1].type == V_STR) {
        const char *f = args[1].u.s;
        if (strcmp(f, "*l") == 0) return file_line(fd, 0, out);
        if (strcmp(f, "*L") == 0) return file_line(fd, 1, out);
        if (strcmp(f, "*a") == 0) return file_all(fd, out);
        if (strcmp(f, "*n") == 0) return file_read_num(fd, out);
        return vfail("bad expression");
    }
    if (type_wide(args[1].type)) {
        int32_t n = args[1].u.i;
        if (n < 0 || n >= VAR_STR) return vfail("bad expression");
        return file_read_n(fd, (int)n, out);
    }
    return vfail("bad expression");
}

static int KEXT file_write(fn_arg_t *args, int argc, val_t *out)
{
    int fd, total = 0, i;

    if (!need_fs()) return -1;
    if (argc < 2) return vfail("bad expression");
    if (file_fd(&args[0], &fd) != 0) return -1;
    for (i = 1; i < argc; i++) {
        char text[VAR_STR];
        unsigned char b;
        const char *p;
        int len;

        if (args[i].type == V_BYTE) {
            b = (unsigned char)args[i].u.i;
            p = (const char *)&b;
            len = 1;
        } else if (args[i].type == V_STR) {
            p = args[i].u.s;
            len = (int)strlen(p);
        } else if (args[i].type == V_INT || args[i].type == V_FLT ||
                   args[i].type == V_BOOL) {
            val_t v;

            memset(&v, 0, sizeof v);
            v.type = args[i].type;
            if (args[i].type == V_FLT) v.f = args[i].u.f;
            else v.i = args[i].u.i;
            val_text(&v, text, (int)sizeof text);
            p = text;
            len = (int)strlen(text);
        } else {
            return vfail("bad expression");
        }
        if (len > 0 && fs_fd_write(fd, p, len) != len)
            return fs_fail("write", NULL, FAT_ERR_IO);
        total += len;
    }
    out->type = V_INT;
    out->i = total;
    return 1;
}

static int KEXT file_close(fn_arg_t *args, int argc, val_t *out)
{
    int fd, rc;

    if (!need_fs()) return -1;
    if (argc != 1) return vfail("bad expression");
    if (file_fd(&args[0], &fd) != 0) return -1;
    rc = fs_fd_close(fd);
    if (rc != 0) return fs_fail("close", NULL, rc);
    out->type = V_INT;
    out->i = 0;
    return 1;
}

static int KEXT file_seek(fn_arg_t *args, int argc, val_t *out)
{
    int fd, whence = FREYA_SEEK_CUR;
    int32_t off = 0, pos;

    if (!need_fs()) return -1;
    if (argc < 1 || argc > 3) return vfail("bad expression");
    if (file_fd(&args[0], &fd) != 0) return -1;
    if (argc == 1) {
        pos = fs_fd_tell(fd);
        if (pos < 0) return fs_fail("seek", NULL, pos);
        out->type = V_INT;
        out->i = pos;
        return 1;
    }
    if (args[1].type != V_STR) return vfail("bad expression");
    if (strcmp(args[1].u.s, "set") == 0) whence = FREYA_SEEK_SET;
    else if (strcmp(args[1].u.s, "cur") == 0) whence = FREYA_SEEK_CUR;
    else if (strcmp(args[1].u.s, "end") == 0) whence = FREYA_SEEK_END;
    else return vfail("bad expression");
    if (argc == 3) {
        if (!type_wide(args[2].type)) return vfail("bad expression");
        off = args[2].u.i;
    }
    if (fs_fd_seek(fd, off, whence) != 0)
        return fs_fail("seek", NULL, FAT_ERR_INVAL);
    pos = fs_fd_tell(fd);
    if (pos < 0) return fs_fail("seek", NULL, pos);
    out->type = V_INT;
    out->i = pos;
    return 1;
}

static int KEXT file_flush(fn_arg_t *args, int argc, val_t *out)
{
    int fd;

    if (!need_fs()) return -1;
    if (argc != 1) return vfail("bad expression");
    if (file_fd(&args[0], &fd) != 0) return -1;
    if (fs_fd_tell(fd) < 0) return fs_fail("flush", NULL, FAT_ERR_INVAL);
    if (fat_sync() != FAT_OK) return fs_fail("flush", NULL, FAT_ERR_IO);
    out->type = V_INT;
    out->i = 0;
    return 1;
}

static int KEXT file_builtin(const char *name, int nlen, fn_arg_t *args,
                             int argc, val_t *out)
{
    if (nlen == 4 && strncmp(name, "open", 4) == 0)
        return file_open(args, argc, out);
    if (nlen == 4 && strncmp(name, "read", 4) == 0)
        return file_read(args, argc, out);
    if (nlen == 5 && strncmp(name, "write", 5) == 0)
        return file_write(args, argc, out);
    if (nlen == 5 && strncmp(name, "close", 5) == 0)
        return file_close(args, argc, out);
    if (nlen == 4 && strncmp(name, "seek", 4) == 0)
        return file_seek(args, argc, out);
    if (nlen == 5 && strncmp(name, "flush", 5) == 0)
        return file_flush(args, argc, out);
    return 0;
}

/*
 * Lua patterns.  A subject is a string of at most 31 characters, so the
 * matcher stays small: classes, sets, * + - ?, ^ $, captures and %b.
 * There is no alternation.  The search does not call back into the
 * shell, so the match state can sit in one place.
 */
#define PAT_CAP     9
#define PAT_DEPTH   32
#define PAT_STEPS   4096
#define CAP_OPEN    (-1)
#define CAP_POS     (-2)

typedef struct {
    const char *src_init;
    const char *src_end;
    const char *p_end;
    const char *err;
    int depth;
    int steps;
    int level;
    struct {
        const char *init;
        int len;
    } cap[PAT_CAP];
} pat_t;

static int KEXT pat_letter(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int KEXT pat_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int KEXT pat_class(int c, int cl)
{
    int low = (cl >= 'A' && cl <= 'Z') ? cl + 32 : cl;
    int res;

    switch (low) {
    case 'a': res = pat_letter(c); break;
    case 'c': res = c < 32 || c == 127; break;
    case 'd': res = pat_digit(c); break;
    case 'g': res = c > 32 && c < 127; break;
    case 'l': res = c >= 'a' && c <= 'z'; break;
    case 'p': res = c > 32 && c < 127 && !pat_letter(c) && !pat_digit(c); break;
    case 's': res = c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                    c == '\v' || c == '\f'; break;
    case 'u': res = c >= 'A' && c <= 'Z'; break;
    case 'w': res = pat_letter(c) || pat_digit(c); break;
    case 'x': res = pat_digit(c) || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F'); break;
    case 'z': res = c == 0; break;
    default:  return cl == c;
    }
    return (low == cl) ? res : !res;
}

static int KEXT pat_set(int c, const char *p, const char *ec)
{
    int sig = 1;

    if (*(p + 1) == '^') { sig = 0; p++; }
    while (++p < ec) {
        if (*p == '%') {
            p++;
            if (pat_class(c, (unsigned char)*p)) return sig;
        } else if (*(p + 1) == '-' && p + 2 < ec) {
            p += 2;
            if ((unsigned char)*(p - 2) <= c && c <= (unsigned char)*p)
                return sig;
        } else if ((unsigned char)*p == c) return sig;
    }
    return !sig;
}

static const char *KEXT pat_classend(pat_t *ms, const char *p)
{
    switch (*p++) {
    case '%':
        if (p == ms->p_end) { ms->err = "bad pattern"; return NULL; }
        return p + 1;
    case '[':
        if (*p == '^') p++;
        do {
            if (p == ms->p_end) { ms->err = "bad pattern"; return NULL; }
            if (*p++ == '%' && p < ms->p_end) p++;
        } while (*p != ']');
        return p + 1;
    default:
        return p;
    }
}

static int KEXT pat_one(pat_t *ms, const char *s, const char *p, const char *ep)
{
    int c;

    if (s >= ms->src_end) return 0;
    c = (unsigned char)*s;
    switch (*p) {
    case '.': return 1;
    case '%': return pat_class(c, (unsigned char)*(p + 1));
    case '[': return pat_set(c, p, ep - 1);
    default:  return (unsigned char)*p == c;
    }
}

static const char *KEXT pat_match(pat_t *ms, const char *s, const char *p);

static const char *KEXT pat_max(pat_t *ms, const char *s,
                                const char *p, const char *ep)
{
    int i = 0;
    const char *res;

    while (pat_one(ms, s + i, p, ep)) i++;
    while (i >= 0) {
        res = pat_match(ms, s + i, ep + 1);
        if (res || ms->err) return res;
        i--;
    }
    return NULL;
}

static const char *KEXT pat_min(pat_t *ms, const char *s,
                                const char *p, const char *ep)
{
    for (;;) {
        const char *res = pat_match(ms, s, ep + 1);
        if (res || ms->err) return res;
        if (!pat_one(ms, s, p, ep)) return NULL;
        s++;
    }
}

static const char *KEXT pat_balance(pat_t *ms, const char *s, const char *p)
{
    int b, e, cont;

    if (p >= ms->p_end || p + 1 >= ms->p_end) {
        ms->err = "bad pattern";
        return NULL;
    }
    if (s >= ms->src_end || *s != *p) return NULL;
    b = (unsigned char)*p;
    e = (unsigned char)*(p + 1);
    cont = 1;
    while (++s < ms->src_end) {
        if ((unsigned char)*s == e) {
            if (--cont == 0) return s + 1;
        } else if ((unsigned char)*s == b) cont++;
    }
    return NULL;
}

static const char *KEXT pat_start(pat_t *ms, const char *s,
                                  const char *p, int what)
{
    const char *res;
    int level = ms->level;

    if (level >= PAT_CAP) { ms->err = "bad pattern"; return NULL; }
    ms->cap[level].init = s;
    ms->cap[level].len = what;
    ms->level = level + 1;
    res = pat_match(ms, s, p);
    if (!res) ms->level = level;
    return res;
}

static const char *KEXT pat_end(pat_t *ms, const char *s, const char *p)
{
    const char *res;
    int level = ms->level;

    for (level--; level >= 0; level--)
        if (ms->cap[level].len == CAP_OPEN) break;
    if (level < 0) { ms->err = "bad pattern"; return NULL; }
    ms->cap[level].len = (int)(s - ms->cap[level].init);
    res = pat_match(ms, s, p);
    if (!res) ms->cap[level].len = CAP_OPEN;
    return res;
}

static const char *KEXT pat_back(pat_t *ms, const char *s, int which)
{
    int idx = which - '1';
    int len;

    if (idx < 0 || idx >= ms->level || ms->cap[idx].len == CAP_OPEN ||
        ms->cap[idx].len == CAP_POS) {
        ms->err = "bad pattern";
        return NULL;
    }
    len = ms->cap[idx].len;
    if ((int)(ms->src_end - s) >= len &&
        memcmp(ms->cap[idx].init, s, (size_t)len) == 0)
        return s + len;
    return NULL;
}

static const char *KEXT pat_match(pat_t *ms, const char *s, const char *p)
{
    if (ms->err) return NULL;
    if (--ms->depth < 0 || ++ms->steps > PAT_STEPS) {
        ms->err = "pattern too complex";
        return NULL;
    }
    for (;;) {
        const char *ep, *res;

        if (p == ms->p_end) { ms->depth++; return s; }
        switch (*p) {
        case '(':
            res = (*(p + 1) == ')') ? pat_start(ms, s, p + 2, CAP_POS)
                                    : pat_start(ms, s, p + 1, CAP_OPEN);
            ms->depth++;
            return res;
        case ')':
            res = pat_end(ms, s, p + 1);
            ms->depth++;
            return res;
        case '$':
            if (p + 1 != ms->p_end) break;
            ms->depth++;
            return (s == ms->src_end) ? s : NULL;
        case '%':
            if (p + 1 < ms->p_end && *(p + 1) == 'b') {
                s = pat_balance(ms, s, p + 2);
                if (!s) { ms->depth++; return NULL; }
                p += 4;
                continue;
            }
            if (p + 1 < ms->p_end && *(p + 1) >= '0' && *(p + 1) <= '9') {
                if (*(p + 1) == '0') {
                    ms->err = "bad pattern";
                    ms->depth++;
                    return NULL;
                }
                s = pat_back(ms, s, (unsigned char)*(p + 1));
                if (!s) { ms->depth++; return NULL; }
                p += 2;
                continue;
            }
            break;
        default:
            break;
        }
        ep = pat_classend(ms, p);
        if (!ep) { ms->depth++; return NULL; }
        if (!pat_one(ms, s, p, ep)) {
            if (*ep == '*' || *ep == '?' || *ep == '-') {
                p = ep + 1;
                continue;
            }
            ms->depth++;
            return NULL;
        }
        switch (*ep) {
        case '?':
            res = pat_match(ms, s + 1, ep + 1);
            if (res || ms->err) { ms->depth++; return res; }
            p = ep + 1;
            continue;
        case '+':
            s++;
            /* FALLTHROUGH */
        case '*':
            res = pat_max(ms, s, p, ep);
            ms->depth++;
            return res;
        case '-':
            res = pat_min(ms, s, p, ep);
            ms->depth++;
            return res;
        default:
            s++;
            p = ep;
            continue;
        }
    }
}

static int KEXT pat_search(pat_t *ms, const char *src, const char *p, int init,
                           const char **from, const char **to)
{
    const char *s, *e;
    int anchor = 0;

    ms->src_init = src;
    ms->src_end = src + strlen(src);
    ms->err = NULL;
    if (*p == '^') { anchor = 1; p++; }
    ms->p_end = p + strlen(p);
    s = src + (init - 1);
    if (s > ms->src_end) return 0;
    do {
        ms->level = 0;
        ms->depth = PAT_DEPTH;
        ms->steps = 0;
        e = pat_match(ms, s, p);
        if (ms->err) return -1;
        if (e) {
            int i;
            for (i = 0; i < ms->level; i++)
                if (ms->cap[i].len == CAP_OPEN) {
                    ms->err = "bad pattern";
                    return -1;
                }
            *from = s;
            *to = e;
            return 1;
        }
    } while (!anchor && s++ < ms->src_end);
    return 0;
}

static void KEXT pat_ret_int(fn_arg_t *a, int v)
{
    memset(a, 0, sizeof *a);
    a->type = V_INT;
    a->u.i = v;
}

static void KEXT pat_ret_str(fn_arg_t *a, const char *s, int n)
{
    memset(a, 0, sizeof *a);
    a->type = V_STR;
    memcpy(a->u.s, s, (size_t)n);
    a->u.s[n] = '\0';
}

static int KEXT pat_none(val_t *out)
{
    out->type = V_NIL;
    return 1;
}

static int KEXT pat_one_str(val_t *out, const char *s, int n)
{
    if (n >= VAR_STR) return vfail("string too long");
    out->type = V_STR;
    memcpy(out->s, s, (size_t)n);
    out->s[n] = '\0';
    return 1;
}

/* Captures, and for find the span in front of them.  One value is left
 * in out.  Several are left in s_fn_retv and reported as 2. */
static int KEXT pat_results(pat_t *ms, const char *m0, const char *m1,
                            int span, val_t *out)
{
    int n = 0, i;

    if (span) {
        pat_ret_int(&s_fn_retv[n++], (int)(m0 - ms->src_init) + 1);
        pat_ret_int(&s_fn_retv[n++], (int)(m1 - ms->src_init));
    }
    if (!span && ms->level == 0)
        return pat_one_str(out, m0, (int)(m1 - m0));
    for (i = 0; i < ms->level; i++) {
        int len = ms->cap[i].len;

        if (len == CAP_OPEN) return vfail("bad pattern");
        if (n >= FN_ARGS) return vfail("too many values");
        if (len == CAP_POS)
            pat_ret_int(&s_fn_retv[n], (int)(ms->cap[i].init - ms->src_init) + 1);
        else {
            if (len >= VAR_STR) return vfail("string too long");
            pat_ret_str(&s_fn_retv[n], ms->cap[i].init, len);
        }
        n++;
    }
    if (n == 1) {
        if (s_fn_retv[0].type == V_INT) {
            out->type = V_INT;
            out->i = s_fn_retv[0].u.i;
        } else return pat_one_str(out, s_fn_retv[0].u.s, (int)strlen(s_fn_retv[0].u.s));
        return 1;
    }
    s_fn_nret = n;
    return 2;
}

static int KEXT pat_where(const fn_arg_t *a, int len, int *ip)
{
    int pos;

    if (a->type != V_INT) return -1;
    pos = a->u.i;
    if (pos > 0) { /* already 1-based */ }
    else if (pos == 0 || pos < -len) pos = 1;
    else pos = len + pos + 1;
    if (pos < 1) pos = 1;
    *ip = pos;
    return 0;
}

static int KEXT pat_app(char *buf, int *o, const char *s, int n)
{
    if (n < 0 || *o + n >= VAR_STR) return -1;
    if (n) memcpy(buf + *o, s, (size_t)n);
    *o += n;
    buf[*o] = '\0';
    return 0;
}

static int KEXT pat_subst(pat_t *ms, char *buf, int *o, const char *repl,
                          const char *m0, const char *m1)
{
    while (*repl) {
        if (*repl != '%') {
            if (pat_app(buf, o, repl, 1) != 0) return -1;
            repl++;
            continue;
        }
        repl++;
        if (*repl == '%') {
            if (pat_app(buf, o, repl, 1) != 0) return -1;
            repl++;
        } else if (*repl == '0') {
            if (pat_app(buf, o, m0, (int)(m1 - m0)) != 0) return -1;
            repl++;
        } else if (*repl >= '1' && *repl <= '9') {
            int idx = *repl - '1';
            int len;

            if (idx >= ms->level || ms->cap[idx].len == CAP_OPEN) {
                ms->err = "bad pattern";
                return -1;
            }
            repl++;
            if (ms->cap[idx].len == CAP_POS) {
                char num[12];
                int k, pos = (int)(ms->cap[idx].init - ms->src_init) + 1;

                k = ksnprintf(num, (int)sizeof num, "%d", pos);
                if (k < 0 || pat_app(buf, o, num, k) != 0) return -1;
            } else {
                len = ms->cap[idx].len;
                if (pat_app(buf, o, ms->cap[idx].init, len) != 0) return -1;
            }
        } else {
            ms->err = "bad pattern";
            return -1;
        }
    }
    return 0;
}

static int KEXT pat_gsub(pat_t *ms, const char *src, const char *p,
                         const char *repl, int maxn, val_t *out)
{
    char buf[VAR_STR];
    const char *sp = src;
    int anchor = 0, n = 0, o = 0;

    memset(ms, 0, sizeof *ms);
    ms->src_init = src;
    ms->src_end = src + strlen(src);
    if (*p == '^') { anchor = 1; p++; }
    ms->p_end = p + strlen(p);
    buf[0] = '\0';
    while (n < maxn) {
        const char *e;

        ms->level = 0;
        ms->depth = PAT_DEPTH;
        ms->steps = 0;
        e = pat_match(ms, sp, p);
        if (ms->err) return vfail(ms->err);
        if (e) {
            int i;
            for (i = 0; i < ms->level; i++)
                if (ms->cap[i].len == CAP_OPEN) return vfail("bad pattern");
            n++;
            if (pat_subst(ms, buf, &o, repl, sp, e) != 0) {
                if (ms->err) return vfail(ms->err);
                return vfail("string too long");
            }
        }
        if (e && e > sp) sp = e;
        else if (sp < ms->src_end) {
            if (pat_app(buf, &o, sp, 1) != 0) return vfail("string too long");
            sp++;
        } else break;
        if (anchor) break;
    }
    if (pat_app(buf, &o, sp, (int)(ms->src_end - sp)) != 0)
        return vfail("string too long");
    pat_ret_str(&s_fn_retv[0], buf, o);
    pat_ret_int(&s_fn_retv[1], n);
    s_fn_nret = 2;
    return 2;
}

static int KEXT pat_builtin(const char *name, int nlen, fn_arg_t *args,
                            int argc, val_t *out)
{
    pat_t ms;
    const char *from, *to, *src, *pat;
    int init = 1, rc, kind;

    if (nlen == 5 && strncmp(name, "match", 5) == 0) kind = 1;
    else if (nlen == 4 && strncmp(name, "find", 4) == 0) kind = 2;
    else if (nlen == 4 && strncmp(name, "gsub", 4) == 0) kind = 3;
    else return 0;

    if (kind == 3) {
        int maxn = 32;

        if ((argc != 3 && argc != 4) || args[0].type != V_STR ||
            args[1].type != V_STR || args[2].type != V_STR)
            return vfail("bad expression");
        if (argc == 4) {
            if (args[3].type != V_INT || args[3].u.i < 0)
                return vfail("bad expression");
            maxn = args[3].u.i;
        }
        return pat_gsub(&ms, args[0].u.s, args[1].u.s, args[2].u.s, maxn, out);
    }
    if (argc < 2 || argc > (kind == 2 ? 4 : 3) ||
        args[0].type != V_STR || args[1].type != V_STR)
        return vfail("bad expression");
    src = args[0].u.s;
    pat = args[1].u.s;
    if (argc >= 3 && pat_where(&args[2], (int)strlen(src), &init) != 0)
        return vfail("bad expression");
    if (kind == 2 && argc == 4) {
        int i, sl, pl;

        if (args[3].type != V_BOOL) return vfail("bad expression");
        if (!args[3].u.i)
            goto patterned;
        sl = (int)strlen(src);
        pl = (int)strlen(pat);
        if (init > sl + 1) return pat_none(out);
        if (pl == 0) {
            pat_ret_int(&s_fn_retv[0], init);
            pat_ret_int(&s_fn_retv[1], init - 1);
            s_fn_nret = 2;
            return 2;
        }
        for (i = init - 1; i + pl <= sl; i++) {
            if (memcmp(src + i, pat, (size_t)pl) == 0) {
                pat_ret_int(&s_fn_retv[0], i + 1);
                pat_ret_int(&s_fn_retv[1], i + pl);
                s_fn_nret = 2;
                return 2;
            }
        }
        return pat_none(out);
    }
patterned:
    memset(&ms, 0, sizeof ms);
    rc = pat_search(&ms, src, pat, init, &from, &to);
    if (rc < 0) return vfail(ms.err ? ms.err : "bad pattern");
    if (rc == 0) return pat_none(out);
    return pat_results(&ms, from, to, kind == 2, out);
}

static int KEXT sh_spawn(fn_arg_t *args, int argc, val_t *out);
static int KEXT sh_join(fn_arg_t *args, int argc, val_t *out);
static int KEXT sh_yield_fn(fn_arg_t *args, int argc, val_t *out);

static int KEXT fn_builtin(const char *name, int nlen, fn_arg_t *args,
                           int argc, val_t *out)
{
    const char *ps;
    int pin, rc;

    out->type = V_INT;
    out->i = 0;
    out->f = 0.f;
    out->s[0] = '\0';

    if ((rc = coll_builtin(name, nlen, args, argc, out)) != 0) return rc;
    if ((rc = dt_builtin(name, nlen, args, argc, out)) != 0) return rc;

    if ((nlen == 3 && strncmp(name, "int", 3) == 0) ||
        (nlen == 5 && strncmp(name, "float", 5) == 0) ||
        (nlen == 4 && strncmp(name, "byte", 4) == 0) ||
        (nlen == 4 && strncmp(name, "bool", 4) == 0) ||
        (nlen == 3 && strncmp(name, "str", 3) == 0) ||
        (nlen == 3 && strncmp(name, "hex", 3) == 0)) {
        if (argc != 1) return vfail("bad expression");
        if (name[0] == 'i') return conv_int(&args[0], out);
        if (name[0] == 'f') return conv_float(&args[0], out);
        if (nlen == 4 && name[0] == 'b' && name[1] == 'o')
            return conv_bool(&args[0], out);
        if (name[0] == 'b') return conv_byte(&args[0], out);
        if (name[0] == 's') return conv_str(&args[0], out);
        return conv_hex(&args[0], out);
    }

    if (nlen == 4 && strncmp(name, "true", 4) == 0) {
        if (argc != 0) return vfail("bad expression");
        out->type = V_BOOL;
        out->i = 1;
        return 1;
    }
    if (nlen == 5 && strncmp(name, "false", 5) == 0) {
        if (argc != 0) return vfail("bad expression");
        out->type = V_BOOL;
        out->i = 0;
        return 1;
    }
    if (nlen == 5 && strncmp(name, "empty", 5) == 0) {
        if (argc != 0) return vfail("bad expression");
        out->type = V_EMPTY;
        return 1;
    }
    if (nlen == 4 && strncmp(name, "none", 4) == 0) {
        if (argc != 0) return vfail("bad expression");
        out->type = V_NIL;
        return 1;
    }

    if (nlen == 4 && strncmp(name, "rand", 4) == 0) {
        if (argc != 0) return vfail("bad expression");
        out->i = rand_step();
        return 1;
    }
    if (nlen == 5 && strncmp(name, "srand", 5) == 0) {
        if (argc != 1 || args[0].type != V_INT) return vfail("bad expression");
        s_rand_next = (uint32_t)args[0].u.i;
        return 1;
    }

    if (nlen == 2 && strncmp(name, "pi", 2) == 0) {
        if (argc != 0) return vfail("bad expression");
        out->type = V_FLT;
        out->f = shell_pi();
        return 1;
    }
    if (nlen == 3 && (strncmp(name, "sin", 3) == 0 || strncmp(name, "cos", 3) == 0)) {
        if (argc != 1) return vfail("bad expression");
        return conv_sincos(&args[0], name[0] == 'c', out);
    }

    if (nlen == 3 && strncmp(name, "get", 3) == 0) {
        if (argc != 1 || args[0].type != V_STR) return vfail("bad expression");
        pin = parse_pin(args[0].u.s);
        if (pin < 0) return pin_fail("get", FREYA_ERR_PIN);
        rc = gpio_pin_read(pin);
        if (rc < 0) return pin_fail("get", rc);
        out->i = rc;
        return 1;
    }
    if (nlen == 3 && strncmp(name, "set", 3) == 0) {
        if (argc != 2 || args[0].type != V_STR || args[1].type != V_INT ||
            (args[1].u.i != 0 && args[1].u.i != 1))
            return vfail("bad expression");
        pin = parse_pin(args[0].u.s);
        if (pin < 0) return pin_fail("set", FREYA_ERR_PIN);
        rc = gpio_pin_mode(pin, FREYA_PIN_OUT);
        if (rc != 0) return pin_fail("set", rc);
        rc = gpio_pin_write(pin, (int)args[1].u.i);
        if (rc != 0) return pin_fail("set", rc);
        rc = gpio_pin_read(pin);
        if (rc < 0) return pin_fail("set", rc);
        out->i = rc;
        return 1;
    }
    if (nlen == 3 && strncmp(name, "adc", 3) == 0) {
        int source;

        if (argc != 1 || args[0].type != V_STR) return vfail("bad expression");
        ps = args[0].u.s;
        if (strcmp(ps, "temp") == 0) source = FREYA_ADC_TEMP;
        else if (strcmp(ps, "vref") == 0) source = FREYA_ADC_VREF;
        else source = parse_pin(ps);
        if (source < 0) return pin_fail("adc", FREYA_ERR_PIN);
        rc = adc_read(source);
        if (rc < 0) return pin_fail("adc", rc);
        out->i = rc;
        return 1;
    }
    if (nlen == 3 && strncmp(name, "pwm", 3) == 0) {
        uint32_t hz, duty;
        int ch;

        if ((argc != 1 && argc != 3) || args[0].type != V_STR)
            return vfail("bad expression");
        pin = parse_pin(args[0].u.s);
        ch = (pin < 0) ? FREYA_ERR_PIN : pwm_lookup(pin);
        if (ch < 0) return pin_fail("pwm", ch);
        if (argc == 1) {
            if (pwm_close(ch) != 0) {
                kprintf("pwm: is not running\r\n");
                return -1;
            }
            return 1;
        }
        if (args[1].type != V_INT || args[1].u.i <= 0) return vfail("bad expression");
        hz = (uint32_t)args[1].u.i;
        if (args[2].type == V_INT) {
            if (args[2].u.i < 0 || args[2].u.i > 100) return vfail("bad expression");
            duty = (uint32_t)args[2].u.i * 100U;
        } else if (args[2].type == V_FLT) {
            float d = args[2].u.f;
            if (d < 0.f || d > 100.f) return vfail("bad expression");
            duty = (uint32_t)(d * 100.f + 0.5f);
            if (duty > FREYA_PWM_FULL) duty = FREYA_PWM_FULL;
        } else {
            return vfail("bad expression");
        }
        rc = pwm_open(pin, hz, duty);
        if (rc < 0) return pin_fail("pwm", rc);
        out->i = (int32_t)hz;
        return 1;
    }

    if (nlen == 5 && strncmp(name, "ticks", 5) == 0) {
        uint32_t ms;

        if (argc != 0) return vfail("bad expression");
        ms = sys_uptime_ms();
        if (ms > 2147483647u) return vfail("integer overflow");
        out->i = (int32_t)ms;
        return 1;
    }
    if (nlen == 5 && strncmp(name, "timer", 5) == 0)
        return timer_builtin(args, argc, out);
    if (nlen == 6 && strncmp(name, "tstart", 6) == 0)
        return timer_handle(args, argc, 2, out);
    if (nlen == 5 && strncmp(name, "tstop", 5) == 0)
        return timer_handle(args, argc, 3, out);
    if (nlen == 6 && strncmp(name, "tcount", 6) == 0)
        return timer_handle(args, argc, 1, out);
    if (nlen == 6 && strncmp(name, "tclose", 6) == 0)
        return timer_handle(args, argc, 4, out);
    if (nlen == 7 && strncmp(name, "tperiod", 7) == 0)
        return timer_handle(args, argc, 0, out);
    if (nlen == 3 && strncmp(name, "irq", 3) == 0)
        return irq_builtin(args, argc, out);
    if (nlen == 4 && strncmp(name, "wait", 4) == 0) {
        int hit = 0;

        if (argc != 1 || args[0].type != V_INT || args[0].u.i < 0 ||
            args[0].u.i > 1000000)
            return vfail("bad expression");
        if (shell_wait((uint32_t)args[0].u.i, &hit) != 0) return -1;
        out->i = hit ? 0 : -1;
        return 1;
    }
    if (nlen == 5 && strncmp(name, "spawn", 5) == 0)
        return sh_spawn(args, argc, out);
    if (nlen == 4 && strncmp(name, "join", 4) == 0)
        return sh_join(args, argc, out);
    if (nlen == 5 && strncmp(name, "yield", 5) == 0)
        return sh_yield_fn(args, argc, out);
    if ((rc = file_builtin(name, nlen, args, argc, out)) != 0) return rc;
    if ((rc = pat_builtin(name, nlen, args, argc, out)) != 0) return rc;
    return 0;
}

static int KEXT parse_call(const char *name, int nlen, const char **pp, val_t *out)
{
    const char *beg[FN_ARGS], *end[FN_ARGS];
    int argc = 0, mark, i;
    shell_fn_t *slot;
    char nb[VAR_NAME];

    memcpy(nb, name, (size_t)nlen);
    nb[nlen] = '\0';
    if (s_fn_depth >= FN_NEST || s_fn_stack + (int)sizeof beg > FN_STACK)
        return vfail("calls nest too deeply");
    s_fn_stack += (int)sizeof beg;

    (*pp)++;
    vskip(pp);
    if (**pp != ')') {
        for (;;) {
            const char *s;
            int depth = 0, q = 0;

            vskip(pp);
            s = *pp;
            if (*s == ')' || *s == ',' || *s == '\0') {
                s_fn_stack -= (int)sizeof beg;
                return vfail("bad expression");
            }
            if (argc >= FN_ARGS) {
                s_fn_stack -= (int)sizeof beg;
                return vfail("too many arguments");
            }
            beg[argc] = s;
            while (*s) {
                if (*s == '"') q = !q;
                else if (!q && *s == '(') depth++;
                else if (!q && *s == ')') {
                    if (depth == 0) break;
                    depth--;
                } else if (!q && *s == ',' && depth == 0) break;
                s++;
            }
            if (*s != ',' && *s != ')') {
                s_fn_stack -= (int)sizeof beg;
                return vfail("bad expression");
            }
            end[argc++] = s;
            *pp = s;
            if (*s == ')') break;
            (*pp)++;
        }
    }
    if (**pp != ')') {
        s_fn_stack -= (int)sizeof beg;
        return vfail("bad expression");
    }
    (*pp)++;

    mark = (argc > 0 ? argc : 1) * (int)sizeof(fn_arg_t) + LINE_MAX * 2;
    if (s_fn_stack + mark > FN_STACK) {
        s_fn_stack -= (int)sizeof beg;
        return vfail("calls nest too deeply");
    }
    s_fn_stack += mark;
    {
        fn_arg_t args[argc > 0 ? argc : 1];
        int rc = 0;

        memset(args, 0, sizeof args);
        for (i = 0; i < argc; i++) {
            const char *p = beg[i];
            val_t v;

            memset(&v, 0, sizeof v);
            if (parse_expr(&p, &v) != 0) {
                val_drop(&v);
                args_drop(args, i);
                rc = -1;
                break;
            }
            vskip(&p);
            if (p != end[i]) {
                val_drop(&v);
                args_drop(args, i);
                rc = vfail("bad expression");
                break;
            }
            val_arg(&args[i], &v);
            end_bare();
        }
        if (rc == 0) {
            int b = fn_builtin(nb, nlen, argc ? args : NULL, argc, out);

            if (b < 0) {
                args_drop(args, argc);
                rc = -1;
            } else if (b > 1) {
                /* The builtin already stored every value in s_fn_retv. */
                args_drop(args, argc);
                if (s_fn_nret < 1 || ret_one(out, &s_fn_retv[0]) != 0) {
                    rets_drop();
                    rc = -1;
                }
                if (rc == 0) s_bare_call = 1;
            } else if (b > 0) {
                args_drop(args, argc);
                val_arg(&s_fn_retv[0], out);
                s_fn_nret = 1;
                if (is_coll(out->type)) {
                    int id = coll_clone(out->i);
                    if (id < 0) {
                        rets_drop();
                        out->type = V_NONE;
                        rc = vfail("out of memory");
                    } else out->i = id;
                }
                if (rc == 0) s_bare_call = 1;
            } else {
                slot = fn_slot(nb, nlen, 0);
                if (!slot) {
                    args_drop(args, argc);
                    kprintf("%s: no such function: %s\r\n", s_vwho, nb);
                    rc = -1;
                } else if (fn_invoke(slot->body, argc ? args : NULL, argc, out) != 0) {
                    args_drop(args, argc);
                    rc = -1;
                } else {
                    args_drop(args, argc);
                    s_bare_call = 1;
                }
            }
        }
        s_fn_stack -= mark + (int)sizeof beg;
        return rc;
    }
}

/* -2 when the name is not set.  -1 when a message is already printed. */
static int KEXT var_elem_copy(const char *name, int nlen, const char **pp,
                              char *out, int size)
{
    shell_var_t *slot = var_find(name, nlen, 0);
    val_t idx, elem;
    const char *p = *pp;

    if (!slot) return -2;
    if (!is_coll(slot->type) || *p != '[') return vfail("bad expression");
    p++;
    memset(&idx, 0, sizeof idx);
    if (parse_expr(&p, &idx) != 0) {
        val_drop(&idx);
        return -1;
    }
    vskip(&p);
    if (*p != ']') {
        val_drop(&idx);
        return vfail("bad expression");
    }
    if (coll_get(slot->u.i, &idx, &elem) != 0) {
        val_drop(&idx);
        return -1;
    }
    val_drop(&idx);
    val_text(&elem, out, size);
    val_drop(&elem);
    *pp = p + 1;
    return 0;
}

/* Read one element.  box owns a collection and is released. */
static int KEXT apply_index(const char **pp, val_t *box)
{
    val_t idx, elem;

    if (**pp != '[') return 0;
    if (!is_coll(box->type)) return vfail("bad expression");
    (*pp)++;
    memset(&idx, 0, sizeof idx);
    if (parse_expr(pp, &idx) != 0) {
        val_drop(&idx);
        val_drop(box);
        return -1;
    }
    vskip(pp);
    if (**pp != ']') {
        val_drop(&idx);
        val_drop(box);
        return vfail("bad expression");
    }
    (*pp)++;
    if (coll_get(box->i, &idx, &elem) != 0) {
        val_drop(&idx);
        val_drop(box);
        return -1;
    }
    val_drop(&idx);
    val_drop(box);
    *box = elem;
    end_bare();
    return 0;
}

static int KEXT parse_primary(const char **pp, val_t *out)
{
    const char *s;
    end_bare();
    vskip(pp);
    s = *pp;

    if (*s == '(') {
        *pp = s + 1;
        if (parse_expr(pp, out) != 0) return -1;
        vskip(pp);
        if (**pp != ')') return vfail("bad expression");
        (*pp)++;
        end_bare();
        return 0;
    }

    if (*s == '"') {
        int n = 0;
        s++;
        while (*s && *s != '"') {
            if (n >= VAR_STR - 1) return vfail("string too long");
            out->s[n++] = *s++;
        }
        if (*s != '"') return vfail("bad expression");
        out->s[n] = '\0';
        out->type = V_STR;
        *pp = s + 1;
        for (;;) {
            val_t arg;
            char c;

            memset(&arg, 0, sizeof arg);
            vskip(pp);
            c = **pp;
            if (!((c >= '0' && c <= '9') || c == '.' || c == '"' ||
                  c == '$' || c == '(' || name_char(c, 1)))
                break;
            if (parse_primary(pp, &arg) != 0) {
                val_drop(&arg);
                return -1;
            }
            if (format_step(out, &arg) != 0) {
                val_drop(&arg);
                return -1;
            }
            val_drop(&arg);
        }
        end_bare();
        return 0;
    }

        if (*s == '$') {
        int n = 0;
        s++;
        if (*s == '?') {
            out->type = V_INT;
            out->i = s_status;
            *pp = s + 1;
            return 0;
        }
        if (*s >= '0' && *s <= '9') {
            int idx = 0, k = 0;
            while (s[k] >= '0' && s[k] <= '9') {
                idx = idx * 10 + (s[k] - '0');
                k++;
                if (k > 2 || idx > FN_ARGS) return vfail("bad name");
            }
            *pp = s + k;
            if (arg_load(idx, out) != 0) return -1;
            return apply_index(pp, out);
        }
        if (!name_char(*s, 1)) return vfail("bad name");
        while (name_char(s[n], 0)) n++;
        if (n >= VAR_NAME) return vfail("bad name");
        *pp = s + n;
        if (load_var(s, n, out) != 0) return -1;
        return apply_index(pp, out);
    }

    if ((*s >= '0' && *s <= '9') || *s == '.') {
        int hex = 0, dot = 0, digits = 0;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            uint32_t u = 0;
            s += 2;
            hex = 1;
            while ((*s >= '0' && *s <= '9') ||
                   (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) {
                uint32_t d = (*s <= '9') ? (uint32_t)(*s - '0')
                             : (uint32_t)((*s & ~0x20) - 'A' + 10);
                if (u > (0xFFFFFFFFu - d) / 16u) return vfail("integer overflow");
                u = u * 16u + d;
                digits++;
                s++;
            }
            if (!digits) return vfail("bad expression");
            if (*s == 'b' && !name_char(s[1], 0)) {
                if (u > 255u) return vfail("integer overflow");
                out->type = V_BYTE;
                out->i = (int32_t)u;
                *pp = s + 1;
                (void)hex;
                return 0;
            }
            if (name_char(*s, 0)) return vfail("bad expression");
            /* All 32 bits: the high bit is the sign, so 0xFFFFFFFF is -1. */
            out->type = V_INT;
            out->i = (int32_t)u;
            *pp = s;
            (void)hex;
            return 0;
        }
        {
            const char *t = s;
            while (*t >= '0' && *t <= '9') t++;
            if (*t == '.') dot = 1;
        }
        if (!dot) {
            uint32_t u = 0;
            while (*s >= '0' && *s <= '9') {
                uint32_t d = (uint32_t)(*s - '0');
                if (u > (2147483647u - d) / 10u) return vfail("integer overflow");
                u = u * 10u + d;
                digits++;
                s++;
            }
            if (!digits || *s == '.') return vfail("bad expression");
            if (*s == 'b' && !name_char(s[1], 0)) {
                if (u > 255u) return vfail("integer overflow");
                out->type = V_BYTE;
                out->i = (int32_t)u;
                *pp = s + 1;
                return 0;
            }
            if (name_char(*s, 0)) return vfail("bad expression");
            out->type = V_INT;
            out->i = (int32_t)u;
            *pp = s;
            return 0;
        }
        {
            float ip = 0.f, scale = 0.1f;
            int saw = 0, seen_dot = 0;
            while (*s) {
                if (*s >= '0' && *s <= '9') {
                    int d = *s - '0';
                    saw = 1;
                    if (!seen_dot) ip = ip * 10.f + (float)d;
                    else { ip = ip + (float)d * scale; scale *= 0.1f; }
                    s++;
                } else if (*s == '.' && !seen_dot) {
                    seen_dot = 1;
                    s++;
                } else break;
            }
            if (!saw || name_char(*s, 0)) return vfail("bad expression");
            out->type = V_FLT;
            out->f = ip;
            *pp = s;
            return 0;
        }
    }

    if (name_char(*s, 1)) {
        int n = 0;
        while (name_char(s[n], 0)) n++;
        *pp = s + n;
        vskip(pp);
        if (**pp != '(') {
            if (n == 4 && strncmp(s, "true", 4) == 0) {
                out->type = V_BOOL;
                out->i = 1;
                return 0;
            }
            if (n == 5 && strncmp(s, "false", 5) == 0) {
                out->type = V_BOOL;
                out->i = 0;
                return 0;
            }
            if (n == 5 && strncmp(s, "empty", 5) == 0) {
                out->type = V_EMPTY;
                return 0;
            }
            if (n == 4 && strncmp(s, "none", 4) == 0) {
                out->type = V_NIL;
                return 0;
            }
        }
        if (**pp != '(') return vfail("bad expression");
        return parse_call(s, n, pp, out);
    }

    return vfail("bad expression");
}

static int KEXT parse_unary(const char **pp, val_t *out)
{
    char op;
    vskip(pp);
    op = **pp;
    if (op == '+' || op == '-' || op == '~') {
        (*pp)++;
        if (parse_unary(pp, out) != 0) return -1;
        end_bare();
        if (op == '~') {
            if (!type_wide(out->type)) return vfail("not an integer");
            out->type = V_INT;
            out->i = ~out->i;
            return 0;
        }
        if (out->type == V_BYTE) {
            if (op == '-') {
                out->type = V_INT;
                out->i = -out->i;
            }
            return 0;
        }
        if (out->type == V_INT) {
            if (op == '-') {
                if (out->i == (-2147483647 - 1)) return vfail("integer overflow");
                out->i = -out->i;
            }
            return 0;
        }
        if (out->type == V_FLT) {
            if (op == '-') out->f = -out->f;
            return 0;
        }
        return vfail("bad expression");
    }
    return parse_primary(pp, out);
}

static int KEXT parse_mul(const char **pp, val_t *out)
{
    if (parse_unary(pp, out) != 0) return -1;
    for (;;) {
        int op;
        val_t rhs;
        vskip(pp);
        if (**pp == '*') op = OP_MUL;
        else if (**pp == '/' && (*pp)[1] != '=') op = OP_DIV;
        else if (**pp == '%') op = OP_MOD;
        else break;
        (*pp)++;
        memset(&rhs, 0, sizeof rhs);
        if (parse_unary(pp, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        end_bare();
        if (out->type == V_STR || rhs.type == V_STR ||
            out->type == V_EMPTY || rhs.type == V_EMPTY ||
            out->type == V_NIL || rhs.type == V_NIL ||
            out->type == V_BOOL || rhs.type == V_BOOL ||
            is_coll(out->type) || is_coll(rhs.type)) {
            val_drop(&rhs);
            return vfail("bad expression");
        }
        if (apply_num(op, out, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        val_drop(&rhs);
    }
    return 0;
}

static int KEXT parse_add(const char **pp, val_t *out)
{
    if (parse_mul(pp, out) != 0) return -1;
    for (;;) {
        int sub;
        val_t rhs;
        vskip(pp);
        if (**pp != '+' && **pp != '-') break;
        sub = (**pp == '-');
        (*pp)++;
        memset(&rhs, 0, sizeof rhs);
        if (parse_mul(pp, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        end_bare();
        if (sub) {
            if (out->type == V_STR || rhs.type == V_STR ||
                out->type == V_EMPTY || rhs.type == V_EMPTY ||
                out->type == V_NIL || rhs.type == V_NIL ||
                out->type == V_BOOL || rhs.type == V_BOOL ||
                is_coll(out->type) || is_coll(rhs.type)) {
                val_drop(&rhs);
                return vfail("bad expression");
            }
            if (apply_num(OP_SUB, out, &rhs) != 0) {
                val_drop(&rhs);
                return -1;
            }
        } else if (apply_add(out, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        val_drop(&rhs);
    }
    return 0;
}

static int KEXT parse_shift(const char **pp, val_t *out)
{
    if (parse_add(pp, out) != 0) return -1;
    for (;;) {
        int op;
        val_t rhs;
        vskip(pp);
        if ((*pp)[0] == '<' && (*pp)[1] == '<') op = OP_SHL;
        else if ((*pp)[0] == '>' && (*pp)[1] == '>') op = OP_SHR;
        else break;
        *pp += 2;
        memset(&rhs, 0, sizeof rhs);
        if (parse_add(pp, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        end_bare();
        if (apply_num(op, out, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        val_drop(&rhs);
    }
    return 0;
}

static int KEXT parse_bit(const char **pp, val_t *out, char tok, int op,
                          int (*lower)(const char **, val_t *))
{
    if (lower(pp, out) != 0) return -1;
    for (;;) {
        val_t rhs;
        vskip(pp);
        if (**pp != tok) break;
        (*pp)++;
        memset(&rhs, 0, sizeof rhs);
        if (lower(pp, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        end_bare();
        if (apply_num(op, out, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        val_drop(&rhs);
    }
    return 0;
}

static int KEXT parse_and(const char **pp, val_t *out)
{
    return parse_bit(pp, out, '&', OP_AND, parse_shift);
}

static int KEXT parse_xor(const char **pp, val_t *out)
{
    return parse_bit(pp, out, '^', OP_XOR, parse_and);
}

static int KEXT parse_or(const char **pp, val_t *out)
{
    return parse_bit(pp, out, '|', OP_OR, parse_xor);
}

static int KEXT values_equal(const val_t *a, const val_t *b)
{
    if (is_coll(a->type) || is_coll(b->type)) {
        if (a->type != b->type) return 0;
        return same_cells(a->i, b->i);
    }
    if (a->type == V_EMPTY || b->type == V_EMPTY)
        return a->type == V_EMPTY && b->type == V_EMPTY;
    if (a->type == V_NIL || b->type == V_NIL)
        return a->type == V_NIL && b->type == V_NIL;
    if (a->type == V_BOOL || b->type == V_BOOL)
        return a->type == V_BOOL && b->type == V_BOOL && a->i == b->i;
    if (a->type == V_STR || b->type == V_STR) {
        if (a->type != b->type) return 0;
        return strcmp(a->s, b->s) == 0;
    }
    if (type_wide(a->type) && type_wide(b->type)) return a->i == b->i;
    {
        float x = type_wide(a->type) ? (float)a->i : a->f;
        float y = type_wide(b->type) ? (float)b->i : b->f;
        return x == y;
    }
}

/* -1 when either side is a string.  *cmp is -1, 0 or 1, as a - b. */
static int KEXT values_order(const val_t *a, const val_t *b, int *cmp)
{
    if (a->type == V_STR || b->type == V_STR ||
        a->type == V_EMPTY || b->type == V_EMPTY ||
        a->type == V_NIL || b->type == V_NIL ||
        a->type == V_BOOL || b->type == V_BOOL ||
        is_coll(a->type) || is_coll(b->type))
        return -1;
    if (type_wide(a->type) && type_wide(b->type)) {
        *cmp = (a->i > b->i) - (a->i < b->i);
        return 0;
    }
    {
        float x = type_wide(a->type) ? (float)a->i : a->f;
        float y = type_wide(b->type) ? (float)b->i : b->f;
        *cmp = (x > y) - (x < y);
        return 0;
    }
}

static int KEXT parse_expr(const char **pp, val_t *out)
{
    if (parse_or(pp, out) != 0) return -1;
    for (;;) {
        int ne;
        val_t rhs;
        vskip(pp);
        if ((*pp)[0] == '=' && (*pp)[1] == '=') ne = 0;
        else if ((*pp)[0] == '/' && (*pp)[1] == '=') ne = 1;
        else if ((*pp)[0] == '>' && (*pp)[1] == '<') ne = 2;
        else if ((*pp)[0] == '<' && (*pp)[1] != '<') ne = 3;
        else if ((*pp)[0] == '>' && (*pp)[1] != '>') ne = 4;
        else break;
        *pp += (ne >= 3) ? 1 : 2;
        memset(&rhs, 0, sizeof rhs);
        if (parse_or(pp, &rhs) != 0) {
            val_drop(&rhs);
            return -1;
        }
        end_bare();
        {
            int bit;

            if (ne <= 1) {
                bit = values_equal(out, &rhs) ? 1 : 0;
                if (ne) bit = !bit;
            } else {
                int cmp;

                if (values_order(out, &rhs, &cmp) != 0) {
                    val_drop(out);
                    val_drop(&rhs);
                    return vfail("not a number");
                }
                if (ne == 2) bit = cmp != 0;
                else if (ne == 3) bit = cmp < 0;
                else bit = cmp > 0;
            }
            val_drop(out);
            val_drop(&rhs);
            out->i = bit;
        }
        out->type = V_INT;
    }
    return 0;
}

static void KEXT var_store(shell_var_t *slot, const fn_arg_t *a)
{
    if (is_coll(slot->type)) coll_free(slot->u.i);
    slot->type = a->type;
    if (is_coll(a->type) || type_hold_i(a->type)) slot->u.i = a->u.i;
    else if (a->type == V_FLT) slot->u.f = a->u.f;
    else if (a->type == V_STR) memcpy(slot->u.s, a->u.s, VAR_STR);
}

/* 1..32 expressions.  The last one, when it is only a call, contributes
 * every value that call returned.  Results land in s_fn_retv. */
static int KEXT parse_rets(const char **pp)
{
    int n = 0;

    rets_drop();
    s_bare_call = 0;
    for (;;) {
        int bytes = n * (int)sizeof(fn_arg_t);
        int last, bare, m;
        val_t v;

        memset(&v, 0, sizeof v);
        vskip(pp);
        if (n > 0) {
            if (s_fn_stack + bytes > FN_STACK)
                goto too_deep;
            s_fn_stack += bytes;
        }
        {
            fn_arg_t keep[n > 0 ? n : 1];

            if (n > 0) memcpy(keep, s_fn_retv, (size_t)bytes);
            s_fn_nret = 0;
            s_bare_call = 0;
            if (parse_expr(pp, &v) != 0) {
                if (n > 0) s_fn_stack -= bytes;
                val_drop(&v);
                rets_drop();
                args_drop(keep, n);
                return -1;
            }
            if (n > 0) s_fn_stack -= bytes;
            vskip(pp);
            last = (**pp != ',');
            bare = s_bare_call;
            m = s_fn_nret;
            if (last && bare) {
                val_drop(&v);
                if (n + m > FN_ARGS) {
                    rets_drop();
                    args_drop(keep, n);
                    return vfail("too many values");
                }
                if (n > 0) {
                    memmove(s_fn_retv + n, s_fn_retv,
                            (size_t)m * sizeof(fn_arg_t));
                    memcpy(s_fn_retv, keep, (size_t)bytes);
                }
                n += m;
            } else {
                if (bare) rets_drop();
                if (n >= FN_ARGS) {
                    val_drop(&v);
                    args_drop(keep, n);
                    return vfail("too many values");
                }
                if (n > 0) memcpy(s_fn_retv, keep, (size_t)bytes);
                val_arg(&s_fn_retv[n], &v);
                n++;
                s_bare_call = 0;
            }
            s_fn_nret = n;
        }
        if (last) break;
        (*pp)++;
    }
    return 0;

too_deep:
    rets_drop();
    return vfail("calls nest too deeply");
}

/* set name[index] value.  A new name and an integer index is an array.
 * Any other index starts a dict.  An array grows up to the index. */
static int KEXT set_at(const char *name, int nlen, const char **pp)
{
    val_t idx, val;
    cell_t key, elem;
    shell_var_t *slot;
    int id, kind, created = 0;

    memset(&idx, 0, sizeof idx);
    memset(&val, 0, sizeof val);
    (*pp)++;
    if (parse_expr(pp, &idx) != 0) {
        val_drop(&idx);
        return -1;
    }
    vskip(pp);
    if (**pp != ']') {
        val_drop(&idx);
        return vfail("bad expression");
    }
    (*pp)++;
    vskip(pp);
    if (!**pp) {
        val_drop(&idx);
        return usage("set [<name> [, <name>]... <expr>]");
    }
    if (parse_expr(pp, &val) != 0) {
        val_drop(&idx);
        val_drop(&val);
        return -1;
    }
    vskip(pp);
    if (**pp) {
        val_drop(&idx);
        val_drop(&val);
        return vfail("bad expression");
    }
    cell_from_val(&elem, &val);
    slot = var_find(name, nlen, 0);
    if (slot && !is_coll(slot->type)) {
        val_drop(&idx);
        val_drop(&val);
        return vfail("type mismatch");
    }
    if (!slot) {
        if (!scalar_ok(idx.type)) {
            val_drop(&idx);
            val_drop(&val);
            return vfail("type mismatch");
        }
        kind = type_wide(idx.type) ? V_ARR : V_DICT;
        slot = var_find(name, nlen, 1);
        if (!slot) {
            val_drop(&idx);
            val_drop(&val);
            return vfail("too many variables");
        }
        id = coll_new(kind);
        if (id < 0) {
            slot->type = V_NONE;
            slot->name[0] = '\0';
            val_drop(&idx);
            val_drop(&val);
            return vfail("out of memory");
        }
        slot->type = (uint8_t)kind;
        slot->u.i = id;
        created = 1;
    }
    if (slot->type == V_ARR) {
        if (!type_wide(idx.type)) {
            if (created) {
                coll_free(slot->u.i);
                slot->type = V_NONE;
                slot->name[0] = '\0';
            }
            val_drop(&idx);
            val_drop(&val);
            return vfail("not an integer");
        }
        if (array_put(&s_coll[slot->u.i], idx.i, &elem) != 0) {
            if (created) {
                coll_free(slot->u.i);
                slot->type = V_NONE;
                slot->name[0] = '\0';
            }
            val_drop(&idx);
            val_drop(&val);
            return -1;
        }
    } else {
        cell_from_val(&key, &idx);
        if (dict_put(&s_coll[slot->u.i], &key, &elem) != 0) {
            if (created) {
                coll_free(slot->u.i);
                slot->type = V_NONE;
                slot->name[0] = '\0';
            }
            val_drop(&idx);
            val_drop(&val);
            return -1;
        }
    }
    val_drop(&idx);
    val_drop(&val);
    rets_drop();
    return 0;
}

static int KEXT set_exec(const char *line)
{
    const char *s;
    char names[VAR_MAX][VAR_NAME];
    int lens[VAR_MAX];
    int nnames = 0, i, need, free_n;
    val_t v;

    s_vwho = "set";
    if (!word_is(line, "set", &s)) return -1;
    if (!*s) {
        var_list();
        return 0;
    }
    for (;;) {
        int n = 0;

        if (!name_char(*s, 1)) return vfail("bad name");
        while (name_char(s[n], 0)) n++;
        if (n >= VAR_NAME) return vfail("bad name");
        if (nnames >= VAR_MAX) return vfail("too many variables");
        memcpy(names[nnames], s, (size_t)n);
        names[nnames][n] = '\0';
        lens[nnames++] = n;
        s += n;
        vskip(&s);
        if (*s == '[') {
            if (nnames != 1) return vfail("bad name");
            return set_at(names[0], lens[0], &s);
        }
        if (*s != ',') break;
        s++;
        vskip(&s);
    }
    if (!*s) return usage("set [<name> [, <name>]... <expr>]");
    memset(&v, 0, sizeof v);
    if (parse_expr(&s, &v) != 0) {
        val_drop(&v);
        return -1;
    }
    vskip(&s);
    if (*s) {
        val_drop(&v);
        rets_drop();
        return vfail("bad expression");
    }
    if (nnames > 1 && (!s_bare_call || s_fn_nret < nnames)) {
        val_drop(&v);
        rets_drop();
        return vfail("too few values");
    }

    need = 0;
    for (i = 0; i < nnames; i++) {
        int j, again = 0;
        if (var_find(names[i], lens[i], 0)) continue;
        for (j = 0; j < i; j++)
            if (lens[j] == lens[i] &&
                memcmp(names[j], names[i], (size_t)lens[i]) == 0)
                again = 1;
        if (!again) need++;
    }
    free_n = 0;
    for (i = 0; i < VAR_MAX; i++)
        if (s_var[i].type == V_NONE) free_n++;
    if (need > free_n) {
        val_drop(&v);
        rets_drop();
        return vfail("too many variables");
    }

    for (i = 0; i < nnames; i++) {
        shell_var_t *slot = var_find(names[i], lens[i], 1);
        fn_arg_t one;

        if (!slot) {
            val_drop(&v);
            rets_drop();
            return vfail("too many variables");
        }
        if (nnames == 1) val_arg(&one, &v);
        else one = s_fn_retv[i];
        var_store(slot, &one);
        if (is_coll(one.type)) {
            if (nnames == 1) v.type = V_NONE;
            else s_fn_retv[i].type = V_NONE;
        }
    }
    val_drop(&v);
    rets_drop();
    return 0;
}

static int KEXT cmd_unset(int argc, char **argv)
{
    shell_var_t *slot;
    const char *n;
    int nlen = 0;

    s_vwho = "unset";
    if (argc != 2) return usage("unset <name>");
    n = argv[1];
    if (!name_char(n[0], 1)) return vfail("bad name");
    while (n[nlen]) {
        if (!name_char(n[nlen], 0) || nlen >= VAR_NAME - 1) return vfail("bad name");
        nlen++;
    }
    slot = var_find(n, nlen, 0);
    if (!slot) {
        kprintf("unset: no such variable: %s\r\n", n);
        return -1;
    }
    if (is_coll(slot->type)) coll_free(slot->u.i);
    slot->type = V_NONE;
    slot->name[0] = '\0';
    return 0;
}

/* true, false, or bool(...) are conditions without a comparison. */
static int KEXT is_bool_cond(const char *s)
{
    int n = 0;

    while (*s == ' ' || *s == '\t') s++;
    if (!name_char(*s, 1)) return 0;
    while (name_char(s[n], 0)) n++;
    if (n == 4 && strncmp(s, "true", 4) == 0) return 1;
    if (n == 5 && strncmp(s, "false", 5) == 0) return 1;
    if (n == 4 && strncmp(s, "bool", 4) == 0 && s[n] == '(') return 1;
    return 0;
}

/* 1 when the text is a comparison.  'if' runs a command otherwise. */
static int KEXT is_condition(const char *s)
{
    int q = 0;

    while (*s) {
        if (*s == '"') q = !q;
        else if (!q && s[0] == '=' && s[1] == '=') return 1;
        else if (!q && s[0] == '/' && s[1] == '=') return 1;
        else if (!q && s[0] == '>' && s[1] == '<') return 1;
        else if (!q && s[0] == '<' && s[1] != '<') return 1;
        else if (!q && s[0] == '>' && s[1] != '>') return 1;
        s++;
    }
    return 0;
}

/* 1 true, 0 false, -1 bad (a message is already printed). */
static int KEXT cond_eval(const char *s)
{
    val_t v;

    s_vwho = "if";
    memset(&v, 0, sizeof v);
    if (parse_expr(&s, &v) != 0) {
        val_drop(&v);
        return -1;
    }
    vskip(&s);
    if (*s || (v.type != V_INT && v.type != V_BOOL)) {
        val_drop(&v);
        rets_drop();
        return vfail("bad expression");
    }
    val_drop(&v);
    rets_drop();
    return v.i ? 1 : 0;
}

static int KEXT run_command(const char *line)
{
    char buf[LINE_MAX];
    char *argv[MAX_ARGS];
    int argc;

    if (word_is(line, "set", NULL))
        return s_status = status_of(set_exec(line));
    if (expand_status(line, buf, (int)sizeof buf) != 0)
        return s_status;
    argc = split_args(buf, argv, MAX_ARGS);
    if (argc == 0) return s_status;

    for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++) {
        if (strcmp(s_cmds[i].name, argv[0]) == 0)
            return s_status = status_of(s_cmds[i].fn(argc, argv));
    }
    kprintf("%s: command not found (try 'help')\r\n", argv[0]);
    return s_status = FREYA_EXIT_NOTFOUND;
}

/* ------------------------------------------------------ script threads */
/*
 * A script thread runs a function on this same interpreter.  It is not
 * a scheduler thread: the worker stacks are too small, and the
 * interpreter is one set of globals.  The thread runs until it sleeps,
 * yields, or returns, and then another ready one runs.  A larger
 * priority goes first.  Two of them fit.  Variables and $? are shared,
 * and a statement finishes before another thread starts.
 */
#define SH_READY  1
#define SH_SLEEP  2
#define SH_RUN    3

#define SH_STEP   0
#define SH_SLEPT  1
#define SH_YIELDED 2
#define SH_DONE   3

static uint32_t s_now;
static int s_sh_cur = -1;
static int s_sh_last = -1;
static int s_yield_req;
static int s_sh_kill;
static int s_pumping;

static void KEXT sh_wake(void)
{
    int i;

    for (i = 0; i < SH_MAX; i++) {
        if (s_sh[i].state == SH_SLEEP && (int32_t)(s_now - s_sh[i].wake) >= 0)
            s_sh[i].state = SH_READY;
    }
}

static void KEXT s_now_add(uint32_t ms)
{
    s_now += ms;
    sh_wake();
}

static uint32_t KEXT sh_soon(void)
{
    uint32_t best = 0;
    int i, any = 0;

    for (i = 0; i < SH_MAX; i++) {
        uint32_t d;

        if (s_sh[i].state != SH_SLEEP) continue;
        d = s_sh[i].wake - s_now;
        if ((int32_t)d <= 0) return 1;
        if (!any || d < best) {
            best = d;
            any = 1;
        }
    }
    return any ? best : 0;
}

static int KEXT sh_alive(void)
{
    int i;

    if (g_app.running) return 0;
    for (i = 0; i < SH_MAX; i++)
        if (s_sh[i].state) return 1;
    return 0;
}

static void KEXT sh_clear(sh_thr_t *t)
{
    t->state = 0;
    t->name[0] = '\0';
    t->yielded = 0;
}

static void KEXT sh_stop_all(void)
{
    int i;

    if (g_app.running) return;
    for (i = 0; i < SH_MAX; i++) {
        if (i == s_sh_cur) s_sh_kill = 1;
        else sh_clear(&s_sh[i]);
    }
}

static int KEXT sh_stop_named(const char *name)
{
    int i;

    if (g_app.running) return 0;
    if (!name || !name[0]) return 0;
    for (i = 0; i < SH_MAX; i++) {
        if (!s_sh[i].state) continue;
        if (strcmp(s_sh[i].name, name) != 0) continue;
        if (i == s_sh_cur) s_sh_kill = 1;
        else sh_clear(&s_sh[i]);
        return 1;
    }
    return 0;
}

static void KEXT sh_list(void)
{
    int i;

    if (g_app.running) return;
    for (i = 0; i < SH_MAX; i++) {
        const char *st = "ready";

        if (!s_sh[i].state) continue;
        if (s_sh[i].state == SH_RUN || i == s_sh_cur) st = "running";
        else if (s_sh[i].state == SH_SLEEP) st = "sleep";
        kprintf("  %2d  %3d  %-7s  %s\r\n", i + 2, s_sh[i].priority,
                st, s_sh[i].name);
    }
}

static sh_thr_t *KEXT sh_pick(void)
{
    sh_thr_t *best = NULL;
    int i, best_i = -1;

    for (i = 0; i < SH_MAX; i++) {
        sh_thr_t *t = &s_sh[i];

        if (t->state != SH_READY || t->yielded || i == s_sh_cur) continue;
        if (!best || t->priority > best->priority ||
            (t->priority == best->priority && best_i == s_sh_last)) {
            best = t;
            best_i = i;
        }
    }
    return best;
}

static int KEXT sh_skip(const sh_thr_t *t)
{
    int i;

    for (i = 0; i < t->sp; i++)
        if (t->fr[i].skip) return 1;
    return 0;
}

static int KEXT sh_step(sh_thr_t *t)
{
    char walk[LINE_MAX];
    char one[LINE_MAX];
    const char *rest;
    int ns, skip;

    if (s_sh_kill) return -1;
    ns = next_stmt(&t->pos, walk, LINE_MAX);
    if (ns < 0) return -1;
    if (ns > 0) {
        if (t->sp > 0) {
            kprintf("missing end\r\n");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        return SH_DONE;
    }
    skip = sh_skip(t);

    if (word_is(walk, "end", NULL)) {
        sh_fr_t *f;

        if (t->sp <= 0) {
            kprintf("unexpected end\r\n");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        f = &t->fr[t->sp - 1];
        if (f->kind == BLK_LOOP && !f->skip && f->left > 1) {
            f->left--;
            t->pos = f->restart;
            return SH_STEP;
        }
        t->sp--;
        return SH_STEP;
    }

    if (word_is(walk, "else", NULL)) {
        sh_fr_t *f;

        if (t->sp <= 0 || t->fr[t->sp - 1].kind != BLK_IF ||
            t->fr[t->sp - 1].arm) {
            kprintf("unexpected else\r\n");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        f = &t->fr[t->sp - 1];
        f->arm = 1;
        f->skip = f->outer || f->live;
        return SH_STEP;
    }

    if (word_is(walk, "return", &rest)) {
        const char *rp = rest;

        if (skip) return SH_STEP;
        if (!*rest) {
            usage("return <expr> [, <expr>]...");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        s_vwho = "return";
        if (parse_rets(&rp) != 0) {
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        vskip(&rp);
        if (*rp) {
            vfail("bad expression");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        rets_drop();
        s_status = 0;
        return SH_DONE;
    }

    if (word_is(walk, "fn", &rest)) {
        if (!*rest) {
            if (!skip) {
                fn_list();
                s_status = 0;
            }
            return SH_STEP;
        }
        if (fn_define(rest, &t->pos, walk, skip) != 0) {
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        if (!skip) s_status = 0;
        return SH_STEP;
    }

    if (word_is(walk, "if", &rest)) {
        sh_fr_t *f;
        int took = 0;

        if (!skip) {
            if (is_condition(rest) || is_bool_cond(rest)) {
                int c = cond_eval(rest);

                if (c < 0) {
                    s_status = FREYA_EXIT_FAIL;
                    return -1;
                }
                s_status = c ? 0 : FREYA_EXIT_FAIL;
                took = c;
            } else {
                run_command(rest);
                took = (s_status == 0);
            }
        }
        if (t->sp >= SCRIPT_NEST) {
            kprintf("too many nested blocks\r\n");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        f = &t->fr[t->sp++];
        memset(f, 0, sizeof *f);
        f->kind = BLK_IF;
        f->outer = (uint8_t)skip;
        f->live = (uint8_t)took;
        f->skip = (uint8_t)(skip || !took);
        return SH_STEP;
    }

    if (word_is(walk, "break", &rest)) {
        int i;

        if (*rest) {
            usage("break");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        if (skip) return SH_STEP;
        for (i = t->sp - 1; i >= 0; i--) {
            if (t->fr[i].kind == BLK_LOOP) {
                t->fr[i].skip = 1;
                return SH_STEP;
            }
        }
        kprintf("unexpected break\r\n");
        s_status = FREYA_EXIT_FAIL;
        return -1;
    }

    if (word_is(walk, "loop", &rest)) {
        sh_fr_t *f;
        uint32_t count = 0;

        if (!skip) {
            int pr;

            if (expand_status(rest, one, LINE_MAX) != 0) return -1;
            pr = parse_count(one, &count);
            if (pr != 0) {
                count_error(pr);
                return -1;
            }
        }
        if (t->sp >= SCRIPT_NEST) {
            kprintf("too many nested blocks\r\n");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        f = &t->fr[t->sp++];
        memset(f, 0, sizeof *f);
        f->kind = BLK_LOOP;
        f->restart = t->pos;
        f->left = count;
        f->outer = (uint8_t)skip;
        f->skip = (uint8_t)(skip || count == 0);
        return SH_STEP;
    }

    if (word_is(walk, "yield", &rest)) {
        if (*rest) {
            usage("yield");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        if (skip) return SH_STEP;
        return SH_YIELDED;
    }

    if (word_is(walk, "sleep", &rest)) {
        uint32_t ms;
        int pr;

        if (skip) return SH_STEP;
        if (expand_status(rest, one, LINE_MAX) != 0) return -1;
        pr = parse_count(one, &ms);
        if (pr != 0) {
            if (pr < -1) kprintf("sleep: count too large\r\n");
            else usage("sleep <ms>");
            s_status = FREYA_EXIT_FAIL;
            return -1;
        }
        if (ms == 0) return SH_YIELDED;
        t->wake = s_now + ms;
        return SH_SLEPT;
    }

    if (!skip) {
        int was = s_pumping;

        s_pumping = 0;
        s_yield_req = 0;
        run_command(walk);
        s_pumping = was;
        if (s_sh_kill || s_script_stop) return -1;
        if (s_yield_req) {
            s_yield_req = 0;
            return SH_YIELDED;
        }
    }
    return SH_STEP;
}

static int KEXT sh_pump(void)
{
    int ran = 0;

    if (g_app.running || s_pumping) return 0;
    s_pumping = 1;
    for (int i = 0; i < SH_MAX; i++) s_sh[i].yielded = 0;

    for (;;) {
        sh_thr_t *t;
        int idx, rc;

        if (script_interrupted()) {
            if (s_sh_cur < 0) sh_stop_all();
            else s_sh_kill = 1;
            s_pumping = 0;
            return -1;
        }
        sh_wake();
        t = sh_pick();
        if (!t) break;
        idx = (int)(t - s_sh);
        s_sh_cur = idx;
        t->state = SH_RUN;
        rc = sh_step(t);
        s_sh_cur = -1;
        s_sh_last = idx;
        ran = 1;
        if (s_sh_kill || rc < 0 || rc == SH_DONE) {
            s_sh_kill = 0;
            sh_clear(t);
            if (s_script_stop) {
                sh_stop_all();
                s_pumping = 0;
                return -1;
            }
            continue;
        }
        if (rc == SH_SLEPT) t->state = SH_SLEEP;
        else if (rc == SH_YIELDED) {
            t->state = SH_READY;
            t->yielded = 1;
        } else {
            t->state = SH_READY;
        }
    }
    s_pumping = 0;
    return ran ? 1 : 0;
}

static int KEXT sh_getc(void)
{
    for (;;) {
        int c;
        uint32_t step;

        if (!sh_alive()) return uart_getc();
        if (sh_pump() < 0) return 0x03;
        c = uart_getc_nb();
        if (c >= 0) return c;
        step = sh_soon();
        if (!step) {
            c = uart_getc_timeout(1);
            if (c >= 0) return c;
            continue;
        }
        if (step > SLEEP_SLICE_MS) step = SLEEP_SLICE_MS;
        c = uart_getc_timeout(step);
        if (c >= 0) return c;
        s_now_add(step);
    }
}

static int KEXT sh_spawn(fn_arg_t *args, int argc, val_t *out)
{
    shell_fn_t *fn;
    sh_thr_t *slot = NULL;
    const char *name;
    int i, nlen, pri;

    s_vwho = "spawn";
    if (busy_running("spawn")) return -1;
    if (s_script_stop) return vfail("bad expression");
    if (argc != 2 || args[0].type != V_STR || args[1].type != V_INT)
        return vfail("bad expression");
    name = args[0].u.s;
    pri = (int)args[1].u.i;
    if (pri < FREYA_PRIO_MIN || pri > FREYA_PRIO_MAX)
        return vfail("bad expression");
    if (!name[0] || strcmp(name, "shell") == 0 || strcmp(name, "idle") == 0)
        return vfail("bad name");
    nlen = (int)strlen(name);
    if (nlen >= VAR_NAME) return vfail("bad name");
    if (named_fn(name) != 0) return -1;
    fn = fn_slot(name, nlen, 0);
    if (!fn || !fn->body) return vfail("no such function");
    for (i = 0; i < SH_MAX; i++) {
        if (s_sh[i].state && strcmp(s_sh[i].name, name) == 0)
            return vfail("that name is in use");
        if (!s_sh[i].state && !slot) slot = &s_sh[i];
    }
    if (!slot) return vfail("too many threads");
    memset(slot, 0, sizeof *slot);
    memcpy(slot->name, name, (size_t)nlen + 1U);
    memcpy(slot->body, fn->body, strlen(fn->body) + 1U);
    slot->priority = (int8_t)pri;
    slot->pos = slot->body;
    slot->state = SH_READY;
    out->type = V_INT;
    out->i = (int32_t)(slot - s_sh) + 2;
    return 1;
}

static int KEXT sh_join(fn_arg_t *args, int argc, val_t *out)
{
    int id, idx;

    s_vwho = "join";
    if (argc != 1 || args[0].type != V_INT) return vfail("bad expression");
    id = (int)args[0].u.i;
    if (id < 2 || id >= 2 + SH_MAX) return vfail("bad expression");
    idx = id - 2;
    if (idx == s_sh_cur) return vfail("cannot join itself");
    while (s_sh[idx].state) {
        int pr;
        uint32_t step;

        if (script_interrupted()) return -1;
        pr = sh_pump();
        if (pr < 0) return -1;
        if (!s_sh[idx].state) break;
        if (pr > 0) continue;
        step = sh_soon();
        if (!step) return vfail("that thread is busy");
        if (step > SLEEP_SLICE_MS) step = SLEEP_SLICE_MS;
        sys_delay_ms(step);
        s_now_add(step);
    }
    out->type = V_INT;
    out->i = 0;
    return 1;
}

static int KEXT sh_yield_fn(fn_arg_t *args, int argc, val_t *out)
{
    (void)args;
    s_vwho = "yield";
    if (argc != 0) return vfail("bad expression");
    out->type = V_INT;
    out->i = 0;
    if (s_sh_cur >= 0) s_yield_req = 1;
    else if (sh_pump() < 0) return -1;
    return 1;
}

/* 1 if rc is the 'end' that closes this block.  Anything else is already
 * a failure, or becomes one here. */
static int KEXT block_closed(int rc)
{
    if (rc == SCR_END) return 1;
    if (rc != SCR_ERR) {
        kprintf("%s\r\n", rc == SCR_ELSE ? "unexpected else" : "missing end");
        s_status = FREYA_EXIT_FAIL;
    }
    return 0;
}

/* Run statements until 'end' or 'else' at this level.  skip means the
 * commands are parsed, so the block still matches, but not run. */
static int KEXT exec_block(const char **pp, int skip, char *walk, char *one)
{
    int ns = 1;

    while ((ns = next_stmt(pp, walk, LINE_MAX)) == 0) {
        const char *rest;
        int rc;

        if (script_interrupted()) return SCR_ERR;
        if (sh_pump() < 0) return SCR_ERR;

        if (word_is(walk, "end", NULL)) return SCR_END;
        if (word_is(walk, "else", NULL)) return SCR_ELSE;

        if (word_is(walk, "return", &rest)) {
            if (!*rest) {
                usage("return <expr> [, <expr>]...");
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            if (skip) continue;
            if (s_fn_depth <= 0) {
                kprintf("unexpected return\r\n");
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            s_vwho = "return";
            if (parse_rets(&rest) != 0) {
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            vskip(&rest);
            if (*rest) {
                vfail("bad expression");
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            s_status = 0;
            return SCR_RETURN;
        }

        if (word_is(walk, "fn", &rest)) {
            if (!*rest) {
                if (!skip) {
                    fn_list();
                    s_status = 0;
                }
                continue;
            }
            if (fn_define(rest, pp, walk, skip) != 0) {
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            if (!skip) s_status = 0;
            continue;
        }

        if (word_is(walk, "if", &rest)) {
            int took = 0;

            /* 'one' is shared with the nested call, so the condition is
             * run before that call reuses it. */
            if (!skip) {
                if (is_condition(rest) || is_bool_cond(rest)) {
                    int t = cond_eval(rest);

                    if (t < 0) {
                        s_status = FREYA_EXIT_FAIL;
                        took = 0;
                    } else {
                        s_status = t ? 0 : FREYA_EXIT_FAIL;
                        took = t;
                    }
                } else {
                    strncpy(one, rest, LINE_MAX - 1);
                    one[LINE_MAX - 1] = '\0';
                    run_command(one);
                    took = (s_status == 0);
                }
            }
            rc = exec_block(pp, skip || !took, walk, one);
            if (rc == SCR_RETURN) return SCR_RETURN;
            if (rc == SCR_BREAK) {
                /* The branch stopped early.  Skip the rest of this 'if'
                 * so its 'end' is still found, then tell the loop. */
                rc = exec_block(pp, 1, walk, one);
                if (rc == SCR_ELSE)
                    rc = exec_block(pp, 1, walk, one);
                if (!block_closed(rc)) return SCR_ERR;
                return SCR_BREAK;
            }
            if (rc == SCR_ERR) return SCR_ERR;
            if (rc == SCR_ELSE)
                rc = exec_block(pp, skip || took, walk, one);
            if (rc == SCR_RETURN) return SCR_RETURN;
            if (rc == SCR_BREAK) {
                if (!block_closed(exec_block(pp, 1, walk, one))) return SCR_ERR;
                return SCR_BREAK;
            }
            if (!block_closed(rc)) return SCR_ERR;
            continue;
        }

        if (word_is(walk, "break", &rest)) {
            if (*rest) {
                usage("break");
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            if (skip) continue;
            s_status = 0;
            return SCR_BREAK;
        }

        if (word_is(walk, "loop", &rest)) {
            uint32_t count = 0;
            const char *body;

            if (!skip) {
                int pr;

                if (expand_status(rest, one, LINE_MAX) != 0)
                    return SCR_ERR;
                pr = parse_count(one, &count);
                if (pr != 0) {
                    count_error(pr);
                    return SCR_ERR;
                }
            }
            body = *pp;
            if (skip || count == 0) {
                rc = exec_block(pp, 1, walk, one);
                if (!block_closed(rc)) return SCR_ERR;
                continue;
            }
            while (count--) {
                const char *q = body;

                if (script_interrupted()) return SCR_ERR;
                rc = exec_block(&q, 0, walk, one);
                if (rc == SCR_RETURN) return SCR_RETURN;
                if (rc == SCR_BREAK) {
                    rc = exec_block(&q, 1, walk, one);
                    if (!block_closed(rc)) return SCR_ERR;
                    *pp = q;
                    break;
                }
                if (!block_closed(rc)) return SCR_ERR;
                *pp = q;
            }
            continue;
        }

        if (!skip) run_command(walk);
    }
    return (ns < 0) ? SCR_ERR : SCR_DONE;
}

static void KEXT script_discard(void)
{
    s_script_len = 0;
    s_script[0] = '\0';
}

static int KEXT script_append(const char *line)
{
    int n = (int)strlen(line);
    int extra = n + (s_script_len ? 1 : 0);

    if (s_script_len + extra >= SCRIPT_MAX) {
        kprintf("script too long\r\n");
        script_discard();
        s_status = FREYA_EXIT_FAIL;
        return -1;
    }
    if (s_script_len) s_script[s_script_len++] = '\n';
    memcpy(s_script + s_script_len, line, (size_t)n);
    s_script_len += n;
    s_script[s_script_len] = '\0';
    return 0;
}

/* A line typed while a program is running.  Only the commands that look
 * at threads are taken: anything else would re-enter the card or the
 * loader under a thread that may be using them.  The line is assembled
 * across polls, and one command is run per call so PendSV is not held
 * across a long print. */
void shell_poll_runtime(void)
{
    int c;

    if (!g_app.running || uart_waiters() || uart_is_raw()) return;

    for (;;) {
        c = uart_getc_nb();
        if (c < 0) return;

        if (c == '\r' || c == '\n') {
            char tmp[LINE_MAX];
            char *argv[MAX_ARGS];
            int argc;

            uart_puts("\r\n");
            s_poll_line[s_poll_len] = '\0';
            s_poll_len = 0;
            if (!s_poll_line[0]) return;

            strncpy(tmp, s_poll_line, sizeof tmp - 1);
            tmp[sizeof tmp - 1] = '\0';
            argc = split_args(tmp, argv, MAX_ARGS);
            if (argc < 1) return;
            if (strcmp(argv[0], "threads") != 0 &&
                strcmp(argv[0], "stop") != 0 &&
                strcmp(argv[0], "help") != 0) {
                kprintf("%s: a program is running - stop it first\r\n", argv[0]);
                return;
            }
            hist_push(s_poll_line);
            shell_exec(s_poll_line);
            return;
        }

        if (c == 0x15) {                    /* Ctrl-U */
            while (s_poll_len) {
                uart_puts("\b \b");
                s_poll_len--;
            }
            continue;
        }
        if (c == 8 || c == 0x7F) {
            if (s_poll_len) {
                s_poll_len--;
                uart_puts("\b \b");
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (s_poll_len < LINE_MAX - 1) {
            s_poll_line[s_poll_len++] = (char)c;
            uart_putc((char)c);
        }
    }
}

int KEXT shell_exec(const char *line)
{
    char walk[LINE_MAX];
    char one[LINE_MAX];
    const char *p;
    int st;

    if (!line) return s_status;
    st = script_check(line, walk);
    if (st < 0) return s_status;
    if (st > 0) {
        kprintf("missing end\r\n");
        return s_status = FREYA_EXIT_FAIL;
    }
    /* A nested 'source' must not forget a Ctrl-C the outer script saw. */
    if (s_exec_depth == 0)
        s_script_stop = 0;
    s_exec_depth++;
    p = line;
    (void)exec_block(&p, 0, walk, one);
    s_exec_depth--;
    if (sh_pump() < 0) return s_status;
    return s_status;
}

void console_banner(void)
{
    static const char art[] =
        "  ______\r\n"
        " |  ____|\r\n"
        " | |__ _ __ ___ _   _  __ _\r\n"
        " |  __| '__/ _ \\ | | |/ _` |\r\n"
        " | |  | | |  __/ |_| | (_| |\r\n"
        " |_|  |_|  \\___|\\__, |\\__,_|\r\n"
        "                 __/ |\r\n"
        "                |___/\r\n";

    uart_puts("\r\n");
    uart_puts(art);
    kprintf("Freya %s \"%s\" for %s - built %s\r\n",
            FREYA_VERSION, FREYA_CODENAME, BOARD_MCU, FREYA_BUILD_ID);
    kprintf("%u MHz, %s reset. Type 'help'.\r\n\r\n",
            g_clocks.hclk_hz / 1000000UL, sys_reset_cause_str());
}

void shell_run(void)
{
    static char line[LINE_MAX];

    for (;;) {
        char walk[LINE_MAX];
        int n, st;

        if (s_script_len) uart_puts("> ");
        else kprintf("freya:%s> ", fat_mounted() ? fs_cwd() : "(no fs)");

        n = readline(line, (int)sizeof line);
        if (n < 0) {                        /* Ctrl-C abandons the block */
            script_discard();
            continue;
        }
        if (n == 0) continue;

        hist_push(line);
        /* A finished line runs at once.  An open block is kept and
         * read on the following lines, until 'end' brings it level. */
        if (!s_script_len) {
            st = script_check(line, walk);
            if (st < 0) continue;
            if (st == 0) {
                shell_exec(line);
                continue;
            }
        }
        if (script_append(line) != 0) continue;
        st = script_check(s_script, walk);
        if (st > 0) continue;
        if (st == 0) shell_exec(s_script);
        script_discard();
    }
}
