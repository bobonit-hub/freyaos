/*
 * Freya - pin numbering and the timer period rule.
 *
 * Neither a pin nor a timer exists on the host, but the two pieces of
 * arithmetic behind them do not need one: how a pin is packed into the
 * integer a program passes around, and how a period in microseconds
 * becomes the prescaler and the reload a 16-bit timer can hold.  The
 * second is the part most likely to be quietly wrong - a period that is
 * off by a factor of two looks like working code on the bench - so
 * src/timer.c is compiled here unchanged and its divider is driven over
 * the whole range it accepts, at every clock the two boards can run at.
 *
 * The kernel it expects around it is not here, so the few symbols it
 * refers to are defined below; nothing in the test calls the register
 * side of the driver.
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

#include "../src/timer.c"

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

    /* ------------------------------------------- the table a program sees */
    memset(&api, 0, sizeof(api));
    api.size = sizeof(freya_api_t);
    check("a full table has the pin calls", 1, FREYA_API_HAS(&api, pin_mode) ? 1 : 0);
    check("and the timer calls", 1, FREYA_API_HAS(&api, timer_open) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, exit_reason_str) +
               sizeof(api.exit_reason_str);
    check("a kernel from before them says so", 0,
          FREYA_API_HAS(&api, pin_mode) ? 1 : 0);
    check("including for irq_wait, the last of them", 0,
          FREYA_API_HAS(&api, irq_wait) ? 1 : 0);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
