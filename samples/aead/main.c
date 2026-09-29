/*
 * aead - check Ascon-AEAD128, or seal and open a file with it.
 *
 *     run aead.bin
 *     run aead.bin <keyhex> <noncehex> <in> <out>
 *     run aead.bin -d <keyhex> <noncehex> <in> <out>
 *
 * With no arguments the sample seals an empty message and one published
 * block, opens both, and compares them with the NIST answers.
 *
 * With four arguments it reads <in> and writes <out>.  The key and the
 * nonce are 32 hex digits each, with no 0x.  -d checks the tag and
 * writes the plaintext.  The two paths must differ.  A file sealed by
 * tools/aead on a PC, with the same key and nonce and no associated
 * data, opens here.
 */
#include "freya_api.h"

static const uint8_t s_key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
static const uint8_t s_nonce[16] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
static const uint8_t s_tag[16] = {
    0x4f, 0x9c, 0x27, 0x82, 0x11, 0xbe, 0xc9, 0x31,
    0x6b, 0xf6, 0x8f, 0x46, 0xee, 0x8b, 0x2e, 0xc6
};

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

static const char *why(int rc)
{
    switch (rc) {
    case FREYA_ERR_ARG:          return "bad argument";
    case FREYA_ERR_IO:           return "authentication failed";
    case FREYA_ERR_UNSUPPORTED:  return "this board has no cipher";
    default:                     return "failed";
    }
}

static int self_test(const freya_api_t *api)
{
    uint8_t ct[32], pt[16];
    static const uint8_t block[16] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
    };
    int n, i;

    n = api->aead_encrypt(s_key, s_nonce, 0, 0, 0, 0, ct, 16);
    if (n == FREYA_ERR_UNSUPPORTED) {
        api->puts("aead: this board has no cipher\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (n != 16) {
        api->printf("aead: self-test: %s (%d)\r\n", why(n), n);
        return FREYA_EXIT_FAIL;
    }
    for (i = 0; i < 16; i++) {
        if (ct[i] != s_tag[i]) {
            api->printf("aead: self-test: tag byte %d differs\r\n", i);
            return FREYA_EXIT_FAIL;
        }
    }
    n = api->aead_encrypt(s_key, s_nonce, 0, 0, block, 16, ct, 32);
    if (n != 32) {
        api->printf("aead: self-test: %s (%d)\r\n", why(n), n);
        return FREYA_EXIT_FAIL;
    }
    n = api->aead_decrypt(s_key, s_nonce, 0, 0, ct, 32, pt, 16);
    if (n != 16) {
        api->printf("aead: self-test: %s (%d)\r\n", why(n), n);
        return FREYA_EXIT_FAIL;
    }
    for (i = 0; i < 16; i++) {
        if (pt[i] != block[i]) {
            api->printf("aead: self-test: byte %d differs\r\n", i);
            return FREYA_EXIT_FAIL;
        }
    }
    ct[31] ^= 1;
    n = api->aead_decrypt(s_key, s_nonce, 0, 0, ct, 32, pt, 16);
    if (n != FREYA_ERR_IO) {
        api->puts("aead: self-test: a bad tag was accepted\r\n");
        return FREYA_EXIT_FAIL;
    }
    api->puts("aead: self-test ok\r\n");
    return FREYA_EXIT_OK;
}

static uint8_t *read_file(const freya_api_t *api, const char *path, int *len)
{
    uint8_t *buf;
    int fd, size, got = 0, n;

    *len = -1;
    fd = api->open(path, FREYA_O_RDONLY);
    if (fd < 0) {
        api->printf("aead: cannot open %s\r\n", path);
        return 0;
    }
    size = api->fsize(fd);
    if (size < 0) {
        api->puts("aead: read failed\r\n");
        api->close(fd);
        return 0;
    }
    buf = size > 0 ? api->malloc((uint32_t)size) : 0;
    if (size > 0 && !buf) {
        api->printf("aead: %s does not fit in memory\r\n", path);
        api->close(fd);
        return 0;
    }
    while (got < size) {
        n = api->read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += n;
    }
    api->close(fd);
    if (got != size) {
        api->puts("aead: read failed\r\n");
        api->free(buf);
        return 0;
    }
    *len = size;
    return buf ? buf : api->malloc(1);
}

static int write_file(const freya_api_t *api, const char *path,
                      const uint8_t *buf, int len)
{
    int fd, rc;

    fd = api->open(path, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd < 0) {
        api->printf("aead: cannot create %s\r\n", path);
        return -1;
    }
    rc = (len == 0 || api->write(fd, buf, len) == len) ? 0 : -1;
    if (api->close(fd) != 0) rc = -1;
    if (rc) api->puts("aead: write failed\r\n");
    return rc;
}

static int do_file(const freya_api_t *api, int decode,
                   const char *keyhex, const char *noncehex,
                   const char *in_path, const char *out_path)
{
    uint8_t key[FREYA_AEAD_KEY_LEN], nonce[FREYA_AEAD_NONCE_LEN];
    uint8_t *in = 0, *out = 0;
    int in_len = 0, cap, n, rc = FREYA_EXIT_FAIL;

    if (parse_hex(keyhex, key, FREYA_AEAD_KEY_LEN) != 0 ||
        parse_hex(noncehex, nonce, FREYA_AEAD_NONCE_LEN) != 0) {
        api->puts("aead: the key is 32 hex digits and the nonce is 32\r\n");
        return FREYA_EXIT_USAGE;
    }
    if (streq(in_path, out_path)) {
        api->puts("aead: input and output are the same file\r\n");
        return FREYA_EXIT_USAGE;
    }
    in = read_file(api, in_path, &in_len);
    if (in_len < 0) return FREYA_EXIT_FAIL;
    cap = decode ? in_len - FREYA_AEAD_TAG_LEN : in_len + FREYA_AEAD_TAG_LEN;
    if (cap < 0) {
        api->puts("aead: file is too short to hold a tag\r\n");
        goto done;
    }
    if (cap > 0) {
        out = api->malloc((uint32_t)cap);
        if (!out) {
            api->puts("aead: the result does not fit in memory\r\n");
            goto done;
        }
    }
    n = decode ? api->aead_decrypt(key, nonce, 0, 0, in, in_len, out, cap)
               : api->aead_encrypt(key, nonce, 0, 0, in_len ? in : 0, in_len,
                                   out, cap);
    if (n < 0) {
        api->printf("aead: %s (%d)\r\n", why(n), n);
        goto done;
    }
    if (write_file(api, out_path, out, n) != 0) goto done;
    api->printf("aead: %d -> %d B\r\n", in_len, n);
    rc = FREYA_EXIT_OK;
done:
    api->free(in);
    api->free(out);
    return rc;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    if (!FREYA_API_HAS(api, aead_decrypt)) {
        api->puts("aead: this kernel has no cipher\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc == 1) return self_test(api);
    if (argc == 5)
        return do_file(api, 0, argv[1], argv[2], argv[3], argv[4]);
    if (argc == 6 && streq(argv[1], "-d"))
        return do_file(api, 1, argv[2], argv[3], argv[4], argv[5]);
    api->puts("usage: aead [-d] <key> <nonce> <in> <out>\r\n");
    return FREYA_EXIT_USAGE;
}
