/*
 * opusrec - record the headset's microphone to an Ogg Opus file.
 *
 *     install("opusrec.xip.bin")                   once: it runs from flash
 *     run("@flash")                                10 s to /spi1/rec.opus
 *     run("@flash", "/spi1/memo.opus", "30", "24000")   30 s at 24 kbit/s
 *
 * Arguments: the file (default /spi1/rec.opus), seconds (default 10, 0
 * until a key), the bitrate in bit/s (default 16000) and the encoder's
 * complexity, 0..10 (default 3).  Any key, Ctrl-C included, ends the
 * recording early and still closes the file properly.
 *
 * 16 kHz wideband, mono, Opus in VoIP mode with 20 ms frames, in an Ogg
 * Opus file any player opens.  A timer interrupt every 10 ms moves the
 * microphone's samples into a half-second buffer of this program's, so a
 * slow flash erase while a page is written costs no audio; the loop
 * encodes from that buffer and writes.  It links the codec pack: build
 * with make USB=1 AUDIO=1 CODECS=1, on a board with SPI flash for /spi1
 * (the STM32U585 or the STM32H723), or give a path on the card or a stick.
 */
#include <stddef.h>
#include "freya_api.h"
#include "freya_codecs.h"

#define RATE        16000
#define FRAME       320                 /* 20 ms                         */
#define RING        8192                /* 512 ms, a power of two        */
#define TICK_US     10000

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
            n = s_api->audio_read(drop, 64);    /* full: keep the newest out */
            if (n > 0) s_lost += (uint32_t)n;
            if (n <= 0) return;
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
    int fd = (int)(intptr_t)ctx;

    return s_api->write(fd, buf, len) == len ? len : -1;
}

static int number(const char *s, uint32_t *out)
{
    uint32_t n = 0;

    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') n = n * 10U + (uint32_t)(*s++ - '0');
    *out = n;
    return *s ? -1 : 0;
}

static const char *audio_why(int rc)
{
    if (rc == FREYA_ERR_IO) return "no headset - plug one in and run usb(\"mount\")";
    if (rc == FREYA_ERR_UNSUPPORTED) return "this kernel or headset has no microphone at 16 kHz";
    if (rc == FREYA_ERR_BUSY) return "audio is in use";
    return "refused";
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/spi1/rec.opus";
    uint32_t secs = 10, bitrate = 16000, complexity = 3, frames = 0, bytes = 0;
    OpusEncoder *enc;
    opus_int32 lookahead = 0;
    int16_t pcm[FRAME];
    uint8_t pkt[256];
    int fd, timer, rc, status = 0;

    s_api = api;
    if (!FREYA_API_HAS(api, audio_gain)) {
        api->puts("opusrec: this kernel has no audio calls\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc > 5 || (argc > 2 && number(argv[2], &secs) != 0) ||
        (argc > 3 && number(argv[3], &bitrate) != 0) ||
        (argc > 4 && number(argv[4], &complexity) != 0) ||
        bitrate < 6000 || bitrate > 64000 || complexity > 10) {
        api->puts("usage: opusrec [file] [seconds, 0 = until a key] "
                  "[bit/s 6000..64000] [complexity 0..10]\r\n");
        return FREYA_EXIT_USAGE;
    }

    enc = api->malloc((uint32_t)opus_encoder_get_size(1));
    if (!enc) {
        api->printf("opusrec: no memory for the encoder (%d B)\r\n",
                    opus_encoder_get_size(1));
        return FREYA_EXIT_FAIL;
    }
    if (opus_encoder_init(enc, RATE, 1, OPUS_APPLICATION_VOIP) != OPUS_OK) {
        api->free(enc);
        api->puts("opusrec: the encoder did not start\r\n");
        return FREYA_EXIT_FAIL;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE((opus_int32)bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY((opus_int32)complexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

    fd = api->open(path, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd < 0) {
        api->free(enc);
        api->printf("opusrec: cannot create %s (%d)\r\n", path, fd);
        return FREYA_EXIT_FAIL;
    }
    if (freya_oggopus_open(&s_ogg, file_write, (void *)(intptr_t)fd, 1, RATE,
                           (uint16_t)(lookahead * (48000 / RATE)),
                           api->ticks_ms() ^ 0x46524559UL) != 0) {
        api->printf("opusrec: cannot write %s\r\n", path);
        status = FREYA_EXIT_FAIL;
        goto out_file;
    }

    rc = api->audio_open(RATE, FREYA_AUDIO_MIC);
    if (rc != 0) {
        api->printf("opusrec: %s\r\n", audio_why(rc));
        status = FREYA_EXIT_FAIL;
        goto out_ogg;
    }
    timer = api->timer_open(TICK_US, 0, on_tick, NULL);
    if (timer < 0) {
        api->printf("opusrec: no timer free (%d)\r\n", timer);
        status = FREYA_EXIT_FAIL;
        goto out_audio;
    }
    api->console_raw(1);                        /* Ctrl-C is a key: close properly */
    api->timer_start(timer);
    api->printf("opusrec: %s, %u kbit/s, complexity %u - a key stops it\r\n",
                path, bitrate / 1000, complexity);

    while (secs == 0 || frames < secs * 50U) {
        if (api->kbhit()) {
            (void)api->getc();
            break;
        }
        if (s_gone) {
            api->puts("opusrec: the headset is gone\r\n");
            status = FREYA_EXIT_FAIL;
            break;
        }
        if (s_head - s_tail < FRAME) {
            api->thread_sleep(2);
            continue;
        }
        for (int i = 0; i < FRAME; i++)
            pcm[i] = s_ring[(s_tail + (uint32_t)i) & (RING - 1)];
        s_tail += FRAME;

        rc = opus_encode(enc, pcm, FRAME, pkt, sizeof(pkt));
        if (rc < 0) {
            api->printf("opusrec: encoder error %d\r\n", rc);
            status = FREYA_EXIT_FAIL;
            break;
        }
        if (freya_oggopus_packet(&s_ogg, pkt, rc, FRAME * (48000 / RATE)) != 0) {
            api->puts("opusrec: the file is full, or the write failed\r\n");
            status = FREYA_EXIT_FAIL;
            break;
        }
        bytes += (uint32_t)rc;
        if (++frames % 250 == 0)
            api->printf("  %u s, %u B\r\n", frames / 50, bytes);
    }

    api->timer_stop(timer);
    api->timer_close(timer);
    api->console_raw(0);
out_audio:
    api->audio_close();
out_ogg:
    if (freya_oggopus_close(&s_ogg) != 0 && status == 0) status = FREYA_EXIT_FAIL;
out_file:
    api->close(fd);
    api->free(enc);
    api->printf("opusrec: %u.%02u s, %u B of Opus in %s%s\r\n", frames / 50,
                (frames % 50) * 2, bytes, path,
                s_lost ? " - some audio was lost: the file system was too slow" : "");
    return status;
}
