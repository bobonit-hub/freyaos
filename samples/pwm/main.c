/*
 * pwm - a fading LED and a sweeping servo, from a timer channel.
 *
 * The hardware does the work: pwm_open() starts a square wave on a pin
 * and it keeps going with no attention at all, so the program's loop
 * only decides what the duty cycle should be next and sleeps.
 *
 *     run pwm.bin                 fade an LED on PB6, 1 kHz
 *     run pwm.bin B7              the same on PB7
 *     run pwm.bin B7 200          and a slower, visibly flickering 200 Hz
 *     run pwm.bin A0 servo        sweep a servo on PA0 instead
 *
 * The eight pins with a channel behind them are PA0, PA1 (TIM2), PB0,
 * PB1 (TIM3) and PB6..PB9 (TIM4), the same on both boards; 'pwm' at the
 * console lists them.
 */
#include "freya_api.h"

#define DEFAULT_PIN     FREYA_PB(6)
#define DEFAULT_HZ      1000u

#define SERVO_HZ        50u             /* what a hobby servo expects   */
#define SERVO_MIN_US    1000u           /* one end of its travel        */
#define SERVO_MAX_US    2000u           /* the other                    */
#define SERVO_STEP_US   10u

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

/* "B6" or "b6" -> FREYA_PB(6); -1 when that is not a pin. */
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

/*
 * A lamp's brightness does not follow its duty cycle: the eye is roughly
 * logarithmic, so a fade that steps the duty cycle evenly spends most of
 * its time looking bright.  Squaring a level from 0 to 100 gives both a
 * fade that looks even and, for free, exactly the 0..10000 range the API
 * wants.
 */
static uint32_t fade_duty(uint32_t level)
{
    return level * level;
}

/* --------------------------------------------------------------- main */
int app_main(const freya_api_t *api, int argc, char **argv)
{
    int pin = (argc > 1) ? parse_pin(argv[1]) : DEFAULT_PIN;
    int servo = (argc > 2) && argv[2][0] == 's';
    uint32_t hz = (argc > 2 && !servo) ? parse_uint(argv[2], DEFAULT_HZ)
                                       : DEFAULT_HZ;
    uint32_t us = SERVO_MIN_US;
    uint32_t level = 0;
    int step = 1, pwm;

    /* PWM was appended to the service table, so a program built against
     * this header can be handed an older kernel and say so rather than
     * call into nothing. */
    if (!FREYA_API_HAS(api, pwm_open)) {
        api->puts("pwm: this kernel has no PWM\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (pin < 0) {
        api->puts("usage: pwm [pin] [hz|servo]   e.g. 'pwm B7 200'\r\n");
        return FREYA_EXIT_USAGE;
    }

    pwm = api->pwm_open(pin, servo ? SERVO_HZ : hz, servo ? 0 : fade_duty(0));
    if (pwm < 0) {
        api->printf("pwm: P%c%d: %s\r\n", 'A' + FREYA_PIN_PORT(pin),
                    FREYA_PIN_NUM(pin),
                    pwm == FREYA_ERR_PIN  ? "no timer channel on that pin"
                  : pwm == FREYA_ERR_BUSY ? "its timer is taken"
                                          : "frequency out of range");
        return FREYA_EXIT_FAIL;
    }

    api->printf("pwm: P%c%d at %u Hz, %s. Ctrl-C stops it\r\n",
                'A' + FREYA_PIN_PORT(pin), FREYA_PIN_NUM(pin),
                servo ? SERVO_HZ : hz,
                servo ? "sweeping a servo" : "fading an LED");

    /*
     * Nothing in here touches the pin: the timer is already driving it,
     * and each pass only says what the next duty cycle should be.  A
     * servo is told the same thing as a pulse width, which is how its
     * datasheet puts it - 1.0 to 2.0 ms out of every 20.
     */
    while (!api->should_stop()) {
        if (servo) {
            api->pwm_pulse_us(pwm, us);
            if (us >= SERVO_MAX_US) step = -1;
            if (us <= SERVO_MIN_US) step = 1;
            us = (uint32_t)((int)us + step * (int)SERVO_STEP_US);
            api->printf("\r  %u us   ", us);
            api->delay_ms(20);
        } else {
            api->pwm_duty(pwm, fade_duty(level));
            if (level >= 100) step = -1;
            if (level == 0)   step = 1;
            level = (uint32_t)((int)level + step);
            api->printf("\r  %u.%02u%%   ", fade_duty(level) / 100u,
                        fade_duty(level) % 100u);
            api->delay_ms(10);
        }
    }

    /* Freya would close the channel anyway when the run ends, Ctrl-C
     * included, and put the pin back to an input - but a program that
     * knows it is finished says so itself. */
    api->pwm_close(pwm);
    api->puts("\r\npwm: channel closed, the pin is an input again\r\n");
    return FREYA_EXIT_OK;
}
