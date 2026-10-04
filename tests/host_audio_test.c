/*
 * Host side exercise for USB headsets and the audio calls (AUDIO=1).
 *
 * src/uac.c, src/audio.c and src/usbdev.c compiled unchanged.  The
 * headset is a configuration descriptor shaped like a common UAC1 one -
 * a 48 kHz stereo speaker, a 16/48 kHz mono microphone, a 24-bit setting
 * Freya cannot use and a HID interface - and the USB interrupt is this
 * test calling audio_frame_out() and audio_frame_in() once per
 * simulated millisecond.  Tones go through both resamplers and are
 * measured on the far side: their level, their frequency, and what is
 * left of the images and aliases the filters are there to remove.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "freya.h"

/* ------------------------------------------------- board stubs */
sys_clocks_t g_clocks;
app_state_t  g_app;

static int s_fail, s_checks;
static uint32_t s_ticks;

void uart_putc(char c) { fputc(c, stdout); }
void uart_puts(const char *s) { fputs(s, stdout); }
uint32_t sys_ticks(void) { return s_ticks += 1; }
void sys_delay_ms(uint32_t ms) { s_ticks += ms; }

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

/* ------------------------------------------------- the simulated headset */
static const uint8_t s_cfg_headset[] = {
    9, 2, 0, 0, 4, 1, 0, 0x80, 50,                      /* total patched */
    /* AudioControl */
    9, 4, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    10, 0x24, 0x01, 0x00, 0x01, 0x46, 0x00, 2, 1, 2,    /* header       */
    12, 0x24, 0x02, 1, 0x01, 0x01, 0, 2, 0x03, 0, 0, 0, /* IT streaming */
    10, 0x24, 0x06, 2, 1, 1, 0x03, 0x01, 0x01, 0,       /* FU 2: mute+vol, mute L, R */
    9, 0x24, 0x03, 3, 0x02, 0x04, 0, 2, 0,              /* OT headset   */
    12, 0x24, 0x02, 4, 0x01, 0x02, 0, 1, 0x00, 0, 0, 0, /* IT microphone */
    9, 0x24, 0x06, 5, 4, 1, 0x03, 0x00, 0,              /* FU 5: mute+vol */
    9, 0x24, 0x03, 6, 0x01, 0x01, 0, 5, 0,              /* OT streaming */
    /* the speaker */
    9, 4, 1, 0, 0, 0x01, 0x02, 0x00, 0,
    9, 4, 1, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 1, 1, 0x01, 0x00,
    14, 0x24, 0x02, 1, 2, 2, 16, 2, 0x80, 0xBB, 0x00, 0x44, 0xAC, 0x00,
    9, 0x05, 0x01, 0x09, 0xC4, 0x00, 1, 0, 0,          /* OUT, adaptive, 196 */
    7, 0x25, 0x01, 0x01, 0, 0, 0,
    /* the microphone */
    9, 4, 2, 0, 0, 0x01, 0x02, 0x00, 0,
    9, 4, 2, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 6, 1, 0x01, 0x00,
    14, 0x24, 0x02, 1, 1, 2, 16, 2, 0x80, 0x3E, 0x00, 0x80, 0xBB, 0x00,
    9, 0x05, 0x82, 0x05, 0x62, 0x00, 1, 0, 0,          /* IN, async, 98 */
    7, 0x25, 0x01, 0x01, 0, 0, 0,
    9, 4, 2, 2, 1, 0x01, 0x02, 0x00, 0,                 /* 24-bit: not for us */
    7, 0x24, 0x01, 6, 1, 0x01, 0x00,
    11, 0x24, 0x02, 1, 1, 3, 24, 1, 0x80, 0xBB, 0x00,
    9, 0x05, 0x82, 0x05, 0x92, 0x00, 1, 0, 0,
    7, 0x25, 0x01, 0x01, 0, 0, 0,
    /* buttons */
    9, 4, 3, 0, 1, 0x03, 0x00, 0x00, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x20, 0,
    7, 0x05, 0x83, 0x03, 0x04, 0x00, 16,
};

static uint8_t  s_mic_rate_only48;      /* the microphone offers 48 kHz alone */
static int      s_set_alt[4];           /* SET_INTERFACE per interface    */
static uint32_t s_set_freq[3];          /* SET_CUR frequency per endpoint */
static int      s_unmuted;
static int      s_iso_on;
static uint8_t  s_iso_out, s_iso_in;

int usbh_open(uint32_t wait_ms) { (void)wait_ms; return USBH_OK; }
void usbh_close(void) { }
int usbh_bulk(uint8_t addr, uint8_t ep, uint16_t mps, uint8_t *toggle,
              void *buf, uint32_t len, uint32_t *done, uint32_t wait_ms)
{ return USBH_STALL; }

int usbh_control(uint8_t addr, uint8_t mps0, const uint8_t setup[8],
                 void *data, uint16_t *len)
{
    uint16_t value = (uint16_t)(setup[2] | (setup[3] << 8));
    uint16_t index = (uint16_t)(setup[4] | (setup[5] << 8));
    const uint8_t *d = data;

    if (s_iso_on) {
        printf("  FAIL  a control transfer while streaming\n");
        s_fail++;
    }
    if (setup[0] == 0x01 && setup[1] == 0x0B) {
        if (index < 4) s_set_alt[index] = value;
        return USBH_OK;
    }
    if (setup[0] == 0x22 && setup[1] == 0x01 && value == 0x0100) {
        s_set_freq[index & 3] = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                                ((uint32_t)d[2] << 16);
        return USBH_OK;
    }
    if (setup[0] == 0x21 && setup[1] == 0x01 && (value >> 8) == 1 && d[0] == 0) {
        s_unmuted++;
        return USBH_OK;
    }
    if (len) *len = 0;
    return USBH_STALL;
}

int usbh_iso_start(uint8_t out_ep, uint16_t out_mps, uint8_t in_ep, uint16_t in_mps)
{
    s_iso_on = 1;
    s_iso_out = out_ep;
    s_iso_in = in_ep;
    return USBH_OK;
}

void usbh_iso_stop(void) { s_iso_on = 0; }

/* No stick in this test. */
int usbvol_attach(int boot) { (void)boot; return -1; }
void usbvol_unmount(void) { }

static void plug(void)
{
    memset(&g_usbdev, 0, sizeof(g_usbdev));
    memcpy(g_usbdev.cfg, s_cfg_headset, sizeof(s_cfg_headset));
    g_usbdev.cfg_len = sizeof(s_cfg_headset);
    g_usbdev.cfg[2] = (uint8_t)sizeof(s_cfg_headset);
    g_usbdev.cfg[3] = (uint8_t)(sizeof(s_cfg_headset) >> 8);
    if (s_mic_rate_only48) {
        /* the microphone's 16 kHz entry becomes a second 48 kHz one */
        for (uint16_t i = 0; i + 14 <= g_usbdev.cfg_len; i++)
            if (g_usbdev.cfg[i] == 14 && g_usbdev.cfg[i + 1] == 0x24 &&
                g_usbdev.cfg[i + 2] == 0x02 && g_usbdev.cfg[i + 4] == 1) {
                g_usbdev.cfg[i + 8] = 0x80; g_usbdev.cfg[i + 9] = 0xBB;
            }
    }
    g_usbdev.addr = 1;
    g_usbdev.mps0 = 64;
    g_usbdev.kind = USB_KIND_AUDIO;
    strcpy(g_usbdev.product, "SimHeadset");
}

/* ------------------------------------------------- measuring */
#define MAXS 48000

/* The level of frequency f in x[0..n), in the units of x. */
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

static double db(double ratio) { return 20 * log10(ratio > 1e-12 ? ratio : 1e-12); }

static double s_out[MAXS];
static int    s_nout;

/* Runs ms frames: the speaker's packets are collected (left channel),
 * and the microphone, if fed, gets a tone at the headset's rate. */
static void run_frames(int ms, int stereo, uint32_t mic_rate, double mic_hz,
                       double mic_amp, int16_t *feed_phase_buf)
{
    static uint8_t pkt[400];
    static uint32_t mic_n;
    (void)feed_phase_buf;

    for (int f = 0; f < ms; f++) {
        int n = audio_frame_out(pkt, sizeof(pkt));
        for (int i = 0; i + 2 * stereo <= n; i += 2 * stereo) {
            int16_t l = (int16_t)(pkt[i] | (pkt[i + 1] << 8));
            if (stereo == 2) {
                int16_t r = (int16_t)(pkt[i + 2] | (pkt[i + 3] << 8));
                if (l != r) { s_fail++; printf("  FAIL  left and right differ\n"); return; }
            }
            if (s_nout < MAXS) s_out[s_nout++] = l;
        }
        if (mic_rate) {
            uint8_t in[200];
            int per = (int)(mic_rate / 1000);
            for (int i = 0; i < per; i++, mic_n++) {
                int16_t v = (int16_t)lrint(mic_amp * sin(2 * M_PI * mic_hz * mic_n / mic_rate));
                in[2 * i] = (uint8_t)v;
                in[2 * i + 1] = (uint8_t)((uint16_t)v >> 8);
            }
            audio_frame_in(in, 2 * per);
        }
    }
}

static void write_tone(double hz, double amp, uint32_t rate, int count, uint32_t *n)
{
    int16_t buf[64];

    while (count > 0) {
        int k = count < 64 ? count : 64;
        for (int i = 0; i < k; i++, (*n)++)
            buf[i] = (int16_t)lrint(amp * sin(2 * M_PI * hz * *n / rate));
        k = audio_write(buf, k);
        if (k <= 0) break;
        count -= k;
    }
}

/* ------------------------------------------------- the tests */
static void test_descriptors(void)
{
    uint32_t r = 0;
    const uac_alt_t *a;
    int n;

    printf("\n--- the headset's descriptors ---\n");
    plug();
    n = uac_parse(g_usbdev.cfg, g_usbdev.cfg_len);
    check(n == 2, "two usable settings: the 24-bit one is left out");
    a = uac_pick(0, 16000, &r);
    check(a && a->ep == 0x01 && r == 48000 && a->channels == 2,
          "16 kHz speaker: the headset's 48 kHz stereo, three times");
    a = uac_pick(1, 16000, &r);
    check(a && a->ep == 0x82 && r == 16000 && a->channels == 1,
          "16 kHz microphone: its own 16 kHz, no resampling");
    a = uac_pick(0, 8000, &r);
    check(a && r == 48000, "8 kHz speaker: 48 kHz, six times");
    a = uac_pick(1, 8000, &r);
    check(a && r == 16000, "8 kHz microphone: 16 kHz, twice");
    (void)uac_alts(&n);
    check(n == 2, "uac_alts() agrees");

    /* A headset at 44.1 kHz alone cannot be resampled by a fixed ratio. */
    for (uint16_t i = 0; i + 14 <= g_usbdev.cfg_len; i++)
        if (g_usbdev.cfg[i] == 14 && g_usbdev.cfg[i + 1] == 0x24 && g_usbdev.cfg[i + 2] == 0x02) {
            g_usbdev.cfg[i + 7] = 1;
            g_usbdev.cfg[i + 8] = 0x44; g_usbdev.cfg[i + 9] = 0xAC; g_usbdev.cfg[i + 10] = 0;
        }
    uac_parse(g_usbdev.cfg, g_usbdev.cfg_len);
    check(uac_pick(0, 16000, NULL) == NULL && uac_pick(1, 8000, NULL) == NULL,
          "a 44.1 kHz-only headset has nothing to offer");
    plug();
}

static void test_speaker(uint32_t rate)
{
    freya_audio_status_t st;
    uint32_t n = 0;
    double hz = rate == 16000 ? 1000 : 600, amp = 10000, level, image;
    char what[96];
    int start;

    printf("\n--- speaker at %u Hz ---\n", rate);
    plug();
    check(uac_attach(0) == 0, "attach parses and unmutes");
    check(s_unmuted == 4, "both feature units unmuted, master and channels");
    s_unmuted = 0;
    check(audio_open(rate, FREYA_AUDIO_SPK) == 0, "audio_open, the speaker");
    check(s_set_alt[1] == 1 && s_set_freq[1] == 48000, "alt 1 on interface 1, 48 kHz on EP 1");
    check(s_iso_on && s_iso_out == 0x01 && s_iso_in == 0, "streaming the speaker alone");

    s_nout = 0;
    write_tone(hz, amp, rate, rate / 1000 * 10, &n);           /* 10 ms */
    run_frames(5, 2, 0, 0, 0, NULL);
    audio_status(&st);
    check(st.spk_queued == rate / 100, "under 20 ms queued: nothing played yet");
    for (int i = 0; i < s_nout; i++) if (s_out[i] != 0) { s_fail++; break; }
    check(st.underruns == 0, "and silence before the start is no underrun");

    s_nout = 0;
    for (int ms = 0; ms < 400; ms++) {
        write_tone(hz, amp, rate, rate / 1000, &n);
        run_frames(1, 2, 0, 0, 0, NULL);
    }
    start = 48 * 100;                                           /* settled */
    level = goertzel(s_out + start, 48 * 200, hz, 48000);
    snprintf(what, sizeof(what), "a %.0f Hz tone comes out at its level (%.1f dB)",
             hz, db(level / amp));
    check(fabs(db(level / amp)) < 0.2, what);
    image = goertzel(s_out + start, 48 * 200, rate - hz, 48000);
    if (rate == 8000) image = fmax(image, goertzel(s_out + start, 48 * 200, 2 * rate - hz, 48000));
    snprintf(what, sizeof(what), "its first image is %.0f dB down", db(image / amp));
    check(db(image / amp) < -55, what);
    audio_status(&st);
    check(st.underruns == 0 && st.errors == 0, "no underrun while it is fed");

    run_frames(30, 2, 0, 0, 0, NULL);
    audio_status(&st);
    check(st.underruns == 1, "running dry is one underrun");
    write_tone(hz, amp, rate, rate / 1000 * 25, &n);
    run_frames(10, 2, 0, 0, 0, NULL);
    audio_status(&st);
    check(st.underruns == 1 && st.spk_queued < rate / 1000 * 25,
          "it plays again once 20 ms are queued");
    check(audio_close() == 0 && !s_iso_on && s_set_alt[1] == 0,
          "audio_close stops the stream and frees the bus");
}

static void test_microphone(uint32_t rate, int only48)
{
    freya_audio_status_t st;
    static int16_t got[8192];
    static double x[8192];
    uint32_t dev;
    int n = 0, r;
    double hz = rate == 16000 ? 1000 : 500, level, alias;
    char what[96];

    s_mic_rate_only48 = (uint8_t)only48;
    plug();
    uac_attach(0);
    dev = rate == 16000 ? (only48 ? 48000 : 16000) : (only48 ? 48000 : 16000);
    printf("\n--- microphone at %u Hz from %u Hz ---\n", rate, dev);
    check(audio_open(rate, FREYA_AUDIO_MIC | FREYA_AUDIO_SPK) == 0, "audio_open, both ways");
    check(s_set_alt[2] == 1 && s_set_freq[2] == dev, "alt 1 on interface 2, at the right rate");
    check(s_iso_out == 0x01 && s_iso_in == 0x82, "streaming both endpoints");

    for (int ms = 0; ms < 300; ms++) {
        run_frames(1, 2, dev, hz, 12000, NULL);
        r = audio_read(got + n, (int)(sizeof(got) / 2) - n);
        if (r > 0) n += r;
    }
    snprintf(what, sizeof(what), "%d samples in 300 ms", n);
    check(n >= (int)(rate / 1000 * 299) && n <= (int)(rate / 1000 * 300), what);
    for (int i = 0; i < n; i++) x[i] = got[i];
    level = goertzel(x + rate / 10, n - rate / 10, hz, rate);
    snprintf(what, sizeof(what), "a %.0f Hz tone arrives at its level (%.1f dB)",
             hz, db(level / 12000));
    check(fabs(db(level / 12000)) < 0.2, what);

    if (dev != rate) {
        /* A tone above the program's Nyquist must not fold back in. */
        double high = rate * 0.65;
        int m = 0;
        audio_close();
        audio_open(rate, FREYA_AUDIO_MIC);
        for (int ms = 0; ms < 300; ms++) {
            run_frames(1, 2, dev, high, 12000, NULL);
            r = audio_read(got + m, (int)(sizeof(got) / 2) - m);
            if (r > 0) m += r;
        }
        for (int i = 0; i < m; i++) x[i] = got[i];
        alias = goertzel(x + rate / 10, m - rate / 10, rate - high, rate);
        snprintf(what, sizeof(what), "%.0f Hz is %.0f dB down where it would alias",
                 high, db(alias / 12000));
        check(db(alias / 12000) < -55, what);
    }

    audio_gain(FREYA_AUDIO_MIC, FREYA_AUDIO_UNITY / 2);
    n = 0;
    while (audio_read(got, 512) > 0) { }
    for (int ms = 0; ms < 100; ms++) {
        run_frames(1, 2, dev, hz, 12000, NULL);
        r = audio_read(got + n, 4096 - n);
        if (r > 0) n += r;
    }
    for (int i = 0; i < n; i++) x[i] = got[i];
    level = goertzel(x + n / 2, n / 2, hz, rate);
    check(fabs(db(level / 6000)) < 0.3, "gain 128 halves it");

    for (int ms = 0; ms < 200; ms++) run_frames(1, 2, dev, hz, 12000, NULL);
    audio_status(&st);
    check(st.overruns > 0 && st.mic_avail == FREYA_AUDIO_RING,
          "unread for 200 ms: the ring is full and the rest is counted");
    audio_close();
    s_mic_rate_only48 = 0;
}

static void test_calls(void)
{
    int16_t b[4];

    printf("\n--- the calls ---\n");
    plug();
    uac_attach(0);
    check(audio_open(44100, FREYA_AUDIO_SPK) == FREYA_ERR_ARG, "44.1 kHz is not a program rate");
    check(audio_open(16000, 0) == FREYA_ERR_ARG, "no direction is an error");
    check(audio_open(16000, 4) == FREYA_ERR_ARG, "nor is an unknown one");
    check(audio_read(b, 4) == FREYA_ERR_ARG, "reading a closed microphone");
    check(audio_open(0, FREYA_AUDIO_SPK) == 0, "rate 0 is 16 kHz");
    check(audio_open(0, FREYA_AUDIO_SPK) == FREYA_ERR_BUSY, "opening twice is busy");
    check(audio_read(b, 4) == FREYA_ERR_ARG, "the microphone was not opened");
    check(audio_gain(FREYA_AUDIO_SPK, FREYA_AUDIO_GAIN_MAX + 1) == FREYA_ERR_ARG,
          "gain past x4 is refused");
    audio_gone();
    check(audio_write(b, 4) == FREYA_ERR_IO, "unplugged: writing fails");
    check(audio_close() == 0, "and it still closes");
    g_usbdev.kind = USB_KIND_MSC;
    check(audio_open(0, FREYA_AUDIO_SPK) == FREYA_ERR_IO, "a stick is no headset");
}

int main(void)
{
    printf("USB headset\n");
    test_descriptors();
    test_speaker(16000);
    test_speaker(8000);
    test_microphone(16000, 0);
    test_microphone(16000, 1);
    test_microphone(8000, 0);
    test_calls();
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
