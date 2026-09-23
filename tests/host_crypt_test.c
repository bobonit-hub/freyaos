/*
 * Freya - XTEA-CTR.
 *
 * src/crypt.c compiled unchanged.  The published block vector pins the
 * rounds and the byte order; CTR is then checked against that block
 * function, including a counter that carries and a piece that starts
 * in the middle of a block.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freya.h"

static int checks, fails;

static void check(const char *what, long expected, long got)
{
    checks++;
    if (expected == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %ld, got %ld\n", what, expected, got);
        fails++;
    }
}

static void check_mem(const char *what, const void *exp, const void *got, int n)
{
    checks++;
    if (n >= 0 && memcmp(exp, got, (size_t)n) == 0) {
        printf("  ok    %s\n", what);
        return;
    }
    printf("  FAIL  %s\n", what);
    fails++;
}

static const uint8_t key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
static const uint8_t block[8] = {
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48
};
static const uint8_t block_ct[8] = {
    0x49, 0x7d, 0xf3, 0xd0, 0x72, 0x61, 0x2c, 0xb5
};

/* The counter rule the call is specified with, independent of crypt.c. */
static void add_u64(uint8_t b[8], uint32_t n)
{
    uint32_t lo, hi, lo0;

    lo0 = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
          ((uint32_t)b[6] << 8) | (uint32_t)b[7];
    hi  = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
          ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    lo = lo0 + n;
    if (lo < lo0) hi++;
    b[0] = (uint8_t)(hi >> 24);
    b[1] = (uint8_t)(hi >> 16);
    b[2] = (uint8_t)(hi >> 8);
    b[3] = (uint8_t)hi;
    b[4] = (uint8_t)(lo >> 24);
    b[5] = (uint8_t)(lo >> 16);
    b[6] = (uint8_t)(lo >> 8);
    b[7] = (uint8_t)lo;
}

int main(void)
{
    uint8_t out[FREYA_CRYPT_MAX_LEN];
    uint8_t plain[64], buf[64], again[64];
    uint8_t ctr[8], ks[8], zeros[8];
    uint8_t wrapped[8];
    int i;

    check("32 rounds", 32, FREYA_CRYPT_ROUNDS);
    check("a 16-byte key", 16, FREYA_CRYPT_KEY_LEN);
    check("an 8-byte nonce", 8, FREYA_CRYPT_NONCE_LEN);
    check("an 8-byte block", 8, FREYA_CRYPT_BLOCK);
    check("a call holds 4096 bytes", 4096, FREYA_CRYPT_MAX_LEN);

    check("a block with no key is refused", FREYA_ERR_ARG, crypt_block(NULL, block, out));
    check("a block with no input is refused", FREYA_ERR_ARG, crypt_block(key, NULL, out));
    check("a block with no output is refused", FREYA_ERR_ARG, crypt_block(key, block, NULL));
    check("the published block encrypts", 0, crypt_block(key, block, out));
    check_mem("to 497df3d072612cb5", block_ct, out, 8);

    memset(zeros, 0, sizeof zeros);
    check("eight zero bytes under that nonce",
          0, crypt_apply(key, block, 0, zeros, out, 8));
    check_mem("are the published ciphertext", block_ct, out, 8);
    check("and a second pass", 0, crypt_apply(key, block, 0, out, out, 8));
    check_mem("restores the zeros", zeros, out, 8);

    for (i = 0; i < 64; i++) plain[i] = (uint8_t)(i * 17 + 3);
    check("a 64-byte message", 0, crypt_apply(key, block, 0, plain, buf, 64));
    check("in place matches", 0, crypt_apply(key, block, 0, plain, again, 64));
    memcpy(again, plain, 64);
    check("in place runs", 0, crypt_apply(key, block, 0, again, again, 64));
    check_mem("in place agrees with a separate buffer", buf, again, 64);
    check("decrypting that", 0, crypt_apply(key, block, 0, buf, out, 64));
    check_mem("returns the message", plain, out, 64);

    /* A piece that starts mid-block is the same keystream. */
    check("three bytes at offset 7",
          0, crypt_apply(key, block, 7, plain + 7, buf, 3));
    check("the whole message from 0",
          0, crypt_apply(key, block, 0, plain, again, 16));
    check_mem("match the middle of it", again + 7, buf, 3);

    /* Block 1's keystream is the block cipher of nonce+1. */
    memcpy(ctr, block, 8);
    add_u64(ctr, 1);
    check("nonce plus one encrypts", 0, crypt_block(key, ctr, ks));
    check("the byte at offset 8", 0, crypt_apply(key, block, 8, zeros, out, 1));
    check("is the first keystream byte of that block", ks[0], out[0]);

    /* All-ones nonce plus one block wraps to zero. */
    memset(wrapped, 0xff, sizeof wrapped);
    check("a wrapped counter's next byte",
          0, crypt_apply(key, wrapped, 8, zeros, out, 1));
    memset(ctr, 0, sizeof ctr);
    check("is the cipher of a zero block", 0, crypt_block(key, ctr, ks));
    check("byte for byte", ks[0], out[0]);

    check("a negative length is refused", FREYA_ERR_ARG,
          crypt_apply(key, block, 0, plain, out, -1));
    check("past the maximum is refused", FREYA_ERR_ARG,
          crypt_apply(key, block, 0, plain, out, FREYA_CRYPT_MAX_LEN + 1));
    check("a null key is refused", FREYA_ERR_ARG,
          crypt_apply(NULL, block, 0, plain, out, 1));
    check("a null nonce is refused", FREYA_ERR_ARG,
          crypt_apply(key, NULL, 0, plain, out, 1));
    check("a null input is refused", FREYA_ERR_ARG,
          crypt_apply(key, block, 0, NULL, out, 1));
    check("a null output is refused", FREYA_ERR_ARG,
          crypt_apply(key, block, 0, plain, NULL, 1));
    check("a length of zero needs no pointers", 0,
          crypt_apply(NULL, NULL, 0, NULL, NULL, 0));
    check("offset plus length may not wrap", FREYA_ERR_ARG,
          crypt_apply(key, block, 0xFFFFFFF8u, plain, out, 9));
    check("the last eight bytes of the range are allowed", 0,
          crypt_apply(key, block, 0xFFFFFFF8u, zeros, out, 8));

    memset(out, 0, sizeof out);
    check("a full-size call", 0,
          crypt_apply(key, block, 0, out, out, FREYA_CRYPT_MAX_LEN));
    /* The last byte of a zero message is byte 7 of block 511. */
    memset(out, 0, 8);
    check("the last byte, taken alone", 0,
          crypt_apply(key, zeros, FREYA_CRYPT_MAX_LEN - 1, out, out, 1));
    memset(ctr, 0, sizeof ctr);
    add_u64(ctr, (FREYA_CRYPT_MAX_LEN - 1) >> 3);
    check("that counter encrypts", 0, crypt_block(key, ctr, ks));
    check("and matches keystream byte 7", ks[7], out[0]);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
