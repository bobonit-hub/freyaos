/*
 * Freya - hardware timers for user programs.
 *
 * timer_open() hands out one of the board's general purpose timers,
 * programmed to raise an interrupt every so many microseconds.  The
 * program can attach a handler to it, or leave the handler out and read
 * timer_count() - or sleep in irq_wait() - from thread mode.
 *
 * A period is microseconds and nothing else: the prescaler and the reload
 * are worked out from the timer clock, which is PCLK1 doubled, because
 * neither board leaves the APB1 prescaler at one.  Sixteen bits of each
 * puts the ceiling a little past forty seconds at 96 MHz, and the floor
 * is where the kernel would spend the whole run inside its own dispatch.
 *
 * The timers themselves are TIM2..TIM4 on both boards, at the same
 * addresses and with the same registers, so only the list of them - and
 * the clock enable bits - come from the board.
 */
#include "freya.h"

typedef struct {
    TIM_TypeDef *tim;
    uint8_t      irq;
    uint32_t     enr;             /* RCC->APB1ENR bit                    */
} timer_hw_t;

static const timer_hw_t s_hw[] = BOARD_TIMER_LIST;
static const char *const s_name[] = BOARD_TIMER_NAMES;

#define TIMER_COUNT  ((int)ARRAY_SIZE(s_hw))

typedef struct {
    freya_irq_fn fn;
    void    *arg;
    uint32_t count;
    uint32_t period_us;
    uint16_t psc;
    uint16_t arr;
    uint8_t  open;
    uint8_t  running;
    uint8_t  oneshot;
} timer_state_t;

static timer_state_t s_timer[ARRAY_SIZE(s_hw)];

/* One bit per timer, held by src/pwm.c.  A timer raises a periodic
 * interrupt or it drives PWM pins, never both: the two want different
 * things of the reload register, so whichever asks first gets it. */
static uint8_t s_pwm_taken;

const char *timer_name(int timer)
{
    return (timer >= 0 && timer < TIMER_COUNT) ? s_name[timer] : "?";
}

/*
 * The timer clock is PCLK1 when the APB1 prescaler is one and twice it
 * otherwise - the rule the reference manuals state for every STM32, and
 * the reason both boards clock their timers at the full HCLK.
 */
uint32_t timer_clock_hz(void)
{
    return (g_clocks.pclk1_hz == g_clocks.hclk_hz) ? g_clocks.pclk1_hz
                                                   : g_clocks.pclk1_hz * 2U;
}

/*
 * A period in microseconds becomes a prescaler and a reload, both of
 * which are sixteen bits and both of which count from zero.  The
 * prescaler is the smallest one that brings the count within a reload,
 * because that keeps the resolution as fine as the period allows.
 *
 * The whole sum stays in 32 bits: the longest period the ceiling allows
 * is forty seconds, which at 96 MHz is 3.84e9 timer ticks and still
 * inside a word.  Anything a faster board could overflow is refused
 * instead of wrapping, and 64-bit arithmetic - three quarters of a KiB
 * of libgcc on a part with 128 KiB of flash - is never reached for.
 */
__attribute__((noinline))
static int timer_divide(uint32_t hz, uint32_t us, uint16_t *psc, uint16_t *arr)
{
    uint32_t per_us = hz / 1000000U;
    uint32_t ticks, div;

    if (us < FREYA_TIMER_MIN_US || us > FREYA_TIMER_MAX_US) return FREYA_ERR_ARG;
    if (per_us == 0 || us > (0xFFFFFFFFUL - 65535U) / per_us) return FREYA_ERR_ARG;

    ticks = us * per_us;
    div = (ticks + 65535U) >> 16;           /* prescaler + 1, rounded up */

    *psc = (uint16_t)(div - 1);
    ticks /= div;
    if (ticks < 2) return FREYA_ERR_ARG;    /* below one usable reload */
    *arr = (uint16_t)(ticks - 1);
    return 0;
}

static timer_state_t *get(int timer)
{
    if (timer < 0 || timer >= TIMER_COUNT || !s_timer[timer].open) return NULL;
    return &s_timer[timer];
}

static void hw_stop(int timer)
{
    s_hw[timer].tim->CR1  = 0;
    s_hw[timer].tim->DIER = 0;
    s_hw[timer].tim->SR   = 0;
    s_timer[timer].running = 0;
}

/* Give a timer back: stopped, its interrupt off and its clock with it. */
static void hw_release(int timer)
{
    hw_stop(timer);
    nvic_disable(s_hw[timer].irq);
    RCC->APB1ENR &= ~s_hw[timer].enr;
    s_timer[timer].open = 0;
    s_timer[timer].fn = NULL;
    s_timer[timer].count = 0;
}

int timer_open(uint32_t period_us, int flags, freya_irq_fn fn, void *arg)
{
    timer_state_t *t;
    uint16_t psc, arr;
    int i, rc;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (flags & ~FREYA_TIMER_ONESHOT) return FREYA_ERR_ARG;

    rc = timer_divide(timer_clock_hz(), period_us, &psc, &arr);
    if (rc != 0) return rc;

    for (i = 0; i < TIMER_COUNT; i++)
        if (!s_timer[i].open && !(s_pwm_taken & (1U << i))) break;
    if (i == TIMER_COUNT) return FREYA_ERR_BUSY;

    t = &s_timer[i];
    t->fn        = fn;
    t->arg       = arg;
    t->count     = 0;
    t->period_us = period_us;
    t->psc       = psc;
    t->arr       = arr;
    t->oneshot   = (uint8_t)((flags & FREYA_TIMER_ONESHOT) ? 1 : 0);
    t->running   = 0;
    t->open      = 1;

    RCC->APB1ENR |= s_hw[i].enr;
    (void)RCC->APB1ENR;
    hw_stop(i);
    nvic_set_priority(s_hw[i].irq, IRQ_PRIO_HANDLER);
    nvic_enable(s_hw[i].irq);
    return i;
}

int timer_close(int timer)
{
    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!get(timer)) return FREYA_ERR_ARG;

    hw_release(timer);
    return 0;
}

/*
 * Counting starts from zero: UG loads the prescaler and the reload, and
 * URS keeps that reload from being reported as an expiry of its own.
 */
int timer_start(int timer)
{
    timer_state_t *t = get(timer);
    TIM_TypeDef *tim;

    if (!t) return FREYA_ERR_ARG;
    tim = s_hw[timer].tim;

    tim->CR1  = 0;
    tim->PSC  = t->psc;
    tim->ARR  = t->arr;
    tim->EGR  = TIM_EGR_UG;
    tim->SR   = 0;
    tim->DIER = TIM_DIER_UIE;
    tim->CR1  = TIM_CR1_CEN | TIM_CR1_URS | TIM_CR1_ARPE |
                (t->oneshot ? TIM_CR1_OPM : 0);
    t->running = 1;
    return 0;
}

int timer_stop(int timer)
{
    if (!get(timer)) return FREYA_ERR_ARG;
    hw_stop(timer);
    return 0;
}

/* A running timer takes the new period at its next expiry, because ARR
 * is buffered; a stopped one takes it when it is started. */
int timer_period(int timer, uint32_t period_us)
{
    timer_state_t *t = get(timer);
    uint16_t psc, arr;
    int rc;

    if (!t) return FREYA_ERR_ARG;
    rc = timer_divide(timer_clock_hz(), period_us, &psc, &arr);
    if (rc != 0) return rc;

    t->psc = psc;
    t->arr = arr;
    t->period_us = period_us;
    if (t->running) {
        s_hw[timer].tim->PSC = psc;
        s_hw[timer].tim->ARR = arr;
    }
    return 0;
}

uint32_t timer_count(int timer)
{
    timer_state_t *t = get(timer);

    return t ? t->count : 0;
}

void timer_release(void)
{
    uint32_t pm = irq_save();

    for (int i = 0; i < TIMER_COUNT; i++)
        if (s_timer[i].open) hw_release(i);
    irq_restore(pm);
}

/* ------------------------------------------------------ lending to PWM */
/*
 * src/pwm.c drives the compare channels of these same timers, and the
 * counter underneath them is one thing that cannot be shared: it belongs
 * to a program's periodic interrupt or to its PWM pins.  A timer is
 * borrowed with its clock on and its interrupt off, and given back
 * stopped, so timer_open() can hand it out again afterwards.
 */
TIM_TypeDef *timer_take(int timer)
{
    if (timer < 0 || timer >= TIMER_COUNT) return NULL;
    if (s_timer[timer].open) return NULL;

    s_pwm_taken |= (uint8_t)(1U << timer);
    RCC->APB1ENR |= s_hw[timer].enr;
    (void)RCC->APB1ENR;
    hw_stop(timer);
    nvic_disable(s_hw[timer].irq);
    return s_hw[timer].tim;
}

void timer_give(int timer)
{
    if (timer < 0 || timer >= TIMER_COUNT) return;

    hw_stop(timer);
    RCC->APB1ENR &= ~s_hw[timer].enr;
    s_pwm_taken &= (uint8_t)~(1U << timer);
}

/*
 * One expiry.  A timer whose program has gone - or is going - is stopped
 * here rather than left firing into nothing, which is also what makes a
 * runaway period recoverable: Ctrl-C asks the run to stop, and the next
 * expiry switches the timer off instead of starving thread mode.
 */
static void timer_event(int timer)
{
    timer_state_t *t = &s_timer[timer];

    if (!t->open || !g_app.running || app_should_stop()) {
        hw_stop(timer);
        return;
    }
    if (t->oneshot) t->running = 0;         /* one pulse mode stopped it */

    t->count++;
    g_irq_events++;

    if (t->fn) app_handler_call(t->fn, timer, t->arg);
}

static void timer_isr(int timer)
{
    TIM_TypeDef *tim = s_hw[timer].tim;

    if (!(tim->SR & TIM_SR_UIF)) return;
    tim->SR = ~(uint32_t)TIM_SR_UIF;        /* write 0 to clear */
    timer_event(timer);
}

void TIM2_IRQHandler(void) { timer_isr(0); }
void TIM3_IRQHandler(void) { timer_isr(1); }
void TIM4_IRQHandler(void) { timer_isr(2); }

_Static_assert(ARRAY_SIZE(s_hw) == 3,
               "the board's timer list and the handlers above must match");
_Static_assert(ARRAY_SIZE(s_name) == ARRAY_SIZE(s_hw) &&
               BOARD_TIMER_COUNT == (int)ARRAY_SIZE(s_hw),
               "the board must name every timer it lists, and count them");
