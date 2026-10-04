/*
 * Freya codec pack - Ogg Opus files (RFC 7845 on RFC 3533).
 *
 * One logical stream: a page with OpusHead (beginning of stream), a page
 * with OpusTags, then pages of Opus packets.  A page's granule position
 * is the number of 48 kHz samples a decoder has produced by the end of
 * its last packet, the encoder's pre-skip included.  A page is written
 * once it holds 50 packets (a second of 20 ms frames), or when the next
 * packet would not fit; the last one carries the end-of-stream flag.
 */
#include <string.h>
#include "freya_codecs.h"

#define PAGE_PACKETS    50
#define FLAG_BOS        0x02
#define FLAG_EOS        0x04

/* Ogg's CRC-32: polynomial 0x04C11DB7, not reflected, no final XOR. */
static uint32_t crc_add(uint32_t crc, const uint8_t *p, int n)
{
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x80000000UL) ? (crc << 1) ^ 0x04C11DB7UL : crc << 1;
    }
    return crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int flush(freya_oggopus_t *o, int flags)
{
    uint8_t hdr[27 + 255];
    uint32_t crc;
    int n = 27 + o->nseg, rc;

    memcpy(hdr, "OggS", 4);
    hdr[4] = 0;
    hdr[5] = (uint8_t)flags;
    put32(hdr + 6, (uint32_t)o->page_granule);
    put32(hdr + 10, (uint32_t)(o->page_granule >> 32));
    put32(hdr + 14, o->serial);
    put32(hdr + 18, o->seq++);
    put32(hdr + 22, 0);
    hdr[26] = (uint8_t)o->nseg;
    memcpy(hdr + 27, o->lacing, (size_t)o->nseg);
    crc = crc_add(crc_add(0, hdr, n), o->data, o->len);
    put32(hdr + 22, crc);

    rc = o->write(o->ctx, hdr, n);
    if (rc < 0) return rc;
    if (o->len) {
        rc = o->write(o->ctx, o->data, o->len);
        if (rc < 0) return rc;
    }
    o->nseg = o->len = o->packets = 0;
    return 0;
}

/* Adds a packet to the page being built: its length as 255s and a
 * remainder, as Ogg's lacing has it.  The caller has made room. */
static void add(freya_oggopus_t *o, const uint8_t *pkt, int len)
{
    int left = len;

    for (;;) {
        int seg = left < 255 ? left : 255;
        o->lacing[o->nseg++] = (uint8_t)seg;
        left -= seg;
        if (seg < 255) break;
    }
    memcpy(o->data + o->len, pkt, (size_t)len);
    o->len += len;
    o->packets++;
}

static int fits(const freya_oggopus_t *o, int len)
{
    return o->nseg + len / 255 + 1 <= 255 && o->len + len <= FREYA_OGG_PAGE_DATA;
}

int freya_oggopus_open(freya_oggopus_t *o, freya_ogg_write_fn write, void *ctx,
                       int channels, uint32_t input_rate, uint16_t pre_skip,
                       uint32_t serial)
{
    static const char vendor[] = "Freya codec pack, libopus 1.5.2";
    uint8_t head[19], tags[8 + 4 + sizeof(vendor) - 1 + 4];
    int rc;

    memset(o, 0, sizeof(*o));
    o->write = write;
    o->ctx = ctx;
    o->serial = serial;

    memcpy(head, "OpusHead", 8);
    head[8] = 1;                                /* version          */
    head[9] = (uint8_t)channels;
    head[10] = (uint8_t)pre_skip;
    head[11] = (uint8_t)(pre_skip >> 8);
    put32(head + 12, input_rate);
    head[16] = 0;                               /* output gain      */
    head[17] = 0;
    head[18] = 0;                               /* mapping family 0 */
    add(o, head, sizeof(head));
    rc = flush(o, FLAG_BOS);
    if (rc < 0) return rc;

    memcpy(tags, "OpusTags", 8);
    put32(tags + 8, sizeof(vendor) - 1);
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put32(tags + 12 + sizeof(vendor) - 1, 0);   /* no comments      */
    add(o, tags, sizeof(tags));
    return flush(o, 0);
}

int freya_oggopus_packet(freya_oggopus_t *o, const uint8_t *pkt, int len,
                         uint32_t samples48)
{
    int rc;

    if (len < 0 || len > FREYA_OGG_PAGE_DATA) return -1;
    if (o->packets && !fits(o, len)) {
        rc = flush(o, 0);
        if (rc < 0) return rc;
    }
    add(o, pkt, len);
    o->granule += samples48;
    o->page_granule = o->granule;
    if (o->packets >= PAGE_PACKETS) return flush(o, 0);
    return 0;
}

int freya_oggopus_close(freya_oggopus_t *o)
{
    o->page_granule = o->granule;
    return flush(o, FLAG_EOS);
}
