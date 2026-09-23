/*
 * Freya - PWM on the pins the timer channels reach.
 *
 * pwm_open() takes a pin, a frequency and a duty cycle and hands back a
 * handle; the pin is square-waving before the call returns and keeps
 * doing it with no further attention, because the hardware, not the
 * kernel, is what toggles it.
 *
 * The timers are the same three timer_open() hands out, so a program
 * that drives pins from one cannot also take a periodic interrupt from
 * it: whichever asks first gets it, and the other is told the timer is
 * busy.  Within one timer the counter is shared as well, which is why
 * every channel of it runs at one frequency and only the duty cycles are
 * separate - that is the hardware, and the console's 'pwm' listing shows
 * which pins sit behind which timer.
 *
 * A duty cycle is a fraction of the period in ten-thousandths rather
 * than a count of ticks, so it means the same thing after the frequency
 * changes and on a board whose timer clock is different.  The compare
 * register is preloaded, so a duty cycle written in the middle of a
 * period arrives at the end of it and no pulse is ever cut short.
 *
 * Which pins these are is the board's (BOARD_PWM_MAP); everything here
 * is the same on both.
 */
#include "freya.h"

typedef struct {
    uint8_t pin;             /* FREYA_PA(0) and the rest                */
    uint8_t timer;           /* index into the board's timer list       */
    uint8_t ch;              /* compare channel, 1..4                   */
    uint8_t af;              /* alternate function; the F1 has none     */
} pwm_pin_t;

static const pwm_pin_t s_map[] = BOARD_PWM_MAP;

#define PWM_COUNT  ((int)ARRAY_SIZE(s_map))

/* A channel keeps the duty cycle it was given rather than reading the
 * compare register back, so a new frequency can be programmed from the
 * fraction that was asked for instead of from a count of ticks that no
 * longer means the same thing. */
typedef struct {
    uint32_t duty;           /* 0 .. FREYA_PWM_FULL                     */
    uint8_t  open;
    uint8_t  from_app;       /* a run's channel, dropped when it ends   */
} pwm_chan_t;

/* The counter under a group of channels, borrowed from src/timer.c. */
typedef struct {
    TIM_TypeDef *tim;
    uint32_t     freq_hz;
    uint16_t     psc;
    uint16_t     arr;
    uint8_t      used;       /* channels open on it                     */
} pwm_timer_t;

static pwm_chan_t  s_chan[ARRAY_SIZE(s_map)];
static pwm_timer_t s_tim[BOARD_TIMER_COUNT];

/*
 * A frequency becomes a prescaler and a reload, both sixteen bits and
 * both counting from zero.  The smallest prescaler that fits is the one
 * used, because what is left over is the duty cycle's resolution: 1 kHz
 * at 96 MHz counts 96000 ticks, which is a reload of 47999 behind a
 * prescaler of 1 and a duty cycle settable to two parts in a hundred
 * thousand.  The whole sum stays in 32 bits - the timer clock itself is
 * the largest number in it, because the lowest frequency is 1 Hz.
 */
__attribute__((noinline))
static int pwm_divide(uint32_t hz, uint32_t freq_hz, uint16_t *psc, uint16_t *arr)
{
    uint32_t ticks, div;

    if (freq_hz < FREYA_PWM_MIN_HZ || freq_hz > FREYA_PWM_MAX_HZ)
        return FREYA_ERR_ARG;

    ticks = (hz + freq_hz / 2U) / freq_hz;      /* one period, in ticks */
    div = (ticks + 65535U) >> 16;               /* prescaler + 1        */
    if (div == 0) div = 1;

    *psc = (uint16_t)(div - 1);
    ticks /= div;
    if (ticks < 2) return FREYA_ERR_ARG;        /* no room for a duty cycle */
    *arr = (uint16_t)(ticks - 1);
    return 0;
}

static int pwm_get(int pwm)
{
    return (pwm >= 0 && pwm < PWM_COUNT && s_chan[pwm].open) ? 0 : FREYA_ERR_ARG;
}

/* The channel a pin is, or FREYA_ERR_PIN when the pin has none.  A pin
 * Freya keeps for itself never appears in the map at all. */
int pwm_lookup(int pin)
{
    if (pin < 0 || pin > 0xFF) return FREYA_ERR_PIN;
    for (int i = 0; i < PWM_COUNT; i++)
        if (s_map[i].pin == (uint8_t)pin) return i;
    return FREYA_ERR_PIN;
}

int pwm_pin_busy(int pin)
{
    int idx = pwm_lookup(pin);
    return (idx >= 0 && s_chan[idx].open) ? 1 : 0;
}

/* ---------------------------------------------------------- hardware */
/* Counting from zero to the reload, over and over.  UG loads both the
 * prescaler and the reload immediately, which costs the channels already
 * running one truncated period - the price of changing the frequency
 * they share. */
static void hw_period(pwm_timer_t *t)
{
    TIM_TypeDef *tim = t->tim;

    tim->CR1 = 0;
    tim->PSC = t->psc;
    tim->ARR = t->arr;
    tim->EGR = TIM_EGR_UG;
    tim->SR  = 0;
    tim->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

/* The compare value a duty cycle comes to.  A full duty cycle programs
 * one more than the reload, which no count ever reaches, so the pin
 * stays high for the whole period rather than dipping for a tick. */
static void hw_duty(int idx)
{
    const pwm_timer_t *t = &s_tim[s_map[idx].timer];
    uint32_t steps = (uint32_t)t->arr + 1U;
    uint32_t duty = s_chan[idx].duty;

    t->tim->CCR[s_map[idx].ch - 1] = (duty >= FREYA_PWM_FULL)
                                     ? steps : steps * duty / FREYA_PWM_FULL;
}

/* A new period moves every channel of the timer, because a duty cycle is
 * a fraction of it. */
static void hw_retime(int ti, uint16_t psc, uint16_t arr, uint32_t freq_hz)
{
    pwm_timer_t *t = &s_tim[ti];

    t->psc     = psc;
    t->arr     = arr;
    t->freq_hz = freq_hz;
    hw_period(t);
    for (int i = 0; i < PWM_COUNT; i++)
        if (s_chan[i].open && s_map[i].timer == ti) hw_duty(i);
}

static void hw_channel(const pwm_timer_t *t, int ch, int on)
{
    __IO uint32_t *ccmr = (ch <= 2) ? &t->tim->CCMR1 : &t->tim->CCMR2;
    uint32_t sh = TIM_CCMR_SHIFT(ch);

    *ccmr = (*ccmr & ~(0xFFUL << sh)) | (on ? (TIM_CCMR_PWM1 << sh) : 0);
    if (on) t->tim->CCER |=  TIM_CCER_CCE(ch);
    else    t->tim->CCER &= ~TIM_CCER_CCE(ch);
}

/*
 * Take a channel down: the output off, the pin back to an input, and the
 * timer handed back with the last of its channels.  The pin does not
 * stay an output, because a stopped PWM pin is frozen at whatever level
 * the period happened to be at - which for whatever it drives is neither
 * on nor off but an arbitrary one of the two.
 */
static void chan_free(int idx)
{
    pwm_timer_t *t = &s_tim[s_map[idx].timer];
    GPIO_TypeDef *port = board_gpio_port(FREYA_PIN_PORT(s_map[idx].pin));

    hw_channel(t, s_map[idx].ch, 0);
    if (port) board_pin_mode(port, FREYA_PIN_NUM(s_map[idx].pin), FREYA_PIN_IN);

    s_chan[idx].open = 0;
    s_chan[idx].from_app = 0;
    s_chan[idx].duty = 0;

    if (t->used && --t->used == 0) {
        timer_give(s_map[idx].timer);
        t->tim = NULL;
        t->freq_hz = 0;
    }
}

/* -------------------------------------------------------------- calls */
int pwm_open(int pin, uint32_t freq_hz, uint32_t duty)
{
    GPIO_TypeDef *port;
    pwm_timer_t *t;
    uint16_t psc, arr;
    int idx, rc;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (duty > FREYA_PWM_FULL) return FREYA_ERR_ARG;

    idx = pwm_lookup(pin);
    if (idx < 0) return idx;

    /* An open I2C bus or 1-Wire pin owns that line.  Taking it as a
     * compare output would pull it off the bus without either side
     * saying so. */
    if (i2c_owns_pin(pin) || w1_owns_pin(pin)) return FREYA_ERR_BUSY;

    t = &s_tim[s_map[idx].timer];

    rc = pwm_divide(timer_clock_hz(), freq_hz, &psc, &arr);
    if (rc != 0) return rc;

    /* The channels of one timer count on one counter, so they run at one
     * frequency: a second pin on the same timer has to ask for the
     * frequency the first one set, or change it with pwm_freq(). */
    if (t->freq_hz != freq_hz && t->used > (s_chan[idx].open ? 1U : 0U))
        return FREYA_ERR_BUSY;

    port = board_gpio_port(FREYA_PIN_PORT(pin));
    if (!port) return FREYA_ERR_PIN;

    if (!t->used) {
        t->tim = timer_take(s_map[idx].timer);
        if (!t->tim) return FREYA_ERR_BUSY;     /* timer_open() holds it */
    }
    if (!s_chan[idx].open) {
        s_chan[idx].open = 1;
        t->used++;
    }
    s_chan[idx].from_app = (uint8_t)(g_app.running ? 1 : 0);
    s_chan[idx].duty     = duty;

    /* A timer that was given back has no frequency, so the first channel
     * to take it again always programs one: it comes back stopped. */
    if (t->freq_hz != freq_hz) hw_retime(s_map[idx].timer, psc, arr, freq_hz);
    else                       hw_duty(idx);

    /* The channel is driving the right level before the pin is handed to
     * it, so a pin never shows the duty cycle of a previous run. */
    hw_channel(t, s_map[idx].ch, 1);
    board_pin_af(port, FREYA_PIN_NUM(pin), s_map[idx].af);
    return idx;
}

int pwm_close(int pwm)
{
    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (pwm_get(pwm) != 0) return FREYA_ERR_ARG;

    chan_free(pwm);
    return 0;
}

/* Safe from a handler: one store to a preloaded compare register. */
int pwm_duty(int pwm, uint32_t duty)
{
    if (pwm_get(pwm) != 0) return FREYA_ERR_ARG;
    if (duty > FREYA_PWM_FULL) return FREYA_ERR_ARG;

    s_chan[pwm].duty = duty;
    hw_duty(pwm);
    return 0;
}

/*
 * The same thing said as a high time, which is how a servo is spoken to:
 * 1500 us out of a 20 ms period.  The compare value is worked out from
 * the timer clock rather than from the duty fraction, so the pulse lands
 * on the finest step the prescaler left - 312 ns at 50 Hz on either
 * board - rather than on a ten-thousandth of the period.
 */
int pwm_pulse_us(int pwm, uint32_t us)
{
    uint32_t per_us = timer_clock_hz() / 1000000U;
    pwm_timer_t *t;
    uint32_t steps, ticks;

    if (pwm_get(pwm) != 0) return FREYA_ERR_ARG;
    t = &s_tim[s_map[pwm].timer];
    steps = (uint32_t)t->arr + 1U;

    if (per_us == 0 || us > 0xFFFFFFFFUL / per_us) return FREYA_ERR_ARG;
    ticks = us * per_us / ((uint32_t)t->psc + 1U);
    if (ticks > steps) return FREYA_ERR_ARG;    /* longer than the period */

    s_chan[pwm].duty = ticks * FREYA_PWM_FULL / steps;
    t->tim->CCR[s_map[pwm].ch - 1] = ticks;
    return 0;
}

/* Every channel of the timer this one is on follows, each keeping the
 * duty cycle it was given. */
int pwm_freq(int pwm, uint32_t freq_hz)
{
    uint16_t psc, arr;
    int rc;

    if (pwm_get(pwm) != 0) return FREYA_ERR_ARG;
    rc = pwm_divide(timer_clock_hz(), freq_hz, &psc, &arr);
    if (rc != 0) return rc;

    hw_retime(s_map[pwm].timer, psc, arr, freq_hz);
    return 0;
}

/*
 * A run's channels, and only those: one started at the console outlives
 * the program that ran next, the way the LED does.
 */
void pwm_release(void)
{
    uint32_t pm = irq_save();

    for (int i = 0; i < PWM_COUNT; i++)
        if (s_chan[i].open && s_chan[i].from_app) chan_free(i);
    irq_restore(pm);
}

/* What the console lists: the board's channels in order, and what each
 * of them is doing. */
int pwm_info(int idx, pwm_info_t *info)
{
    if (idx < 0 || idx >= PWM_COUNT || !info) return -1;

    info->pin     = s_map[idx].pin;
    info->timer   = timer_name(s_map[idx].timer);
    info->ch      = s_map[idx].ch;
    info->open    = s_chan[idx].open;
    info->freq_hz = s_chan[idx].open ? s_tim[s_map[idx].timer].freq_hz : 0;
    info->duty    = s_chan[idx].duty;
    return 0;
}
