/*
 * Freya - pin numbering, the timer period rule and the PWM one.
 *
 * Neither a pin nor a timer exists on the host, but the arithmetic
 * behind them does not need one: how a pin is packed into the integer a
 * program passes around, how a period in microseconds becomes the
 * prescaler and the reload a 16-bit timer can hold, and how a frequency
 * in hertz becomes the same pair.  Those two dividers are the part most
 * likely to be quietly wrong - a period off by a factor of two looks
 * like working code on the bench - so src/timer.c and src/pwm.c are
 * compiled here unchanged and driven over the whole range they accept,
 * at every clock the two boards can run at.
 *
 * The board's PWM map is checked beside them, because it is a table
 * written by hand: every pin in it has to be one a program may have, and
 * no two entries may name the same pin or the same timer channel.
 *
 * The kernel they expect around them is not here, so the few symbols
 * they refer to are defined below; nothing in the test calls the
 * register side of either driver.
 */
#include <stdio.h>
#include <string.h>

#include "freya.h"

/* The driver's own bookkeeping, which on a board belongs to the kernel. */
sys_clocks_t g_clocks;
app_state_t  g_app;
volatile uint32_t g_irq_events;

uint32_t sys_ticks(void) { return 0; }
int  app_in_handler(void) { return 0; }
int  app_should_stop(void) { return 0; }
int  app_handler_call(freya_irq_fn fn, int source, void *arg) { return 0; }

/* Masking interrupts is one instruction the host does not have; the
 * definitions in the board header go unused once these take their place. */
#define irq_save()      0u
#define irq_restore(pm) ((void)(pm))

/* Configuring a pin belongs to the board, and there is no board here. */
GPIO_TypeDef *board_gpio_port(int port) { return NULL; }
void board_pin_mode(GPIO_TypeDef *port, int pin, int mode) { }
void board_pin_af(GPIO_TypeDef *port, int pin, int af) { }

#include "../src/timer.c"
#include "../src/pwm.c"

static int checks, fails;

static void check(const char *what, long expected, long got)
{
    checks++;
    if (expected == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %ld, got %ld\n", what, expected, got);
        fails++;
    }
}

/*
 * What the hardware will actually do with a prescaler and a reload, in
 * microseconds: both count from zero, so the period is one more of each.
 */
static uint64_t period_of(uint32_t hz, uint16_t psc, uint16_t arr)
{
    uint64_t ticks = ((uint64_t)psc + 1) * ((uint64_t)arr + 1);

    return ticks * 1000000ULL / hz;
}

/* Every period the kernel accepts must come back as a period the timer
 * can be programmed with, and be the period that was asked for. */
static void sweep(uint32_t hz)
{
    uint32_t worst_us = 0;
    uint64_t worst_ppm = 0;
    uint32_t us;
    int bad_range = 0, bad_error = 0, bad_order = 0;
    uint64_t last = 0;

    for (us = FREYA_TIMER_MIN_US; us <= FREYA_TIMER_MAX_US;
         us += (us / 7) + 1) {
        uint16_t psc = 0xFFFF, arr = 0xFFFF;
        uint64_t got, err_ppm;

        if (timer_divide(hz, us, &psc, &arr) != 0) {
            bad_range++;
            continue;
        }
        got = period_of(hz, psc, arr);

        /* psc and arr are uint16_t, so the only way to leave the range
         * the hardware has is to have wrapped getting here. */
        if (got == 0) bad_range++;
        if (got < last) bad_order++;
        last = got;

        err_ppm = (got > us ? got - us : us - got) * 1000000ULL / us;
        if (err_ppm > worst_ppm) { worst_ppm = err_ppm; worst_us = us; }
        if (err_ppm > 2000) bad_error++;        /* 0.2 % */
    }

    printf("  --    %u MHz: worst error %u ppm at %u us\n",
           hz / 1000000U, (unsigned)worst_ppm, worst_us);
    check("every period in range is accepted", 0, bad_range);
    check("every period comes out within 0.2 %", 0, bad_error);
    check("a longer period never programs a shorter one", 0, bad_order);
}

/* What the hardware will make of a prescaler and a reload, in millihertz,
 * which is fine enough to tell 1 Hz apart from what misses it. */
static uint64_t freq_of(uint32_t hz, uint16_t psc, uint16_t arr)
{
    uint64_t ticks = ((uint64_t)psc + 1) * ((uint64_t)arr + 1);

    return (uint64_t)hz * 1000ULL / ticks;
}

/*
 * Every frequency the kernel accepts has to come back as one the timer
 * can be programmed with, and be near the one that was asked for.  How
 * near is set by the counts left in a period: a megahertz at 96 MHz is
 * 96 ticks, so the next reload up is a percent away and nothing can do
 * better.  Ten times that is the bound here, and the sweep reports what
 * it actually found.
 */
static void pwm_sweep(uint32_t hz)
{
    uint32_t worst_hz = 0, freq;
    uint64_t worst_ppm = 0;
    int bad_range = 0, bad_error = 0, bad_steps = 0;

    for (freq = FREYA_PWM_MIN_HZ; freq <= FREYA_PWM_MAX_HZ;
         freq += (freq / 7) + 1) {
        uint16_t psc = 0xFFFF, arr = 0xFFFF;
        uint64_t got, err_ppm;

        if (pwm_divide(hz, freq, &psc, &arr) != 0) {
            bad_range++;
            continue;
        }
        /* A reload of zero would be a pin with no duty cycle to set. */
        if (arr < 1) bad_steps++;

        got = freq_of(hz, psc, arr);
        err_ppm = (got > freq * 1000ULL ? got - freq * 1000ULL
                                        : freq * 1000ULL - got)
                  * 1000000ULL / (freq * 1000ULL);
        if (err_ppm > worst_ppm) { worst_ppm = err_ppm; worst_hz = freq; }
        if (err_ppm > 20000) bad_error++;       /* 2 % */
    }

    printf("  --    %u MHz: worst error %u ppm at %u Hz\n",
           hz / 1000000U, (unsigned)worst_ppm, worst_hz);
    check("every frequency in range is accepted", 0, bad_range);
    check("every one of them comes out within 2 %", 0, bad_error);
    check("and with at least two counts to divide a duty cycle into",
          0, bad_steps);
}

/* A frequency, like a period, is exact when the timer clock divides it
 * into a prescaler and a reload that both fit. */
static void pwm_within(uint32_t hz, uint32_t freq, unsigned ppm)
{
    char what[80];
    uint16_t psc = 0, arr = 0;
    uint64_t got, err;

    if (pwm_divide(hz, freq, &psc, &arr) != 0) {
        snprintf(what, sizeof(what), "%u Hz at %u MHz is accepted",
                 freq, hz / 1000000U);
        check(what, 1, 0);
        return;
    }
    got = freq_of(hz, psc, arr);
    err = (got > freq * 1000ULL ? got - freq * 1000ULL : freq * 1000ULL - got)
          * 1000000ULL / (freq * 1000ULL);

    snprintf(what, sizeof(what), "%u Hz at %u MHz is %s", freq,
             hz / 1000000U, ppm ? "close enough" : "exact");
    check(what, 1, err <= ppm);
}

/*
 * The board's PWM map is a hand written table, and the two mistakes it
 * invites are naming a pin the kernel keeps for the console or the card,
 * and naming one timer channel twice - which would be two pins fighting
 * over one compare register.
 */
static void pwm_map(void)
{
    static const uint16_t reserved[] = BOARD_PIN_RESERVED;
    int bad_pin = 0, bad_timer = 0, bad_ch = 0, dup = 0;

    for (int i = 0; i < PWM_COUNT; i++) {
        int port = FREYA_PIN_PORT(s_map[i].pin);
        int num  = FREYA_PIN_NUM(s_map[i].pin);

        if (port >= BOARD_PIN_PORTS || (reserved[port] & (1U << num)))
            bad_pin++;
        if (s_map[i].timer >= BOARD_TIMER_COUNT) bad_timer++;
        if (s_map[i].ch < 1 || s_map[i].ch > 4) bad_ch++;

        for (int j = 0; j < i; j++) {
            if (s_map[j].pin == s_map[i].pin) dup++;
            if (s_map[j].timer == s_map[i].timer &&
                s_map[j].ch == s_map[i].ch) dup++;
        }
    }

    printf("  --    %d PWM channels on this board\n", PWM_COUNT);
    check("no PWM pin is one the kernel keeps for itself", 0, bad_pin);
    check("every channel is on a timer the board lists", 0, bad_timer);
    check("and is one of that timer's four", 0, bad_ch);
    check("no pin and no timer channel appears twice", 0, dup);
    check("there is at least one channel to open", 1, PWM_COUNT > 0);
}

/*
 * A period is exact when the timer clock divides it into a prescaler and
 * a reload that both fit; when it does not, what a program gets is the
 * closest the pair can come, and 'ppm' is how close that has to be.
 */
static void within(uint32_t hz, uint32_t us, unsigned ppm)
{
    char what[80];
    uint16_t psc = 0, arr = 0;
    uint64_t got, err;

    if (timer_divide(hz, us, &psc, &arr) != 0) {
        snprintf(what, sizeof(what), "%u us at %u MHz is accepted",
                 us, hz / 1000000U);
        check(what, 1, 0);
        return;
    }
    got = period_of(hz, psc, arr);
    err = (got > us ? got - us : us - got) * 1000000ULL / us;

    if (ppm == 0)
        snprintf(what, sizeof(what), "%u us at %u MHz is exact",
                 us, hz / 1000000U);
    else
        snprintf(what, sizeof(what), "%u us at %u MHz is within %u ppm "
                 "(it is %u)", us, hz / 1000000U, ppm, (unsigned)err);
    check(what, 1, err <= ppm);
}

int main(void)
{
    const uint32_t f4 = 96000000U;      /* Black Pill, and its HSI fallback */
    const uint32_t f1 = 72000000U;      /* Blue Pill                        */
    const uint32_t f1_hsi = 64000000U;  /* Blue Pill without its crystal    */
    freya_api_t api;
    uint16_t psc, arr;

    /* ------------------------------------------------- pin numbering */
    check("a pin is its port and its number in one integer",
          FREYA_PB(7), FREYA_PIN(1, 7));
    check("the port comes back out", 2, FREYA_PIN_PORT(FREYA_PC(13)));
    check("the number comes back out", 13, FREYA_PIN_NUM(FREYA_PC(13)));
    check("PA0 is not a negative number, so an error is never a pin",
          0, FREYA_PA(0));
    check("the highest pin still fits a byte", 1, FREYA_PC(15) <= 0xFF ? 1 : 0);
    check("ports do not overlap", 1, FREYA_PB(0) > FREYA_PA(15) ? 1 : 0);
    check("the debounce flag is not an edge",
          0, FREYA_EDGE_DEBOUNCE & FREYA_EDGE_BOTH);
    check("both edges is rising and falling",
          FREYA_EDGE_BOTH, FREYA_EDGE_RISING | FREYA_EDGE_FALLING);

    /* --------------------------------------------- the timer divider */
    check("the timer clock is PCLK1 doubled when APB1 is divided",
          (long)f4, (long)(g_clocks.pclk1_hz = f4 / 2,
                           g_clocks.hclk_hz = f4, timer_clock_hz()));
    check("and PCLK1 itself when it is not",
          (long)f4, (long)(g_clocks.pclk1_hz = f4,
                           g_clocks.hclk_hz = f4, timer_clock_hz()));

    check("a period below the floor is refused",
          FREYA_ERR_ARG, timer_divide(f4, FREYA_TIMER_MIN_US - 1, &psc, &arr));
    check("a period of zero is refused",
          FREYA_ERR_ARG, timer_divide(f4, 0, &psc, &arr));
    check("a period past the ceiling is refused",
          FREYA_ERR_ARG, timer_divide(f4, FREYA_TIMER_MAX_US + 1, &psc, &arr));
    check("the floor itself is accepted", 0,
          timer_divide(f4, FREYA_TIMER_MIN_US, &psc, &arr));
    check("the ceiling itself is accepted", 0,
          timer_divide(f4, FREYA_TIMER_MAX_US, &psc, &arr));

    /* A period short enough to need no prescaler must not get one: that
     * is where the resolution a program asked for is either kept or lost. */
    timer_divide(f4, 100, &psc, &arr);
    check("100 us at 96 MHz needs no prescaler", 0, psc);
    check("and counts 9600 ticks", 9599, arr);

    /* The periods a program is most likely to ask for, to the
     * microsecond; a second is the one place where sixteen bits of each
     * cannot land on the number exactly, and it misses by a millionth. */
    within(f4, 1000, 0);
    within(f4, 20000, 0);
    within(f1, 1000, 0);
    within(f1_hsi, 1000, 0);
    within(f4, 1000000, 1);
    within(f1, 500000, 10);

    sweep(f4);
    sweep(f1);
    sweep(f1_hsi);

    /* ----------------------------------------------- the PWM divider */
    check("a frequency below the floor is refused",
          FREYA_ERR_ARG, pwm_divide(f4, FREYA_PWM_MIN_HZ - 1, &psc, &arr));
    check("a frequency past the ceiling is refused",
          FREYA_ERR_ARG, pwm_divide(f4, FREYA_PWM_MAX_HZ + 1, &psc, &arr));
    check("the floor itself is accepted", 0,
          pwm_divide(f4, FREYA_PWM_MIN_HZ, &psc, &arr));
    check("the ceiling itself is accepted", 0,
          pwm_divide(f4, FREYA_PWM_MAX_HZ, &psc, &arr));

    /* A frequency the counter can reach without a prescaler must not get
     * one: what the prescaler divides away is duty cycle resolution. */
    pwm_divide(f4, 2000, &psc, &arr);
    check("2 kHz at 96 MHz needs no prescaler", 0, psc);
    check("and counts 48000 ticks", 47999, arr);

    /* The three a program is most likely to ask for: a servo's frame, an
     * LED that cannot be seen to flicker, and a quiet motor. */
    pwm_within(f4, 50, 0);
    pwm_within(f1, 50, 10);
    pwm_within(f1_hsi, 50, 10);
    pwm_within(f4, 1000, 0);
    pwm_within(f1, 20000, 0);

    /* A servo is set by pulse width, which is worked out from the same
     * prescaler: at 50 Hz it has to land inside a microsecond of what
     * was asked for, or the arm sits visibly off. */
    pwm_divide(f4, 50, &psc, &arr);
    check("50 Hz at 96 MHz steps a pulse by 312 ns",
          312, (long)((uint64_t)((uint32_t)psc + 1) * 1000000000ULL / f4));
    check("and counts the 20 ms frame in 64000 steps", 63999, arr);

    pwm_sweep(f4);
    pwm_sweep(f1);
    pwm_sweep(f1_hsi);

    pwm_map();

    /* ------------------------------------------- the table a program sees */
    memset(&api, 0, sizeof(api));
    api.size = sizeof(freya_api_t);
    check("a full table has the pin calls", 1, FREYA_API_HAS(&api, pin_mode) ? 1 : 0);
    check("and the timer calls", 1, FREYA_API_HAS(&api, timer_open) ? 1 : 0);
    check("and the PWM calls", 1, FREYA_API_HAS(&api, pwm_freq) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, exit_reason_str) +
               sizeof(api.exit_reason_str);
    check("a kernel from before them says so", 0,
          FREYA_API_HAS(&api, pin_mode) ? 1 : 0);
    check("including for irq_wait, the last of them", 0,
          FREYA_API_HAS(&api, irq_wait) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, irq_wait) + sizeof(api.irq_wait);
    check("a kernel with the interrupts but not PWM says that too", 0,
          FREYA_API_HAS(&api, pwm_open) ? 1 : 0);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
