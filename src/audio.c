/*
 * Freya - audio on a USB headset (AUDIO=1).
 *
 * The program's side is mono 16-bit samples at 8 or 16 kHz, in two rings
 * of FREYA_AUDIO_RING samples: the microphone's, filled by the USB
 * interrupt and drained by audio_read(), and the speaker's, filled by
 * audio_write() and drained by the interrupt.  Each ring has one writer
 * and one reader, so neither side takes a lock.
 *
 * The headset's side is what src/uac.c chose: 16-bit, one or two
 * channels, at 1, 2, 3 or 6 times the program's rate.  Once a 1 ms USB
 * frame the interrupt (src/usbh.c) asks audio_frame_out() for the
 * speaker's next packet and hands each microphone packet it receives to
 * audio_frame_in().  Between the two rates sits one low-pass FIR per
 * ratio, 24 taps per phase (tools/firgen.py): run at the headset's rate
 * and kept every k-th sample for the microphone, run as k polyphase
 * branches for the speaker.  Stereo is mixed down on the way in and the
 * one channel is sent to both on the way out.
 *
 * A kernel built without AUDIO=1 keeps only the calls, which answer
 * FREYA_ERR_UNSUPPORTED.
 */
#include "freya.h"

#ifndef FREYA_AUDIO

int audio_open(uint32_t rate, int dirs) { (void)rate; (void)dirs; return FREYA_ERR_UNSUPPORTED; }
int audio_close(void) { return FREYA_ERR_UNSUPPORTED; }
int audio_read(int16_t *buf, int count) { (void)buf; (void)count; return FREYA_ERR_UNSUPPORTED; }
int audio_write(const int16_t *buf, int count) { (void)buf; (void)count; return FREYA_ERR_UNSUPPORTED; }
int audio_status(freya_audio_status_t *st) { (void)st; return FREYA_ERR_UNSUPPORTED; }
int audio_gain(int dirs, int gain) { (void)dirs; (void)gain; return FREYA_ERR_UNSUPPORTED; }
void audio_release(void) { }

#else

#define RING        FREYA_AUDIO_RING
#define MASK        (RING - 1U)
#define PHASE_TAPS  24
#define MAX_K       6
#define MAX_TAPS    (PHASE_TAPS * MAX_K)
#define PRIME_MS    20          /* the speaker starts with this much queued */

_Static_assert((RING & (RING - 1)) == 0, "the ring is a power of two");

/* The compiler may not move a sample past the index that publishes it. */
#define BARRIER()   __asm__ volatile("" ::: "memory")

/* tools/firgen.py: Q15, unity gain at DC, -6 dB at the program's Nyquist. */
static const int16_t s_fir2[48] = {
        -5,     -9,     16,     25,    -37,    -52,     71,     95,
      -125,   -161,    205,    257,   -320,   -396,    488,    600,
      -739,   -917,   1152,   1481,  -1983,  -2860,   4863,  14735,
     14735,   4863,  -2860,  -1983,   1481,   1152,   -917,   -739,
       600,    488,   -396,   -320,    257,    205,   -161,   -125,
        95,     71,    -52,    -37,     25,     16,     -9,     -5,
};
static const int16_t s_fir3[72] = {
        -2,     -7,     -5,      7,     20,     13,    -17,    -43,
       -27,     33,     80,     48,    -58,   -137,    -80,     94,
       219,    127,   -147,   -339,   -195,    224,    513,    294,
      -338,   -778,   -450,    523,   1229,    732,   -888,  -2218,
     -1451,   2058,   6921,  10426,  10432,   6921,   2058,  -1451,
     -2218,   -888,    732,   1229,    523,   -450,   -778,   -338,
       294,    513,    224,   -195,   -339,   -147,    127,    219,
        94,    -80,   -137,    -58,     48,     80,     33,    -27,
       -43,    -17,     13,     20,      7,     -5,     -7,     -2,
};
static const int16_t s_fir6[144] = {
        -1,     -2,     -3,     -4,     -4,     -2,      2,      6,
         9,     11,      9,      4,     -4,    -13,    -20,    -23,
       -19,     -8,      8,     25,     38,     42,     33,     13,
       -15,    -43,    -64,    -70,    -56,    -22,     24,     70,
       103,    111,     88,     35,    -37,   -109,   -160,   -171,
      -134,    -53,     56,    165,    241,    258,    202,     79,
       -85,   -248,   -364,   -391,   -308,   -121,    131,    386,
       571,    620,    495,    199,   -219,   -662,  -1010,  -1140,
      -954,   -406,    483,   1622,   2860,   4017,   4912,   5399,
      5403,   4912,   4017,   2860,   1622,    483,   -406,   -954,
     -1140,  -1010,   -662,   -219,    199,    495,    620,    571,
       386,    131,   -121,   -308,   -391,   -364,   -248,    -85,
        79,    202,    258,    241,    165,     56,    -53,   -134,
      -171,   -160,   -109,    -37,     35,     88,    111,    103,
        70,     24,    -22,    -56,    -70,    -64,    -43,    -15,
        13,     33,     42,     38,     25,      8,     -8,    -19,
       -23,    -20,    -13,     -4,      4,      9,     11,      9,
         6,      2,     -2,     -4,     -4,     -3,     -2,     -1,
};

typedef struct {
    int16_t           buf[RING];
    volatile uint32_t head;     /* samples ever written */
    volatile uint32_t tail;     /* samples ever read    */
} ring_t;

typedef struct {
    const int16_t *h;
    uint16_t taps;              /* of the filter; a phase has taps / k   */
    uint16_t len;               /* of the history                        */
    uint16_t pos;
    uint8_t  k;
    uint8_t  phase;
    int16_t  hist[2 * MAX_TAPS];  /* each sample twice: a window is one run */
} fir_t;

typedef struct {
    uint8_t  on;
    uint8_t  k;                 /* headset rate / program rate           */
    uint8_t  channels;
    uint16_t gain;
    fir_t    fir;
} side_t;

static ring_t   s_mic, s_spk;
static side_t   s_in, s_out;
static uint32_t s_rate;
static uint8_t  s_open;
static uint8_t  s_owner_app;
static volatile uint8_t s_gone;
static uint8_t  s_primed;
static uint32_t s_prime;
static volatile uint32_t s_frames, s_underruns, s_overruns, s_errors;
static audio_stream_t s_mic_stream, s_spk_stream;

static int16_t sat16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int16_t gained(int16_t x, uint16_t gain)
{
    if (gain == FREYA_AUDIO_UNITY) return x;
    return sat16(((int32_t)x * gain) >> 8);
}

static void fir_init(fir_t *f, int k, int decimate)
{
    memset(f, 0, sizeof(*f));
    f->k = (uint8_t)k;
    switch (k) {
    case 2:  f->h = s_fir2; break;
    case 3:  f->h = s_fir3; break;
    case 6:  f->h = s_fir6; break;
    default: f->h = NULL; return;
    }
    f->taps = (uint16_t)(PHASE_TAPS * k);
    f->len = decimate ? f->taps : PHASE_TAPS;
}

static void fir_push(fir_t *f, int16_t x)
{
    f->hist[f->pos] = x;
    f->hist[f->pos + f->len] = x;
    if (++f->pos == f->len) f->pos = 0;
}

/* The microphone: one headset sample in, one program sample out every
 * k-th time.  The filter is symmetric, so each pair of samples the same
 * distance from the middle shares one multiply. */
static int fir_decimate(fir_t *f, int16_t x, int16_t *out)
{
    const int16_t *w, *h = f->h;
    int32_t acc = 0;
    int n = f->taps;

    fir_push(f, x);
    if (++f->phase < f->k) return 0;
    f->phase = 0;
    w = f->hist + f->pos;                       /* oldest .. newest */
    for (int i = 0; i < n / 2; i++)
        acc += (int32_t)h[i] * ((int32_t)w[i] + w[n - 1 - i]);
    *out = sat16((acc + (1 << 14)) >> 15);
    return 1;
}

/* The speaker: one program sample in, k headset samples out.  Branch j
 * is taps j, j+k, j+2k ... against the newest samples, and the k zeros
 * stuffed between program samples are why each branch is scaled by k. */
static void fir_interpolate(fir_t *f, int16_t x, int16_t *out)
{
    const int16_t *w;
    int k = f->k, n = PHASE_TAPS;

    fir_push(f, x);
    w = f->hist + f->pos + n - 1;               /* the newest */
    for (int j = 0; j < k; j++) {
        const int16_t *h = f->h + j;
        int32_t acc = 0;
        for (int m = 0; m < n; m++)
            acc += (int32_t)h[m * k] * w[-m];
        out[j] = sat16((acc * k + (1 << 14)) >> 15);
    }
}

static void put_sample(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

/* ---------------------------------------------- the USB interrupt side */
int audio_frame_out(uint8_t *pkt, int max)
{
    int16_t y[MAX_K];
    uint32_t per_ms = s_rate / 1000U;
    int bytes = 0;

    s_frames++;
    if (!s_out.on) return 0;

    if (!s_primed && s_spk.head - s_spk.tail >= s_prime) s_primed = 1;
    for (uint32_t i = 0; i < per_ms; i++) {
        int16_t x = 0;

        if (s_primed) {
            if (s_spk.head != s_spk.tail) {
                x = s_spk.buf[s_spk.tail & MASK];
                BARRIER();
                s_spk.tail++;
            } else {
                s_primed = 0;                   /* dry: silence until refilled */
                s_underruns++;
            }
        }
        x = gained(x, s_out.gain);
        if (s_out.k == 1) y[0] = x;
        else fir_interpolate(&s_out.fir, x, y);
        for (int j = 0; j < s_out.k; j++) {
            for (int c = 0; c < s_out.channels; c++) {
                if (bytes + 2 > max) return bytes;
                put_sample(pkt + bytes, y[j]);
                bytes += 2;
            }
        }
    }
    return bytes;
}

void audio_frame_in(const uint8_t *pkt, int len)
{
    int step = 2 * s_in.channels;

    if (!s_in.on) return;
    for (int i = 0; i + step <= len; i += step) {
        int32_t x = (int16_t)(pkt[i] | (pkt[i + 1] << 8));
        int16_t y;

        if (s_in.channels == 2)
            x = (x + (int16_t)(pkt[i + 2] | (pkt[i + 3] << 8))) / 2;
        if (s_in.k == 1) y = (int16_t)x;
        else if (!fir_decimate(&s_in.fir, (int16_t)x, &y)) continue;

        if (s_mic.head - s_mic.tail >= RING) {
            s_overruns++;
            continue;
        }
        s_mic.buf[s_mic.head & MASK] = gained(y, s_in.gain);
        BARRIER();
        s_mic.head++;
    }
}

void audio_frame_error(void)
{
    s_errors++;
}

void audio_gone(void)
{
    s_gone = 1;
}

/* ------------------------------------------------- the program's side */
static int in_handler(void)
{
#ifdef FREYA_HOST
    return 0;
#else
    return app_in_handler();
#endif
}

static void side_setup(side_t *s, const audio_stream_t *st, int decimate)
{
    memset(s, 0, sizeof(*s));
    s->gain = FREYA_AUDIO_UNITY;
    if (!st->ep) return;
    s->on = 1;
    s->k = (uint8_t)(st->rate / s_rate);
    s->channels = st->channels;
    fir_init(&s->fir, s->k, decimate);
}

int audio_open(uint32_t rate, int dirs)
{
    int rc;

    if (in_handler()) return FREYA_ERR_HANDLER;
    if (rate == 0) rate = FREYA_AUDIO_RATE;
    if (rate != 8000 && rate != 16000) return FREYA_ERR_ARG;
    if (dirs == 0 || (dirs & ~(FREYA_AUDIO_MIC | FREYA_AUDIO_SPK)))
        return FREYA_ERR_ARG;
    if (s_open) return FREYA_ERR_BUSY;
    if (g_usbdev.kind != USB_KIND_AUDIO) return FREYA_ERR_IO;

    memset(&s_mic_stream, 0, sizeof(s_mic_stream));
    memset(&s_spk_stream, 0, sizeof(s_spk_stream));
    rc = uac_stream_start(rate, dirs, &s_mic_stream, &s_spk_stream);
    if (rc != 0) return rc;

    s_rate = rate;
    side_setup(&s_in, &s_mic_stream, 1);
    side_setup(&s_out, &s_spk_stream, 0);
    s_mic.head = s_mic.tail = 0;
    s_spk.head = s_spk.tail = 0;
    s_primed = 0;
    s_prime = rate / 1000U * PRIME_MS;
    s_frames = s_underruns = s_overruns = s_errors = 0;
    s_gone = 0;

    rc = usbh_iso_start(s_spk_stream.ep, s_spk_stream.mps,
                        s_mic_stream.ep, s_mic_stream.mps);
    if (rc != USBH_OK) {
        uac_stream_stop();
        return FREYA_ERR_IO;
    }
    s_open = 1;
#ifndef FREYA_HOST
    s_owner_app = g_app.running;
#endif
    return 0;
}

int audio_close(void)
{
    if (in_handler()) return FREYA_ERR_HANDLER;
    if (!s_open) return 0;
    usbh_iso_stop();
    s_in.on = s_out.on = 0;
    s_open = 0;
    s_owner_app = 0;
    uac_stream_stop();
    return 0;
}

/* The run that opened audio has ended. */
void audio_release(void)
{
    if (s_open && s_owner_app) (void)audio_close();
}

int audio_read(int16_t *buf, int count)
{
    uint32_t avail, n;

    if (!s_open || !s_in.on || count < 0) return FREYA_ERR_ARG;
    if (s_gone) return FREYA_ERR_IO;
    avail = s_mic.head - s_mic.tail;
    n = MIN((uint32_t)count, avail);
    for (uint32_t i = 0; i < n; i++)
        buf[i] = s_mic.buf[(s_mic.tail + i) & MASK];
    BARRIER();
    s_mic.tail += n;
    return (int)n;
}

int audio_write(const int16_t *buf, int count)
{
    uint32_t room, n;

    if (!s_open || !s_out.on || count < 0) return FREYA_ERR_ARG;
    if (s_gone) return FREYA_ERR_IO;
    room = RING - (s_spk.head - s_spk.tail);
    n = MIN((uint32_t)count, room);
    for (uint32_t i = 0; i < n; i++)
        s_spk.buf[(s_spk.head + i) & MASK] = buf[i];
    BARRIER();
    s_spk.head += n;
    return (int)n;
}

int audio_status(freya_audio_status_t *st)
{
    uint32_t queued;

    if (!st) return FREYA_ERR_ARG;
    memset(st, 0, sizeof(*st));
    st->connected = g_usbdev.kind == USB_KIND_AUDIO && !s_gone;
    strncpy(st->name, usbdev_name(), sizeof(st->name) - 1);
    if (!st->connected) st->name[0] = '\0';
    if (!s_open) return 0;
    st->rate = s_rate;
    st->dirs = (uint8_t)((s_in.on ? FREYA_AUDIO_MIC : 0) |
                         (s_out.on ? FREYA_AUDIO_SPK : 0));
    if (s_in.on) {
        st->mic_rate = s_mic_stream.rate;
        st->mic_channels = s_in.channels;
        st->mic_avail = (uint16_t)(s_mic.head - s_mic.tail);
        st->mic_gain = s_in.gain;
    }
    if (s_out.on) {
        queued = s_spk.head - s_spk.tail;
        st->spk_rate = s_spk_stream.rate;
        st->spk_channels = s_out.channels;
        st->spk_queued = (uint16_t)queued;
        st->spk_free = (uint16_t)(RING - queued);
        st->spk_gain = s_out.gain;
    }
    st->frames = s_frames;
    st->underruns = s_underruns;
    st->overruns = s_overruns;
    st->errors = s_errors;
    return 0;
}

int audio_gain(int dirs, int gain)
{
    if (dirs == 0 || (dirs & ~(FREYA_AUDIO_MIC | FREYA_AUDIO_SPK)))
        return FREYA_ERR_ARG;
    if (gain < 0 || gain > FREYA_AUDIO_GAIN_MAX) return FREYA_ERR_ARG;
    if (dirs & FREYA_AUDIO_MIC) s_in.gain = (uint16_t)gain;
    if (dirs & FREYA_AUDIO_SPK) s_out.gain = (uint16_t)gain;
    return 0;
}

#endif /* FREYA_AUDIO */
