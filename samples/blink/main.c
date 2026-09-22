/*
 * blink - the smallest useful Freya program.
 *
 * Flashes the board LED on PC13 through the pin and timer calls.  PC13
 * sinks the LED, so a low level lights it.  Both arguments are optional:
 *
 *     run blink.bin           500 ms on, 500 ms off, until Ctrl-C
 *     run blink.bin 100       the same, but fast
 *     run blink.bin 100 20    twenty fast blinks, then back to the shell
 *
 * The timer has no handler.  irq_wait() sleeps until the next expiry and
 * returns once a stop has been requested, so the loop notices Ctrl-C
 * between half-cycles without a busy wait.  The pin is toggled here, in
 * thread mode.
 */
#include "freya_api.h"

#define LED_PIN     FREYA_PC(13)
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
    uint32_t period_us;
    int on = 1;
    int timer, rc;

    /* The pin and timer calls were appended to the service table, so a
     * program can be given an older kernel than the one it was built
     * against and say so rather than call into nothing. */
    if (!FREYA_API_HAS(api, irq_wait)) {
        api->puts("blink: this kernel has no pin or timer interrupts\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (ms == 0) ms = DEFAULT_MS;
    if (ms > FREYA_TIMER_MAX_US / 1000UL) {
        api->printf("blink: %u ms is longer than a timer can run\r\n", ms);
        return FREYA_EXIT_USAGE;
    }
    period_us = ms * 1000UL;

    rc = api->pin_mode(LED_PIN, FREYA_PIN_OUT);
    if (rc != 0) {
        api->puts("blink: cannot drive PC13\r\n");
        return FREYA_EXIT_FAIL;
    }

    timer = api->timer_open(period_us, 0, (freya_irq_fn)0, (void *)0);
    if (timer < 0) {
        api->printf("blink: no timer (%d)\r\n", timer);
        api->pin_write(LED_PIN, 1);
        return FREYA_EXIT_FAIL;
    }

    api->printf("blink: %u ms on, %u ms off, ", ms, ms);
    if (count) api->printf("%u blinks\r\n", count);
    else       api->puts("until Ctrl-C\r\n");

    /* Low lights the LED.  Each expiry ends one half of the blink. */
    api->pin_write(LED_PIN, 0);
    api->timer_start(timer);

    while ((count == 0 || done < count) && !api->should_stop()) {
        if (api->irq_wait(0) != 0)
            break;
        api->pin_toggle(LED_PIN);
        on = !on;
        if (on) done++;
    }

    api->timer_close(timer);
    api->pin_write(LED_PIN, 1);
    api->printf("blink: %u blinks done\r\n", done);
    return FREYA_EXIT_OK;
}
