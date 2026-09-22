/*
 * Freya - SysTick time base, reset cause, delays and the software RTC.
 *
 * The clock tree itself is the board's business: board_clock_init() brings
 * the PLL up and fills in g_clocks, and everything here works from those
 * frequencies.
 */
#include "freya.h"

sys_clocks_t g_clocks;

static volatile uint32_t s_ticks;      /* milliseconds since boot */
static volatile uint32_t s_rtc_secs;   /* software wall clock     */
static uint32_t s_rtc_frac;

static uint8_t detect_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    uint8_t cause = RESET_UNKNOWN;

    if (csr & (1UL << 31))      cause = RESET_LOWPOWER;   /* LPWRRSTF */
    else if (csr & (1UL << 30)) cause = RESET_WWDG;
    else if (csr & (1UL << 29)) cause = RESET_IWDG;
    else if (csr & (1UL << 28)) cause = RESET_SOFTWARE;
    else if (csr & (1UL << 27)) cause = RESET_POWER_ON;   /* PORRSTF  */
    else if (csr & (1UL << 26)) cause = RESET_PIN;
    else if (csr & (1UL << 25)) cause = RESET_BROWNOUT;

    RCC->CSR |= RCC_CSR_RMVF;
    return cause;
}

static void systick_init(void)
{
    SysTick->LOAD = (g_clocks.hclk_hz / 1000UL) - 1UL;
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSRC | SysTick_CTRL_TICKINT |
                    SysTick_CTRL_ENABLE;
    /* Lowest priority so the console never loses characters to it.
     * PendSV shares that level: it carries out program aborts and must
     * only run once every other handler has finished. */
    SCB->SHPR[10] = 0xF0;        /* PendSV  */
    SCB->SHPR[11] = 0xF0;        /* SysTick */
}

void SysTick_Handler(void)
{
    s_ticks++;
    if (++s_rtc_frac >= 1000) {
        s_rtc_frac = 0;
        s_rtc_secs++;
    }
}

uint32_t sys_ticks(void)    { return s_ticks; }
uint32_t sys_uptime_ms(void){ return s_ticks; }

void sys_delay_ms(uint32_t ms)
{
    uint32_t start = s_ticks;
    while ((uint32_t)(s_ticks - start) < ms)
        __asm volatile ("nop");
}

/* Busy loop scaled to the measured HCLK; accurate enough for card timing. */
void sys_delay_us(uint32_t us)
{
    uint32_t cycles = us * (g_clocks.hclk_hz / 1000000UL) / 4UL;
    while (cycles--)
        __asm volatile ("nop");
}

void sys_reboot(void)
{
    uart_drain_tx();
    __dsb();
    SCB->AIRCR = SCB_AIRCR_SYSRESETREQ;
    __dsb();
    for (;;) { }
}

const char *sys_reset_cause_str(void)
{
    switch (g_clocks.reset_cause) {
    case RESET_POWER_ON: return "power-on";
    case RESET_PIN:      return "NRST pin";
    case RESET_SOFTWARE: return "software";
    case RESET_IWDG:     return "IWDG";
    case RESET_WWDG:     return "WWDG";
    case RESET_LOWPOWER: return "low-power";
    case RESET_BROWNOUT: return "brown-out";
    default:             return "unknown";
    }
}

/* ------------------------------------------------------ software RTC */
static const uint16_t s_mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

static int is_leap(uint32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Freya boots believing it is 2026-01-01 00:00:00 until 'date' says otherwise. */
static uint32_t s_rtc_base = 1767225600UL;   /* 2026-01-01T00:00:00Z */

void rtc_set(const rtc_time_t *t)
{
    uint32_t days = 0, y;
    int m;

    for (y = 1970; y < t->year; y++)
        days += is_leap(y) ? 366 : 365;
    for (m = 0; m < t->mon - 1; m++) {
        days += s_mdays[m];
        if (m == 1 && is_leap(t->year)) days++;
    }
    days += (uint32_t)t->day - 1;

    irq_disable();
    s_rtc_base = days * 86400UL + t->hour * 3600UL + t->min * 60UL + t->sec;
    s_rtc_secs = 0;
    s_rtc_frac = 0;
    irq_enable();
}

void rtc_get(rtc_time_t *t)
{
    uint32_t secs = s_rtc_base + s_rtc_secs;
    uint32_t days = secs / 86400UL;
    uint32_t rem  = secs % 86400UL;
    uint32_t y = 1970;
    int m = 0;

    t->hour = (uint8_t)(rem / 3600);
    t->min  = (uint8_t)((rem % 3600) / 60);
    t->sec  = (uint8_t)(rem % 60);

    for (;;) {
        uint32_t len = is_leap(y) ? 366 : 365;
        if (days < len) break;
        days -= len;
        y++;
    }
    for (m = 0; m < 12; m++) {
        uint32_t len = s_mdays[m] + ((m == 1 && is_leap(y)) ? 1u : 0u);
        if (days < len) break;
        days -= len;
    }
    t->year = (uint16_t)y;
    t->mon  = (uint8_t)(m + 1);
    t->day  = (uint8_t)(days + 1);
}

uint16_t rtc_fat_date(void)
{
    rtc_time_t t;
    rtc_get(&t);
    if (t.year < 1980) t.year = 1980;
    return (uint16_t)(((t.year - 1980) << 9) | ((uint32_t)t.mon << 5) | t.day);
}

uint16_t rtc_fat_time(void)
{
    rtc_time_t t;
    rtc_get(&t);
    return (uint16_t)(((uint32_t)t.hour << 11) | ((uint32_t)t.min << 5) | (t.sec / 2));
}

/* ------------------------------------------------------------- init */
void sys_init(void)
{
    g_clocks.reset_cause = detect_reset_cause();
    board_clock_init();
    systick_init();

    /* Enable the configurable faults so we get precise reports instead of
     * every error escalating to a bare HardFault.  STKALIGN is already
     * fixed at one on Cortex-M4 but has to be asked for on Cortex-M3, and
     * the fault and abort paths rewrite exception frames that must follow
     * the same alignment rule the hardware used to build them. */
    SCB->SHCSR |= SCB_SHCSR_USGFAULTENA | SCB_SHCSR_BUSFAULTENA |
                  SCB_SHCSR_MEMFAULTENA;
    SCB->CCR   |= SCB_CCR_DIV_0_TRP | SCB_CCR_STKALIGN;

    led_init();
}
