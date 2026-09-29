/*
 * Freya - Ascon-AEAD128 on the PC.
 *
 * The key and the nonce are made here.  The MCU never generates either;
 * it only seals and opens with the bytes it is given.  A file this tool
 * writes is ciphertext with the 16-byte tag appended and no header, the
 * associated data empty, which is what `aead` on the board reads.
 *
 *     aead key
 *     aead nonce
 *     aead seal <keyhex> <noncehex> <in> <out>
 *     aead open <keyhex> <noncehex> <in> <out>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "api.h"
#include "crypto_aead.h"

#define KEY_LEN   CRYPTO_KEYBYTES
#define NONCE_LEN CRYPTO_NPUBBYTES
#define TAG_LEN   CRYPTO_ABYTES

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* exact bytes, or -1.  No 0x, no spaces. */
static int parse_hex(const char *s, uint8_t *out, int exact)
{
    int n = 0;

    while (s[0]) {
        int hi = hexval((unsigned char)s[0]);
        int lo = s[1] ? hexval((unsigned char)s[1]) : -1;

        if (n >= exact || hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n == exact ? 0 : -1;
}

static int random_bytes(uint8_t *buf, int n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;

    if (!f) return -1;
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n ? 0 : -1;
}

static int print_random(void)
{
    uint8_t buf[16];
    int i;

    if (random_bytes(buf, 16) != 0) {
        fprintf(stderr, "aead: cannot read /dev/urandom\n");
        return 1;
    }
    for (i = 0; i < 16; i++) printf("%02x", buf[i]);
    printf("\n");
    return 0;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f;
    uint8_t *buf;
    long sz;
    size_t got;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "aead: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "aead: cannot read %s\n", path);
        fclose(f);
        return NULL;
    }
    /* A board call stops at 4096 bytes of plaintext.  The tool allows
     * more, up to a bound that still fits a small machine. */
    if ((unsigned long)sz > 16UL * 1024UL * 1024UL) {
        fprintf(stderr, "aead: %s is larger than 16 MiB\n", path);
        fclose(f);
        return NULL;
    }
    buf = sz ? malloc((size_t)sz) : malloc(1);
    if (!buf) {
        fprintf(stderr, "aead: out of memory\n");
        fclose(f);
        return NULL;
    }
    got = sz ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (got != (size_t)sz) {
        fprintf(stderr, "aead: cannot read %s\n", path);
        free(buf);
        return NULL;
    }
    *len = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t put;

    if (!f) {
        fprintf(stderr, "aead: cannot create %s\n", path);
        return -1;
    }
    put = len ? fwrite(buf, 1, len, f) : 0;
    if (fclose(f) != 0 || put != len) {
        fprintf(stderr, "aead: cannot write %s\n", path);
        return -1;
    }
    return 0;
}

static int seal_open(int decode, const char *keyhex, const char *noncehex,
                     const char *in_path, const char *out_path)
{
    uint8_t key[KEY_LEN], nonce[NONCE_LEN];
    uint8_t *in, *out;
    size_t in_len = 0, out_len;
    unsigned long long n = 0;
    int rc;

    if (parse_hex(keyhex, key, KEY_LEN) != 0 ||
        parse_hex(noncehex, nonce, NONCE_LEN) != 0) {
        fprintf(stderr, "aead: the key is 32 hex digits and the nonce is 32\n");
        return 1;
    }
    in = read_file(in_path, &in_len);
    if (!in) return 1;
    if (decode) {
        if (in_len < TAG_LEN) {
            fprintf(stderr, "aead: %s is too short to hold a tag\n", in_path);
            free(in);
            return 1;
        }
        out_len = in_len - TAG_LEN;
    } else {
        out_len = in_len + TAG_LEN;
    }
    out = malloc(out_len ? out_len : 1);
    if (!out) {
        fprintf(stderr, "aead: out of memory\n");
        free(in);
        return 1;
    }
    if (decode) {
        rc = crypto_aead_decrypt(out_len ? out : NULL, &n, NULL,
                                 in, in_len, NULL, 0, nonce, key);
        if (rc != 0) {
            fprintf(stderr, "aead: authentication failed\n");
            free(out);
            free(in);
            return 1;
        }
    } else {
        rc = crypto_aead_encrypt(out, &n, in_len ? in : NULL, in_len,
                                 NULL, 0, NULL, nonce, key);
        if (rc != 0) {
            fprintf(stderr, "aead: encryption failed\n");
            free(out);
            free(in);
            return 1;
        }
    }
    rc = write_file(out_path, out, (size_t)n);
    free(out);
    free(in);
    if (rc != 0) return 1;
    printf("%zu -> %llu B\n", in_len, n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "key") == 0) return print_random();
    if (argc == 2 && strcmp(argv[1], "nonce") == 0) return print_random();
    if (argc == 6 && strcmp(argv[1], "seal") == 0)
        return seal_open(0, argv[2], argv[3], argv[4], argv[5]);
    if (argc == 6 && strcmp(argv[1], "open") == 0)
        return seal_open(1, argv[2], argv[3], argv[4], argv[5]);
    fprintf(stderr,
            "usage: aead key\n"
            "       aead nonce\n"
            "       aead seal <key> <nonce> <in> <out>\n"
            "       aead open <key> <nonce> <in> <out>\n");
    return argc == 1 ? 0 : 1;
}
