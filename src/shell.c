/*
 * Freya - console shell on USART2.
 *
 * Line editing with backspace, Ctrl-U, Ctrl-C and a small command
 * history on the cursor keys, plus the built-in command set.  Every
 * command leaves an exit status behind, which '$?' and 'status' read.
 */
#include "freya.h"
#include "fat.h"

#define LINE_MAX    160
#define MAX_ARGS    16
#define HIST_DEPTH  8

static char s_hist[HIST_DEPTH][LINE_MAX];
static int  s_hist_count;
static int  s_hist_pos;
static int  s_status;               /* status of the last command, '$?' */

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

/*
 * '$?' in a line becomes the status of the command before it, which is
 * what makes 'echo $?' and 'write /runs.txt $?' work.  Nothing else is
 * expanded: this is a console, not a scripting language.
 */
static void expand_status(const char *in, char *out, int size)
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
        out[o++] = *in++;
    }
    out[o] = '\0';
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

    inf("sd card");    kprintf("%s", sd_type_str());
    if (g_sd.initialised) {
        kprintf(", ");
        kput_size((uint64_t)g_sd.blocks * 512ULL);
        kprintf(" (%u blocks)", g_sd.blocks);
    }
    kprintf("\r\n");

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
            kprintf("     empty - 'install <file>', or flash one in with the kernel\r\n");
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

    if (!app_flash_header()) {
        kprintf("no program is installed in flash\r\n");
        return 0;
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

static int cmd_stop(int argc, char **argv)
{
    (void)argc; (void)argv;

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
        why = (strcmp(cmd, "i2c") == 0) ? "that bus or its pins are taken"
                                        : "that timer is taken";
        break;
    case FREYA_ERR_ARG:
        why = (strcmp(cmd, "i2c") == 0)
              ? "speed, address or length out of range" : "out of range";
        break;
    case FREYA_ERR_NACK:    why = "no answer"; break;
    case FREYA_ERR_TIMEOUT: why = "timed out"; break;
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

/* ------------------------------------------------------ command table */
typedef struct {
    const char *name;
    int (*fn)(int, char **);
    const char *help;
} command_t;

static const command_t s_cmds[] = {
    { "help",     cmd_help,     "help [command]" },
    { "sysinfo",  cmd_sysinfo,  "CPU, clocks, reset, card, fs" },
    { "meminfo",  cmd_meminfo,  "flash and RAM usage" },
    { "mount",    cmd_mount,    "mount the SD card" },
    { "ls",       cmd_ls,       "ls [-l] [path]" },
    { "ll",       cmd_ls,       "ll [path]" },
    { "cd",       cmd_cd,       "cd [path]" },
    { "pwd",      cmd_pwd,      "print directory" },
    { "mkdir",    cmd_mkdir,    "mkdir <dir>..." },
    { "rm",       cmd_rm,       "rm [-r] <path>..." },
    { "rename",   cmd_rename,   "rename <old> <new>" },
    { "mv",       cmd_rename,   "mv <old> <new>" },
    { "download", cmd_download, "download <file> [--raw]" },
    { "cat",      cmd_cat,      "cat <file>" },
    { "write",    cmd_write,    "write <file> <text...>" },
    { "hexdump",  cmd_hexdump,  "hexdump <file> [off] [len]" },
    { "flashdump",cmd_flashdump,"flashdump [file]" },
    { "df",       cmd_df,       "show free space" },
    { "load",     cmd_load,     "load " PROG_ARG },
    { "run",      cmd_run,      "run [" PROG_ARG "] [args]" },
#ifdef FREYA_APP_FLASH_ADDR
    { "runflash", cmd_runflash, "runflash [args...]" },
#endif
    { "stop",     cmd_stop,     "unload the program" },
    { "status",   cmd_status,   "last exit status (also $?)" },
#ifdef FREYA_APP_FLASH_ADDR
    { "install",  cmd_install,  "install <file>" },
    { "saveflash",cmd_saveflash,"saveflash [file]" },
    { "uninstall",cmd_uninstall,"erase flash program" },
    { "autostart",cmd_autostart,"autostart [on|off]" },
    { "ramdump",  cmd_ramdump,  "ramdump [on|off]" },
#endif
    { "date",     cmd_date,     "date [YYYY-MM-DD HH:MM:SS]" },
    { "loglevel", cmd_loglevel, "loglevel [level]" },
    { "uptime",   cmd_uptime,   "time since reset" },
    { "led",      cmd_led,      "led on|off|blink" },
    { "pin",      cmd_pin,      PIN_USAGE },
    { "pwm",      cmd_pwm,      PWM_USAGE },
    { "i2c",      cmd_i2c,      I2C_USAGE },
    { "echo",     cmd_echo,     "echo <text...>" },
    { "clear",    cmd_clear,    "clear the screen" },
    { "reboot",   cmd_reboot,   "restart the MCU" },
};

static int cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++) {
            if (strcmp(s_cmds[i].name, argv[1]) == 0) {
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

/* --------------------------------------------------------------- loop */
/* Runs one line and records its status, which the next line's '$?' and
 * the 'status' command read back.  An empty line changes nothing, the
 * way a shell leaves '$?' alone. */
int shell_exec(char *line)
{
    char *argv[MAX_ARGS];
    int argc = split_args(line, argv, MAX_ARGS);

    if (argc == 0) return s_status;

    for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++) {
        if (strcmp(s_cmds[i].name, argv[0]) == 0)
            return s_status = status_of(s_cmds[i].fn(argc, argv));
    }
    kprintf("%s: command not found (try 'help')\r\n", argv[0]);
    return s_status = FREYA_EXIT_NOTFOUND;
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
    static char cmd[LINE_MAX];

    for (;;) {
        int n;

        kprintf("freya:%s> ", fat_mounted() ? fs_cwd() : "(no fs)");
        n = readline(line, sizeof(line));
        if (n <= 0) continue;

        hist_push(line);                    /* history keeps what was typed */
        expand_status(line, cmd, sizeof(cmd));
        shell_exec(cmd);
    }
}
