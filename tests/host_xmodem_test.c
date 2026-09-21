/*
 * Host side exercise for Freya's XMODEM receiver.
 *
 * src/xmodem.c is compiled unchanged and driven by an emulated sender:
 * every byte the receiver transmits (C, NAK, ACK) is fed into a small
 * state machine that answers with real XMODEM packets.  The received
 * data is then read back through the FAT layer and compared.
 *
 * Covers CRC mode, checksum fallback, 128 and 1024 byte packets, a
 * corrupted packet that has to be retransmitted, a duplicated packet and
 * the SUB padding of the final block.
 */
#include <stdio.h>
#include <stdlib.h>

#include "freya.h"
#include "fat.h"

#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define SUB 0x1A

/* ------------------------------------------------- board stubs */
sd_info_t    g_sd;
sys_clocks_t g_clocks;
app_state_t  g_app;

static FILE *s_img;
static int   s_fail, s_checks;

uint32_t sys_ticks(void) { return 0; }
uint16_t rtc_fat_date(void) { return (uint16_t)(((2026 - 1980) << 9) | (9 << 5) | 21); }
uint16_t rtc_fat_time(void) { return 0; }
int sd_init(void) { return 0; }

int sd_read_block(uint32_t lba, uint8_t *buf)
{
    if (fseek(s_img, (long)lba * 512, SEEK_SET) != 0) return -1;
    return (fread(buf, 1, 512, s_img) == 512) ? 0 : -1;
}

int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    if (fseek(s_img, (long)lba * 512, SEEK_SET) != 0) return -1;
    return (fwrite(buf, 1, 512, s_img) == 512) ? 0 : -1;
}

int sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
    while (count--) {
        if (sd_read_block(lba++, buf) != 0) return -1;
        buf += 512;
    }
    return 0;
}

/* ------------------------------------------------- emulated sender */
#define RXQ_SIZE    (1024 * 64)

static uint8_t  s_rxq[RXQ_SIZE];
static int      s_rx_head, s_rx_tail;

static const uint8_t *s_data;       /* what the sender wants to deliver */
static int      s_data_len;
static int      s_pkt_size;         /* 128 or 1024                      */
static int      s_crc_capable;      /* 0 = sender ignores the C header  */
static int      s_crc_mode;
static int      s_cur_pkt;          /* 1 based, 0 = not started         */
static int      s_eot_sent;
static int      s_corrupt_pkt;      /* packet number to damage once     */
static int      s_corrupted;
static int      s_dup_pkt;          /* packet number to send twice      */
static int      s_duped;
static int      s_noise;            /* leading garbage before packet 1  */

static void rx_push(uint8_t b)
{
    if (s_rx_head < RXQ_SIZE) s_rxq[s_rx_head++] = b;
}

static uint16_t crc16_x(const uint8_t *p, int len)
{
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static int total_packets(void)
{
    return (s_data_len + s_pkt_size - 1) / s_pkt_size;
}

static void send_packet(int n)
{
    uint8_t body[1024], wire[1024];
    int off = (n - 1) * s_pkt_size;
    int avail = s_data_len - off;

    if (avail > s_pkt_size) avail = s_pkt_size;
    for (int i = 0; i < s_pkt_size; i++)
        body[i] = (i < avail) ? s_data[off + i] : SUB;      /* pad like sx */

    /* The check bytes always describe the intended payload; damage is
     * applied to the copy that goes on the wire, as a line glitch would. */
    memcpy(wire, body, (size_t)s_pkt_size);
    if (n == s_corrupt_pkt && !s_corrupted) {
        wire[3] ^= 0xFF;
        s_corrupted = 1;
    }

    rx_push(s_pkt_size == 1024 ? STX : SOH);
    rx_push((uint8_t)n);
    rx_push((uint8_t)~n);
    for (int i = 0; i < s_pkt_size; i++) rx_push(wire[i]);

    if (s_crc_mode) {
        uint16_t crc = crc16_x(body, s_pkt_size);
        rx_push((uint8_t)(crc >> 8));
        rx_push((uint8_t)crc);
    } else {
        uint8_t sum = 0;
        for (int i = 0; i < s_pkt_size; i++) sum = (uint8_t)(sum + body[i]);
        rx_push(sum);
    }
}

/* Every byte the receiver sends drives the sender. */
void uart_putc(char ch)
{
    uint8_t c = (uint8_t)ch;

    switch (c) {
    case 'C':
        if (!s_crc_capable) break;             /* sender does not speak CRC */
        s_crc_mode = 1;
        if (s_cur_pkt == 0) {
            if (s_noise) { rx_push(0x7E); rx_push(0x00); }
            s_cur_pkt = 1;
            send_packet(1);
        }
        break;

    case NAK:
        if (s_cur_pkt == 0) {
            s_crc_mode = 0;
            s_cur_pkt = 1;
            send_packet(1);
        } else if (!s_eot_sent) {
            send_packet(s_cur_pkt);            /* retransmit */
        }
        break;

    case ACK:
        if (s_eot_sent) break;
        if (s_cur_pkt == s_dup_pkt && !s_duped) {
            s_duped = 1;
            send_packet(s_cur_pkt);            /* the same packet again */
            break;
        }
        if (s_cur_pkt < total_packets()) {
            s_cur_pkt++;
            send_packet(s_cur_pkt);
        } else {
            s_eot_sent = 1;
            rx_push(EOT);
        }
        break;

    default:
        break;                                  /* CAN, backspaces, text */
    }
}

void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
void uart_set_raw(int raw) { (void)raw; }
void uart_rx_flush(void) { }

int uart_getc_raw_timeout(uint32_t ms)
{
    (void)ms;
    if (s_rx_tail >= s_rx_head) return -1;      /* nothing pending: timeout */
    return s_rxq[s_rx_tail++];
}

/* ------------------------------------------------------- test harness */
static void check(int cond, const char *what)
{
    s_checks++;
    if (cond) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); s_fail++; }
}

static void sender_reset(const uint8_t *data, int len, int pkt, int crc_capable)
{
    s_rx_head = s_rx_tail = 0;
    s_data = data;
    s_data_len = len;
    s_pkt_size = pkt;
    s_crc_capable = crc_capable;
    s_crc_mode = 0;
    s_cur_pkt = 0;
    s_eot_sent = 0;
    s_corrupt_pkt = 0;
    s_corrupted = 0;
    s_dup_pkt = 0;
    s_duped = 0;
    s_noise = 0;
}

static int verify_file(const char *path, const uint8_t *want, int len, int strip)
{
    static uint8_t got[70000];
    fat_file_t f;
    uint32_t n = 0;
    int expect = len;

    if (fat_open(&f, path, FAT_READ) != FAT_OK) { printf("    (cannot reopen)\n"); return 0; }
    fat_read(&f, got, sizeof(got), &n);
    fat_close(&f);

    if (!strip) {
        /* With --raw the padding up to the packet boundary stays. */
        int pkts = (len + s_pkt_size - 1) / s_pkt_size;
        expect = pkts * s_pkt_size;
    }
    if ((int)n != expect) {
        printf("    length %u, expected %d\n", n, expect);
        return 0;
    }
    if (memcmp(got, want, (size_t)len) != 0) {
        printf("    payload differs\n");
        return 0;
    }
    return 1;
}

static void scenario(const char *title, const char *path, const uint8_t *data,
                     int len, int pkt, int crc_capable, int corrupt, int dup,
                     int noise, int strip)
{
    uint32_t got = 0;
    int rc;

    printf("\n%s\n", title);
    sender_reset(data, len, pkt, crc_capable);
    s_corrupt_pkt = corrupt;
    s_dup_pkt = dup;
    s_noise = noise;

    rc = xmodem_receive_to_file(path, &got, strip);
    check(rc == 0, "  transfer completed");
    check(verify_file(path, data, len, strip), "  file content matches the source");
}

int main(int argc, char **argv)
{
    static uint8_t data[50000];

    if (argc < 2) { fprintf(stderr, "usage: hostxmodem <image>\n"); return 2; }
    s_img = fopen(argv[1], "r+b");
    if (!s_img) { perror("open image"); return 2; }

    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + (i >> 9) + 1);      /* never 0x1A at the end */
    data[sizeof(data) - 1] = 0x42;

    if (fat_mount() != FAT_OK) { printf("cannot mount image\n"); return 2; }
    printf("mounted %s\n", fat_type_str());

    scenario("128 byte packets, CRC mode", "/x128.bin", data, 300, 128, 1, 0, 0, 0, 1);
    scenario("1024 byte packets, CRC mode", "/x1k.bin", data, 5000, 1024, 1, 0, 0, 0, 1);
    scenario("checksum fallback", "/xsum.bin", data, 700, 128, 0, 0, 0, 0, 1);
    scenario("corrupted packet 3 is retransmitted", "/xbad.bin", data, 600, 128, 1, 3, 0, 0, 1);
    scenario("duplicated packet 2 is ignored", "/xdup.bin", data, 600, 128, 1, 0, 2, 0, 1);
    scenario("line noise before the first packet", "/xnoise.bin", data, 400, 128, 1, 0, 0, 1, 1);
    scenario("large transfer", "/xbig.bin", data, 50000, 1024, 1, 0, 0, 0, 1);
    scenario("--raw keeps the padding", "/xraw.bin", data, 300, 128, 1, 0, 0, 0, 0);

    printf("\nexact packet multiple (no padding at all)\n");
    {
        uint32_t got = 0;
        sender_reset(data, 1024, 1024, 1);
        check(xmodem_receive_to_file("/xexact.bin", &got, 1) == 0, "  transfer completed");
        check(verify_file("/xexact.bin", data, 1024, 1), "  file content matches the source");
    }

    fat_unmount();
    fclose(s_img);

    printf("\n%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
