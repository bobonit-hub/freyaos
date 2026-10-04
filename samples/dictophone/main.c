/*
 * dictophone - a voice recorder on the WeAct STM32F4 64-pin board.
 *
 *     install("dictophone.xip.bin")        once: it runs from flash
 *     run("@flash")                        /voice.opus on the card
 *     run("@flash", "/notes/voice.opus", "24000")
 *
 * Press KEY (PC13) to start recording the USB headset's microphone, and
 * again to stop: the take is written to voice.opus on the SD card, an
 * Ogg Opus file any player opens, and the next take replaces it.  The
 * blue LED is lit while it records.  A key on the console, or Ctrl-C,
 * ends the program, closing a take in progress properly first.
 *
 * Arguments: the file (default /voice.opus) and the bitrate in bit/s
 * (6000..64000, default 16000).  16 kHz wideband, mono, Opus in VoIP mode
 * with 20 ms frames.  A timer interrupt every 10 ms moves the
 * microphone's samples into a half-second buffer, so a slow card write
 * costs no audio; the loop encodes from that buffer and writes.
 *
 * Build: make BOARD=weact_f405 SD=1 USB=1 AUDIO=1 CODECS=1.  The
 * headset is the one device on the USB-C socket, the card is the board's
 * own microSD slot.
 */
#include <stddef.h>
#include "freya_api.h"
#include "freya_codecs.h"

#ifndef FREYA_BOARD_WEACT_F405
#error "dictophone is for the WeAct STM32F4 64-pin board (BOARD=weact_f405): KEY is its PC13"
#endif

#define KEY_PIN     FREYA_PC(13)        /* pulls high when pressed       */
#define DEBOUNCE_MS 30
#define RATE        16000
#define FRAME       320                 /* 20 ms                         */
#define RING        8192                /* 512 ms, a power of two        */
#define TICK_US     10000
#define COMPLEXITY  3                   /* real time on the 168 MHz F405 */

static const freya_api_t *s_api;
static int16_t s_ring[RING];
static volatile uint32_t s_head, s_tail;
static volatile uint32_t s_lost;
static volatile int s_gone;
static freya_oggopus_t s_ogg;

/* The timer: whatever the headset has, into the ring.  audio_read() may
 * be called from a handler; nothing here touches Opus or the file. */
static void on_tick(int source, void *arg)
{
    (void)source; (void)arg;
    for (;;) {
        uint32_t room = RING - (s_head - s_tail);
        uint32_t at = s_head & (RING - 1), run = RING - at;
        int n;

        if (room == 0) {
            int16_t drop[64];
            n = s_api->audio_read(drop, 64);
            if (n <= 0) return;
            s_lost += (uint32_t)n;
            continue;
        }
        n = s_api->audio_read(s_ring + at, (int)(run < room ? run : room));
        if (n < 0) { s_gone = 1; return; }
        if (n == 0) return;
        s_head += (uint32_t)n;
    }
}

static int file_write(void *ctx, const void *buf, int len)
{
    return s_api->write((int)(intptr_t)ctx, buf, len) == len ? len : -1;
}

static int number(const char *s, uint32_t *out)
{
    uint32_t n = 0;

    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') n = n * 10U + (uint32_t)(*s++ - '0');
    *out = n;
    return *s ? -1 : 0;
}

/* KEY, debounced: 1 once per press, when it has been held DEBOUNCE_MS. */
static int key_pressed(void)
{
    static int stable, last;
    static uint32_t since;
    int now = s_api->pin_read(KEY_PIN) == 1;

    if (now != last) {
        last = now;
        since = s_api->ticks_ms();
        return 0;
    }
    if (now != stable && s_api->ticks_ms() - since >= DEBOUNCE_MS) {
        stable = now;
        return stable;
    }
    return 0;
}

typedef struct {
    OpusEncoder *enc;
    int          fd;
    int          timer;
    uint32_t     frames;
    uint32_t     bytes;
} take_t;

/* Opens the file, the headset and the timer.  0, or -1 having said why. */
static int take_start(take_t *t, const char *path, uint32_t bitrate)
{
    opus_int32 lookahead = 0;
    int rc;

    t->frames = t->bytes = 0;
    s_head = s_tail = s_lost = 0;
    s_gone = 0;
    if (opus_encoder_init(t->enc, RATE, 1, OPUS_APPLICATION_VOIP) != OPUS_OK) {
        s_api->puts("dictophone: the encoder did not start\r\n");
        return -1;
    }
    opus_encoder_ctl(t->enc, OPUS_SET_BITRATE((opus_int32)bitrate));
    opus_encoder_ctl(t->enc, OPUS_SET_COMPLEXITY(COMPLEXITY));
    opus_encoder_ctl(t->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(t->enc, OPUS_GET_LOOKAHEAD(&lookahead));

    rc = s_api->audio_open(RATE, FREYA_AUDIO_MIC);
    if (rc != 0) {
        s_api->printf("dictophone: %s\r\n",
                      rc == FREYA_ERR_IO ? "no headset - plug one in and run usb(\"mount\")" :
                      rc == FREYA_ERR_UNSUPPORTED ? "the headset has no microphone at 16 kHz" :
                      "audio is in use");
        return -1;
    }
    t->fd = s_api->open(path, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (t->fd < 0) {
        s_api->printf("dictophone: cannot create %s (%d) - is the card in?\r\n",
                      path, t->fd);
        s_api->audio_close();
        return -1;
    }
    if (freya_oggopus_open(&s_ogg, file_write, (void *)(intptr_t)t->fd, 1, RATE,
                           (uint16_t)(lookahead * (48000 / RATE)),
                           s_api->ticks_ms() ^ 0x56434531UL) != 0) {
        s_api->printf("dictophone: cannot write %s\r\n", path);
        s_api->close(t->fd);
        s_api->audio_close();
        return -1;
    }
    t->timer = s_api->timer_open(TICK_US, 0, on_tick, NULL);
    if (t->timer < 0) {
        s_api->printf("dictophone: no timer free (%d)\r\n", t->timer);
        freya_oggopus_close(&s_ogg);
        s_api->close(t->fd);
        s_api->audio_close();
        return -1;
    }
    s_api->timer_start(t->timer);
    s_api->led(1);
    s_api->printf("recording to %s - press KEY to stop\r\n", path);
    return 0;
}

/* Encodes one frame if a whole one is waiting.  1 when it did, 0 when
 * there was nothing yet, -1 when the take cannot go on. */
static int take_frame(take_t *t)
{
    int16_t pcm[FRAME];
    uint8_t pkt[256];
    int n;

    if (s_gone) {
        s_api->puts("dictophone: the headset is gone\r\n");
        return -1;
    }
    if (s_head - s_tail < FRAME) return 0;
    for (int i = 0; i < FRAME; i++)
        pcm[i] = s_ring[(s_tail + (uint32_t)i) & (RING - 1)];
    s_tail += FRAME;

    n = opus_encode(t->enc, pcm, FRAME, pkt, sizeof(pkt));
    if (n < 0) {
        s_api->printf("dictophone: encoder error %d\r\n", n);
        return -1;
    }
    if (freya_oggopus_packet(&s_ogg, pkt, n, FRAME * (48000 / RATE)) != 0) {
        s_api->puts("dictophone: the card is full, or the write failed\r\n");
        return -1;
    }
    t->bytes += (uint32_t)n;
    t->frames++;
    return 1;
}

/* Stops the take: what is still in the ring is encoded first, then the
 * last page closes the file. */
static void take_stop(take_t *t, const char *path)
{
    s_api->timer_stop(t->timer);
    s_api->timer_close(t->timer);
    s_api->audio_close();
    while (take_frame(t) == 1) { }
    freya_oggopus_close(&s_ogg);
    s_api->close(t->fd);
    s_api->led(0);
    s_api->printf("saved %s: %u.%02u s, %u B%s\r\n", path, t->frames / 50,
                  (t->frames % 50) * 2, t->bytes,
                  s_lost ? " - some audio was lost: the card was too slow" : "");
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/voice.opus";
    uint32_t bitrate = 16000;
    take_t take;
    int recording = 0, status = 0;

    s_api = api;
    if (!FREYA_API_HAS(api, audio_gain)) {
        api->puts("dictophone: this kernel has no audio calls\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc > 3 || (argc > 2 && number(argv[2], &bitrate) != 0) ||
        bitrate < 6000 || bitrate > 64000) {
        api->puts("usage: dictophone [file] [bit/s 6000..64000]\r\n");
        return FREYA_EXIT_USAGE;
    }
    if (api->pin_mode(KEY_PIN, FREYA_PIN_IN_PULLDOWN) != 0) {
        api->puts("dictophone: KEY (PC13) is not free\r\n");
        return FREYA_EXIT_FAIL;
    }
    take.enc = api->malloc((uint32_t)opus_encoder_get_size(1));
    if (!take.enc) {
        api->printf("dictophone: no memory for the encoder (%d B)\r\n",
                    opus_encoder_get_size(1));
        return FREYA_EXIT_FAIL;
    }

    api->console_raw(1);                /* Ctrl-C is a key: close properly */
    api->printf("dictophone: press KEY to record to %s, any console key to quit\r\n",
                path);
    for (;;) {
        if (api->kbhit()) {
            (void)api->getc();
            break;
        }
        if (key_pressed()) {
            if (!recording) {
                recording = take_start(&take, path, bitrate) == 0;
            } else {
                take_stop(&take, path);
                recording = 0;
            }
            continue;
        }
        if (recording) {
            int rc = take_frame(&take);
            if (rc < 0) {
                take_stop(&take, path);
                recording = 0;
                status = FREYA_EXIT_FAIL;
            } else if (rc == 1 && take.frames % 250 == 0) {
                api->printf("  %u s\r\n", take.frames / 50);
            }
            if (rc != 1) api->thread_sleep(2);
        } else {
            api->thread_sleep(5);
        }
    }
    if (recording) take_stop(&take, path);
    api->console_raw(0);
    api->free(take.enc);
    return status;
}
