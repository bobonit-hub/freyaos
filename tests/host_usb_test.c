/*
 * Host side exercise for the USB stick (USB=1).
 *
 * src/usbdev.c, src/usbmsc.c, src/usbvol.c, src/fat.c and src/fs.c
 * compiled unchanged,
 * against two disk images: the SD card at / and a simulated stick at
 * /usb.  The stick stands in for src/usbh.c: it answers the control
 * requests of enumeration and runs Bulk-Only Transport and SCSI over the
 * image, checking the data toggles it is sent.  Files are written on
 * both volumes at once, so every write swaps the live volume.
 *
 *   ./hostusb sd.img stick.img
 */
#include <stdio.h>
#include <stdlib.h>

#include "freya.h"
#include "fat.h"

/* ------------------------------------------------- board stubs */
sd_info_t    g_sd;
sys_clocks_t g_clocks;
app_state_t  g_app;

static FILE *s_sd;
static int   s_fail;
static int   s_checks;
static uint32_t s_ticks;

void uart_putc(char c) { fputc(c, stdout); }
void uart_puts(const char *s) { fputs(s, stdout); }

uint32_t sys_ticks(void) { return s_ticks += 10; }
void sys_delay_ms(uint32_t ms) { s_ticks += ms; }
uint16_t rtc_fat_date(void) { return (uint16_t)(((2026 - 1980) << 9) | (10 << 5) | 4); }
uint16_t rtc_fat_time(void) { return (uint16_t)((12 << 11) | (0 << 5)); }

int sd_init(void) { g_sd.initialised = 1; return 0; }

static int img_read(FILE *f, uint32_t lba, uint8_t *buf)
{
    if (fseek(f, (long)lba * 512, SEEK_SET) != 0) return -1;
    return (fread(buf, 1, 512, f) == 512) ? 0 : -1;
}

static int img_write(FILE *f, uint32_t lba, const uint8_t *buf)
{
    if (fseek(f, (long)lba * 512, SEEK_SET) != 0) return -1;
    return (fwrite(buf, 1, 512, f) == 512) ? 0 : -1;
}

int sd_read_block(uint32_t lba, uint8_t *buf) { return img_read(s_sd, lba, buf); }
int sd_write_block(uint32_t lba, const uint8_t *buf) { return img_write(s_sd, lba, buf); }

static void check(int cond, const char *what)
{
    s_checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        s_fail++;
    }
}

/* ------------------------------------------------- the simulated stick */
static FILE    *s_img;
static uint32_t s_blocks;
static uint32_t s_block_size = 512;
static int      s_plugged = 1;
static int      s_is_msc = 1;
static int      s_not_ready = 1;    /* the first TEST UNIT READY fails */
static int      s_tur_count;
static int      s_sense_asked;
static uint8_t  s_addr;
static uint8_t  s_tog_in, s_tog_out;

static enum { WANT_CBW, WANT_DATA, SEND_DATA, SEND_CSW } s_state;
static uint8_t  s_resp[512];
static uint32_t s_resp_len;
static uint32_t s_tag;
static uint8_t  s_status;
static uint8_t  s_sense;
static uint32_t s_wr_lba;

static const uint8_t s_dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 1, 2, 3, 1
};

static const uint8_t s_cfg_msc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, 0x81, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0
};

static const uint8_t s_cfg_hid[25] = {
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 0x03, 0x01, 0x02, 0,
    7, 5, 0x81, 3, 8, 0, 10
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

int usbh_open(uint32_t wait_ms)
{
    (void)wait_ms;
    s_addr = 0;
    s_state = WANT_CBW;
    return s_plugged ? USBH_OK : USBH_NODEV;
}

void usbh_close(void) { }
int usbh_connected(void) { return s_plugged; }

int usbh_control(uint8_t addr, uint8_t mps0, const uint8_t setup[8],
                 void *data, uint16_t *len)
{
    uint16_t value = (uint16_t)(setup[2] | (setup[3] << 8));
    uint16_t want = (uint16_t)(setup[6] | (setup[7] << 8));
    const uint8_t *src = NULL;
    uint16_t n = 0;

    (void)mps0;
    if (!s_plugged) return USBH_GONE;
    if (addr != s_addr) return USBH_TIMEOUT;     /* nobody at that address */
    if (len && *len < want) want = *len;

    if (setup[0] == 0x80 && setup[1] == 6) {
        static uint8_t str[64];
        const char *text = (value & 0xFF) == 1 ? "FREYA" : "SimStick";
        if ((value >> 8) == 3) {
            int len = (int)strlen(text);
            str[0] = (uint8_t)(2 + 2 * len);
            str[1] = 3;
            for (int i = 0; i < len; i++) { str[2 + 2 * i] = (uint8_t)text[i]; str[3 + 2 * i] = 0; }
            src = str;
            n = str[0];
        } else if ((value >> 8) == 1) { src = s_dev_desc; n = sizeof(s_dev_desc); }
        else if ((value >> 8) == 2 && s_is_msc) { src = s_cfg_msc; n = sizeof(s_cfg_msc); }
        else if ((value >> 8) == 2) { src = s_cfg_hid; n = sizeof(s_cfg_hid); }
        else return USBH_STALL;
        if (n > want) n = want;
        memcpy(data, src, n);
    } else if (setup[0] == 0x00 && setup[1] == 5) {
        s_addr = (uint8_t)value;
    } else if (setup[0] == 0x00 && setup[1] == 9) {
        s_tog_in = s_tog_out = 0;
    } else if (setup[0] == 0xA1 && setup[1] == 0xFE) {
        return USBH_STALL;                      /* as many sticks do */
    } else if (setup[0] == 0x02 && setup[1] == 1) {
        if (setup[4] & 0x80) s_tog_in = 0; else s_tog_out = 0;
    } else if (setup[0] == 0x21 && setup[1] == 0xFF) {
        s_state = WANT_CBW;
    } else {
        return USBH_STALL;
    }
    if (len) *len = n;
    return USBH_OK;
}

static void command(const uint8_t *cbw)
{
    const uint8_t *cb = cbw + 15;

    s_tag = (uint32_t)cbw[4] | ((uint32_t)cbw[5] << 8) |
            ((uint32_t)cbw[6] << 16) | ((uint32_t)cbw[7] << 24);
    s_status = 0;
    s_resp_len = 0;
    s_state = SEND_CSW;

    switch (cb[0]) {
    case 0x12:                                  /* INQUIRY */
        memset(s_resp, 0, 36);
        memcpy(s_resp + 8, "FREYA   SimStick        ", 24);
        s_resp_len = 36;
        break;
    case 0x00:                                  /* TEST UNIT READY */
        s_tur_count++;
        if (s_not_ready) { s_not_ready = 0; s_status = 1; s_sense = 2; }
        break;
    case 0x03:                                  /* REQUEST SENSE */
        s_sense_asked++;
        memset(s_resp, 0, 18);
        s_resp[0] = 0x70;
        s_resp[2] = s_sense;
        s_resp_len = 18;
        s_sense = 0;
        break;
    case 0x25:                                  /* READ CAPACITY(10) */
        put_be32(s_resp, s_blocks - 1);
        put_be32(s_resp + 4, s_block_size);
        s_resp_len = 8;
        break;
    case 0x28:                                  /* READ(10), one block */
        if (be32(cb + 2) >= s_blocks || img_read(s_img, be32(cb + 2), s_resp) != 0) {
            s_status = 1;
            break;
        }
        s_resp_len = 512;
        break;
    case 0x2A:                                  /* WRITE(10), one block */
        s_wr_lba = be32(cb + 2);
        s_state = WANT_DATA;
        return;
    default:
        s_status = 1;
        s_sense = 5;                            /* ILLEGAL REQUEST */
        return;
    }
    if (s_resp_len) s_state = SEND_DATA;
}

/* Every packet flips the toggle; the stick checks the host started on
 * the one it expects, as a real one would silently drop the rest. */
static int toggles(uint8_t *host, uint8_t *dev, uint32_t len, uint16_t mps)
{
    uint32_t pkts = len ? (len + mps - 1) / mps : 1;

    if (*host != *dev) {
        printf("  FAIL  data toggle: host %u, stick %u\n", *host, *dev);
        s_fail++;
        return -1;
    }
    if (pkts & 1) { *host ^= 1; *dev ^= 1; }
    return 0;
}

int usbh_bulk(uint8_t addr, uint8_t ep, uint16_t mps, uint8_t *toggle,
              void *buf, uint32_t len, uint32_t *done, uint32_t wait_ms)
{
    uint32_t n;

    (void)wait_ms;
    if (done) *done = 0;
    if (!s_plugged) return USBH_GONE;
    if (addr != s_addr) return USBH_TIMEOUT;

    if (ep == 0x02) {
        if (toggles(toggle, &s_tog_out, len, mps) != 0) return USBH_ERR;
        if (s_state == WANT_CBW) {
            const uint8_t *c = buf;
            if (len != 31 || c[0] != 'U' || c[1] != 'S' || c[2] != 'B' || c[3] != 'C')
                return USBH_STALL;
            command(c);
        } else if (s_state == WANT_DATA) {
            if (len != 512 || img_write(s_img, s_wr_lba, buf) != 0) s_status = 1;
            s_state = SEND_CSW;
        } else {
            return USBH_STALL;
        }
        if (done) *done = len;
        return USBH_OK;
    }
    if (ep == 0x81) {
        if (s_state == SEND_DATA) {
            n = s_resp_len < len ? s_resp_len : len;
            if (toggles(toggle, &s_tog_in, n, mps) != 0) return USBH_ERR;
            memcpy(buf, s_resp, n);
            s_state = SEND_CSW;
        } else if (s_state == SEND_CSW) {
            uint8_t *c = buf;
            if (len < 13) return USBH_ERR;
            if (toggles(toggle, &s_tog_in, 13, mps) != 0) return USBH_ERR;
            memset(c, 0, 13);
            c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'S';
            c[4] = (uint8_t)s_tag; c[5] = (uint8_t)(s_tag >> 8);
            c[6] = (uint8_t)(s_tag >> 16); c[7] = (uint8_t)(s_tag >> 24);
            c[12] = s_status;
            n = 13;
            s_state = WANT_CBW;
        } else {
            return USBH_STALL;
        }
        if (done) *done = n;
        return USBH_OK;
    }
    return USBH_STALL;
}

/* ------------------------------------------------- the tests */
static void pattern(uint8_t *buf, int n, int seed)
{
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)(seed * 31 + i * 7);
}

static int read_back(const char *path, int chunks, int seed)
{
    fat_file_t f;
    uint8_t want[100], got[100];
    uint32_t n;
    int ok = 1;

    if (fat_open(&f, path, FAT_READ) != FAT_OK) return 0;
    for (int i = 0; i < chunks; i++) {
        pattern(want, sizeof(want), seed + i);
        if (fat_read(&f, got, sizeof(got), &n) != FAT_OK || n != sizeof(got) ||
            memcmp(want, got, sizeof(got)) != 0) { ok = 0; break; }
    }
    fat_close(&f);
    return ok;
}

static void test_refusals(void)
{
    printf("\n--- what is refused ---\n");
    s_plugged = 0;
    check(usbdev_open(10) == USBH_NODEV, "an empty socket is no device");
    check(usb_attach(0) != 0 && g_usbdev.kind == USB_KIND_NONE,
          "and attaching it says so");
    s_plugged = 1;

    s_is_msc = 0;
    check(usbdev_open(10) == USBH_OK && g_usbdev.kind == USB_KIND_OTHER,
          "a keyboard enumerates, as neither a stick nor a headset");
    check(g_usbdev.vid == 0x1234 && g_usbdev.pid == 0x5678,
          "its VID:PID is read");
    check(strcmp(usbdev_name(), "FREYA SimStick") == 0,
          "its string descriptors name it");
    check(usb_attach(0) != 0 && g_usbdev.kind == USB_KIND_NONE,
          "attaching it is refused and lets go");
    s_is_msc = 1;

    s_block_size = 4096;
    check(usbdev_open(10) == USBH_OK && g_usbdev.kind == USB_KIND_MSC &&
          usbmsc_start() == USBH_UNSUP, "4096 byte blocks are refused");
    check(!g_usb.present, "a refused stick is not present");
    usbdev_close();
    s_block_size = 512;
}

int main(int argc, char **argv)
{
    enum { CHUNKS = 120 };
    fat_file_t a, b;
    fat_dirent_t e;
    fat_dir_t d;
    uint8_t buf[100];
    uint32_t n;
    int rc, saw = 0, fd;

    if (argc != 3) {
        fprintf(stderr, "usage: %s sd.img stick.img\n", argv[0]);
        return 2;
    }
    s_sd = fopen(argv[1], "r+b");
    s_img = fopen(argv[2], "r+b");
    if (!s_sd || !s_img) { perror("open"); return 2; }
    fseek(s_img, 0, SEEK_END);
    s_blocks = (uint32_t)(ftell(s_img) / 512);

    printf("USB stick\n");
    test_refusals();

    printf("\n--- the card and the stick ---\n");
    s_not_ready = 1;
    s_tur_count = s_sense_asked = 0;
    check(fat_mount() == FAT_OK, "the card mounts on /");
    check(usb_attach(0) == 0, "the stick mounts on /usb");
    check(s_tur_count >= 2 && s_sense_asked >= 1,
          "a stick that is not ready yet is asked for sense and again");
    check(g_usb.blocks == s_blocks, "READ CAPACITY gives the image size");
    check(strcmp(g_usb.vendor, "FREYA") == 0 && strcmp(g_usb.product, "SimStick") == 0,
          "INQUIRY names the stick");
    check(fat_mounted() && usbvol_mounted(), "both are mounted");
    check(fat_stat("/usb", &e) == FAT_OK && (e.attr & FAT_ATTR_DIR),
          "/usb is the stick's root directory");

    printf("\n--- one file on each, written in turns ---\n");
    rc = fat_open(&a, "/usb/ONE.TXT", FAT_WRITE | FAT_CREATE | FAT_TRUNC);
    check(rc == FAT_OK, "create /usb/ONE.TXT");
    rc = fat_open(&b, "/Two on the card.txt", FAT_WRITE | FAT_CREATE | FAT_TRUNC);
    check(rc == FAT_OK, "create /Two on the card.txt");
    rc = FAT_OK;
    for (int i = 0; i < CHUNKS && rc == FAT_OK; i++) {
        pattern(buf, sizeof(buf), 100 + i);
        rc = fat_write(&a, buf, sizeof(buf), &n);
        pattern(buf, sizeof(buf), 500 + i);
        if (rc == FAT_OK) rc = fat_write(&b, buf, sizeof(buf), &n);
    }
    check(rc == FAT_OK, "120 writes of 100 bytes to each, alternating");
    check(fat_close(&a) == FAT_OK && fat_close(&b) == FAT_OK, "close both");
    check(read_back("/usb/ONE.TXT", CHUNKS, 100), "the stick's file reads back");
    check(read_back("/Two on the card.txt", CHUNKS, 500), "the card's file reads back");
    check(fat_stat("/ONE.TXT", &e) == FAT_ERR_NOENT, "the stick's file is not on the card");

    printf("\n--- directories, names and the working directory ---\n");
    check(fat_mkdir("/usb/sub") == FAT_OK, "mkdir /usb/sub");
    check(fat_stat("/usb/sub", &e) == FAT_OK && (e.attr & FAT_ATTR_DIR), "it is a directory");
    check(fat_rename("/usb/ONE.TXT", "/usb/sub/one.txt") == FAT_OK, "rename inside the stick");
    check(fat_rename("/usb/sub/one.txt", "/one.txt") == FAT_ERR_INVAL,
          "rename from the stick to the card is refused");
    if (fat_opendir(&d, "/usb/sub") == FAT_OK) {
        while (fat_readdir(&d, &e) == 0)
            if (strcmp(e.name, "one.txt") == 0) saw = 1;
        fat_closedir(&d);
    }
    check(saw, "the renamed file is listed in /usb/sub");
    check(fs_chdir("/usb/sub") == FAT_OK, "cd /usb/sub");
    fd = fs_fd_open("rel.txt", FAT_WRITE | FAT_CREATE | FAT_TRUNC);
    check(fd >= 0 && fs_fd_write(fd, "relative", 8) == 8 && fs_fd_close(fd) == FAT_OK,
          "a relative path lands on the stick");
    check(fat_stat("/usb/sub/rel.txt", &e) == FAT_OK && e.size == 8, "/usb/sub/rel.txt");
    check(fs_chdir("/") == FAT_OK, "cd / is the card again");

    printf("\n--- eject and mount again ---\n");
    usb_detach();
    check(!usbvol_mounted() && fat_mounted(), "the stick is gone, the card is not");
    check(fat_stat("/usb/sub/one.txt", &e) == FAT_ERR_NOFS, "/usb has no filesystem");
    check(read_back("/Two on the card.txt", CHUNKS, 500), "the card still reads");
    check(usb_attach(0) == 0, "mount the stick again");
    check(read_back("/usb/sub/one.txt", CHUNKS, 100), "what was written is still there");

    printf("\n--- unplugged while mounted ---\n");
    s_plugged = 0;
    rc = fat_open(&a, "/usb/sub/one.txt", FAT_READ);
    if (rc == FAT_OK) {
        rc = fat_read(&a, buf, sizeof(buf), &n);
        fat_close(&a);
    }
    check(rc != FAT_OK, "reading the stick fails, and returns");
    check(!g_usb.present, "the stick is marked gone");
    check(read_back("/Two on the card.txt", CHUNKS, 500), "the card is not disturbed");
    usb_detach();
    s_plugged = 1;
    check(!g_usb.present && !usbvol_mounted(), "eject after the fact is clean");
    check(usb_attach(0) == 0, "plugged in again, it mounts");

    usb_detach();
    fat_unmount();
    fclose(s_img);
    fclose(s_sd);
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
