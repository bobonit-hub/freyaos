/*
 * compress - check heatshrink, or compress and decompress a file with it.
 *
 *     run compress.bin
 *     run compress.bin <in> <out>
 *     run compress.bin -d <in> <out>
 *
 * With no arguments the sample compresses a built-in text, decompresses
 * the result, compares the two and prints the sizes.
 *
 * With two paths it reads <in> whole, compresses it and writes <out>.
 * -d does the reverse.  The file has to fit in the heap together with
 * its result, because the two service calls take whole buffers; a
 * decompressed size is not known in advance, so the output buffer is
 * doubled while the call reports that it did not fit.  The two paths
 * must differ.  The stream is what `heatshrink -e -w 8 -l 4` writes on
 * a host, so a file made there decompresses here and the other way.
 */
#include "freya_api.h"

static const char s_text[] =
    "Freya is a 32-bit, single-user, text OS for STMicroelectronics "
    "STM32 small MCUs, written from scratch in C and ARM assembly.  "
    "Freya is a 32-bit, single-user, text OS for STMicroelectronics "
    "STM32 small MCUs, written from scratch in C and ARM assembly.  "
    "Freya is a 32-bit, single-user, text OS for STMicroelectronics "
    "STM32 small MCUs, written from scratch in C and ARM assembly.  ";

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char *why(int rc)
{
    switch (rc) {
    case FREYA_ERR_ARG:         return "the result does not fit";
    case FREYA_ERR_IO:          return "not a heatshrink stream";
    case FREYA_ERR_BUSY:        return "the heap cannot hold the state";
    case FREYA_ERR_HANDLER:     return "called from a handler";
    case FREYA_ERR_UNSUPPORTED: return "unsupported on this board";
    default:                    return "refused";
    }
}

static int self_test(const freya_api_t *api)
{
    int len = (int)sizeof s_text - 1;
    uint8_t *packed, *back;
    int packed_len, back_len, i, rc = FREYA_EXIT_FAIL;

    packed = api->malloc((uint32_t)FREYA_COMPRESS_BOUND(len));
    back = api->malloc((uint32_t)len);
    if (!packed || !back) {
        api->puts("compress: out of memory\r\n");
        goto out;
    }
    packed_len = api->compress(s_text, len, packed, FREYA_COMPRESS_BOUND(len));
    if (packed_len < 0) {
        api->printf("compress: self-test: %s (%d)\r\n", why(packed_len), packed_len);
        goto out;
    }
    back_len = api->decompress(packed, packed_len, back, len);
    if (back_len < 0) {
        api->printf("compress: self-test: %s (%d)\r\n", why(back_len), back_len);
        goto out;
    }
    if (back_len != len) {
        api->printf("compress: self-test: %d bytes back, not %d\r\n", back_len, len);
        goto out;
    }
    for (i = 0; i < len; i++) {
        if (back[i] != (uint8_t)s_text[i]) {
            api->printf("compress: self-test: byte %d differs\r\n", i);
            goto out;
        }
    }
    api->printf("compress: self-test ok, %d -> %d -> %d B\r\n",
                len, packed_len, back_len);
    rc = FREYA_EXIT_OK;
out:
    api->free(packed);
    api->free(back);
    return rc;
}

/* The whole file, or NULL with the reason printed. */
static uint8_t *read_file(const freya_api_t *api, const char *path, int *len)
{
    uint8_t *buf;
    int fd, size, got = 0, n;

    fd = api->open(path, FREYA_O_RDONLY);
    if (fd < 0) {
        api->printf("compress: cannot open %s\r\n", path);
        return 0;
    }
    size = api->fsize(fd);
    buf = size > 0 ? api->malloc((uint32_t)size) : api->malloc(1);
    if (!buf) {
        api->printf("compress: %s does not fit in memory\r\n", path);
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
        api->puts("compress: read failed\r\n");
        api->free(buf);
        return 0;
    }
    *len = size;
    return buf;
}

static int write_file(const freya_api_t *api, const char *path,
                      const uint8_t *buf, int len)
{
    int fd, rc;

    fd = api->open(path, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd < 0) {
        api->printf("compress: cannot create %s\r\n", path);
        return -1;
    }
    rc = api->write(fd, buf, len) == len ? 0 : -1;
    if (api->close(fd) != 0) rc = -1;
    if (rc) api->puts("compress: write failed\r\n");
    return rc;
}

static int do_file(const freya_api_t *api, int decode, const char *in_path,
                   const char *out_path)
{
    uint8_t *in, *out = 0;
    int in_len, cap, n, rc = FREYA_EXIT_FAIL;

    if (streq(in_path, out_path)) {
        api->puts("compress: input and output are the same file\r\n");
        return FREYA_EXIT_USAGE;
    }
    in = read_file(api, in_path, &in_len);
    if (!in) return FREYA_EXIT_FAIL;

    /* Compression has a bound.  Decompression does not: start at four
     * times the input and double while the call says it did not fit. */
    cap = decode ? in_len * 4 + 64 : FREYA_COMPRESS_BOUND(in_len);
    for (;;) {
        out = api->malloc((uint32_t)cap);
        if (!out) {
            api->puts("compress: the result does not fit in memory\r\n");
            goto done;
        }
        n = decode ? api->decompress(in, in_len, out, cap)
                   : api->compress(in, in_len, out, cap);
        if (n >= 0 || n != FREYA_ERR_ARG || !decode) break;
        api->free(out);
        out = 0;
        cap *= 2;
    }
    if (n < 0) {
        api->printf("compress: %s (%d)\r\n", why(n), n);
        goto done;
    }
    if (write_file(api, out_path, out, n) != 0) goto done;
    api->printf("compress: %d -> %d B\r\n", in_len, n);
    rc = FREYA_EXIT_OK;
done:
    api->free(in);
    api->free(out);
    return rc;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    if (!FREYA_API_HAS(api, decompress)) {
        api->puts("compress: this kernel has no compressor\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc == 1) return self_test(api);
    if (argc == 3) return do_file(api, 0, argv[1], argv[2]);
    if (argc == 4 && streq(argv[1], "-d")) return do_file(api, 1, argv[2], argv[3]);
    api->puts("usage: compress [-d] <in> <out>\r\n");
    return FREYA_EXIT_USAGE;
}
