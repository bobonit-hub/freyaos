/*
 * Freya - system entry point.
 *
 * The board's Reset_Handler hands control here with .data copied, .bss
 * cleared and the core ready to run C.  Everything from this point on is
 * Freya's own bring-up: clocks, console, storage, then the shell.
 *
 * A NOSHELL=1 build (FREYA_NO_SHELL) has no shell: it boots straight into
 * the autorun program and runs it again whenever it exits.
 */
#include "freya.h"
#include "fat.h"

#define AUTORUN_PATH    "/autorun.bin"
#define AUTORUN_GRACE   2000    /* ms to interrupt the autorun */
#define AUTORUN_RESTART 1000    /* ms between runs without a shell */

/* The autorun program's arguments: its path, and '-e' and a text: the
 * one the program in flash saved after its image, or else with
 * BASIC=file in the build that file's. */
static int autorun_args(const char *path, char **argv)
{
    int argc = 0;
    const char *text = NULL;

    argv[argc++] = (char *)path;
#ifdef FREYA_APP_FLASH_ADDR
    if (strcmp(path, APP_FLASH_PATH) == 0) text = app_flash_text();
#endif
#ifdef FREYA_AUTORUN_TEXT
    if (!text) text = autorun_text;
#endif
    if (text) {
        argv[argc++] = "-e";
        argv[argc++] = (char *)text;
    }
    argv[argc] = NULL;
    return argc;
}

#ifdef FREYA_SD
#ifdef BOARD_SPIFLASH_OWN_BUS
static void boot_card(void)
#else
static void boot_storage(void)
#endif
{
    int rc;

    kprintf("[boot] SD card    : ");
    if (sd_init() != 0) {
        kprintf("not present\r\n");
#if defined(BOARD_SPIFLASH) && !defined(BOARD_SPIFLASH_OWN_BUS)
        spiflash_boot();
#endif
        return;
    }
    kprintf("%s, ", sd_type_str());
    kput_size((uint64_t)g_sd.blocks * 512ULL);
    kprintf(" (%u blocks)\r\n", g_sd.blocks);

    kprintf("[boot] filesystem : ");
    rc = fat_mount();
    if (rc != FAT_OK) {
        kprintf("%s\r\n", fat_err_str(rc));
        return;
    }
    kprintf("%s", fat_type_str());
    if (g_fs.label[0]) kprintf(" \"%s\"", g_fs.label);
    kprintf(", cluster ");
    kput_size(g_fs.bytes_per_clus);
    kprintf(", mounted on /\r\n");
}

#ifdef BOARD_SPIFLASH_OWN_BUS
/* The SPI flash has a bus of its own here, so it is mounted beside the
 * card rather than only in its absence. */
static void boot_storage(void)
{
    boot_card();
    spiflash_boot();
}
#endif
#else
/* Built without SD=1 there is no card to ask, so the SPI flash, on a
 * board that has one, is the only volume. */
static void boot_storage(void)
{
#ifdef BOARD_SPIFLASH
    spiflash_boot();
#endif
}
#endif /* FREYA_SD */

#ifndef FREYA_NO_SHELL
/*
 * The card is asked first, so a program on it always overrides one in
 * flash.  The installed image — a program, or a shell script — is started
 * only when the auto-start flag is set, which is what lets a board with
 * nothing in the card socket still boot into that image without doing so
 * on every reset by default.
 */
static void boot_autorun(void)
{
    fat_dirent_t e;
    char *argv[4];
    const char *path = NULL;
    const char *what = NULL;
    const char *script = NULL;

    if (fat_mounted() && fat_stat(AUTORUN_PATH, &e) == FAT_OK &&
        !(e.attr & FAT_ATTR_DIR)) {
        path = AUTORUN_PATH;
        what = AUTORUN_PATH " found";
    }
#ifdef FREYA_APP_FLASH_ADDR
    else if (app_autostart_enabled() && app_flash_header()) {
        path = APP_FLASH_PATH;
        what = "auto-start enabled, program in flash";
    } else if (app_autostart_enabled() &&
               app_script_find(&script, NULL) > 0) {
        what = "auto-start enabled, script in flash";
    } else if (app_autostart_enabled()) {
        kprintf("[boot] auto-start on, but no flash program\r\n");
    }
#endif
    if (!path && !script) return;

    kprintf("[boot] %s - starting in %u s, press a key to cancel\r\n",
            what, AUTORUN_GRACE / 1000);
    if (uart_getc_timeout(AUTORUN_GRACE) >= 0) {
        uart_rx_flush();
        kprintf("[boot] autorun cancelled\r\n");
        return;
    }

    if (script) {
        int st;

        kprintf("--- script starting (Ctrl-C stops it) ---\r\n");
        st = shell_exec(script);
        kprintf("\r\n--- autorun script, exit status %d ---\r\n", st);
        return;
    }

    if (app_load(path) != 0) return;
    kprintf("--- %s starting (Ctrl-C stops it) ---\r\n",
            g_app.name[0] ? g_app.name : path);
    app_run(autorun_args(path, argv), argv);
    kprintf("\r\n--- autorun %s, exit status %d ---\r\n",
            app_stop_reason_str(g_app.last_stop_reason), g_app.last_status);
}
#endif

#ifdef FREYA_NO_SHELL
/*
 * Without a shell there is nothing to return to, so autorun is always on:
 * /autorun.bin on the card, else the program in flash, whether or not its
 * auto-start flag is set.  There is no grace period to cancel it, and when
 * the program exits, or Ctrl-C stops it, it is started again.  A script
 * in flash needs the shell and is not run.
 */
static void __attribute__((noreturn)) autorun_forever(void)
{
    fat_dirent_t e;
    char *argv[4];
    const char *path;

    for (;;) {
        path = NULL;
        if (fat_mounted() && fat_stat(AUTORUN_PATH, &e) == FAT_OK &&
            !(e.attr & FAT_ATTR_DIR))
            path = AUTORUN_PATH;
#ifdef FREYA_APP_FLASH_ADDR
        else if (app_flash_header())
            path = APP_FLASH_PATH;
#endif
        if (!path) {
            kprintf("[boot] no shell and no program to run - halted\r\n");
            break;
        }
        if (app_load(path) != 0) {
            kprintf("[boot] %s does not load - halted\r\n", path);
            break;
        }
        kprintf("--- %s starting (Ctrl-C restarts it) ---\r\n",
                g_app.name[0] ? g_app.name : path);
        app_run(autorun_args(path, argv), argv);
        kprintf("\r\n--- %s, exit status %d - restarting in %u s ---\r\n",
                app_stop_reason_str(g_app.last_stop_reason),
                g_app.last_status, AUTORUN_RESTART / 1000);
        sys_delay_ms(AUTORUN_RESTART);
    }
    for (;;) __wfi();
}

/* The shell's banner, less the art and the pointer to help(). */
void console_banner(void)
{
    kprintf("\r\nFreya %s \"%s\" for %s - built %s, no shell\r\n",
            FREYA_OS_VERSION, FREYA_CODENAME, BOARD_MCU, FREYA_BUILD_ID);
    kprintf("%u MHz, %s reset.\r\n\r\n",
            g_clocks.hclk_hz / 1000000UL, sys_reset_cause_str());
}
#endif

void freya_main(void)
{
    thread_init();
    sys_init();
    heap_init();
    log_init();
    uart_init(921600);

    console_banner();
    kprintf("[boot] clocks     : %s + PLL, sysclk %u MHz, flash %u WS\r\n",
            g_clocks.clock_source ? BOARD_HSE_NAME : BOARD_HSI_NAME,
            g_clocks.hclk_hz / 1000000UL, (unsigned)BOARD_FLASH_WS);
    kprintf("[boot] console    : %s\r\n", BOARD_CONSOLE_NAME);
    kprintf("[boot] checksum   : ");
    fw_cksum_show();
#ifdef FREYA_RTC_DS3231
    ds3231_boot();
#elif defined(FREYA_RTC_INTERNAL)
    rtcin_boot();
#endif

    boot_storage();
#ifdef FREYA_USB
    (void)usb_attach(1);
#endif
#ifdef FREYA_NO_SHELL
    autorun_forever();
#else
    boot_autorun();

    led_set(1);
    sys_delay_ms(60);
    led_set(0);

    kprintf("\r\n");
    shell_run();
#endif
}
