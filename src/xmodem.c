/*
 * Freya - XMODEM receiver for the 'download' command.
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

int xmodem_receive_to_file(const char *path, uint32_t *received, int strip_pad)
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
                if (fs_fd_write(fd, pending, pending_len) != pending_len) {
                    rc = -6;
                    break;
                }
                total += (uint32_t)pending_len;
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
        if (strip_pad)
            while (pending_len > 0 && pending[pending_len - 1] == SUB) pending_len--;
        if (fs_fd_write(fd, pending, pending_len) != pending_len) rc = -6;
        else total += (uint32_t)pending_len;
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
