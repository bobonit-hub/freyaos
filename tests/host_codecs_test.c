/*
 * Host side exercise for the codec pack (CODECS=1).
 *
 * codecs/g711.c, codecs/oggopus.c, codecs/opus_glue.c and libopus from
 * third_party/opus, built as the pack builds them: fixed point, no float
 * API, the pseudostack.  G.711 is checked against the reference coding;
 * Opus encodes and decodes a tone at 16 kHz the way samples/opusrec
 * does, and the pseudostack's high-water mark is checked against the
 * scratch the pack keeps; the Ogg writer writes a file that
 * tests/run_tests.sh then hands to ffprobe and ffmpeg.
 *
 *   ./hostcodecs out.opus
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freya_codecs.h"

size_t freya_opus_scratch_used(void);
void   freya_opus_scratch_paint(void);

static int s_fail, s_checks;

static void check(int cond, const char *what)
{
    s_checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        s_fail++;
    }
}

static double goertzel(const double *x, int n, double f, double rate)
{
    double w = 2 * M_PI * f / rate, c = 2 * cos(w), s1 = 0, s2 = 0;

    for (int i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2 * sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

/* ------------------------------------------------------------ G.711 */
static void test_g711(void)
{
    int ok_a = 1, ok_u = 1, mono_a = 1, mono_u = 1;
    double sig = 0, err_a = 0, err_u = 0;
    char what[96];

    printf("\n--- G.711 ---\n");
    check(g711_alaw_encode(0) == 0xD5 && g711_ulaw_encode(0) == 0xFF,
          "silence is 0xD5 in A-law and 0xFF in mu-law");
    check(g711_alaw_decode(0xD5) == 8 && g711_ulaw_decode(0xFF) == 0,
          "and decodes to the reference values");
    check(g711_alaw_decode(0xAA) == 32256 && g711_alaw_decode(0x2A) == -32256,
          "A-law's largest codes are +/-32256");
    check(g711_ulaw_decode(0x80) == 32124 && g711_ulaw_decode(0x00) == -32124,
          "mu-law's largest codes are +/-32124");
    for (int c = 0; c < 256; c++) {
        if (g711_alaw_encode(g711_alaw_decode((uint8_t)c)) != c) ok_a = 0;
        if (c != 0x7F && g711_ulaw_encode(g711_ulaw_decode((uint8_t)c)) != c) ok_u = 0;
    }
    check(ok_a, "every A-law code survives decode and encode");
    check(ok_u, "every mu-law code does too, but 0x7F, the other zero");
    for (int x = -32768; x < 32767; x++) {
        if (g711_alaw_decode(g711_alaw_encode((int16_t)x)) >
            g711_alaw_decode(g711_alaw_encode((int16_t)(x + 1)))) mono_a = 0;
        if (g711_ulaw_decode(g711_ulaw_encode((int16_t)x)) >
            g711_ulaw_decode(g711_ulaw_encode((int16_t)(x + 1)))) mono_u = 0;
    }
    check(mono_a && mono_u, "both are monotonic over every 16-bit sample");
    for (int i = 0; i < 8000; i++) {
        int16_t x = (int16_t)lrint(10000 * sin(2 * M_PI * 1000 * i / 8000.0 + 0.3));
        double ea = x - g711_alaw_decode(g711_alaw_encode(x));
        double eu = x - g711_ulaw_decode(g711_ulaw_encode(x));
        sig += (double)x * x;
        err_a += ea * ea;
        err_u += eu * eu;
    }
    snprintf(what, sizeof(what), "a -10 dBFS tone: A-law %.1f dB, mu-law %.1f dB SNR",
             10 * log10(sig / err_a), 10 * log10(sig / err_u));
    check(10 * log10(sig / err_a) > 35 && 10 * log10(sig / err_u) > 35, what);
}

/* ------------------------------------------------------------- Opus */
static int file_write(void *ctx, const void *buf, int len)
{
    return fwrite(buf, 1, (size_t)len, (FILE *)ctx) == (size_t)len ? len : -1;
}

static void test_opus(const char *path)
{
    enum { RATE = 16000, FRAME = 320, SECONDS = 5 };
    static uint8_t enc_mem[64 * 1024], dec_mem[64 * 1024];
    static double out[RATE * SECONDS];
    OpusEncoder *enc = (OpusEncoder *)enc_mem;
    OpusDecoder *dec = (OpusDecoder *)dec_mem;
    freya_oggopus_t ogg;
    opus_int32 lookahead = 0;
    int16_t pcm[FRAME], back[FRAME];
    uint8_t pkt[400];
    long bytes = 0;
    int n = 0, rc, bad = 0;
    double level;
    char what[128];
    FILE *f;

    printf("\n--- Opus ---\n");
    printf("  --    encoder %d B, decoder %d B (64-bit host)\n",
           opus_encoder_get_size(1), opus_decoder_get_size(1));
    check(opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &rc) == NULL,
          "opus_encoder_create() allocates nothing: NULL");
    freya_opus_scratch_paint();
    check(opus_encoder_init(enc, RATE, 1, OPUS_APPLICATION_VOIP) == OPUS_OK,
          "opus_encoder_init() at 16 kHz, VoIP");
    check(opus_decoder_init(dec, RATE, 1) == OPUS_OK, "opus_decoder_init()");
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

    f = fopen(path, "wb");
    if (!f) { perror(path); s_fail++; return; }
    check(freya_oggopus_open(&ogg, file_write, f, 1, RATE,
                             (uint16_t)(lookahead * (48000 / RATE)), 0x46524559) == 0,
          "the Ogg headers are written");

    for (int fr = 0; fr < RATE * SECONDS / FRAME; fr++) {
        for (int i = 0; i < FRAME; i++) {
            int t = fr * FRAME + i;
            pcm[i] = (int16_t)lrint(8000 * sin(2 * M_PI * 440 * t / RATE) *
                                    (0.6 + 0.4 * sin(2 * M_PI * 3 * t / RATE)));
        }
        rc = opus_encode(enc, pcm, FRAME, pkt, sizeof(pkt));
        if (rc <= 0) { bad++; continue; }
        bytes += rc;
        if (freya_oggopus_packet(&ogg, pkt, rc, FRAME * (48000 / RATE)) != 0) bad++;
        if (opus_decode(dec, pkt, rc, back, FRAME, 0) != FRAME) bad++;
        for (int i = 0; i < FRAME && n < RATE * SECONDS; i++) out[n++] = back[i];
    }
    check(freya_oggopus_close(&ogg) == 0, "the last page is written");
    fclose(f);
    check(bad == 0, "250 frames of 20 ms encode, store and decode");
    snprintf(what, sizeof(what), "%.1f kbit/s at a 24 kbit/s target", bytes * 8.0 / SECONDS / 1000);
    check(bytes * 8 / SECONDS > 12000 && bytes * 8 / SECONDS < 32000, what);

    /* The envelope is 3 Hz, so measure the 440 Hz carrier over whole
     * periods of it, after the decoder has settled. */
    level = goertzel(out + RATE, RATE * 3, 440, RATE);
    snprintf(what, sizeof(what), "the 440 Hz tone decodes at %.1f dB of its level",
             20 * log10(level / 4800));
    check(fabs(20 * log10(level / 4800)) < 1.5, what);

    snprintf(what, sizeof(what), "the pseudostack peaked at %zu of %d bytes",
             freya_opus_scratch_used(), FREYA_OPUS_SCRATCH);
    check(freya_opus_scratch_used() > 0 &&
          freya_opus_scratch_used() + 2048 <= FREYA_OPUS_SCRATCH, what);
}

/* --g711-decode codes out_al.raw out_ul.raw: each code byte decoded both
 * ways, for comparison with SoX's decoders, which agree with ITU-T
 * G.711 code for code.  (Encoding is not compared: SoX rounds a 16-bit
 * sample to 13 or 14 bits first, where the reference truncates.) */
static int g711_decode_files(const char *in, const char *al, const char *ul)
{
    FILE *fi = fopen(in, "rb"), *fa = fopen(al, "wb"), *fu = fopen(ul, "wb");
    int c;

    if (!fi || !fa || !fu) { perror("g711"); return 2; }
    while ((c = fgetc(fi)) != EOF) {
        int16_t a = g711_alaw_decode((uint8_t)c), u = g711_ulaw_decode((uint8_t)c);
        fwrite(&a, 2, 1, fa);
        fwrite(&u, 2, 1, fu);
    }
    fclose(fi); fclose(fa); fclose(fu);
    return 0;
}

/* Every mono stream the pack can be asked for must stay inside the
 * scratch: the pseudostack has no bounds check of its own. */
static void test_scratch(void)
{
    static uint8_t em[64 * 1024], dm[64 * 1024];
    static const int rates[] = { 8000, 16000, 48000 };
    static const int apps[] = { OPUS_APPLICATION_VOIP, OPUS_APPLICATION_AUDIO };
    size_t worst = 0;
    char what[96];

    printf("\n--- Opus scratch, every mono stream ---\n");
    for (int r = 0; r < 3; r++)
        for (int a = 0; a < 2; a++)
            for (int cx = 0; cx <= 10; cx += 5) {
                OpusEncoder *e = (OpusEncoder *)em;
                OpusDecoder *d = (OpusDecoder *)dm;
                int fs = rates[r] / 50;
                int16_t pcm[960], out[960];
                uint8_t pkt[1500];

                freya_opus_scratch_paint();
                opus_encoder_init(e, rates[r], 1, apps[a]);
                opus_decoder_init(d, rates[r], 1);
                opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(cx));
                opus_encoder_ctl(e, OPUS_SET_BITRATE(64000));
                for (int f = 0; f < 50; f++) {
                    int n;
                    for (int i = 0; i < fs; i++)
                        pcm[i] = (int16_t)(8000 * sin(0.05 * i + f) + (rand() % 2000));
                    n = opus_encode(e, pcm, fs, pkt, sizeof(pkt));
                    if (n > 0 && opus_decode(d, pkt, n, out, fs, 0) < 0) s_fail++;
                }
                if (freya_opus_scratch_used() > worst) worst = freya_opus_scratch_used();
            }
    snprintf(what, sizeof(what), "the worst of 18 mono streams used %zu of %d bytes",
             worst, FREYA_OPUS_SCRATCH);
    check(worst + 2048 <= FREYA_OPUS_SCRATCH, what);
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--g711-decode") == 0)
        return g711_decode_files(argv[2], argv[3], argv[4]);
    if (argc != 2) {
        fprintf(stderr, "usage: %s out.opus\n", argv[0]);
        return 2;
    }
    printf("codec pack\n");
    test_g711();
    test_opus(argv[1]);
    test_scratch();
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
