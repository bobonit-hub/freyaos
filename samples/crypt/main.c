/*
 * crypt - check XTEA-CTR, or encrypt a file with it.
 *
 *     run crypt.bin
 *     run crypt.bin <keyhex> <noncehex> <in> <out>
 *
 * With no arguments the sample checks the published block vector: eight
 * zero bytes under the nonce 4142434445464748 come out as
 * 497df3d072612cb5, and a second pass restores the zeros.
 *
 * With four arguments it reads <in> and writes <out>.  The key is 32 hex
 * digits and the nonce is 16, with no 0x.  The same command decrypts,
 * because CTR applied twice is the original.  The two paths must differ.
 */
#include "freya_api.h"

#define CHUNK  256

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* exact bytes, or -1.  No odd length, no 0x. */
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

static int self_test(const freya_api_t *api)
{
    static const uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    static const uint8_t nonce[8] = {
        0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48
    };
    static const uint8_t expect[8] = {
        0x49, 0x7d, 0xf3, 0xd0, 0x72, 0x61, 0x2c, 0xb5
    };
    uint8_t buf[8];
    int i, rc;

    for (i = 0; i < 8; i++) buf[i] = 0;
    rc = api->crypt(key, nonce, 0, buf, buf, 8);
    if (rc != 0) {
        api->printf("crypt: self-test refused (%d)\r\n", rc);
        return FREYA_EXIT_FAIL;
    }
    for (i = 0; i < 8; i++) {
        if (buf[i] != expect[i]) {
            api->printf("crypt: self-test mismatch at byte %d\r\n", i);
            return FREYA_EXIT_FAIL;
        }
    }
    rc = api->crypt(key, nonce, 0, buf, buf, 8);
    if (rc != 0) return FREYA_EXIT_FAIL;
    for (i = 0; i < 8; i++) {
        if (buf[i] != 0) {
            api->puts("crypt: round trip failed\r\n");
            return FREYA_EXIT_FAIL;
        }
    }
    api->puts("crypt: self-test ok\r\n");
    return FREYA_EXIT_OK;
}

static int crypt_file(const freya_api_t *api, const uint8_t *key,
                      const uint8_t *nonce, const char *in_path,
                      const char *out_path)
{
    uint8_t buf[CHUNK];
    uint32_t off = 0, total = 0;
    int in, out, n, rc = FREYA_EXIT_OK;

    if (streq(in_path, out_path)) {
        api->puts("crypt: input and output are the same file\r\n");
        return FREYA_EXIT_USAGE;
    }
    in = api->open(in_path, FREYA_O_RDONLY);
    if (in < 0) {
        api->printf("crypt: cannot open %s\r\n", in_path);
        return FREYA_EXIT_FAIL;
    }
    out = api->open(out_path, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (out < 0) {
        api->printf("crypt: cannot create %s\r\n", out_path);
        api->close(in);
        return FREYA_EXIT_FAIL;
    }

    for (;;) {
        if (api->should_stop()) {
            rc = FREYA_EXIT_STOPPED;
            break;
        }
        n = api->read(in, buf, CHUNK);
        if (n < 0) {
            api->puts("crypt: read failed\r\n");
            rc = FREYA_EXIT_FAIL;
            break;
        }
        if (n == 0) {
            rc = FREYA_EXIT_OK;
            break;
        }
        if (api->crypt(key, nonce, off, buf, buf, n) != 0) {
            api->puts("crypt: refused\r\n");
            rc = FREYA_EXIT_FAIL;
            break;
        }
        if (api->write(out, buf, n) != n) {
            api->puts("crypt: write failed\r\n");
            rc = FREYA_EXIT_FAIL;
            break;
        }
        off += (uint32_t)n;
        total += (uint32_t)n;
        rc = FREYA_EXIT_OK;
    }
    api->close(in);
    api->close(out);
    if (rc == FREYA_EXIT_OK)
        api->printf("crypt: %u bytes\r\n", total);
    return rc;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint8_t key[FREYA_CRYPT_KEY_LEN];
    uint8_t nonce[FREYA_CRYPT_NONCE_LEN];

    if (!FREYA_API_HAS(api, crypt)) {
        api->puts("crypt: this kernel has no cipher\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc == 1) return self_test(api);
    if (argc != 5) {
        api->puts("usage: crypt [<key> <nonce> <in> <out>]\r\n");
        return FREYA_EXIT_USAGE;
    }
    if (parse_hex(argv[1], key, FREYA_CRYPT_KEY_LEN) != 0 ||
        parse_hex(argv[2], nonce, FREYA_CRYPT_NONCE_LEN) != 0) {
        api->puts("crypt: key is 32 hex digits, nonce is 16\r\n");
        return FREYA_EXIT_USAGE;
    }
    return crypt_file(api, key, nonce, argv[3], argv[4]);
}
