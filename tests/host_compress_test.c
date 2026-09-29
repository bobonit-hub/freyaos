/*
 * Freya - heatshrink.
 *
 * src/lz.c and third_party/heatshrink compiled unchanged.  The stream of
 * `heatshrink -e -w 8 -l 4` on a host pins the format; round trips over
 * text, runs and noise pin the two service calls against each other; the
 * bound in freya_api.h is checked against noise, which is the worst case;
 * and every refusal the header promises is provoked.
 *
 * On a board without the code (the Blue Pill) the two calls report
 * FREYA_ERR_UNSUPPORTED, and that is all this test asks of them there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "freya.h"

static int checks, fails;
static int s_handler;
static int s_no_heap;

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

static void __attribute__((unused))
check_mem(const char *what, const void *exp, const void *got, int n)
{
    checks++;
    if (n >= 0 && memcmp(exp, got, (size_t)n) == 0) {
        printf("  ok    %s\n", what);
        return;
    }
    printf("  FAIL  %s\n", what);
    fails++;
}

/* What lz.c asks of the kernel. */
int   app_in_handler(void) { return s_handler; }
void *kmalloc(uint32_t size) { return s_no_heap ? NULL : malloc(size); }
void  kfree(void *p) { free(p); }

#if BOARD_COMPRESS

/* `heatshrink -e -w 8 -l 4` of the sentence below, from upstream 0.4.1. */
static const char s_fox[] =
    "The quick brown fox jumps over the lazy dog. "
    "The quick brown fox jumps over the lazy dog.";
static const uint8_t s_fox_hs[] = {
    0xaa, 0x5a, 0x2c, 0xb2, 0x0b, 0x8d, 0xd6, 0xd3, 0x63, 0xb5, 0xc8, 0x2c,
    0x57, 0x2b, 0x7d, 0xde, 0xdd, 0x20, 0xb3, 0x5b, 0xef, 0x12, 0x0b, 0x55,
    0xd6, 0xdb, 0x70, 0xb9, 0xc8, 0x2d, 0xf7, 0x6b, 0x2d, 0xca, 0x41, 0x74,
    0x0f, 0x15, 0xb2, 0xc3, 0x7a, 0xbc, 0xc8, 0x2c, 0x96, 0xfb, 0x3c, 0xba,
    0x40, 0x2c, 0xf1, 0x67, 0x8b, 0x2c,
};

static uint32_t s_seed = 1;
static uint8_t noise(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return (uint8_t)(s_seed >> 16);
}

/* One buffer through both calls; the number of packed bytes, or the
 * first error.  Packed and unpacked buffers are sized by the caller. */
static int round_trip(const char *what, const uint8_t *in, int len,
                      uint8_t *packed, int packed_cap,
                      uint8_t *back, int back_cap)
{
    int n, m;
    char msg[96];

    n = lz_compress(in, len, packed, packed_cap);
    snprintf(msg, sizeof msg, "%s compresses", what);
    check(msg, 1, n >= 0);
    if (n < 0) return n;
    snprintf(msg, sizeof msg, "%s stays within the bound", what);
    check(msg, 1, n <= FREYA_COMPRESS_BOUND(len));
    m = lz_decompress(packed, n, back, back_cap);
    snprintf(msg, sizeof msg, "%s decompresses to its length", what);
    check(msg, len, m);
    snprintf(msg, sizeof msg, "%s comes back byte for byte", what);
    check_mem(msg, in, back, m == len ? len : -1);
    return n;
}

/* The stream calls the shell uses, driven the way the shell drives
 * them: small pieces in, small pieces out. */
static int stream(int decode, const uint8_t *in, int len, uint8_t *out, int cap)
{
    lz_stream_t *s = lz_open(decode);
    int off = 0, n = 0, rc, idle;

    if (!s) return FREYA_ERR_BUSY;
    while (off < len) {
        int piece = len - off < 7 ? len - off : 7;
        int took = lz_sink(s, in + off, piece);

        if (took < 0) { lz_close(s); return took; }
        off += took;
        for (;;) {
            int got = lz_poll(s, out + n, cap - n < 5 ? cap - n : 5);
            if (got < 0) { lz_close(s); return got; }
            n += got;
            if (got < 5) break;
        }
    }
    idle = 0;
    for (;;) {
        int got;

        rc = lz_finish(s);
        if (rc <= 0) break;
        got = lz_poll(s, out + n, cap - n < 5 ? cap - n : 5);
        if (got < 0) { rc = got; break; }
        n += got;
        if (got == 0 && ++idle > 1) { rc = FREYA_ERR_IO; break; }
    }
    lz_close(s);
    return rc < 0 ? rc : n;
}

#endif /* BOARD_COMPRESS */

int main(void)
{
    static uint8_t plain[20000], packed[FREYA_COMPRESS_BOUND(20000)], back[20000];
    static uint8_t noisy[4096];
    int i, n, m, len;

    check("an 8-bit window", 8, FREYA_COMPRESS_WINDOW_BITS);
    check("a 4-bit lookahead", 4, FREYA_COMPRESS_LOOKAHEAD_BITS);
    check("the bound of 0 is 0", 0, FREYA_COMPRESS_BOUND(0));
    check("the bound of 1 is 2", 2, FREYA_COMPRESS_BOUND(1));
    check("the bound of 8 is 9", 9, FREYA_COMPRESS_BOUND(8));
    check("the bound of 9 is 11", 11, FREYA_COMPRESS_BOUND(9));

#if BOARD_COMPRESS

    /* The format is heatshrink's. */
    len = (int)sizeof s_fox - 1;
    n = lz_compress(s_fox, len, packed, sizeof packed);
    check("the fox compresses", (long)sizeof s_fox_hs, n);
    check_mem("to the bytes the host tool wrote", s_fox_hs, packed,
              n == (int)sizeof s_fox_hs ? n : -1);
    m = lz_decompress(s_fox_hs, (int)sizeof s_fox_hs, back, sizeof back);
    check("the host tool's stream decompresses", len, m);
    check_mem("to the sentence", s_fox, back, m == len ? len : -1);

    /* A length of zero is nothing, and needs nothing. */
    check("compressing nothing is nothing", 0, lz_compress(NULL, 0, NULL, 0));
    check("decompressing nothing is nothing", 0, lz_decompress(NULL, 0, NULL, 0));
    check("compressing nothing into nothing", 0, lz_compress(plain, 0, packed, 0));

    /* Runs, text and noise. */
    memset(plain, 'a', 3000);
    n = round_trip("a run of 3000 bytes", plain, 3000, packed, sizeof packed,
                   back, sizeof back);
    /* A back reference covers 16 bytes for 13 bits: 3000 bytes are
     * about 305, plus the literal that starts the run. */
    check("a run shrinks to a back reference per 16 bytes", 1, n > 0 && n < 320);

    for (i = 0; i < (int)sizeof plain; i++)
        plain[i] = (uint8_t)("Freya runs bare metal on the Black Pill. "[i % 41]);
    n = round_trip("20000 bytes of text", plain, (int)sizeof plain,
                   packed, sizeof packed, back, sizeof back);
    check("repeated text shrinks", 1, n > 0 && n < 4000);

    for (i = 0; i < (int)sizeof noisy; i++) noisy[i] = noise();
    n = round_trip("4096 bytes of noise", noisy, (int)sizeof noisy,
                   packed, sizeof packed, back, sizeof back);
    check("noise grows, but not past the bound",
          1, n > 4096 && n <= FREYA_COMPRESS_BOUND(4096));

    /* The bound holds for noise at every small length. */
    {
        int worst = 1;
        for (len = 1; len <= 300 && worst; len++) {
            n = lz_compress(noisy, len, packed, FREYA_COMPRESS_BOUND(len));
            if (n < 0 || n > FREYA_COMPRESS_BOUND(len)) worst = 0;
            m = lz_decompress(packed, n, back, len);
            if (m != len || memcmp(noisy, back, (size_t)len) != 0) worst = 0;
        }
        check("noise of 1..300 bytes fits its bound and comes back", 1, worst);
    }

    /* A single byte. */
    n = round_trip("one byte", noisy, 1, packed, 2, back, 1);
    check("one byte is two", 2, n);

    /* Room that is exactly enough is enough; one byte less is not. */
    n = lz_compress(noisy, 64, packed, sizeof packed);
    check("64 bytes of noise pack to 72", 72, n);
    check("72 bytes of room is enough", 72, lz_compress(noisy, 64, packed, 72));
    check("71 is not", FREYA_ERR_ARG, lz_compress(noisy, 64, packed, 71));
    check("no room at all is not", FREYA_ERR_ARG, lz_compress(noisy, 64, packed, 0));
    check("unpacking into exactly 64 works", 64, lz_decompress(packed, 72, back, 64));
    check("unpacking into 63 is refused", FREYA_ERR_ARG,
          lz_decompress(packed, 72, back, 63));
    memset(plain, 'b', 500);
    n = lz_compress(plain, 500, packed, sizeof packed);
    check("a run that unpacks past a small buffer is refused", FREYA_ERR_ARG,
          lz_decompress(packed, n, back, 499));
    check("and fits an exact one", 500, lz_decompress(packed, n, back, 500));

    /* Bad arguments. */
    check("a negative input length", FREYA_ERR_ARG,
          lz_compress(plain, -1, packed, sizeof packed));
    check("a negative output length", FREYA_ERR_ARG,
          lz_compress(plain, 1, packed, -1));
    check("a null input", FREYA_ERR_ARG, lz_compress(NULL, 1, packed, 8));
    check("a null output", FREYA_ERR_ARG, lz_compress(plain, 1, NULL, 8));
    check("a null input to decompress", FREYA_ERR_ARG, lz_decompress(NULL, 1, back, 8));
    check("a null output to decompress", FREYA_ERR_ARG, lz_decompress(plain, 1, NULL, 8));
    check("output over the input is refused", FREYA_ERR_ARG,
          lz_compress(plain, 100, plain + 50, 100));
    check("input over the output is refused", FREYA_ERR_ARG,
          lz_decompress(plain + 50, 100, plain, 100));
    check("output ending where input starts is allowed", 1,
          lz_compress(plain + 100, 10, plain, 100) > 0);

    /* Garbage in: whatever comes out, the call returns. */
    m = lz_decompress(noisy, 1000, back, sizeof back);
    check("noise decompresses to something", 1, m >= 0);
    m = lz_decompress(noisy, 1000, back, 10);
    check("or is refused for want of room", FREYA_ERR_ARG, m);

    /* Handlers and an empty heap. */
    s_handler = 1;
    check("compress from a handler", FREYA_ERR_HANDLER,
          lz_compress(plain, 8, packed, 16));
    check("decompress from a handler", FREYA_ERR_HANDLER,
          lz_decompress(packed, 8, back, 16));
    check("but nothing from a handler is still nothing", 0,
          lz_compress(NULL, 0, NULL, 0));
    check("a stream from a handler is refused", 1, lz_open(0) == NULL);
    s_handler = 0;
    s_no_heap = 1;
    check("compress with no heap", FREYA_ERR_BUSY, lz_compress(plain, 8, packed, 16));
    check("decompress with no heap", FREYA_ERR_BUSY, lz_decompress(packed, 8, back, 16));
    check("a stream with no heap", 1, lz_open(1) == NULL);
    s_no_heap = 0;

    /* The stream calls, in pieces, agree with the whole-buffer calls. */
    len = (int)sizeof s_fox - 1;
    n = stream(0, (const uint8_t *)s_fox, len, packed, sizeof packed);
    check("the fox through the stream calls", (long)sizeof s_fox_hs, n);
    check_mem("is the same stream", s_fox_hs, packed, n == (int)sizeof s_fox_hs ? n : -1);
    m = stream(1, s_fox_hs, (int)sizeof s_fox_hs, back, sizeof back);
    check("and comes back through them", len, m);
    check_mem("as the sentence", s_fox, back, m == len ? len : -1);
    n = stream(0, plain, 20000, packed, sizeof packed);
    check("20000 bytes through the stream calls", 1, n > 0);
    m = stream(1, packed, n, back, sizeof back);
    check("come back", 20000, m);
    check_mem("byte for byte", plain, back, m == 20000 ? 20000 : -1);
    n = stream(0, NULL, 0, packed, sizeof packed);
    check("nothing through the stream calls is nothing", 0, n);
    {
        lz_stream_t *s = lz_open(0);
        check("a stream opens", 1, s != NULL);
        check("a null sink is refused", FREYA_ERR_ARG, lz_sink(s, NULL, 1));
        check("a negative sink is refused", FREYA_ERR_ARG, lz_sink(s, plain, -1));
        check("an empty sink takes nothing", 0, lz_sink(s, plain, 0));
        check("a poll into no room is refused", FREYA_ERR_ARG, lz_poll(s, back, 0));
        check("finishing an empty stream is done", 1, lz_finish(s) >= 0);
        check("input after finish is refused", FREYA_ERR_ARG, lz_sink(s, plain, 1));
        lz_close(s);
        lz_close(NULL);
        check("closing nothing is harmless", 1, 1);
    }

#else /* !BOARD_COMPRESS */

    (void)i; (void)n; (void)m; (void)len;
    check("compress is unsupported on this board", FREYA_ERR_UNSUPPORTED,
          lz_compress(plain, 8, packed, 16));
    check("decompress is unsupported on this board", FREYA_ERR_UNSUPPORTED,
          lz_decompress(packed, 8, back, 16));
    (void)s_handler; (void)s_no_heap; (void)noisy;

#endif

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
