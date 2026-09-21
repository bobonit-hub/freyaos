/*
 * Freya - console shell on USART2.
 *
 * Line editing with backspace, Ctrl-U, Ctrl-C and a small command
 * history on the cursor keys, plus the built-in command set.
 */
#include "freya.h"
#include "fat.h"

#define LINE_MAX    160
#define MAX_ARGS    16
#define HIST_DEPTH  8

static char s_hist[HIST_DEPTH][LINE_MAX];
static int  s_hist_count;
static int  s_hist_pos;

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

static void print_fat_time(uint16_t date, uint16_t time)
{
    kprintf("%04u-%02u-%02u %02u:%02u",
            1980 + (date >> 9), (date >> 5) & 0x0F, date & 0x1F,
            (time >> 11) & 0x1F, (time >> 5) & 0x3F);
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

static int need_fs(void)
{
    if (fat_mounted()) return 1;
    kprintf("no filesystem mounted - run 'mount'\r\n");
    return 0;
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

/* ----------------------------------------------------------- commands */
static int cmd_help(int argc, char **argv);

static int cmd_sysinfo(int argc, char **argv)
{
    uint32_t idcode = DBGMCU_IDCODE;
    uint16_t fl_kb = *(volatile uint16_t *)FLASHSIZE_BASE;
    const uint32_t *uid = (const uint32_t *)UID_BASE;
    uint32_t up = sys_uptime_ms();
    rtc_time_t t;

    (void)argc; (void)argv;
    rtc_get(&t);

    kprintf("Freya %s  (built %s)\r\n", FREYA_VERSION, FREYA_BUILD_ID);
    kprintf("  board      : %s\r\n", BOARD_NAME);
    kprintf("  core       : %s, CPUID 0x%08x\r\n", BOARD_CORE, SCB->CPUID);
    kprintf("  device id  : 0x%03x  rev 0x%04x\r\n",
            idcode & 0xFFF, (idcode >> 16) & 0xFFFF);
    kprintf("  unique id  : %08x-%08x-%08x\r\n", uid[0], uid[1], uid[2]);
    kprintf("  flash      : %u KiB internal\r\n", fl_kb);
    kprintf("  clock src  : %s -> PLL\r\n",
            g_clocks.clock_source ? BOARD_HSE_NAME
                                  : BOARD_HSI_NAME " (crystal not found)");
    kprintf("  sysclk     : %u Hz   AHB %u Hz\r\n", g_clocks.sysclk_hz, g_clocks.hclk_hz);
    kprintf("  apb1/apb2  : %u Hz / %u Hz\r\n", g_clocks.pclk1_hz, g_clocks.pclk2_hz);
    kprintf("  console    : %s\r\n", BOARD_CONSOLE_NAME);
    kprintf("  reset by   : %s\r\n", sys_reset_cause_str());
    kprintf("  uptime     : %u.%03u s\r\n", up / 1000, up % 1000);
    kprintf("  date/time  : %04u-%02u-%02u %02u:%02u:%02u\r\n",
            t.year, t.mon, t.day, t.hour, t.min, t.sec);

    kprintf("  sd card    : %s", sd_type_str());
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

static void print_bar(uint32_t used, uint32_t total)
{
    int filled = total ? (int)((uint64_t)used * 20 / total) : 0;

    uart_putc('[');
    for (int i = 0; i < 20; i++) uart_putc(i < filled ? '#' : '.');
    kprintf("] %u%%", total ? (uint32_t)((uint64_t)used * 100 / total) : 0);
}

static int cmd_meminfo(int argc, char **argv)
{
    uint32_t data_sz = (uint32_t)((uint8_t *)__data_end - (uint8_t *)__data_start);
    uint32_t bss_sz  = (uint32_t)((uint8_t *)__bss_end  - (uint8_t *)__bss_start);
    uint32_t flash_used = (uint32_t)((uint8_t *)__kernel_flash_end - (uint8_t *)0x08000000UL);
    uint32_t flash_total = (uint32_t)(*(volatile uint16_t *)FLASHSIZE_BASE) * 1024UL;
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
        kprintf("  auto-start     : %s  at 0x%08x\r\n",
                app_autostart_enabled() ? "on" : "off",
                (unsigned)FREYA_AUTOSTART_ADDR);
    }
#endif

    kprintf("SRAM  0x%08x .. 0x%08x  (%u KiB)\r\n",
            (uint32_t)(uintptr_t)__ram_start, (uint32_t)(uintptr_t)__ram_end,
            (uint32_t)(__ram_end - __ram_start) / 1024U);
    kprintf("  .data          : %6u B  at 0x%08x\r\n", data_sz, (uint32_t)(uintptr_t)__data_start);
    kprintf("  .bss           : %6u B  at 0x%08x\r\n", bss_sz, (uint32_t)(uintptr_t)__bss_start);
    kprintf("  system heap    : %6u B  at 0x%08x\r\n", heap_total, (uint32_t)(uintptr_t)__heap_start);
    kprintf("     used %u B, free %u B, largest free block %u B, %u blocks\r\n",
            heap_used, heap_free, heap_big, heap_blocks);
    kprintf("     ");
    print_bar(heap_used, heap_total);
    kprintf("\r\n");

    kprintf("  program region : %6u B  at 0x%08x\r\n", app_total, (uint32_t)FREYA_APP_LOAD_ADDR);
    if (g_app.loaded && (g_app.flags & FREYA_APP_F_XIP)) {
        /* The image is in flash; only its variables are here. */
        uint32_t data_sz2 = g_app.data_end - g_app.data_start;

        kprintf("     %s (from flash): data %u B + bss %u B\r\n",
                g_app.name[0] ? g_app.name : g_app.path, data_sz2, g_app.bss_size);
        kprintf("     ");
        print_bar(data_sz2 + g_app.bss_size, app_total);
        kprintf("\r\n");
    } else if (g_app.loaded) {
        kprintf("     %s: image %u B + bss %u B\r\n",
                g_app.name[0] ? g_app.name : g_app.path, g_app.image_size, g_app.bss_size);
        kprintf("     ");
        print_bar(g_app.image_size + g_app.bss_size, app_total);
        kprintf("\r\n");
    } else {
        kprintf("     empty\r\n");
    }

    kprintf("  main stack     : %6u B  at 0x%08x\r\n", stack_total, (uint32_t)(uintptr_t)__stack_limit);
    kprintf("     in use now %u B, peak since boot %u B\r\n", stack_used(), stack_peak());
    kprintf("     ");
    print_bar(stack_peak(), stack_total);
    kprintf("\r\n");
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
    if (rc != FAT_OK) {
        kprintf("mount: %s\r\n", fat_err_str(rc));
        return -1;
    }
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
    if (fs_abspath(target ? target : ".", path, sizeof(path)) != 0) {
        kprintf("ls: path too long\r\n");
        return -1;
    }

    rc = fat_opendir(&dir, path);
    if (rc != FAT_OK) {
        kprintf("ls: %s: %s\r\n", path, fat_err_str(rc));
        return -1;
    }

    if (long_fmt) kprintf("%s:\r\n", path);

    for (;;) {
        rc = fat_readdir(&dir, &e);
        if (rc == 1) break;
        if (rc < 0) { kprintf("ls: %s\r\n", fat_err_str(rc)); break; }
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
    if (rc != FAT_OK) {
        kprintf("cd: %s: %s\r\n", argc > 1 ? argv[1] : "/", fat_err_str(rc));
        return -1;
    }
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
    if (argc < 2) { kprintf("usage: mkdir <directory>\r\n"); return -1; }

    for (int i = 1; i < argc; i++) {
        if (fs_abspath(argv[i], path, sizeof(path)) != 0) {
            kprintf("mkdir: path too long\r\n");
            return -1;
        }
        rc = fat_mkdir(path);
        if (rc != FAT_OK) {
            kprintf("mkdir: %s: %s\r\n", argv[i], fat_err_str(rc));
            return -1;
        }
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
        if (fs_abspath(argv[i], path, sizeof(path)) != 0) {
            kprintf("rm: path too long\r\n");
            return -1;
        }
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
    if (!count) kprintf("usage: rm [-r] <file|directory>\r\n");
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
        kprintf("usage: download <file> [--raw]\r\n");
        kprintf("  receives an XMODEM / XMODEM-1K stream, e.g. 'sx -k file'\r\n");
        return -1;
    }

    kprintf("Ready to receive '%s' over XMODEM.\r\n", name);
    kprintf("Start the transfer on the host now (Ctrl-X twice on the host to abort).\r\n");

    rc = xmodem_receive_to_file(name, &got, strip);
    if (rc == 0) {
        kprintf("\r\nreceived ");
        kput_size(got);
        kprintf(" (%u bytes) into %s\r\n", got, name);
        return 0;
    }

    kprintf("\r\ndownload failed: ");
    switch (rc) {
    case -2: kprintf("timed out waiting for the sender\r\n"); break;
    case -3: kprintf("cancelled by the sender\r\n"); break;
    case -4: kprintf("too many bad packets\r\n"); break;
    case -5: kprintf("packet sequence error\r\n"); break;
    case -6: kprintf("cannot write to the card\r\n"); break;
    default: kprintf("error %d\r\n", rc); break;
    }
    return -1;
}

static int cmd_cat(int argc, char **argv)
{
    uint8_t buf[128];
    int fd, n;

    if (!need_fs()) return -1;
    if (argc < 2) { kprintf("usage: cat <file>\r\n"); return -1; }

    fd = fs_fd_open(argv[1], FREYA_O_RDONLY);
    if (fd < 0) { kprintf("cat: %s: %s\r\n", argv[1], fat_err_str(fd)); return -1; }

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
    if (argc < 3) { kprintf("usage: write <file> <text...>\r\n"); return -1; }

    fd = fs_fd_open(argv[1], FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_APPEND);
    if (fd < 0) { kprintf("write: %s: %s\r\n", argv[1], fat_err_str(fd)); return -1; }

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
    if (argc < 2) { kprintf("usage: hexdump <file> [offset] [length]\r\n"); return -1; }
    if (argc > 2) str_to_u32(argv[2], &off);
    if (argc > 3) str_to_u32(argv[3], &limit);

    fd = fs_fd_open(argv[1], FREYA_O_RDONLY);
    if (fd < 0) { kprintf("hexdump: %s: %s\r\n", argv[1], fat_err_str(fd)); return -1; }
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
    if (argc < 2) { kprintf("usage: load " PROG_ARG "\r\n"); return -1; }
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
    if (argc < 2) {
        kprintf("usage: install <file>\r\n");
        kprintf("  Copies a flash image - one built with app_flash.ld - from\r\n"
                "  the card into the %u KiB program flash region, where it\r\n"
                "  survives a power cycle.  'runflash' then runs it with no\r\n"
                "  card in the socket at all.\r\n",
                (unsigned)(FREYA_APP_FLASH_SIZE / 1024));
        return -1;
    }
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
            if (app_load(argv[1]) != 0) return -1;
        } else if (!g_app.loaded) {
            kprintf("run: %s: %s\r\n", argv[1], "no such program");
            return -1;
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
            return -1;
        }
        app_argv[app_argc++] = g_app.path;
    }

    kprintf("--- %s starting (Ctrl-C stops it) ---\r\n",
            g_app.name[0] ? g_app.name : g_app.path);
    uart_drain_tx();

    ret = app_run(app_argc, app_argv);

    kprintf("\r\n--- %s %s, exit code %d, %u ms ---\r\n",
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

static int cmd_autostart(int argc, char **argv)
{
    int rc, enable;

    if (argc < 2) {
        kprintf("auto-start is %s (flag at 0x%08x)\r\n",
                app_autostart_enabled() ? "on" : "off",
                (unsigned)FREYA_AUTOSTART_ADDR);
        kprintf("usage: autostart on|off\r\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) enable = 1;
    else if (strcmp(argv[1], "off") == 0) enable = 0;
    else {
        kprintf("usage: autostart on|off\r\n");
        return -1;
    }

    if (g_app.running) {
        kprintf("autostart: a program is running - stop it first\r\n");
        return -1;
    }

    if (enable == app_autostart_enabled()) {
        kprintf("auto-start is already %s\r\n", enable ? "on" : "off");
        return 0;
    }

    kprintf("autostart: console input is dropped while flash is busy\r\n");
    uart_drain_tx();
    rc = app_autostart_set(enable);
    uart_rx_flush();
    if (rc != FLASH_OK) {
        kprintf("autostart: %s\r\n", flash_err_str(rc));
        return -1;
    }
    kprintf("auto-start %s\r\n", enable ? "on" : "off");
    if (enable && !app_flash_header())
        kprintf("(no program is installed in flash yet - 'install <file>')\r\n");
    return 0;
}
#endif

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
    if (g_app.last_stop_reason != APP_STOP_NONE || g_app.last_run_ms)
        kprintf(" (last run: %s, exit code %d)",
                app_stop_reason_str(g_app.last_stop_reason), g_app.last_exit_code);
    kprintf("\r\n");
    app_unload();
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
    kprintf("%04u-%02u-%02u %02u:%02u:%02u\r\n",
            t.year, t.mon, t.day, t.hour, t.min, t.sec);
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
    if (argc < 2) { kprintf("usage: led on|off|blink\r\n"); return -1; }
    if (strcmp(argv[1], "on") == 0) led_set(1);
    else if (strcmp(argv[1], "off") == 0) led_set(0);
    else if (strcmp(argv[1], "blink") == 0) {
        for (int i = 0; i < 10; i++) { led_toggle(); sys_delay_ms(100); }
        led_set(0);
    } else { kprintf("usage: led on|off|blink\r\n"); return -1; }
    return 0;
}

/* ------------------------------------------------------ command table */
typedef struct {
    const char *name;
    int (*fn)(int, char **);
    const char *usage;
    const char *help;
} command_t;

static const command_t s_cmds[] = {
    { "help",     cmd_help,     "help [command]",            "list commands or describe one" },
    { "sysinfo",  cmd_sysinfo,  "sysinfo",                   "show system information" },
    { "meminfo",  cmd_meminfo,  "meminfo",                   "show flash and RAM usage" },
    { "mount",    cmd_mount,    "mount",                     "initialise the SD card and mount FAT" },
    { "ls",       cmd_ls,       "ls [-l] [path]",            "list a directory" },
    { "ll",       cmd_ls,       "ll [path]",                 "list a directory with sizes and dates" },
    { "cd",       cmd_cd,       "cd [path]",                 "change the working directory" },
    { "pwd",      cmd_pwd,      "pwd",                       "print the working directory" },
    { "mkdir",    cmd_mkdir,    "mkdir <dir>...",            "create directories" },
    { "rm",       cmd_rm,       "rm [-r] <path>...",         "remove files or directories" },
    { "download", cmd_download, "download <file> [--raw]",   "receive a file over XMODEM" },
    { "cat",      cmd_cat,      "cat <file>",                "print a file" },
    { "write",    cmd_write,    "write <file> <text...>",    "append a line of text to a file" },
    { "hexdump",  cmd_hexdump,  "hexdump <file> [off] [len]","dump a file in hex" },
    { "df",       cmd_df,       "df",                        "show free space on the card" },
    { "load",     cmd_load,     "load " PROG_ARG,            "load a program image into RAM" },
    { "run",      cmd_run,      "run [" PROG_ARG "] [args]", "run the loaded program" },
#ifdef FREYA_APP_FLASH_ADDR
    { "runflash", cmd_runflash, "runflash [args...]",        "run the program stored in internal flash" },
#endif
    { "stop",     cmd_stop,     "stop",                      "stop / unload the program (Ctrl-C stops a running one)" },
#ifdef FREYA_APP_FLASH_ADDR
    { "install",  cmd_install,  "install <file>",            "copy a program image into internal flash" },
    { "uninstall",cmd_uninstall,"uninstall",                 "erase the program flash region" },
    { "autostart",cmd_autostart,"autostart [on|off]",        "run the flash program automatically at boot" },
#endif
    { "date",     cmd_date,     "date [YYYY-MM-DD HH:MM:SS]","show or set the clock" },
    { "uptime",   cmd_uptime,   "uptime",                    "time since reset" },
    { "led",      cmd_led,      "led on|off|blink",          "drive the " BOARD_LED_NAME " LED" },
    { "echo",     cmd_echo,     "echo <text...>",            "echo the arguments" },
    { "clear",    cmd_clear,    "clear",                     "clear the screen" },
    { "reboot",   cmd_reboot,   "reboot",                    "restart the MCU" },
};

static int cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++) {
            if (strcmp(s_cmds[i].name, argv[1]) == 0) {
                kprintf("%s\r\n  %s\r\n", s_cmds[i].usage, s_cmds[i].help);
                return 0;
            }
        }
        kprintf("help: no such command: %s\r\n", argv[1]);
        return -1;
    }

    kprintf("Freya commands:\r\n");
    for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++)
        kprintf("  %-26s %s\r\n", s_cmds[i].usage, s_cmds[i].help);
    kprintf("\r\nCtrl-C stops a running program, Ctrl-U clears the line, "
            "the cursor keys walk the history.\r\n");
    return 0;
}

/* --------------------------------------------------------------- loop */
int shell_exec(char *line)
{
    char *argv[MAX_ARGS];
    int argc = split_args(line, argv, MAX_ARGS);

    if (argc == 0) return 0;

    for (unsigned i = 0; i < ARRAY_SIZE(s_cmds); i++) {
        if (strcmp(s_cmds[i].name, argv[0]) == 0)
            return s_cmds[i].fn(argc, argv);
    }
    kprintf("%s: command not found (try 'help')\r\n", argv[0]);
    return -1;
}

void console_banner(void)
{
    kprintf("\r\n");
    kprintf("  ______                    \r\n");
    kprintf(" |  ____|                   \r\n");
    kprintf(" | |__ _ __ ___ _   _  __ _ \r\n");
    kprintf(" |  __| '__/ _ \\ | | |/ _` |\r\n");
    kprintf(" | |  | | |  __/ |_| | (_| |\r\n");
    kprintf(" |_|  |_|  \\___|\\__, |\\__,_|\r\n");
    kprintf("                 __/ |      \r\n");
    kprintf("                |___/       \r\n");
    kprintf("Freya %s for %s - built %s\r\n", FREYA_VERSION, BOARD_MCU, FREYA_BUILD_ID);
    kprintf("%u MHz, %s reset. Type 'help'.\r\n\r\n",
            g_clocks.hclk_hz / 1000000UL, sys_reset_cause_str());
}

void shell_run(void)
{
    static char line[LINE_MAX];

    for (;;) {
        int n;

        kprintf("freya:%s> ", fat_mounted() ? fs_cwd() : "(no fs)");
        n = readline(line, sizeof(line));
        if (n <= 0) continue;

        hist_push(line);
        shell_exec(line);
    }
}
