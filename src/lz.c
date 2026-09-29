/*
 * Freya - compression.
 *
 * heatshrink's LZSS with a 256-byte window and a 16-byte lookahead, the
 * stream `heatshrink -e -w 8 -l 4` writes on a host.  A literal is a
 * one bit and the byte; a back reference is a zero bit, eight bits of
 * distance and four of length, and is only chosen when it is shorter
 * than the literals it replaces, so the worst case is nine bits a byte.
 * There is no header and no checksum: the caller keeps the lengths.
 *
 * The encoder state is 526 bytes and the decoder's 302.  Each is taken
 * from the heap when a call or a stream starts and given back when it
 * ends, so nothing is reserved while the code is idle.  That is also
 * why a pin or timer handler is refused: the heap is not for handlers.
 *
 * The 48 KiB kernel image has no room for this.  The linker script puts
 * the whole file, and heatshrink, in the kernel extension.  The Blue
 * Pill's extension is full too; there the two service calls report
 * FREYA_ERR_UNSUPPORTED and nothing else is built.
 */
#include "freya.h"

#if BOARD_COMPRESS

#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"

_Static_assert(HEATSHRINK_STATIC_WINDOW_BITS == FREYA_COMPRESS_WINDOW_BITS,
               "heatshrink_config.h and freya_api.h disagree on the window");
_Static_assert(HEATSHRINK_STATIC_LOOKAHEAD_BITS == FREYA_COMPRESS_LOOKAHEAD_BITS,
               "heatshrink_config.h and freya_api.h disagree on the lookahead");

struct lz_stream {
    uint8_t decode;
    uint8_t finished;
    union {
        heatshrink_encoder enc;
        heatshrink_decoder dec;
    } u;
};

lz_stream_t *lz_open(int decode)
{
    lz_stream_t *s;
    uint32_t size;

    if (app_in_handler()) return NULL;
    size = (uint32_t)offsetof(lz_stream_t, u) +
           (decode ? (uint32_t)sizeof(heatshrink_decoder)
                   : (uint32_t)sizeof(heatshrink_encoder));
    s = kmalloc(size);
    if (!s) return NULL;
    s->decode = decode ? 1 : 0;
    s->finished = 0;
    if (decode) heatshrink_decoder_reset(&s->u.dec);
    else        heatshrink_encoder_reset(&s->u.enc);
    return s;
}

void lz_close(lz_stream_t *s)
{
    kfree(s);
}

int lz_sink(lz_stream_t *s, const void *in, int len)
{
    size_t took = 0;

    if (!s || len < 0 || (len > 0 && !in)) return FREYA_ERR_ARG;
    if (s->finished) return FREYA_ERR_ARG;
    if (len == 0) return 0;
    if (s->decode) {
        /* HSDR_SINK_FULL leaves took at 0: poll first. */
        if (heatshrink_decoder_sink(&s->u.dec, (uint8_t *)in, (size_t)len,
                                    &took) < 0)
            return FREYA_ERR_ARG;
    } else {
        HSE_sink_res r = heatshrink_encoder_sink(&s->u.enc, (uint8_t *)in,
                                                 (size_t)len, &took);
        /* The encoder refuses input while it still holds a full
         * window that has not been searched: poll first. */
        if (r == HSER_SINK_ERROR_MISUSE) return 0;
        if (r < 0) return FREYA_ERR_ARG;
    }
    return (int)took;
}

int lz_poll(lz_stream_t *s, void *out, int cap)
{
    size_t got = 0;

    if (!s || !out || cap <= 0) return FREYA_ERR_ARG;
    if (s->decode) {
        if (heatshrink_decoder_poll(&s->u.dec, out, (size_t)cap, &got) < 0)
            return FREYA_ERR_IO;
    } else {
        if (heatshrink_encoder_poll(&s->u.enc, out, (size_t)cap, &got) < 0)
            return FREYA_ERR_IO;
    }
    return (int)got;
}

int lz_finish(lz_stream_t *s)
{
    int r;

    if (!s) return FREYA_ERR_ARG;
    s->finished = 1;
    if (s->decode) {
        r = heatshrink_decoder_finish(&s->u.dec);
        if (r == HSDR_FINISH_DONE) return 0;
        if (r == HSDR_FINISH_MORE) return 1;
    } else {
        r = heatshrink_encoder_finish(&s->u.enc);
        if (r == HSER_FINISH_DONE) return 0;
        if (r == HSER_FINISH_MORE) return 1;
    }
    return FREYA_ERR_IO;
}

/* Move what the coder has into out, after *n bytes already there.
 * Returns how many it added, or FREYA_ERR_ARG when out is full and the
 * coder still has more.  A full out is probed with one spare byte, so
 * a coder that has nothing more to say is not mistaken for one that
 * does not fit. */
static int drain(lz_stream_t *s, uint8_t *out, int cap, int *n)
{
    int total = 0;

    for (;;) {
        int room = cap - *n;
        int got;

        if (room > 0) {
            got = lz_poll(s, out + *n, room);
            if (got < 0) return got;
            *n += got;
            total += got;
            if (got < room) return total;
        } else {
            uint8_t spare;

            got = lz_poll(s, &spare, 1);
            if (got < 0) return got;
            return got ? FREYA_ERR_ARG : total;
        }
    }
}

static int run(int decode, const uint8_t *in, int in_len,
               uint8_t *out, int out_cap)
{
    lz_stream_t *s;
    int n = 0, off = 0, idle = 0, rc;

    s = lz_open(decode);
    if (!s) return FREYA_ERR_BUSY;

    while (off < in_len) {
        int took = lz_sink(s, in + off, in_len - off);

        if (took < 0) { rc = took; goto done; }
        off += took;
        rc = drain(s, out, out_cap, &n);
        if (rc < 0) goto done;
        /* A sink that takes nothing is a coder that wanted polling,
         * which the drain just did.  Twice in a row it is stuck. */
        if (took == 0 && rc == 0) {
            if (++idle > 1) { rc = FREYA_ERR_IO; goto done; }
        } else {
            idle = 0;
        }
    }

    idle = 0;
    for (;;) {
        rc = lz_finish(s);
        if (rc <= 0) break;
        rc = drain(s, out, out_cap, &n);
        if (rc < 0) break;
        /* The encoder may say "more" once with nothing to flush;
         * the next finish then says "done".  Twice is stuck. */
        if (rc == 0 && ++idle > 1) { rc = FREYA_ERR_IO; break; }
    }
    if (rc == 0) rc = n;
done:
    lz_close(s);
    return rc;
}

static int check(const void *in, int in_len, const void *out, int out_cap)
{
    uintptr_t a = (uintptr_t)in, b = (uintptr_t)out;

    if (in_len < 0 || out_cap < 0) return FREYA_ERR_ARG;
    if (in_len == 0) return 0;
    if (!in || !out) return FREYA_ERR_ARG;
    /* The coder keeps its own copy of the input, but writes output
     * while input is still being read, so the two may not overlap. */
    if (a < b + (uintptr_t)out_cap && b < a + (uintptr_t)in_len)
        return FREYA_ERR_ARG;
    if (app_in_handler()) return FREYA_ERR_HANDLER;
    return 1;
}

int lz_compress(const void *in, int in_len, void *out, int out_cap)
{
    int rc = check(in, in_len, out, out_cap);

    if (rc <= 0) return rc;
    return run(0, in, in_len, out, out_cap);
}

int lz_decompress(const void *in, int in_len, void *out, int out_cap)
{
    int rc = check(in, in_len, out, out_cap);

    if (rc <= 0) return rc;
    return run(1, in, in_len, out, out_cap);
}

#else /* !BOARD_COMPRESS */

int lz_compress(const void *in, int in_len, void *out, int out_cap)
{
    (void)in; (void)in_len; (void)out; (void)out_cap;
    return FREYA_ERR_UNSUPPORTED;
}

int lz_decompress(const void *in, int in_len, void *out, int out_cap)
{
    (void)in; (void)in_len; (void)out; (void)out_cap;
    return FREYA_ERR_UNSUPPORTED;
}

#endif /* BOARD_COMPRESS */
