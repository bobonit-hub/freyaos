/*
 * Freya - the audio codec pack (make CODECS=1).
 *
 * A library a program links, for use with the audio calls in
 * freya_api.h: G.711 A-law and mu-law, Opus (libopus 1.5.2, fixed
 * point, third_party/opus, its own API in opus.h), and a writer for Ogg
 * Opus files (RFC 7845).  The kernel is not involved: a program that
 * does not link the library does not carry it.  See docs/codecs.md.
 *
 * Opus in this build allocates nothing.  A program keeps its encoder in
 * memory of its own, sized by opus_encoder_get_size(), and calls
 * opus_encoder_init(); opus_encoder_create() returns NULL.  libopus's
 * scratch is one static buffer of FREYA_OPUS_SCRATCH bytes in the
 * library, so one Opus call at a time: not from two threads, and not
 * from a pin or timer handler while the program is in another.
 */
#ifndef FREYA_CODECS_H
#define FREYA_CODECS_H

#include <stdint.h>
#include "opus.h"

/* ------------------------------------------------------------ G.711 */
/* ITU-T G.711: one 8-bit code per 16-bit sample, at 8 kHz in a call. */
uint8_t g711_alaw_encode(int16_t pcm);
int16_t g711_alaw_decode(uint8_t code);
uint8_t g711_ulaw_encode(int16_t pcm);
int16_t g711_ulaw_decode(uint8_t code);

void    g711_alaw_encode_buf(const int16_t *pcm, uint8_t *code, int n);
void    g711_alaw_decode_buf(const uint8_t *code, int16_t *pcm, int n);
void    g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *code, int n);
void    g711_ulaw_decode_buf(const uint8_t *code, int16_t *pcm, int n);

/* ------------------------------------------------------------- Opus */
/* Bytes of libopus scratch the library keeps in its .bss, make
 * OPUS_SCRATCH= (see codecs/opus/config.h).  24 KiB covers every mono
 * stream; stereo at 48 kHz needs about 40 KiB. */
#ifndef FREYA_OPUS_SCRATCH
#define FREYA_OPUS_SCRATCH      (24 * 1024)
#endif

/* An encoder or decoder lives in memory the program provides, of
 * opus_encoder_get_size(channels) or opus_decoder_get_size(channels)
 * bytes: api->malloc(), or a static buffer checked against that size. */

/* -------------------------------------------------- Ogg Opus files */
/*
 * The writer builds Ogg pages in its own struct and hands each finished
 * page to write(ctx, buf, len), which returns len, or a negative number
 * to stop.  open() writes the two header pages (OpusHead, OpusTags);
 * packet() adds one Opus packet of 'samples48' samples at 48 kHz (960
 * for 20 ms) and writes a page about once a second; close() writes what
 * is left as the last page.  All three return 0, or the negative number
 * write() gave.
 */
#define FREYA_OGG_PAGE_DATA     4096

typedef int (*freya_ogg_write_fn)(void *ctx, const void *buf, int len);

typedef struct {
    freya_ogg_write_fn write;
    void     *ctx;
    uint32_t  serial;
    uint32_t  seq;
    uint64_t  granule;          /* 48 kHz samples through the last packet */
    uint64_t  page_granule;
    int       nseg;
    int       len;
    int       packets;
    uint8_t   lacing[255];
    uint8_t   data[FREYA_OGG_PAGE_DATA];
} freya_oggopus_t;

int freya_oggopus_open(freya_oggopus_t *o, freya_ogg_write_fn write, void *ctx,
                       int channels, uint32_t input_rate, uint16_t pre_skip,
                       uint32_t serial);
int freya_oggopus_packet(freya_oggopus_t *o, const uint8_t *pkt, int len,
                         uint32_t samples48);
int freya_oggopus_close(freya_oggopus_t *o);

#endif /* FREYA_CODECS_H */
