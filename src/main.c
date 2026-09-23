/*
 * Freya - system entry point.
 *
 * The board's Reset_Handler hands control here with .data copied, .bss
 * cleared and the core ready to run C.  Everything from this point on is
 * Freya's own bring-up: clocks, console, storage, then the shell.
 */
#include "freya.h"
#include "fat.h"

#define AUTORUN_PATH    "/autorun.bin"
#define AUTORUN_GRACE   2000    /* ms to interrupt the autorun */

static void boot_storage(void)
{
    int rc;

    kprintf("[boot] SD card    : ");
    if (sd_init() != 0) {
        kprintf("not present\r\n");
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
    char *argv[1];
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
    argv[0] = (char *)path;
    kprintf("--- %s starting (Ctrl-C stops it) ---\r\n",
            g_app.name[0] ? g_app.name : path);
    app_run(1, argv);
    kprintf("\r\n--- autorun %s, exit status %d ---\r\n",
            app_stop_reason_str(g_app.last_stop_reason), g_app.last_status);
}

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

    boot_storage();
    boot_autorun();

    led_set(1);
    sys_delay_ms(60);
    led_set(0);

    kprintf("\r\n");
    shell_run();
}
