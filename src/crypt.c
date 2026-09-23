/*
 * Freya - symmetric encryption.
 *
 * XTEA, 32 rounds, used as a keystream.  A block is eight bytes, two
 * big-endian words, so the bytes a program writes in hex are the words
 * the rounds see.  Under the key 000102030405060708090a0b0c0d0e0f the
 * block 4142434445464748 encrypts to 497df3d072612cb5, the usual
 * published vector.  CTR of eight zero bytes with that block as the
 * nonce is those same eight bytes, and applying the call again returns
 * the zeros.
 *
 * The 48 KiB kernel image has no room left for this.  The linker script
 * puts the whole file in the kernel extension.
 */
#include "freya.h"

#define DELTA  0x9E3779B9u

static uint32_t ld32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Add n to a big-endian 64-bit counter.  n fits in 32 bits, so the
 * carry out of the low word is 0 or 1. */
static void add_u64(uint8_t b[8], uint32_t n)
{
    uint32_t lo0 = ld32(b + 4);
    uint32_t lo  = lo0 + n;
    uint32_t hi  = ld32(b);

    if (lo < lo0) hi++;
    st32(b, hi);
    st32(b + 4, lo);
}

static void enc_block(const uint8_t key[16], const uint8_t in[8], uint8_t out[8])
{
    uint32_t k[4], v0, v1, sum;
    int i;

    for (i = 0; i < 4; i++) k[i] = ld32(key + 4 * i);
    v0 = ld32(in);
    v1 = ld32(in + 4);
    sum = 0;
    for (i = 0; i < FREYA_CRYPT_ROUNDS; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + k[sum & 3]);
        sum += DELTA;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + k[(sum >> 11) & 3]);
    }
    st32(out, v0);
    st32(out + 4, v1);
}

int crypt_block(const void *key, const void *in, void *out)
{
    if (!key || !in || !out) return FREYA_ERR_ARG;
    enc_block(key, in, out);
    return 0;
}

int crypt_apply(const void *key, const void *nonce, uint32_t off,
                const void *in, void *out, int len)
{
    const uint8_t *src = in;
    uint8_t *dst = out;
    uint8_t ctr[8], ks[8];
    unsigned skip;
    int i;

    if (len < 0 || len > FREYA_CRYPT_MAX_LEN) return FREYA_ERR_ARG;
    if (len == 0) return 0;
    if (!key || !nonce || !in || !out) return FREYA_ERR_ARG;
    /* The last byte of the message is off+len-1, which has to fit in
     * 32 bits.  off + len itself may be 2^32 when that last byte is
     * 0xFFFFFFFF. */
    if (off > 0xFFFFFFFFu - (uint32_t)(len - 1)) return FREYA_ERR_ARG;

    for (i = 0; i < FREYA_CRYPT_NONCE_LEN; i++)
        ctr[i] = ((const uint8_t *)nonce)[i];
    add_u64(ctr, off >> 3);
    skip = off & 7;

    while (len) {
        enc_block(key, ctr, ks);
        while (skip < 8 && len) {
            *dst++ = (uint8_t)(*src++ ^ ks[skip++]);
            len--;
        }
        skip = 0;
        if (len) add_u64(ctr, 1);
    }
    return 0;
}
