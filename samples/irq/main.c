/*
 * irq - a pin interrupt and a hardware timer.
 *
 * A timer raises an interrupt every so many milliseconds and its handler
 * toggles the LED; a pin raises one on every falling edge of a button
 * wired between that pin and ground.  The program itself sleeps in
 * api->irq_wait() and prints what the two handlers counted.
 *
 *     run irq.bin                 PA0, 500 ms, until Ctrl-C
 *     run irq.bin B1              a button on PB1 instead
 *     run irq.bin B1 100          and a faster timer
 *
 * A handler runs in interrupt context: it can print, drive pins, read the
 * clock and touch the timers, but not allocate memory or use the card.
 * Keeping it to a few instructions - as both handlers here do - is the
 * habit that makes that restriction easy to live with.
 */
#include "freya_api.h"

#define DEFAULT_PIN     FREYA_PA(0)
#define DEFAULT_MS      500

static volatile uint32_t s_presses;     /* written by the pin handler   */
static volatile uint32_t s_blinks;      /* written by the timer handler */
static volatile int      s_led;

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

/* "B1" or "b1" -> FREYA_PB(1); -1 when that is not a pin. */
static int parse_pin(const char *s)
{
    int port = -1;
    uint32_t n;

    if (s[0] == 'A' || s[0] == 'a') port = 0;
    if (s[0] == 'B' || s[0] == 'b') port = 1;
    if (s[0] == 'C' || s[0] == 'c') port = 2;
    if (port < 0) return -1;

    n = parse_uint(s + 1, 99);
    if (n > 15) return -1;
    return FREYA_PIN(port, (int)n);
}

/* ------------------------------------------------------------ handlers */
/* Whatever was registered beside the handler arrives as 'arg', which is
 * how one function can serve several sources without a global. */
static void on_press(int pin, void *arg)
{
    volatile uint32_t *count = arg;

    (*count)++;
}

static void on_tick(int timer, void *arg)
{
    const freya_api_t *api = arg;

    s_blinks++;
    s_led = !s_led;
    api->led(s_led);
}

/* --------------------------------------------------------------- main */
int app_main(const freya_api_t *api, int argc, char **argv)
{
    int pin = (argc > 1) ? parse_pin(argv[1]) : DEFAULT_PIN;
    uint32_t ms = (argc > 2) ? parse_uint(argv[2], DEFAULT_MS) : DEFAULT_MS;
    uint32_t seen = 0;
    int timer, rc;

    /* The pin and timer calls were appended to the service table, so a
     * program can be given an older kernel than the one it was built
     * against and say so rather than call into nothing. */
    if (!FREYA_API_HAS(api, irq_wait)) {
        api->puts("irq: this kernel has no pin or timer interrupts\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (pin < 0 || ms == 0) {
        api->puts("usage: irq [pin] [period_ms]   e.g. 'irq B1 100'\r\n");
        return FREYA_EXIT_USAGE;
    }

    /* A button to ground reads high until it is pressed, so the pin is
     * pulled up and the interrupt is asked for on the falling edge. */
    rc = api->pin_mode(pin, FREYA_PIN_IN_PULLUP);
    if (rc != 0) {
        api->printf("irq: pin %c%d: %s\r\n", 'A' + FREYA_PIN_PORT(pin),
                    FREYA_PIN_NUM(pin),
                    rc == FREYA_ERR_PIN ? "Freya keeps that one"
                                        : "not a pin");
        return FREYA_EXIT_FAIL;
    }
    rc = api->pin_irq_attach(pin, FREYA_EDGE_FALLING | FREYA_EDGE_DEBOUNCE,
                             on_press, (void *)&s_presses);
    if (rc != 0) {
        api->printf("irq: no interrupt on that pin (%d)\r\n", rc);
        return FREYA_EXIT_FAIL;
    }

    timer = api->timer_open(ms * 1000u, 0, on_tick, (void *)api);
    if (timer < 0) {
        api->printf("irq: no timer (%d)\r\n", timer);
        api->pin_irq_detach(pin);
        return FREYA_EXIT_FAIL;
    }
    api->timer_start(timer);

    api->printf("irq: timer every %u ms, falling edges on P%c%d "
                "(button to ground)\r\n", ms, 'A' + FREYA_PIN_PORT(pin),
                FREYA_PIN_NUM(pin));
    api->puts("press the button, or Ctrl-C to stop\r\n");

    /*
     * Nothing here polls: irq_wait() sleeps until one of the two
     * interrupts has run, and gives up after a second so the program
     * notices Ctrl-C and can print even when nothing happened.
     */
    while (!api->should_stop()) {
        if (api->irq_wait(1000) != 0 && api->irq_count() == seen)
            continue;
        seen = api->irq_count();
        api->printf("\r  %u ticks, %u presses, pin %s   ",
                    s_blinks, s_presses,
                    api->pin_read(pin) ? "high" : "low");
    }

    /* The counters belong to the line and the timer, so they are read
     * before those are given back.  Freya would drop both anyway when
     * the run ends, Ctrl-C included, but a program that carries on after
     * its interrupts are done says so itself. */
    api->printf("\r\nirq: %u timer interrupts, %u presses, %u in all\r\n",
                api->timer_count(timer), api->pin_irq_count(pin),
                api->irq_count());

    api->timer_close(timer);
    api->pin_irq_detach(pin);
    api->led(0);
    return FREYA_EXIT_OK;
}
