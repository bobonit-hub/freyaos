/*
 * Freya - USB Audio Class 1 headsets (AUDIO=1).
 *
 * src/usbdev.c has enumerated and configured the device and found an
 * audio streaming interface in its configuration.  This reads that
 * configuration: every alternate setting of every streaming interface
 * that carries 16-bit PCM on an isochronous endpoint, and the feature
 * units of the control interface.
 *
 * audio_open() asks uac_stream_start() for a rate and a direction.  The
 * microphone is the setting whose endpoint is IN, the speaker the one
 * whose endpoint is OUT.  The rate chosen is the program's rate times 1,
 * 2, 3 or 6, the smallest the headset offers, so src/audio.c can
 * resample it with a fixed filter; a headset that only runs at 44.1 kHz
 * is refused.  Choosing selects the alternate setting, sets the sampling
 * frequency on the endpoint, and the stream then runs from the USB
 * interrupt.  Closing selects alternate setting 0, which frees the bus.
 *
 * Every feature unit with a mute control is unmuted once at attach.  The
 * headset's volume is left where it powers up; src/audio.c scales in
 * software.  An asynchronous speaker's feedback endpoint is not read:
 * Freya sends exactly the nominal rate, which a headset's buffer absorbs
 * for the length of a call.
 */
#include "freya.h"

#ifndef FREYA_AUDIO
#error "src/uac.c is compiled only with AUDIO=1"
#endif

#define REQ_SET_CUR         0x01
#define REQ_SET_INTERFACE   0x0B

#define CS_INTERFACE        0x24
#define AC_FEATURE_UNIT     0x06
#define AS_GENERAL          0x01
#define AS_FORMAT_TYPE      0x02

#define FU_MUTE             0x01
#define EP_SAMPLING_FREQ    0x01

#define MAX_FU              4

typedef struct {
    uint8_t id;
    uint8_t nch;                /* channels after the master, at most 2 */
    uint8_t ctl[3];             /* master, 1, 2: the low control byte   */
} uac_fu_t;

static uac_alt_t s_alt[UAC_MAX_ALTS];
static int       s_nalt;
static uac_fu_t  s_fu[MAX_FU];
static int       s_nfu;
static uint8_t   s_ac_iface;
static const uac_alt_t *s_mic, *s_spk;

static uint32_t get24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Reads the configuration.  Returns how many settings Freya can use. */
int uac_parse(const uint8_t *cfg, uint16_t len)
{
    uint8_t iface = 0xFF, alt = 0, cls = 0, sub = 0;
    uac_alt_t *cur = NULL;
    int pcm = 0;

    s_nalt = 0;
    s_nfu = 0;
    s_ac_iface = 0;
    for (uint16_t i = 0; i + 2 <= len && cfg[i] >= 2; i += cfg[i]) {
        const uint8_t *d = cfg + i;

        if (i + d[0] > len) break;
        if (d[1] == 4 && d[0] >= 9) {                   /* interface */
            if (cur && !(cur->ep && pcm && cur->subframe == 2 &&
                         cur->bits == 16 && cur->channels >= 1 &&
                         cur->channels <= 2))
                s_nalt--;                               /* not usable */
            cur = NULL;
            pcm = 0;
            iface = d[2]; alt = d[3]; cls = d[5]; sub = d[6];
            if (cls == 1 && sub == 1) s_ac_iface = iface;
            if (cls == 1 && sub == 2 && alt != 0 && s_nalt < UAC_MAX_ALTS) {
                cur = &s_alt[s_nalt++];
                memset(cur, 0, sizeof(*cur));
                cur->iface = iface;
                cur->alt = alt;
            }
            continue;
        }
        if (cls != 1) continue;
        if (d[1] == CS_INTERFACE && sub == 1 && d[2] == AC_FEATURE_UNIT &&
            d[0] >= 7 && d[5] >= 1 && s_nfu < MAX_FU) {
            uac_fu_t *fu = &s_fu[s_nfu++];
            int size = d[5];
            int n = (d[0] - 7) / size;                  /* master + channels */

            memset(fu, 0, sizeof(*fu));
            fu->id = d[3];
            for (int c = 0; c < n && c < 3; c++) fu->ctl[c] = d[6 + c * size];
            fu->nch = (uint8_t)(n > 1 ? MIN(n - 1, 2) : 0);
        } else if (cur && d[1] == CS_INTERFACE && d[2] == AS_GENERAL && d[0] >= 7) {
            pcm = (d[5] | (d[6] << 8)) == 0x0001;
        } else if (cur && d[1] == CS_INTERFACE && d[2] == AS_FORMAT_TYPE &&
                   d[0] >= 8 && d[3] == 1) {
            int n = d[7];

            cur->channels = d[4];
            cur->subframe = d[5];
            cur->bits = d[6];
            if (n == 0 && d[0] >= 14) {                 /* continuous */
                cur->nfreq = 0;
                cur->freq[0] = get24(d + 8);
                cur->freq[1] = get24(d + 11);
            } else {
                if (n > UAC_MAX_FREQ) n = UAC_MAX_FREQ;
                for (int f = 0; f < n && 8 + 3 * f + 3 <= d[0]; f++)
                    cur->freq[cur->nfreq++] = get24(d + 8 + 3 * f);
            }
        } else if (cur && d[1] == 5 && d[0] >= 7 && (d[3] & 3) == 1 && !cur->ep) {
            cur->ep = d[2];                             /* the data endpoint */
            cur->mps = (uint16_t)((d[4] | (d[5] << 8)) & 0x7FF);
            cur->interval = d[6];
        } else if (cur && d[1] == 0x25 && d[2] == 1 && d[0] >= 4) {
            cur->freq_ctl = d[3] & 1;
        }
    }
    if (cur && !(cur->ep && pcm && cur->subframe == 2 && cur->bits == 16 &&
                 cur->channels >= 1 && cur->channels <= 2))
        s_nalt--;
    return s_nalt;
}

static int has_rate(const uac_alt_t *a, uint32_t rate)
{
    if (a->nfreq == 0) return rate >= a->freq[0] && rate <= a->freq[1];
    for (int i = 0; i < a->nfreq; i++)
        if (a->freq[i] == rate) return 1;
    return 0;
}

/*
 * The setting for one direction at a program rate: the smallest ratio
 * the headset offers, then the fewest channels.  NULL when there is
 * none, or when its packets would not fit its endpoint.
 */
const uac_alt_t *uac_pick(int in, uint32_t rate, uint32_t *dev_rate)
{
    static const uint8_t ratios[] = { 1, 2, 3, 6 };

    for (unsigned r = 0; r < sizeof(ratios); r++) {
        uint32_t want = rate * ratios[r];
        const uac_alt_t *best = NULL;

        for (int i = 0; i < s_nalt; i++) {
            const uac_alt_t *a = &s_alt[i];
            uint32_t bytes = want / 1000U * a->channels * 2U;

            if (((a->ep & 0x80) != 0) != (in != 0)) continue;
            if (a->interval != 1 || bytes > a->mps || !has_rate(a, want)) continue;
            if (!best || a->channels < best->channels) best = a;
        }
        if (best) {
            if (dev_rate) *dev_rate = want;
            return best;
        }
    }
    return NULL;
}

const uac_alt_t *uac_alts(int *n)
{
    *n = s_nalt;
    return s_alt;
}

static int set_alt(uint8_t iface, uint8_t alt)
{
    return usbdev_control(0x01, REQ_SET_INTERFACE, alt, iface, NULL, 0, NULL);
}

static void unmute(void)
{
    uint8_t off = 0;

    for (int i = 0; i < s_nfu; i++)
        for (int c = 0; c <= s_fu[i].nch; c++)
            if (s_fu[i].ctl[c] & 1)
                (void)usbdev_control(0x21, REQ_SET_CUR,
                                     (uint16_t)((FU_MUTE << 8) | c),
                                     (uint16_t)((s_fu[i].id << 8) | s_ac_iface),
                                     &off, 1, NULL);
}

static int open_side(const uac_alt_t *a, uint32_t rate, audio_stream_t *st)
{
    uint8_t f[3] = { (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16) };

    if (set_alt(a->iface, a->alt) != USBH_OK) return FREYA_ERR_IO;
    /* One fixed rate needs no asking; anything else is set, and a
     * headset that stalls it is taken at its word for the rate. */
    if (a->freq_ctl || a->nfreq != 1)
        (void)usbdev_control(0x22, REQ_SET_CUR, EP_SAMPLING_FREQ << 8,
                             a->ep, f, 3, NULL);
    st->ep = a->ep;
    st->mps = a->mps;
    st->channels = a->channels;
    st->rate = rate;
    return 0;
}

void uac_stream_stop(void)
{
    if (s_mic) (void)set_alt(s_mic->iface, 0);
    if (s_spk && (!s_mic || s_spk->iface != s_mic->iface))
        (void)set_alt(s_spk->iface, 0);
    s_mic = s_spk = NULL;
}

int uac_stream_start(uint32_t rate, int dirs, audio_stream_t *mic,
                     audio_stream_t *spk)
{
    uint32_t mic_rate = 0, spk_rate = 0;
    const uac_alt_t *m = NULL, *s = NULL;
    int rc = 0;

    if (dirs & FREYA_AUDIO_MIC) {
        m = uac_pick(1, rate, &mic_rate);
        if (!m) return FREYA_ERR_UNSUPPORTED;
    }
    if (dirs & FREYA_AUDIO_SPK) {
        s = uac_pick(0, rate, &spk_rate);
        if (!s) return FREYA_ERR_UNSUPPORTED;
    }
    if (m) {
        rc = open_side(m, mic_rate, mic);
        if (rc == 0) s_mic = m;
    }
    if (rc == 0 && s) {
        rc = open_side(s, spk_rate, spk);
        if (rc == 0) s_spk = s;
    }
    if (rc != 0) uac_stream_stop();
    return rc;
}

static void describe(const char *what, int in)
{
    uint32_t r;
    const uac_alt_t *a = uac_pick(in, FREYA_AUDIO_RATE, &r);

    if (!a) {
        kprintf("%s none", what);
        return;
    }
    kprintf("%s %u kHz %s", what, r / 1000U, a->channels == 2 ? "stereo" : "mono");
}

int uac_attach(int boot)
{
    kprintf(boot ? "[boot] USB audio  : " : "USB audio: ");
    if (uac_parse(g_usbdev.cfg, g_usbdev.cfg_len) <= 0 ||
        (!uac_pick(1, 8000, NULL) && !uac_pick(0, 8000, NULL))) {
        kprintf("no 16-bit stream at 8, 16, 24, 32 or 48 kHz\r\n");
        return -1;
    }
    unmute();
    describe("mic", 1);
    describe(", speaker", 0);
    kprintf(boot ? "\r\n" : " - audio_open() starts it\r\n");
    return 0;
}

void uac_stop(void)
{
    (void)audio_close();
    s_nalt = 0;
    s_nfu = 0;
    s_mic = s_spk = NULL;
}

void uac_info(void)
{
    freya_audio_status_t st;

    kprintf("  usb audio  : ");
    describe("mic", 1);
    describe(", speaker", 0);
    kprintf("\r\n");
    if (audio_status(&st) == 0 && st.dirs) {
        kprintf("  streaming  : %u Hz", st.rate);
        if (st.dirs & FREYA_AUDIO_MIC)
            kprintf(", mic from %u Hz x%u", st.mic_rate, st.mic_channels);
        if (st.dirs & FREYA_AUDIO_SPK)
            kprintf(", speaker to %u Hz x%u", st.spk_rate, st.spk_channels);
        kprintf("\r\n");
    }
}
