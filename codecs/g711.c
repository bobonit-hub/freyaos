/*
 * Freya codec pack - ITU-T G.711 A-law and mu-law.
 *
 * The segment coding of the reference implementation (Sun Microsystems'
 * g711.c, as in ITU-T G.191): a 13-bit (A-law) or 14-bit (mu-law)
 * magnitude into a 3-bit segment and a 4-bit step, sign on top, A-law
 * with every other bit inverted and mu-law with all of them.
 */
#include "freya_codecs.h"

#define ULAW_BIAS   0x84
#define ULAW_CLIP   32635

uint8_t g711_ulaw_encode(int16_t pcm)
{
    int x = pcm, sign = 0, exp = 7;

    if (x < 0) {
        x = -x;
        sign = 0x80;
    }
    if (x > ULAW_CLIP) x = ULAW_CLIP;
    x += ULAW_BIAS;
    for (int mask = 0x4000; !(x & mask) && exp > 0; mask >>= 1) exp--;
    return (uint8_t)~(sign | (exp << 4) | ((x >> (exp + 3)) & 0x0F));
}

int16_t g711_ulaw_decode(uint8_t code)
{
    int u = (uint8_t)~code;
    int x = ((((u & 0x0F) << 3) + ULAW_BIAS) << ((u >> 4) & 7)) - ULAW_BIAS;

    return (int16_t)((u & 0x80) ? -x : x);
}

uint8_t g711_alaw_encode(int16_t pcm)
{
    int x = pcm >> 3, mask, seg = 0;

    if (x >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        x = -x - 1;
    }
    while (seg < 8 && x > (0x20 << seg) - 1) seg++;     /* segment ends 0x1F .. 0xFFF */
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    return (uint8_t)(((seg << 4) | ((x >> (seg < 2 ? 1 : seg)) & 0x0F)) ^ mask);
}

int16_t g711_alaw_decode(uint8_t code)
{
    int a = code ^ 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;

    if (seg == 0) t += 8;
    else t = (t + 0x108) << (seg - 1);
    return (int16_t)((a & 0x80) ? t : -t);
}

void g711_alaw_encode_buf(const int16_t *pcm, uint8_t *code, int n)
{
    for (int i = 0; i < n; i++) code[i] = g711_alaw_encode(pcm[i]);
}

void g711_alaw_decode_buf(const uint8_t *code, int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++) pcm[i] = g711_alaw_decode(code[i]);
}

void g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *code, int n)
{
    for (int i = 0; i < n; i++) code[i] = g711_ulaw_encode(pcm[i]);
}

void g711_ulaw_decode_buf(const uint8_t *code, int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++) pcm[i] = g711_ulaw_decode(code[i]);
}
