/*
 * Freya - a USB mass storage stick (USB=1).
 *
 * src/usbdev.c has enumerated and configured the device.  This takes the
 * first interface that is mass storage, SCSI transparent command set,
 * Bulk-Only Transport, and talks to its LUN 0: INQUIRY, TEST UNIT READY, READ CAPACITY(10), and
 * READ(10) / WRITE(10) one 512 byte block at a time.  A stick with any
 * other block size is refused.
 *
 * Everything below goes through usbdev_control() and usbh_bulk(), so the
 * host test drives this file against a simulated stick.
 */
#include "freya.h"

#ifndef FREYA_USB
#error "src/usbmsc.c is compiled only with USB=1"
#endif

usb_msc_t g_usb;

#define DATA_MS         5000    /* a stick may NAK while flash is busy */
#define READY_MS        5000    /* spin-up after power                 */

#define CBW_SIG         0x43425355UL    /* "USBC" */
#define CSW_SIG         0x53425355UL    /* "USBS" */

#define REQ_CLEAR_FEATURE   1
#define REQ_BOT_RESET       0xFF
#define REQ_GET_MAX_LUN     0xFE

#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY10    0x25
#define SCSI_READ10             0x28
#define SCSI_WRITE10            0x2A

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int clear_halt(uint8_t ep)
{
    if (ep & 0x80) g_usb.tog_in = 0;
    else g_usb.tog_out = 0;
    return usbdev_control(0x02, REQ_CLEAR_FEATURE, 0, ep, NULL, 0, NULL);
}

static void reset_recovery(void)
{
    (void)usbdev_control(0x21, REQ_BOT_RESET, 0, g_usb.iface, NULL, 0, NULL);
    (void)clear_halt(g_usb.ep_in);
    (void)clear_halt(g_usb.ep_out);
}

static int bulk_in(void *buf, uint32_t len, uint32_t *done)
{
    return usbh_bulk(g_usbdev.addr, g_usb.ep_in, g_usb.mps_in, &g_usb.tog_in,
                     buf, len, done, DATA_MS);
}

static int bulk_out(const void *buf, uint32_t len)
{
    return usbh_bulk(g_usbdev.addr, g_usb.ep_out, g_usb.mps_out, &g_usb.tog_out,
                     (void *)buf, len, NULL, DATA_MS);
}

/*
 * One Bulk-Only command: the CBW, the data stage, the CSW.  Returns 0 when
 * the command passed, 1 when the stick reported it failed, or USBH_*.
 */
static int bot(const uint8_t *cb, uint8_t cblen, int in, void *data,
               uint32_t len)
{
    uint8_t cbw[31], csw[13];
    uint32_t done = 0;
    int rc;

    memset(cbw, 0, sizeof(cbw));
    put32le(cbw, CBW_SIG);
    put32le(cbw + 4, ++g_usb.tag);
    put32le(cbw + 8, len);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = g_usb.lun;
    cbw[14] = cblen;
    memcpy(cbw + 15, cb, cblen);

    rc = bulk_out(cbw, sizeof(cbw));
    if (rc == USBH_STALL) reset_recovery();
    if (rc != USBH_OK) return rc;

    if (len) {
        rc = in ? bulk_in(data, len, &done) : bulk_out(data, len);
        if (rc == USBH_STALL)
            (void)clear_halt(in ? g_usb.ep_in : g_usb.ep_out);
        else if (rc != USBH_OK)
            return rc;
    }

    rc = bulk_in(csw, sizeof(csw), &done);
    if (rc == USBH_STALL) {
        (void)clear_halt(g_usb.ep_in);
        rc = bulk_in(csw, sizeof(csw), &done);
    }
    if (rc != USBH_OK) return rc;
    if (done != sizeof(csw) || get32le(csw) != CSW_SIG ||
        get32le(csw + 4) != g_usb.tag || csw[12] == 2) {
        reset_recovery();               /* phase error, or garbage      */
        return USBH_ERR;
    }
    return csw[12] == 0 ? 0 : 1;
}

static int scsi(uint8_t op, uint32_t lba, uint16_t count, int in,
                void *data, uint32_t len)
{
    uint8_t cb[10];
    uint8_t cblen = 6;

    memset(cb, 0, sizeof(cb));
    cb[0] = op;
    if (op == SCSI_READ10 || op == SCSI_WRITE10 || op == SCSI_READ_CAPACITY10) {
        cblen = 10;
        cb[2] = (uint8_t)(lba >> 24); cb[3] = (uint8_t)(lba >> 16);
        cb[4] = (uint8_t)(lba >> 8);  cb[5] = (uint8_t)lba;
        cb[7] = (uint8_t)(count >> 8); cb[8] = (uint8_t)count;
    } else {
        cb[4] = (uint8_t)len;           /* allocation length */
    }
    return bot(cb, cblen, in, data, len);
}

/* After a failed command the stick holds sense data until it is asked. */
static int scsi_checked(uint8_t op, uint32_t lba, uint16_t count, int in,
                        void *data, uint32_t len)
{
    uint8_t sense[18];
    int rc = scsi(op, lba, count, in, data, len);

    if (rc == 1) {
        (void)scsi(SCSI_REQUEST_SENSE, 0, 0, 1, sense, sizeof(sense));
        return USBH_ERR;
    }
    return rc;
}

static void copy_text(char *dst, const uint8_t *src, int n)
{
    int i;

    for (i = 0; i < n; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    while (i > 0 && dst[i - 1] == ' ') i--;
    dst[i] = '\0';
}

/* Walks a configuration descriptor for a Bulk-Only SCSI interface. */
static int find_interface(const uint8_t *cfg, uint16_t total)
{
    int found = 0;

    for (uint16_t i = 0; i + 2 <= total && cfg[i] >= 2; i += cfg[i]) {
        const uint8_t *d = cfg + i;

        if (i + d[0] > total) break;
        if (d[1] == 4 && d[0] >= 9) {               /* interface        */
            if (found) break;                       /* the one before   */
            if (d[5] == 0x08 && d[6] == 0x06 && d[7] == 0x50) {
                found = 1;
                g_usb.iface = d[2];
                g_usb.ep_in = g_usb.ep_out = 0;
            }
        } else if (found && d[1] == 5 && d[0] >= 7 && (d[3] & 3) == 2) {
            uint16_t mps = (uint16_t)(d[4] | (d[5] << 8));
            if (d[2] & 0x80) { g_usb.ep_in = d[2];  g_usb.mps_in = mps; }
            else             { g_usb.ep_out = d[2]; g_usb.mps_out = mps; }
        }
    }
    if (!found || !g_usb.ep_in || !g_usb.ep_out) return USBH_UNSUP;
    if (g_usb.mps_in == 0 || g_usb.mps_in > 64 ||
        g_usb.mps_out == 0 || g_usb.mps_out > 64) return USBH_UNSUP;
    return USBH_OK;
}

static int scsi_start(void)
{
    uint8_t buf[36];
    uint32_t start, size;
    int rc;

    rc = scsi_checked(SCSI_INQUIRY, 0, 0, 1, buf, sizeof(buf));
    if (rc != USBH_OK) return rc;
    copy_text(g_usb.vendor, buf + 8, 8);
    copy_text(g_usb.product, buf + 16, 16);

    start = sys_ticks();
    for (;;) {
        rc = scsi_checked(SCSI_TEST_UNIT_READY, 0, 0, 0, NULL, 0);
        if (rc == USBH_OK) break;
        if (rc != USBH_ERR || (uint32_t)(sys_ticks() - start) > READY_MS)
            return rc;
        sys_delay_ms(100);
    }

    rc = scsi_checked(SCSI_READ_CAPACITY10, 0, 0, 1, buf, 8);
    if (rc != USBH_OK) return rc;
    size = get32be(buf + 4);
    if (size != 512) return USBH_UNSUP;
    g_usb.blocks = get32be(buf) + 1;
    return USBH_OK;
}

void usbmsc_stop(void)
{
    memset(&g_usb, 0, sizeof(g_usb));
}

int usbmsc_start(void)
{
    uint8_t lun;
    uint16_t got;
    int rc;

    usbmsc_stop();
    rc = find_interface(g_usbdev.cfg, g_usbdev.cfg_len);
    if (rc != USBH_OK) return rc;

    /* Many sticks stall this; that means one LUN. */
    (void)usbdev_control(0xA1, REQ_GET_MAX_LUN, 0, g_usb.iface, &lun, 1, &got);
    g_usb.lun = 0;

    rc = scsi_start();
    if (rc != USBH_OK) {
        usbmsc_stop();
        return rc;
    }
    g_usb.present = 1;
    return USBH_OK;
}

static int block_io(uint8_t op, uint32_t lba, uint8_t *buf)
{
    int rc;

    if (!g_usb.present) return -1;
    if (lba >= g_usb.blocks) return -1;
    rc = scsi_checked(op, lba, 1, op == SCSI_READ10, buf, 512);
    if (rc == USBH_GONE) g_usb.present = 0;
    return rc == USBH_OK ? 0 : -1;
}

int usbmsc_read_block(uint32_t lba, uint8_t *buf)
{
    return block_io(SCSI_READ10, lba, buf);
}

int usbmsc_write_block(uint32_t lba, const uint8_t *buf)
{
    return block_io(SCSI_WRITE10, lba, (uint8_t *)buf);
}
