/*
 * Freya - clock tree, SysTick time base, board LED and software RTC.
 *
 * Black Pill target: 25 MHz HSE crystal.
 *   HSE 25 MHz / M=25 -> 1 MHz -> * N=192 -> 192 MHz VCO -> / P=2 -> 96 MHz
 *   PLLQ = 4 gives the exact 48 MHz the USB/SDIO clock domain wants.
 * If the crystal does not start we fall back to HSI 16 MHz with M=16,
 * which produces the same 96 MHz SYSCLK.
 *
 *   HCLK  = 96 MHz, APB1 = 48 MHz, APB2 = 96 MHz
 */
#include "freya.h"

sys_clocks_t g_clocks;

static volatile uint32_t s_ticks;      /* milliseconds since boot */
static volatile uint32_t s_rtc_secs;   /* software wall clock     */
static uint32_t s_rtc_frac;

#define HSE_HZ      25000000UL
#define HSI_HZ      16000000UL
#define TARGET_HZ   96000000UL

#define LED_PORT    GPIOC
#define LED_PIN     13             /* active low on the Black Pill */

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

static void clock_init(void)
{
    uint32_t timeout;
    uint32_t pllm;
    int use_hse = 1;

    /* Voltage scale 1 is required above 84 MHz. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR = (PWR->CR & ~PWR_CR_VOS_MASK) | PWR_CR_VOS_SCALE1;

    /* Flash: 3 wait states at 96 MHz / 3.3 V, plus prefetch and caches. */
    FLASH_R->ACR = FLASH_ACR_LATENCY(3) | FLASH_ACR_PRFTEN |
                   FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    while ((FLASH_R->ACR & 0xF) != 3) { }

    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) { }

    RCC->CR |= RCC_CR_HSEON;
    for (timeout = 0; timeout < 2000000; timeout++)
        if (RCC->CR & RCC_CR_HSERDY) break;
    if (!(RCC->CR & RCC_CR_HSERDY)) {
        RCC->CR &= ~RCC_CR_HSEON;
        use_hse = 0;
    }

    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) { }

    pllm = use_hse ? 25U : 16U;         /* both give a 1 MHz PLL input */
    RCC->PLLCFGR = pllm |
                   (192UL << 6) |                 /* PLLN = 192        */
                   (0UL << 16) |                  /* PLLP = 2          */
                   (use_hse ? RCC_PLLCFGR_SRC_HSE : 0) |
                   (4UL << 24);                   /* PLLQ = 4 -> 48MHz */

    /* Bus prescalers must be valid before SYSCLK ramps to 96 MHz. */
    RCC->CFGR = (RCC->CFGR & ~(0x0000FFF0UL)) |
                RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) { }

    if (use_hse)
        RCC->CR |= RCC_CR_CSSON;        /* trap a dying crystal        */

    g_clocks.clock_source = (uint8_t)use_hse;
    g_clocks.sysclk_hz = TARGET_HZ;
    g_clocks.hclk_hz   = TARGET_HZ;
    g_clocks.pclk1_hz  = TARGET_HZ / 2;
    g_clocks.pclk2_hz  = TARGET_HZ;
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

/* Busy loop calibrated for 96 MHz; accurate enough for card timing. */
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
    case RESET_IWDG:     return "independent watchdog";
    case RESET_WWDG:     return "window watchdog";
    case RESET_LOWPOWER: return "low-power";
    case RESET_BROWNOUT: return "brown-out";
    default:             return "unknown";
    }
}

/* -------------------------------------------------------------- LED */
void led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    (void)RCC->AHB1ENR;
    LED_PORT->MODER = (LED_PORT->MODER & ~(3UL << (LED_PIN * 2))) |
                      (1UL << (LED_PIN * 2));          /* output      */
    LED_PORT->OSPEEDR &= ~(3UL << (LED_PIN * 2));
    led_set(0);
}

void led_set(int on)
{
    /* PC13 sinks the LED: driving the pin low lights it. */
    LED_PORT->BSRR = on ? (1UL << (LED_PIN + 16)) : (1UL << LED_PIN);
}

void led_toggle(void)
{
    LED_PORT->ODR ^= (1UL << LED_PIN);
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
    clock_init();
    systick_init();

    /* Enable the configurable faults so we get precise reports instead of
     * every error escalating to a bare HardFault. */
    SCB->SHCSR |= SCB_SHCSR_USGFAULTENA | SCB_SHCSR_BUSFAULTENA |
                  SCB_SHCSR_MEMFAULTENA;
    SCB->CCR   |= SCB_CCR_DIV_0_TRP;

    led_init();
}
