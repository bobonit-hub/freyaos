/*
 * Freya - the USB stick as a second FAT volume, at /usb (USB=1).
 *
 * src/fat.c keeps one volume live in its globals: the geometry and the
 * two sector caches.  The SD card is dev 0 and the stick is dev 1; the
 * one that is not live is parked here in a fat_snap_t, and vol_use()
 * swaps them.  A path under /usb is the stick with the prefix taken off,
 * and every other path is the card, as before.  Open files and
 * directories carry their dev, so each call lands on the right volume.
 *
 * These replace the weak single-volume defaults in src/fat.c.
 */
#include "freya.h"
#include "fat.h"

#ifndef FREYA_USB
#error "src/usbvol.c is compiled only with USB=1"
#endif

#define USB_DEV         1
#define USB_PREFIX      "/usb"
#define USB_PREFIX_LEN  4

static fat_snap_t s_parked;     /* the volume that is not live          */
static uint8_t    s_cur;        /* which one is live: 0 card, 1 stick   */
static uint8_t    s_ready;

static void park_init(void)
{
    if (s_ready) return;
    memset(&s_parked, 0, sizeof(s_parked));
    s_parked.buf_lba = 0xFFFFFFFFUL;
    s_parked.fat_sec = 0xFFFFFFFFUL;
    s_parked.rd = usbmsc_read_block;
    s_parked.wr = usbmsc_write_block;
    s_ready = 1;
}

void vol_use(int dev)
{
    if (dev != 0 && dev != USB_DEV) return;
    park_init();
    if (dev == s_cur) return;
    fat_snap_swap(&s_parked);
    s_cur = (uint8_t)dev;
}

int vol_current(void)
{
    return s_cur;
}

int vol_sd_mounted(void)
{
    return s_cur == 0 ? g_fs.mounted : s_parked.fs.mounted;
}

int vol_enter(const char *path, char *local, int size)
{
    if (!path || !local || size < 2) return FAT_ERR_INVAL;
    if (strncmp(path, USB_PREFIX, USB_PREFIX_LEN) == 0 &&
        (path[USB_PREFIX_LEN] == '\0' || path[USB_PREFIX_LEN] == '/')) {
        const char *rest = path + USB_PREFIX_LEN;

        vol_use(USB_DEV);
        if (*rest == '\0') rest = "/";
        strncpy(local, rest, (size_t)size - 1);
    } else {
        vol_use(0);
        strncpy(local, path, (size_t)size - 1);
    }
    local[size - 1] = '\0';
    return 0;
}

/* fat_sync() has written the live volume; this writes the parked one. */
void vol_sync_other(void)
{
    int was = s_cur;

    park_init();
    if (!s_parked.fs.mounted) return;
    vol_use(was ? 0 : USB_DEV);
    (void)fat_sync_here();
    vol_use(was);
}

int usbvol_mounted(void)
{
    if (s_cur == USB_DEV) return g_fs.mounted;
    return s_ready && s_parked.fs.mounted;
}

/* Lets go of the stick without writing to it again: what was not
 * written by now is lost with it. */
static void forget(void)
{
    int was = s_cur;

    vol_use(USB_DEV);
    fat_drop();
    vol_use(was);
}

void usbvol_unmount(void)
{
    int was = s_cur;

    if (usbvol_mounted() && g_usb.present) {
        vol_use(USB_DEV);
        (void)fat_sync_here();
        vol_use(was);
    }
    forget();
    usbmsc_stop();
}

/*
 * The stick src/usbdev.c found: SCSI, then FAT, and what happened, as
 * boot lines or as the rest of the answer to usb("mount").  0 when it is
 * mounted.
 */
int usbvol_attach(int boot)
{
    int was = s_cur, rc;

    forget();
    kprintf(boot ? "[boot] USB stick  : " : "USB stick: ");
    rc = usbmsc_start();
    if (rc != USBH_OK) {
        kprintf("%s\r\n", usbh_err_str(rc));
        return -1;
    }
    kput_size((uint64_t)g_usb.blocks * 512ULL);
    kprintf(" (%s %s)\r\n", g_usb.vendor, g_usb.product);
    kprintf(boot ? "[boot] filesystem : " : "");

    vol_use(USB_DEV);
    rc = fat_mount();
    if (rc == FAT_OK) {
        if (!boot) kprintf("mounted ");
        kprintf("%s", fat_type_str());
        if (g_fs.label[0]) kprintf(" \"%s\"", g_fs.label);
        if (boot) {
            kprintf(", cluster ");
            kput_size(g_fs.bytes_per_clus);
            kprintf(", mounted on " USB_PREFIX "\r\n");
        } else {
            kprintf(" on " USB_PREFIX "\r\n");
        }
    } else {
        kprintf("%s%s\r\n", boot ? "" : "mount: ", fat_err_str(rc));
        usbmsc_stop();
    }
    vol_use(was);
    return rc == FAT_OK ? 0 : -1;
}
