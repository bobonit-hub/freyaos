/*
 * echo - a call's echo test on a USB headset.
 *
 *     run echo.bin              16 kHz, what you say comes back 300 ms later
 *     run echo.bin 500 8000     500 ms later, at 8 kHz
 *
 * What the microphone hears is held in a delay line and played on the
 * speaker, the way a phone network's echo test lets a caller hear their
 * own line.  Once a second it prints the microphone's peak and the
 * stream's counters.  Ctrl-C ends it, and the kernel closes the audio.
 * Needs a kernel built with USB=1 AUDIO=1 and a headset in the socket.
 */
#include "freya_api.h"

#define MAX_DELAY_MS    1000
#define CHUNK           160             /* 10 ms at 16 kHz */

static int16_t s_line[16 * MAX_DELAY_MS];

static int number(const char *s, uint32_t *out)
{
    uint32_t n = 0;

    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') n = n * 10U + (uint32_t)(*s++ - '0');
    *out = n;
    return *s ? -1 : 0;
}

static const char *why(int rc)
{
    if (rc == FREYA_ERR_IO) return "no headset - plug one in and run usb(\"mount\")";
    if (rc == FREYA_ERR_UNSUPPORTED) return "this kernel or headset cannot do it";
    if (rc == FREYA_ERR_BUSY) return "audio is in use";
    return "refused";
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    freya_audio_status_t st;
    int16_t buf[CHUNK];
    uint32_t delay_ms = 300, rate = FREYA_AUDIO_RATE, len, pos = 0, ms = 0;
    int peak = 0, rc;

    if (!FREYA_API_HAS(api, audio_gain)) {
        api->puts("echo: this kernel has no audio calls\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc > 3 || (argc > 1 && number(argv[1], &delay_ms) != 0) ||
        (argc > 2 && number(argv[2], &rate) != 0) ||
        delay_ms == 0 || delay_ms > MAX_DELAY_MS) {
        api->puts("usage: echo [delay ms, 1..1000] [8000|16000]\r\n");
        return FREYA_EXIT_USAGE;
    }
    rc = api->audio_open(rate, FREYA_AUDIO_MIC | FREYA_AUDIO_SPK);
    if (rc != 0) {
        api->printf("echo: %s\r\n", why(rc));
        return FREYA_EXIT_FAIL;
    }
    len = rate / 1000U * delay_ms;
    for (uint32_t i = 0; i < len; i++) s_line[i] = 0;
    api->audio_status(&st);
    api->printf("echo: %s, %u ms at %u Hz - Ctrl-C ends it\r\n",
                st.name, delay_ms, rate);

    for (;;) {
        int n = api->audio_read(buf, CHUNK);

        if (n < 0) {
            api->puts("echo: the headset is gone\r\n");
            return FREYA_EXIT_FAIL;
        }
        /* Swap each new sample with the one said 'delay_ms' ago. */
        for (int i = 0; i < n; i++) {
            int16_t old = s_line[pos];
            int a = buf[i] < 0 ? -buf[i] : buf[i];

            if (a > peak) peak = a;
            s_line[pos] = buf[i];
            buf[i] = old;
            if (++pos == len) pos = 0;
        }
        if (n > 0) api->audio_write(buf, n);
        else if (api->thread_sleep(2) != 0) break;

        if (api->ticks_ms() / 1000U != ms) {
            ms = api->ticks_ms() / 1000U;
            api->audio_status(&st);
            api->printf("  peak %3d %%   underruns %u  overruns %u  lost %u\r\n",
                        peak * 100 / 32768, st.underruns, st.overruns, st.errors);
            peak = 0;
        }
    }
    return 0;
}
