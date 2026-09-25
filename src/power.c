/*
 * Freya - board power domains.
 *
 * FREYA_PWR_SD is the card socket.  Off closes every file, unmounts,
 * releases the SPI pins and drops VDD.  On brings VDD back and waits
 * for the rail; the card stays unidentified until the next mount, so
 * a filesystem is never used across a gap where the card was unpowered.
 * The electrical half is sd_power(); this is the part that knows about
 * open files.  On the Black Pill the linker places this file in the
 * kernel extension, because the 48 KiB image has no room for it.  On
 * the Blue Pill the extension is the one that is full, so the file
 * stays in the main image.  Either way the call is an ordinary branch.
 */
#include "freya.h"
#include "fat.h"

int board_power(int domain, int on)
{
    int was;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (domain != FREYA_PWR_SD || (on != 0 && on != 1))
        return FREYA_ERR_ARG;

    was = sd_powered() ? 1 : 0;
    if (on == was) return was;

    if (!on) {
        fs_close_all();
        if (fat_mounted()) fat_unmount();
#ifdef FREYA_BOARD_BLACKPILL
        spiflash_unmount();
#endif
    }
    sd_power(on);
    return was;
}
