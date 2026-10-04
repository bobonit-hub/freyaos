/*
 * Freya - the one device on the USB port (USB=1).
 *
 * Enumerates whatever is plugged in: its device descriptor, an address,
 * its first configuration and the names in its string descriptors, and
 * then sets that configuration.  What the configuration holds decides
 * who takes the device: a Bulk-Only mass storage interface is a stick
 * (src/usbmsc.c, mounted by src/usbvol.c), an audio streaming interface
 * a headset (src/uac.c, with AUDIO=1).  There is no hub, so there is
 * only ever one.
 *
 * Everything goes through usbh_control(), so the host tests drive this
 * file against simulated devices.
 */
#include "freya.h"

#ifndef FREYA_USB
#error "src/usbdev.c is compiled only with USB=1"
#endif

usb_dev_t g_usbdev;

#define ADDR                1   /* the only device there is            */

#define REQ_GET_DESCRIPTOR  6
#define REQ_SET_ADDRESS     5
#define REQ_SET_CONFIG      9
#define LANG_EN_US          0x0409

/* How long the port waits for a device to connect.  One that is already
 * in the socket, on VBUS, is there at once. */
#define BOOT_WAIT_MS        300
#define ATTACH_WAIT_MS      1500

const char *usbh_err_str(int err)
{
    switch (err) {
    case USBH_OK:      return "ok";
    case USBH_STALL:   return "stalled";
    case USBH_TIMEOUT: return "no answer";
    case USBH_ERR:     return "transfer error";
    case USBH_GONE:    return "unplugged";
    case USBH_NODEV:   return "nothing in the socket";
    case USBH_UNSUP:   return "not a device Freya can use";
    default:           return "unknown error";
    }
}

int usbdev_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                   void *data, uint16_t len, uint16_t *got)
{
    uint8_t setup[8] = {
        type, req, (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8)
    };
    uint16_t n = len;
    int rc = usbh_control(g_usbdev.addr, g_usbdev.mps0, setup, data, &n);

    if (got) *got = n;
    return rc;
}

/* A string descriptor as ASCII; anything else becomes '?'.  A device
 * without strings, or one that stalls the request, leaves "". */
static void get_string(uint8_t index, char *out, int size)
{
    uint8_t buf[64];
    uint16_t got = 0;
    int n = 0;

    out[0] = '\0';
    if (!index) return;
    if (usbdev_control(0x80, REQ_GET_DESCRIPTOR, (uint16_t)(0x0300 | index),
                       LANG_EN_US, buf, sizeof(buf), &got) != USBH_OK)
        return;
    if (got < 2 || buf[1] != 3) return;
    if (buf[0] < got) got = buf[0];
    for (uint16_t i = 2; i + 1 < got && n < size - 1; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

/* What the configuration is: the first interface that names a class
 * Freya speaks wins. */
static uint8_t classify(const uint8_t *cfg, uint16_t len)
{
    uint8_t kind = USB_KIND_OTHER;

    for (uint16_t i = 0; i + 2 <= len && cfg[i] >= 2; i += cfg[i]) {
        const uint8_t *d = cfg + i;

        if (i + d[0] > len) break;
        if (d[1] != 4 || d[0] < 9) continue;
        if (d[5] == 0x08 && d[6] == 0x06 && d[7] == 0x50) return USB_KIND_MSC;
        if (d[5] == 0x01 && d[6] == 0x02) kind = USB_KIND_AUDIO;
    }
    return kind;
}

static int enumerate(void)
{
    uint8_t dev[18];
    uint16_t got, total;
    int rc;

    g_usbdev.addr = 0;
    g_usbdev.mps0 = 64;
    rc = usbdev_control(0x80, REQ_GET_DESCRIPTOR, 0x0100, 0, dev, 8, &got);
    if (rc != USBH_OK) return rc;
    if (got < 8 || dev[1] != 1) return USBH_UNSUP;
    g_usbdev.mps0 = dev[7];
    if (g_usbdev.mps0 != 8 && g_usbdev.mps0 != 16 && g_usbdev.mps0 != 32 &&
        g_usbdev.mps0 != 64) return USBH_UNSUP;

    rc = usbdev_control(0x00, REQ_SET_ADDRESS, ADDR, 0, NULL, 0, NULL);
    if (rc != USBH_OK) return rc;
    sys_delay_ms(10);
    g_usbdev.addr = ADDR;

    rc = usbdev_control(0x80, REQ_GET_DESCRIPTOR, 0x0100, 0, dev, 18, &got);
    if (rc != USBH_OK) return rc;
    if (got < 18) return USBH_UNSUP;
    g_usbdev.vid = (uint16_t)(dev[8] | (dev[9] << 8));
    g_usbdev.pid = (uint16_t)(dev[10] | (dev[11] << 8));

    rc = usbdev_control(0x80, REQ_GET_DESCRIPTOR, 0x0200, 0, g_usbdev.cfg, 9, &got);
    if (rc != USBH_OK) return rc;
    if (got < 9 || g_usbdev.cfg[1] != 2) return USBH_UNSUP;
    total = (uint16_t)(g_usbdev.cfg[2] | (g_usbdev.cfg[3] << 8));
    if (total > sizeof(g_usbdev.cfg)) total = sizeof(g_usbdev.cfg);
    rc = usbdev_control(0x80, REQ_GET_DESCRIPTOR, 0x0200, 0, g_usbdev.cfg,
                        total, &got);
    if (rc != USBH_OK) return rc;
    g_usbdev.cfg_len = got;
    g_usbdev.config = g_usbdev.cfg[5];

    get_string(dev[14], g_usbdev.maker, sizeof(g_usbdev.maker));
    get_string(dev[15], g_usbdev.product, sizeof(g_usbdev.product));

    rc = usbdev_control(0x00, REQ_SET_CONFIG, g_usbdev.config, 0, NULL, 0, NULL);
    if (rc != USBH_OK) return rc;
    g_usbdev.kind = classify(g_usbdev.cfg, g_usbdev.cfg_len);
    return USBH_OK;
}

void usbdev_close(void)
{
    usbh_close();
    memset(&g_usbdev, 0, sizeof(g_usbdev));
}

int usbdev_open(uint32_t wait_ms)
{
    int rc;

    usbdev_close();
    rc = usbh_open(wait_ms);
    if (rc == USBH_OK) rc = enumerate();
    return rc;
}

/* "Maker Product", or what there is of it, or the VID:PID alone. */
const char *usbdev_name(void)
{
    static char name[sizeof(g_usbdev.maker) + sizeof(g_usbdev.product)];

    if (g_usbdev.maker[0] && g_usbdev.product[0] &&
        strncmp(g_usbdev.product, g_usbdev.maker, strlen(g_usbdev.maker)) != 0)
        ksnprintf(name, sizeof(name), "%s %s", g_usbdev.maker, g_usbdev.product);
    else if (g_usbdev.product[0])
        ksnprintf(name, sizeof(name), "%s", g_usbdev.product);
    else if (g_usbdev.maker[0])
        ksnprintf(name, sizeof(name), "%s", g_usbdev.maker);
    else
        ksnprintf(name, sizeof(name), "%04x:%04x", g_usbdev.vid, g_usbdev.pid);
    return name;
}

/* Lets go of whatever is on the port, writing out what a stick has
 * cached and stopping a headset's streams first. */
void usb_detach(void)
{
    if (g_usbdev.kind == USB_KIND_MSC) usbvol_unmount();
#ifdef FREYA_AUDIO
    if (g_usbdev.kind == USB_KIND_AUDIO) uac_stop();
#endif
    usbdev_close();
}

/*
 * Finds the device, hands it to its class, and says what happened: as
 * boot lines, or as the answer to the shell's usb("mount").  0 when the
 * device is in use.
 */
int usb_attach(int boot)
{
    const char *lead = boot ? "[boot] USB        : " : "USB: ";
    int rc;

    usb_detach();
    rc = usbdev_open(boot ? BOOT_WAIT_MS : ATTACH_WAIT_MS);
    if (rc != USBH_OK) {
        kprintf("%s%s", lead, usbh_err_str(rc));
        if (g_usbdev.vid) kprintf(" (%04x:%04x)", g_usbdev.vid, g_usbdev.pid);
        kprintf("\r\n");
        usbdev_close();
        return -1;
    }
    kprintf("%s%s (%04x:%04x)\r\n", lead, usbdev_name(), g_usbdev.vid,
            g_usbdev.pid);

    switch (g_usbdev.kind) {
    case USB_KIND_MSC:
        rc = usbvol_attach(boot);
        break;
#ifdef FREYA_AUDIO
    case USB_KIND_AUDIO:
        rc = uac_attach(boot);
        break;
#endif
#ifndef FREYA_AUDIO
    case USB_KIND_AUDIO:
        kprintf("%sa headset, and this kernel has no AUDIO=1\r\n", lead);
        rc = -1;
        break;
#endif
    default:
        kprintf("%sneither a stick nor a headset\r\n", lead);
        rc = -1;
        break;
    }
    if (rc != 0) usbdev_close();
    return rc;
}
