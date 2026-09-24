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
 * now(), date(), time() and the calendar fields read the software clock.
 * timer() and irq() arm a hardware timer or a pin edge.  wait() runs the
 * script function named for that source, in thread mode, then returns.
 * 'fn' defines a function of up to 32 arguments that returns one value.
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
        int c = uart_getc();

        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            uart_puts("\r\n");
            buf[len] = '\0';
            return len;
        }
        if (c == 0x03) {                    /* Ctrl-C */
            uart_puts("^C\r\n");
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
            if (var_copy(in + 1, n, tmp, (int)sizeof tmp) != 0) {
                kprintf("no such variable\r\n");
                s_status = FREYA_EXIT_FAIL;
                out[0] = '\0';
                return -1;
            }
            for (int i = 0; tmp[i] && o < size - 1; i++) out[o++] = tmp[i];
            in += 1 + n;
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
    int long_fmt = (strcmp(argv[0], "ll") == 0);
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
    const char *name = NULL;
    int rc;

    if (!need_fs()) return -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) strip = 0;
        else name = argv[i];
    }
    if (!name) {
        kprintf("  receives an XMODEM / XMODEM-1K stream, e.g. 'sx -k file'\r\n");
        return usage("download <file> [--raw]");
    }

    kprintf("Ready to receive '%s' over XMODEM.\r\n"
            "Start the transfer on the host now (Ctrl-X twice on the host to abort).\r\n",
            name);

    rc = xmodem_receive_to_file(name, &got, strip);
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
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    int rc;

    if (argc > 2) return usage("stop [thread]");

    if (argc == 2) {
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
    return 1;
}

/* Busy wait, in short slices, so Ctrl-C can land between them.  The
 * console interrupt still queues the key while this thread spins. */
#define SLEEP_SLICE_MS  20

static int KEXT cmd_sleep(int argc, char **argv)
{
    uint32_t ms, done = 0;

    if (argc != 2 || str_to_u32(argv[1], &ms) != 0)
        return usage("sleep <ms>");
    while (done < ms) {
        uint32_t step = ms - done;

        if (step > SLEEP_SLICE_MS) step = SLEEP_SLICE_MS;
        if (script_interrupted()) return -1;
        sys_delay_ms(step);
        done += step;
    }
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
    { "ll",       cmd_ls,       "ll [path]" },
    { "cd",       cmd_cd,       "cd [path]" },
    { "pwd",      cmd_pwd,      "pwd" },
    { "mkdir",    cmd_mkdir,    "mkdir <dir>..." },
    { "rm",       cmd_rm,       "rm [-r] <path>..." },
    { "rename",   cmd_rename,   "rename <old> <new>" },
    { "mv",       cmd_rename,   "mv <old> <new>" },
    { "download", cmd_download, "download <file> [--raw]" },
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
    { "source",   cmd_source,   "source " PROG_ARG },
    { "set",      cmd_script,   "set [<name> <expr>]" },
    { "unset",    cmd_unset,    "unset <name>" },
    { "fn",       cmd_script,   "fn [<name>]" },
    { "return",   cmd_script,   "return <expr>" },
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
                usage("return <expr>");
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
 * Eight names.  A value is an integer, a float or a short string, and
 * the next assignment decides which.  Arithmetic is + - * / ; an
 * integer also has % & | ^ ~ << >>.  A string is concatenated with +
 * and formatted by writing the format and then its arguments:
 * 'set s "%d" $n'.  int(), float() and str() convert, and hex()
 * turns an integer into hex text or hex text into an integer.
 * == and /= compare.  < > and >< order numbers.
 * 'if' treats a line that contains one of those as a condition.
 * 'break' leaves the innermost loop.
 */
#define VAR_MAX   8
#define VAR_NAME  8
#define VAR_STR   32

enum { V_NONE = 0, V_INT, V_FLT, V_STR };

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

static void KEXT val_text(const val_t *v, char *out, int size)
{
    if (v->type == V_INT) ksnprintf(out, size, "%d", (int)v->i);
    else if (v->type == V_FLT) ftoa(out, size, v->f);
    else ksnprintf(out, size, "%s", v->s);
}

static int KEXT var_copy(const char *name, int nlen, char *out, int size)
{
    shell_var_t *slot = var_find(name, nlen, 0);
    val_t v;

    if (!slot) return -1;
    v.type = slot->type;
    v.i = 0;
    v.f = 0.f;
    v.s[0] = '\0';
    if (slot->type == V_INT) v.i = slot->u.i;
    else if (slot->type == V_FLT) v.f = slot->u.f;
    else memcpy(v.s, slot->u.s, VAR_STR);
    val_text(&v, out, size);
    return 0;
}

static void KEXT var_list(void)
{
    int any = 0, i;

    for (i = 0; i < VAR_MAX; i++) {
        char buf[VAR_STR];

        if (s_var[i].type == V_NONE) continue;
        any = 1;
        if (s_var[i].type == V_INT)
            kprintf("%s = %d\r\n", s_var[i].name, (int)s_var[i].u.i);
        else if (s_var[i].type == V_FLT) {
            ftoa(buf, (int)sizeof buf, s_var[i].u.f);
            kprintf("%s = %s\r\n", s_var[i].name, buf);
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
    if (slot->type == V_INT) out->i = slot->u.i;
    else if (slot->type == V_FLT) out->f = slot->u.f;
    else memcpy(out->s, slot->u.s, VAR_STR);
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

    if (op == OP_MOD || op == OP_AND || op == OP_OR || op == OP_XOR ||
        op == OP_SHL || op == OP_SHR) {
        if (a->type != V_INT || b->type != V_INT) return vfail("not an integer");
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
        fa = (a->type == V_INT) ? (float)a->i : a->f;
        fb = (b->type == V_INT) ? (float)b->i : b->f;
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
            if (arg->type == V_STR) return vfail("bad format");
            fv = (arg->type == V_INT) ? (float)arg->i : arg->f;
            ftoa(piece, (int)sizeof piece, fv);
        } else if (*f == 'd' || *f == 'i') {
            int32_t n;
            if (arg->type == V_STR) return vfail("bad format");
            if (arg->type == V_FLT) {
                if (arg->f > 2147483647.f || arg->f < -2147483648.f)
                    return vfail("integer overflow");
                n = (int32_t)arg->f;
            } else n = arg->i;
            ksnprintf(piece, (int)sizeof piece, "%d", (int)n);
        } else {
            if (arg->type != V_INT) return vfail("not an integer");
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
 * so the static cost is the table.  A call takes 0 to 32 arguments and
 * returns one value; the arguments of the call in progress sit on the
 * stack, sized to how many were passed.  $0 is that count, $1 .. $32
 * are the values.  No 'return' leaves 0.
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

static shell_fn_t s_fn[FN_MAX];
static fn_arg_t *s_fn_args;
static int s_fn_argc;
static int s_fn_depth;
static int s_fn_stack;
static val_t *s_fn_ret;

static int KEXT exec_block(const char **pp, int skip, char *walk, char *one);

static int KEXT fn_reserved(const char *s, int n)
{
    static const char *const w[] = {
        "if", "else", "end", "loop", "break", "fn", "return",
        "get", "set", "adc", "pwm", "int", "float", "str", "hex",
        "rand", "srand", "sin", "cos", "pi",
        "now", "date", "time", "year", "month", "day",
        "hour", "minute", "second",
        "ticks", "timer", "tstart", "tstop", "tcount", "tclose", "tperiod",
        "irq", "wait"
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
    if (a->type == V_INT) out->i = a->u.i;
    else if (a->type == V_FLT) out->f = a->u.f;
    else memcpy(out->s, a->u.s, VAR_STR);
    return 0;
}

static int KEXT arg_load(int idx, val_t *out)
{
    if (arg_get(idx, out) != 0) return vfail("no such argument");
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
    if (v->type == V_INT) a->u.i = v->i;
    else if (v->type == V_FLT) a->u.f = v->f;
    else memcpy(a->u.s, v->s, VAR_STR);
}

static int KEXT fn_invoke(const char *body, fn_arg_t *args, int argc, val_t *out)
{
    char walk[LINE_MAX];
    char one[LINE_MAX];
    const char *p = body;
    fn_arg_t *saved_a = s_fn_args;
    int saved_n = s_fn_argc;
    val_t *saved_r = s_fn_ret;
    val_t ret;
    int rc;

    memset(&ret, 0, sizeof ret);
    s_fn_args = args;
    s_fn_argc = argc;
    s_fn_ret = &ret;
    s_fn_depth++;
    rc = exec_block(&p, 0, walk, one);
    s_fn_depth--;
    s_fn_args = saved_a;
    s_fn_argc = saved_n;
    s_fn_ret = saved_r;
    if (rc == SCR_RETURN) {
        *out = ret;
        return 0;
    }
    if (rc == SCR_DONE) {
        out->type = V_INT;
        out->i = 0;
        out->f = 0.f;
        out->s[0] = '\0';
        return 0;
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

    if (a->type == V_INT) {
        out->i = a->u.i;
        return 1;
    }
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
    if (a->type == V_INT) {
        out->f = (float)a->u.i;
        return 1;
    }
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
    if (a->type == V_INT) v.i = a->u.i;
    else if (a->type == V_FLT) v.f = a->u.f;
    else memcpy(v.s, a->u.s, VAR_STR);
    val_text(&v, out->s, VAR_STR);
    out->type = V_STR;
    return 1;
}

static int KEXT conv_hex(const fn_arg_t *a, val_t *out)
{
    int32_t n;
    int rc;

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
    else if (a->type == V_INT) x = (float)a->u.i;
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
        if (fn_invoke(slot->body, &arg, 1, &out) != 0) return -1;
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
        ran = shell_irq_run();
        if (ran < 0) return -1;
        if (ran > 0) { *hit = 1; return 0; }
        if (ms && done >= ms) { *hit = 0; return 0; }
        {
            uint32_t step = SLEEP_SLICE_MS;

            if (ms && ms - done < step) step = ms - done;
            sys_delay_ms(step);
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
static int KEXT fn_builtin(const char *name, int nlen, fn_arg_t *args,
                           int argc, val_t *out)
{
    const char *ps;
    int pin, rc;

    out->type = V_INT;
    out->i = 0;
    out->f = 0.f;
    out->s[0] = '\0';

    if ((rc = dt_builtin(name, nlen, args, argc, out)) != 0) return rc;

    if ((nlen == 3 && strncmp(name, "int", 3) == 0) ||
        (nlen == 5 && strncmp(name, "float", 5) == 0) ||
        (nlen == 3 && strncmp(name, "str", 3) == 0) ||
        (nlen == 3 && strncmp(name, "hex", 3) == 0)) {
        if (argc != 1) return vfail("bad expression");
        if (name[0] == 'i') return conv_int(&args[0], out);
        if (name[0] == 'f') return conv_float(&args[0], out);
        if (name[0] == 's') return conv_str(&args[0], out);
        return conv_hex(&args[0], out);
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

        for (i = 0; i < argc; i++) {
            const char *p = beg[i];
            val_t v;

            if (parse_expr(&p, &v) != 0) {
                s_fn_stack -= mark + (int)sizeof beg;
                return -1;
            }
            vskip(&p);
            if (p != end[i]) {
                s_fn_stack -= mark + (int)sizeof beg;
                return vfail("bad expression");
            }
            val_arg(&args[i], &v);
        }
        {
            int b = fn_builtin(nb, nlen, argc ? args : NULL, argc, out);
            if (b != 0) {
                s_fn_stack -= mark + (int)sizeof beg;
                return (b < 0) ? -1 : 0;
            }
        }
        slot = fn_slot(nb, nlen, 0);
        if (!slot) {
            s_fn_stack -= mark + (int)sizeof beg;
            kprintf("%s: no such function: %s\r\n", s_vwho, nb);
            return -1;
        }
        if (fn_invoke(slot->body, argc ? args : NULL, argc, out) != 0) {
            s_fn_stack -= mark + (int)sizeof beg;
            return -1;
        }
    }
    s_fn_stack -= mark + (int)sizeof beg;
    return 0;
}

static int KEXT parse_primary(const char **pp, val_t *out)
{
    const char *s;
    vskip(pp);
    s = *pp;

    if (*s == '(') {
        *pp = s + 1;
        if (parse_expr(pp, out) != 0) return -1;
        vskip(pp);
        if (**pp != ')') return vfail("bad expression");
        (*pp)++;
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
            vskip(pp);
            c = **pp;
            if (!((c >= '0' && c <= '9') || c == '.' || c == '"' ||
                  c == '$' || c == '('))
                break;
            if (parse_primary(pp, &arg) != 0) return -1;
            if (format_step(out, &arg) != 0) return -1;
        }
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
            return arg_load(idx, out);
        }
        if (!name_char(*s, 1)) return vfail("bad name");
        while (name_char(s[n], 0)) n++;
        if (n >= VAR_NAME) return vfail("bad name");
        *pp = s + n;
        return load_var(s, n, out);
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
            if (!digits || name_char(*s, 0)) return vfail("bad expression");
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
            if (!digits || *s == '.' || name_char(*s, 0)) return vfail("bad expression");
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
        if (op == '~') {
            if (out->type != V_INT) return vfail("not an integer");
            out->i = ~out->i;
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
        if (parse_unary(pp, &rhs) != 0) return -1;
        if (out->type == V_STR || rhs.type == V_STR) return vfail("bad expression");
        if (apply_num(op, out, &rhs) != 0) return -1;
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
        if (parse_mul(pp, &rhs) != 0) return -1;
        if (sub) {
            if (out->type == V_STR || rhs.type == V_STR)
                return vfail("bad expression");
            if (apply_num(OP_SUB, out, &rhs) != 0) return -1;
        } else if (apply_add(out, &rhs) != 0) {
            return -1;
        }
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
        if (parse_add(pp, &rhs) != 0) return -1;
        if (apply_num(op, out, &rhs) != 0) return -1;
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
        if (lower(pp, &rhs) != 0) return -1;
        if (apply_num(op, out, &rhs) != 0) return -1;
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
    if (a->type == V_STR || b->type == V_STR) {
        if (a->type != b->type) return 0;
        return strcmp(a->s, b->s) == 0;
    }
    if (a->type == V_INT && b->type == V_INT) return a->i == b->i;
    {
        float x = (a->type == V_INT) ? (float)a->i : a->f;
        float y = (b->type == V_INT) ? (float)b->i : b->f;
        return x == y;
    }
}

/* -1 when either side is a string.  *cmp is -1, 0 or 1, as a - b. */
static int KEXT values_order(const val_t *a, const val_t *b, int *cmp)
{
    if (a->type == V_STR || b->type == V_STR) return -1;
    if (a->type == V_INT && b->type == V_INT) {
        *cmp = (a->i > b->i) - (a->i < b->i);
        return 0;
    }
    {
        float x = (a->type == V_INT) ? (float)a->i : a->f;
        float y = (b->type == V_INT) ? (float)b->i : b->f;
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
        if (parse_or(pp, &rhs) != 0) return -1;
        if (ne <= 1) {
            out->i = values_equal(out, &rhs) ? 1 : 0;
            if (ne) out->i = !out->i;
        } else {
            int cmp;

            if (values_order(out, &rhs, &cmp) != 0) return vfail("not a number");
            if (ne == 2) out->i = cmp != 0;
            else if (ne == 3) out->i = cmp < 0;
            else out->i = cmp > 0;
        }
        out->type = V_INT;
    }
    return 0;
}

static int KEXT set_exec(const char *line)
{
    const char *s;
    char name[VAR_NAME];
    int nlen = 0;
    val_t v;
    shell_var_t *slot;

    s_vwho = "set";
    if (!word_is(line, "set", &s)) return -1;
    if (!*s) {
        var_list();
        return 0;
    }
    if (!name_char(*s, 1)) return vfail("bad name");
    while (name_char(s[nlen], 0)) nlen++;
    if (nlen >= VAR_NAME) return vfail("bad name");
    memcpy(name, s, (size_t)nlen);
    name[nlen] = '\0';
    s += nlen;
    vskip(&s);
    if (!*s) return usage("set [<name> <expr>]");
    if (parse_expr(&s, &v) != 0) return -1;
    vskip(&s);
    if (*s) return vfail("bad expression");
    slot = var_find(name, nlen, 1);
    if (!slot) return vfail("too many variables");
    slot->type = v.type;
    if (v.type == V_INT) slot->u.i = v.i;
    else if (v.type == V_FLT) slot->u.f = v.f;
    else memcpy(slot->u.s, v.s, VAR_STR);
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
    slot->type = V_NONE;
    slot->name[0] = '\0';
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
    if (parse_expr(&s, &v) != 0) return -1;
    vskip(&s);
    if (*s || v.type != V_INT) return vfail("bad expression");
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

        if (word_is(walk, "end", NULL)) return SCR_END;
        if (word_is(walk, "else", NULL)) return SCR_ELSE;

        if (word_is(walk, "return", &rest)) {
            val_t v;

            if (!*rest) {
                usage("return <expr>");
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            if (skip) continue;
            if (!s_fn_ret) {
                kprintf("unexpected return\r\n");
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            s_vwho = "return";
            if (parse_expr(&rest, &v) != 0) {
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            vskip(&rest);
            if (*rest) {
                vfail("bad expression");
                s_status = FREYA_EXIT_FAIL;
                return SCR_ERR;
            }
            *s_fn_ret = v;
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
                if (is_condition(rest)) {
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
