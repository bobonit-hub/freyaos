/*
 * Freya - XMODEM for 'download' (receive) and 'upload' (send).
 *
 * Speaks XMODEM/CRC and XMODEM-1K, and falls back to the original
 * checksum protocol if the sender ignores the 'C' handshake.  Works with
 * the usual host tools: sx/sb ("sx -k file"), minicom, picocom (with
 * --send-cmd "sx -k"), Tera Term and ExtraPuTTY.
 *
 * The last packet of an XMODEM stream is padded with SUB (0x1A) because
 * the protocol has no length field; that padding is stripped unless the
 * caller asks for the raw stream.
 */
#include "freya.h"
#include "fat.h"

#define SOH     0x01        /* 128 byte packet  */
#define STX     0x02        /* 1024 byte packet */
#define EOT     0x04
#define ACK     0x06
#define NAK     0x15
#define CAN     0x18
#define SUB     0x1A

static uint16_t crc16_xmodem(const uint8_t *p, int len)
{
    uint16_t crc = 0;

    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void cancel_transfer(void)
{
    for (int i = 0; i < 8; i++) uart_putc(CAN);
    for (int i = 0; i < 8; i++) uart_putc('\b');
}

/* Drains whatever the sender still has in flight. */
static void flush_line(void)
{
    while (uart_getc_raw_timeout(300) >= 0) { }
}

/* Write up to 'exact' bytes in total.  exact < 0 writes the whole buffer. */
static int store_bytes(int fd, const uint8_t *data, int len,
                       uint32_t *total, int32_t exact)
{
    if (exact >= 0) {
        if (*total >= (uint32_t)exact) return 0;
        if (*total + (uint32_t)len > (uint32_t)exact)
            len = (int)((uint32_t)exact - *total);
    }
    if (len <= 0) return 0;
    if (fs_fd_write(fd, data, len) != len) return -1;
    *total += (uint32_t)len;
    return 0;
}

int xmodem_receive_to_file(const char *path, uint32_t *received,
                           int strip_pad, int32_t exact)
{
    uint8_t  pkt[1024];
    uint8_t  pending[1024];
    int      pending_len = 0;
    uint8_t  expect = 1;
    int      crc_mode = 1;
    int      handshakes = 0;
    int      retries = 0;
    uint32_t total = 0;
    int      fd;
    int      rc = 0;
    int      done = 0;
    char     abs[FAT_MAX_PATH];

    if (fs_abspath(path, abs, sizeof(abs)) != 0) {
        kprintf("download: path too long\r\n");
        return -1;
    }

    fd = fs_fd_open(abs, FREYA_O_RDWR | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd < 0) {
        kprintf("download: %s: %s\r\n", path, fat_err_str(fd));
        return -1;
    }

    uart_set_raw(1);
    uart_rx_flush();

    while (!done) {
        int c = uart_getc_raw_timeout(1000);

        if (c < 0) {
            /* Nothing yet: keep advertising CRC mode, then fall back. */
            if (++handshakes > 60) { rc = -2; break; }
            if (crc_mode && handshakes <= 20) uart_putc('C');
            else { crc_mode = 0; uart_putc(NAK); }
            continue;
        }

        if (c == CAN) {
            c = uart_getc_raw_timeout(1000);
            if (c == CAN) { rc = -3; break; }
            continue;
        }

        if (c == EOT) {
            uart_putc(ACK);
            done = 1;
            break;
        }

        if (c != SOH && c != STX) continue;      /* noise between packets */

        {
            int len = (c == STX) ? 1024 : 128;
            int blk, nblk, bad = 0;
            int need = len + (crc_mode ? 2 : 1);

            blk  = uart_getc_raw_timeout(1000);
            nblk = uart_getc_raw_timeout(1000);
            if (blk < 0 || nblk < 0 || ((blk + nblk) & 0xFF) != 0xFF) bad = 1;

            for (int i = 0; i < need; i++) {
                int d = uart_getc_raw_timeout(1000);
                if (d < 0) { bad = 1; break; }
                if (i < len) pkt[i] = (uint8_t)d;
                else if (i == len) pkt[len] = (uint8_t)d;      /* crc hi / sum */
                else pkt[len + 1] = (uint8_t)d;                /* crc lo       */
            }

            if (!bad) {
                if (crc_mode) {
                    uint16_t want = (uint16_t)((pkt[len] << 8) | pkt[len + 1]);
                    if (crc16_xmodem(pkt, len) != want) bad = 1;
                } else {
                    uint8_t sum = 0;
                    for (int i = 0; i < len; i++) sum = (uint8_t)(sum + pkt[i]);
                    if (sum != pkt[len]) bad = 1;
                }
            }

            if (bad) {
                if (++retries > 10) { rc = -4; break; }
                flush_line();
                uart_putc(NAK);
                continue;
            }

            if ((uint8_t)blk == (uint8_t)(expect - 1)) {
                uart_putc(ACK);                   /* duplicate, already stored */
                continue;
            }
            if ((uint8_t)blk != expect) { rc = -5; break; }

            /* Hold one packet back so the padding of the last one can go. */
            if (pending_len) {
                if (store_bytes(fd, pending, pending_len, &total, exact) != 0) {
                    rc = -6;
                    break;
                }
            }
            memcpy(pending, pkt, (size_t)len);
            pending_len = len;

            expect++;
            retries = 0;
            handshakes = 0;
            uart_putc(ACK);
        }
    }

    if (rc == 0 && pending_len) {
        if (exact < 0 && strip_pad)
            while (pending_len > 0 && pending[pending_len - 1] == SUB) pending_len--;
        if (store_bytes(fd, pending, pending_len, &total, exact) != 0) rc = -6;
    }

    if (rc != 0) cancel_transfer();
    fs_fd_close(fd);
    uart_set_raw(0);
    flush_line();
    uart_rx_flush();

    if (received) *received = total;
    if (rc != 0 && total == 0) fat_unlink(abs);
    return rc;
}

/* One packet, retried until ACK.  seq is the XMODEM block number. */
static int send_packet(const uint8_t *pkt, int len, uint8_t seq, int crc_mode)
{
    uint8_t hdr[3];
    int tries;

    hdr[0] = (len == 1024) ? STX : SOH;
    hdr[1] = seq;
    hdr[2] = (uint8_t)~seq;

    for (tries = 0; tries < 10; tries++) {
        int c;

        for (int i = 0; i < 3; i++) uart_putc((char)hdr[i]);
        for (int i = 0; i < len; i++) uart_putc((char)pkt[i]);
        if (crc_mode) {
            uint16_t crc = crc16_xmodem(pkt, len);
            uart_putc((char)(crc >> 8));
            uart_putc((char)crc);
        } else {
            uint8_t sum = 0;
            for (int i = 0; i < len; i++) sum = (uint8_t)(sum + pkt[i]);
            uart_putc((char)sum);
        }

        c = uart_getc_raw_timeout(10000);
        if (c == ACK) return 0;
        if (c == CAN) {
            c = uart_getc_raw_timeout(1000);
            if (c == CAN) return -3;
        }
    }
    return -4;
}

int xmodem_send_file(const char *path, uint32_t *sent)
{
    uint8_t pkt[1024];
    fat_dirent_t ent;
    int fd, crc_mode = -1, rc = 0;
    uint8_t seq = 1;
    uint32_t total = 0;
    int blk;

    rc = fat_stat(path, &ent);
    if (rc != FAT_OK) return -1;
    if (ent.attr & FAT_ATTR_DIR) return -1;

    fd = fs_fd_open(path, FREYA_O_RDONLY);
    if (fd < 0) return -6;

    uart_set_raw(1);
    uart_rx_flush();
    rc = 0;

    for (int i = 0; i < 60 && crc_mode < 0; i++) {
        int c = uart_getc_raw_timeout(1000);
        if (c == 'C') crc_mode = 1;
        else if (c == NAK) crc_mode = 0;
        else if (c == CAN) {
            c = uart_getc_raw_timeout(1000);
            if (c == CAN) { rc = -3; break; }
        }
    }
    if (rc == 0 && crc_mode < 0) rc = -2;

    blk = (crc_mode == 1) ? 1024 : 128;
    while (rc == 0) {
        int n = fs_fd_read(fd, pkt, blk);
        int i;

        if (n < 0) { rc = -6; break; }
        if (n == 0) break;
        for (i = n; i < blk; i++) pkt[i] = SUB;
        rc = send_packet(pkt, blk, seq, crc_mode);
        if (rc != 0) break;
        total += (uint32_t)n;
        seq++;
    }

    if (rc == 0) {
        int acked = 0;
        for (int i = 0; i < 10; i++) {
            int c;
            uart_putc(EOT);
            c = uart_getc_raw_timeout(10000);
            if (c == ACK) { acked = 1; break; }
            if (c == CAN) { rc = -3; break; }
        }
        if (rc == 0 && !acked) rc = -2;
    }

    if (rc != 0) cancel_transfer();
    fs_fd_close(fd);
    uart_set_raw(0);
    flush_line();
    uart_rx_flush();
    if (sent) *sent = total;
    return rc;
}
