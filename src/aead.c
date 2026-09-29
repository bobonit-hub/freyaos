/*
 * Freya - authenticated encryption.
 *
 * Ascon-AEAD128, NIST SP 800-232, the reference code in
 * third_party/ascon.  The key is 16 bytes, the nonce is 16 and the tag
 * is 16, appended to the ciphertext.  A call takes the whole message.
 * Nothing is kept afterwards, and the key is the caller's: Freya does
 * not generate one.  The PC tool tools/aead does, and a file it seals
 * with an empty associated data is a file aead_decrypt() opens.
 *
 * The 48 KiB kernel image has no room for this.  The linker script puts
 * the whole file, and the Ascon code, in the kernel extension.  The
 * Blue Pill's extension is full too; there the two service calls report
 * FREYA_ERR_UNSUPPORTED and nothing else is built.
 */
#include "freya.h"

#if BOARD_AEAD

#include "crypto_aead.h"
#include "api.h"

_Static_assert(CRYPTO_KEYBYTES == FREYA_AEAD_KEY_LEN,
               "Ascon and freya_api.h disagree on the key");
_Static_assert(CRYPTO_NPUBBYTES == FREYA_AEAD_NONCE_LEN,
               "Ascon and freya_api.h disagree on the nonce");
_Static_assert(CRYPTO_ABYTES == FREYA_AEAD_TAG_LEN,
               "Ascon and freya_api.h disagree on the tag");
_Static_assert(ASCON_AEAD_RATE == 16,
               "Ascon-AEAD128 absorbs 16 bytes at a time");

/* 1 when the two ranges share a byte.  A zero length shares nothing. */
static int overlap(const void *a, int an, const void *b, int bn)
{
    uintptr_t x, y;

    if (an <= 0 || bn <= 0 || !a || !b) return 0;
    x = (uintptr_t)a;
    y = (uintptr_t)b;
    return x < y + (uintptr_t)bn && y < x + (uintptr_t)an;
}

int aead_encrypt(const void *key, const void *nonce,
                 const void *ad, int ad_len,
                 const void *in, int in_len,
                 void *out, int out_cap)
{
    unsigned long long clen = 0;
    int need;

    if (!key || !nonce) return FREYA_ERR_ARG;
    if (ad_len < 0 || in_len < 0 || out_cap < 0) return FREYA_ERR_ARG;
    if (ad_len > FREYA_AEAD_MAX_LEN || in_len > FREYA_AEAD_MAX_LEN)
        return FREYA_ERR_ARG;
    if ((ad_len && !ad) || (in_len && !in)) return FREYA_ERR_ARG;
    need = in_len + FREYA_AEAD_TAG_LEN;
    if (!out || out_cap < need) return FREYA_ERR_ARG;
    if (overlap(in, in_len, out, need) || overlap(ad, ad_len, out, need))
        return FREYA_ERR_ARG;

    if (crypto_aead_encrypt(out, &clen,
                            in, (unsigned long long)in_len,
                            ad, (unsigned long long)ad_len,
                            0, nonce, key) != 0)
        return FREYA_ERR_IO;
    if (clen != (unsigned long long)need) return FREYA_ERR_IO;
    return need;
}

int aead_decrypt(const void *key, const void *nonce,
                 const void *ad, int ad_len,
                 const void *in, int in_len,
                 void *out, int out_cap)
{
    unsigned long long mlen = 0;
    int plain;

    if (!key || !nonce) return FREYA_ERR_ARG;
    if (ad_len < 0 || in_len < 0 || out_cap < 0) return FREYA_ERR_ARG;
    if (ad_len > FREYA_AEAD_MAX_LEN) return FREYA_ERR_ARG;
    if (ad_len && !ad) return FREYA_ERR_ARG;
    if (in_len < FREYA_AEAD_TAG_LEN || !in) return FREYA_ERR_ARG;
    plain = in_len - FREYA_AEAD_TAG_LEN;
    if (plain > FREYA_AEAD_MAX_LEN) return FREYA_ERR_ARG;
    if (out_cap < plain || (plain && !out)) return FREYA_ERR_ARG;
    if (overlap(in, in_len, out, plain) || overlap(ad, ad_len, out, plain))
        return FREYA_ERR_ARG;

    if (crypto_aead_decrypt(plain ? out : 0, &mlen, 0,
                            in, (unsigned long long)in_len,
                            ad, (unsigned long long)ad_len,
                            nonce, key) != 0) {
        /* The reference writes the plaintext before it checks the tag. */
        if (plain && out) memset(out, 0, (size_t)plain);
        return FREYA_ERR_IO;
    }
    if (mlen != (unsigned long long)plain) return FREYA_ERR_IO;
    return plain;
}

#else /* !BOARD_AEAD */

int aead_encrypt(const void *key, const void *nonce,
                 const void *ad, int ad_len,
                 const void *in, int in_len,
                 void *out, int out_cap)
{
    (void)key; (void)nonce; (void)ad; (void)ad_len;
    (void)in; (void)in_len; (void)out; (void)out_cap;
    return FREYA_ERR_UNSUPPORTED;
}

int aead_decrypt(const void *key, const void *nonce,
                 const void *ad, int ad_len,
                 const void *in, int in_len,
                 void *out, int out_cap)
{
    (void)key; (void)nonce; (void)ad; (void)ad_len;
    (void)in; (void)in_len; (void)out; (void)out_cap;
    return FREYA_ERR_UNSUPPORTED;
}

#endif /* BOARD_AEAD */
