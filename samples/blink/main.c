/*
 * blink - the smallest useful Freya program.
 *
 * Flashes the board LED on PC13 through the service table.  Both
 * arguments are optional:
 *
 *     run blink.bin           500 ms on, 500 ms off, until Ctrl-C
 *     run blink.bin 100       the same, but fast
 *     run blink.bin 100 20    twenty fast blinks, then back to the shell
 *
 * api->delay_ms returns early once a stop has been requested, so the loop
 * notices Ctrl-C between blinks without a busy wait.
 */
#include "freya_api.h"

#define DEFAULT_MS  500

/* The program region has no libc, so digits are converted by hand. */
static uint32_t parse_uint(const char *s, uint32_t fallback)
{
    uint32_t v = 0;
    int digits = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s++ - '0');
        digits++;
    }
    return (digits && *s == '\0') ? v : fallback;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint32_t ms    = (argc > 1) ? parse_uint(argv[1], DEFAULT_MS) : DEFAULT_MS;
    uint32_t count = (argc > 2) ? parse_uint(argv[2], 0) : 0;
    uint32_t done  = 0;

    if (ms == 0) ms = DEFAULT_MS;

    api->printf("blink: %u ms on, %u ms off, ", ms, ms);
    if (count) api->printf("%u blinks\r\n", count);
    else       api->puts("until Ctrl-C\r\n");

    while ((count == 0 || done < count) && !api->should_stop()) {
        api->led(1);
        api->delay_ms(ms);
        api->led(0);
        api->delay_ms(ms);
        done++;
    }

    api->led(0);
    api->printf("blink: %u blinks done\r\n", done);
    return 0;
}
