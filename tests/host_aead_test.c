/*
 * Freya - Ascon-AEAD128.
 *
 * src/aead.c and the reference in third_party/ascon compiled unchanged.
 * Three published NIST SP 800-232 known answers pin the bytes.  A round
 * trip, a tag that does not match, and every refusal the header promises
 * are checked beside them.
 *
 * On a board without the code (the STM32F103) the two calls report
 * FREYA_ERR_UNSUPPORTED, and that is all this test asks of them there.
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

#if BOARD_AEAD

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

static const uint8_t s_key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
static const uint8_t s_nonce[16] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};

/* LWC_AEAD_KAT_128_128.txt, counts 1, 35 and 529. */
static const uint8_t s_tag_empty[16] = {
    0x4f, 0x9c, 0x27, 0x82, 0x11, 0xbe, 0xc9, 0x31,
    0x6b, 0xf6, 0x8f, 0x46, 0xee, 0x8b, 0x2e, 0xc6
};
static const uint8_t s_ad_one[1] = { 0x30 };
static const uint8_t s_pt_one[1] = { 0x20 };
static const uint8_t s_ct_ad[17] = {
    0x96, 0x2b, 0x80, 0x16, 0x83, 0x6c, 0x75, 0xa7,
    0xd8, 0x68, 0x66, 0x58, 0x8c, 0xa2, 0x45, 0xd8, 0x86
};
static const uint8_t s_pt16[16] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
};
static const uint8_t s_ct16[32] = {
    0xe8, 0xc3, 0xde, 0xee, 0x24, 0x6c, 0xc5, 0xea,
    0xe3, 0xe8, 0x72, 0x31, 0x38, 0x97, 0xa2, 0xbb,
    0x9e, 0xaa, 0x91, 0x5c, 0x9d, 0xd3, 0x24, 0x5d,
    0x77, 0x04, 0x8f, 0x24, 0xd4, 0x6d, 0x27, 0xa7
};

static void kat(void)
{
    uint8_t ct[64], pt[64];
    int n;

    n = aead_encrypt(s_key, s_nonce, NULL, 0, NULL, 0, ct, 16);
    check("an empty message seals to the tag", 16, n);
    check_mem("and that tag is the published one", s_tag_empty, ct, 16);
    memset(pt, 0xa5, sizeof pt);
    n = aead_decrypt(s_key, s_nonce, NULL, 0, ct, 16, NULL, 0);
    check("and opens to nothing", 0, n);

    n = aead_encrypt(s_key, s_nonce, s_ad_one, 1, s_pt_one, 1, ct, 17);
    check("one byte with associated data seals", 17, n);
    check_mem("to the published ciphertext", s_ct_ad, ct, 17);
    n = aead_decrypt(s_key, s_nonce, s_ad_one, 1, s_ct_ad, 17, pt, 1);
    check("and opens to one byte", 1, n);
    check_mem("which is the published plaintext", s_pt_one, pt, 1);

    n = aead_encrypt(s_key, s_nonce, NULL, 0, s_pt16, 16, ct, 32);
    check("a full rate block seals", 32, n);
    check_mem("to the published block", s_ct16, ct, 32);
    n = aead_decrypt(s_key, s_nonce, NULL, 0, s_ct16, 32, pt, 16);
    check("and opens to 16 bytes", 16, n);
    check_mem("which match", s_pt16, pt, 16);
}

static void round_trip(void)
{
    uint8_t plain[300], ct[300 + 16], back[300], ad[40];
    int i, n, m;

    for (i = 0; i < 300; i++) plain[i] = (uint8_t)(i * 17 + 3);
    for (i = 0; i < 40; i++) ad[i] = (uint8_t)(0xA0 + i);
    n = aead_encrypt(s_key, s_nonce, ad, 40, plain, 300, ct, (int)sizeof ct);
    check("300 bytes seal", 316, n);
    m = aead_decrypt(s_key, s_nonce, ad, 40, ct, n, back, 300);
    check("and open", 300, m);
    check_mem("byte for byte", plain, back, 300);

    ct[10] ^= 1;
    memset(back, 0xa5, sizeof back);
    m = aead_decrypt(s_key, s_nonce, ad, 40, ct, n, back, 300);
    check("a flipped byte is refused", FREYA_ERR_IO, m);
    check("and the output is cleared", 0, back[0] | back[299]);
}

static void refused(void)
{
    uint8_t buf[64], ct[80];
    int i;

    for (i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    check("no key", FREYA_ERR_ARG,
          aead_encrypt(NULL, s_nonce, NULL, 0, buf, 1, ct, 32));
    check("no nonce", FREYA_ERR_ARG,
          aead_encrypt(s_key, NULL, NULL, 0, buf, 1, ct, 32));
    check("no output", FREYA_ERR_ARG,
          aead_encrypt(s_key, s_nonce, NULL, 0, buf, 1, NULL, 32));
    check("a short output", FREYA_ERR_ARG,
          aead_encrypt(s_key, s_nonce, NULL, 0, buf, 1, ct, 16));
    check("a negative length", FREYA_ERR_ARG,
          aead_encrypt(s_key, s_nonce, NULL, 0, buf, -1, ct, 32));
    check("past the maximum", FREYA_ERR_ARG,
          aead_encrypt(s_key, s_nonce, NULL, 0, buf, FREYA_AEAD_MAX_LEN + 1,
                       ct, 32));
    check("associated data without a pointer", FREYA_ERR_ARG,
          aead_encrypt(s_key, s_nonce, NULL, 1, buf, 1, ct, 32));
    check("an overlap", FREYA_ERR_ARG,
          aead_encrypt(s_key, s_nonce, NULL, 0, buf, 16, buf, 32));
    check("associated data overlapping the output", FREYA_ERR_ARG,
          aead_encrypt(s_key, s_nonce, ct, 4, buf, 1, ct, 32));
    check("a ciphertext shorter than the tag", FREYA_ERR_ARG,
          aead_decrypt(s_key, s_nonce, NULL, 0, buf, 15, ct, 16));
    check("opening into too small a buffer", FREYA_ERR_ARG,
          aead_decrypt(s_key, s_nonce, NULL, 0, s_ct16, 32, ct, 15));
    check("sealing nothing needs no plaintext pointer", 1,
          aead_encrypt(s_key, s_nonce, NULL, 0, NULL, 0, ct, 16) == 16);
}

#else /* !BOARD_AEAD */

static void unsupported(void)
{
    uint8_t b[32];

    memset(b, 0, sizeof b);
    check("encrypt is unsupported on this board", FREYA_ERR_UNSUPPORTED,
          aead_encrypt(b, b, NULL, 0, b, 1, b + 16, 16));
    check("decrypt is unsupported on this board", FREYA_ERR_UNSUPPORTED,
          aead_decrypt(b, b, NULL, 0, b, 17, b + 16, 1));
}

#endif

int main(void)
{
#if BOARD_AEAD
    kat();
    round_trip();
    refused();
#else
    unsupported();
#endif
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
