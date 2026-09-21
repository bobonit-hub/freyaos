/*
 * Freya - SRAM dump to the SD card after a BusFault.
 *
 * Only the Blue Pill does this.  Its 20 KiB of SRAM is a single file on
 * the card; a 128 KiB Black Pill dump would be a different product.
 *
 * The write is gated by the third word of the auto-start slot (`ramdump
 * on`); erased flash means off.  It runs in thread mode, never from the
 * BusFault handler: SysTick does not preempt the fault, and the SD driver
 * times out against that tick.  A program fault dumps from the abort
 * trampoline before longjmp; a kernel BusFault in thread mode is
 * redirected here, dumps, then halts.
 *
 * The file is a raw image of [__ram_start, __ram_end).  Restore it with
 *   restore freya.ram binary 0x20000000
 * The FAT sector caches live in that same SRAM, so those few hundred bytes
 * in the file are whatever the writer last touched, not the faulting
 * contents.
 */
#include "freya.h"
#include "fat.h"

#define FREYA_RAMDUMP_PATH  "/freya.ram"

#if defined(FREYA_BOARD_BLUEPILL)

static int s_busy;

void ramdump_write(void)
{
    fat_file_t f;
    uint8_t chunk[512];
    const uint8_t *ram;
    uint32_t size, off, put;
    int rc;

    if (s_busy)
        return;
    s_busy = 1;

    ram  = (const uint8_t *)(uintptr_t)__ram_start;
    size = (uint32_t)((uintptr_t)__ram_end - (uintptr_t)__ram_start);

    if (!app_ramdump_enabled()) {
        kprintf("[freya] ram dump skipped (disabled)\r\n");
        s_busy = 0;
        return;
    }

    if (!g_sd.initialised) {
        if (sd_init() != 0) {
            kprintf("[freya] ram dump skipped (no SD card)\r\n");
            s_busy = 0;
            return;
        }
    }
    if (!fat_mounted()) {
        rc = fat_mount();
        if (rc != FAT_OK) {
            kprintf("[freya] ram dump skipped (mount failed: %s)\r\n",
                    fat_err_str(rc));
            s_busy = 0;
            return;
        }
    }

    kprintf("[freya] writing %s (%u B) ...\r\n", FREYA_RAMDUMP_PATH, size);
    uart_drain_tx();

    rc = fat_open(&f, FREYA_RAMDUMP_PATH,
                  FAT_WRITE | FAT_CREATE | FAT_TRUNC);
    if (rc != FAT_OK) {
        kprintf("[freya] ram dump failed: %s\r\n", fat_err_str(rc));
        s_busy = 0;
        return;
    }

    off = 0;
    while (off < size) {
        uint32_t n = MIN(512U, size - off);

        memcpy(chunk, ram + off, n);
        put = 0;
        rc = fat_write(&f, chunk, n, &put);
        if (rc != FAT_OK || put != n) {
            fat_close(&f);
            kprintf("[freya] ram dump failed at +%u: %s\r\n",
                    off, fat_err_str(rc));
            s_busy = 0;
            return;
        }
        off += n;
    }

    rc = fat_close(&f);
    if (rc != FAT_OK)
        kprintf("[freya] ram dump close failed: %s\r\n", fat_err_str(rc));
    else
        kprintf("[freya] ram dump wrote %u B to %s\r\n", size, FREYA_RAMDUMP_PATH);

    s_busy = 0;
}

void ramdump_then_halt(void)
{
    ramdump_write();
    kprintf("\r\nSystem halted - press any key to reboot.\r\n");
    uart_drain_tx();

    for (;;) {
        if (uart_rx_ready()) sys_reboot();
        led_toggle();
        for (volatile uint32_t i = 0; i < 1500000; i++) { }
    }
}

#else

void ramdump_write(void)
{
}

void ramdump_then_halt(void)
{
    for (;;) { }
}

#endif /* FREYA_BOARD_BLUEPILL */
