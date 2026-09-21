/*
 * Freya - system entry point.
 *
 * Reset_Handler hands control here with .data copied, .bss cleared and
 * the FPU enabled.  Everything from this point on is Freya's own
 * bring-up: clocks, console, storage, then the shell.
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

static void boot_autorun(void)
{
    fat_dirent_t e;
    char *argv[1];

    if (!fat_mounted()) return;
    if (fat_stat(AUTORUN_PATH, &e) != FAT_OK) return;
    if (e.attr & FAT_ATTR_DIR) return;

    kprintf("[boot] %s found - starting in %u s, press a key to cancel\r\n",
            AUTORUN_PATH, AUTORUN_GRACE / 1000);
    if (uart_getc_timeout(AUTORUN_GRACE) >= 0) {
        uart_rx_flush();
        kprintf("[boot] autorun cancelled\r\n");
        return;
    }

    if (app_load(AUTORUN_PATH) != 0) return;
    argv[0] = (char *)AUTORUN_PATH;
    kprintf("--- %s starting (Ctrl-C stops it) ---\r\n",
            g_app.name[0] ? g_app.name : AUTORUN_PATH);
    app_run(1, argv);
    kprintf("\r\n--- autorun %s, exit code %d ---\r\n",
            app_stop_reason_str(g_app.last_stop_reason), g_app.last_exit_code);
}

void freya_main(void)
{
    sys_init();
    heap_init();
    uart_init(115200);

    console_banner();
    kprintf("[boot] clocks     : %s, sysclk %u MHz, flash 3 WS\r\n",
            g_clocks.clock_source ? "HSE 25 MHz + PLL" : "HSI 16 MHz + PLL",
            g_clocks.hclk_hz / 1000000UL);
    kprintf("[boot] console    : USART2 115200 8N1\r\n");

    boot_storage();
    boot_autorun();

    led_set(1);
    sys_delay_ms(60);
    led_set(0);

    kprintf("\r\n");
    shell_run();
}
