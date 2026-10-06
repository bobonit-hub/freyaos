/*
 * Freya - board support for the WeAct STM32U585CIU6 core board:
 * clock tree, console and SD card pin mux, status LED.
 *
 *   HSE 25 MHz / M=5 -> 5 MHz -> * N=64 -> 320 MHz VCO -> / R=2 -> 160 MHz
 * If the crystal does not start we fall back to HSI 16 MHz with M=4 and
 * N=80, which produces the same 160 MHz SYSCLK.
 *
 *   HCLK  = 160 MHz, APB1 = 80 MHz, APB2 = 80 MHz, APB3 = 160 MHz
 *
 * 160 MHz needs voltage range 1, four flash wait states and the EPOD
 * booster, whose clock is the PLL input divided down to 4..16 MHz
 * (MBOOST).  APB1 and APB2 run at half speed so that the card's SPI1
 * still has a divider inside the identification window, and so that a
 * program's SPI2 reaches down towards the slowest rate the API names.
 */
#include "freya.h"
#include <stddef.h>

#define TARGET_HZ   160000000UL

#define LED_PORT    GPIOC
#define LED_PIN     13             /* active low, as on the Black Pill */

/* The register header is written by hand from RM0456.  A field in the
 * wrong place is a silent fault, so the offsets that matter are pinned. */
_Static_assert(offsetof(RCC_TypeDef, AHB1ENR) == 0x88, "RCC AHB1ENR");
_Static_assert(offsetof(RCC_TypeDef, APB1ENR1) == 0x9C, "RCC APB1ENR1");
_Static_assert(offsetof(RCC_TypeDef, APB2ENR) == 0xA4, "RCC APB2ENR");
_Static_assert(offsetof(RCC_TypeDef, CCIPR3) == 0xE8, "RCC CCIPR3");
_Static_assert(offsetof(RCC_TypeDef, CSR) == 0xF4, "RCC CSR");
_Static_assert(offsetof(FLASH_TypeDef, NSCR) == 0x28, "FLASH NSCR");
_Static_assert(offsetof(EXTI_TypeDef, EXTICR) == 0x60, "EXTI EXTICR");
_Static_assert(offsetof(EXTI_TypeDef, IMR1) == 0x80, "EXTI IMR1");
_Static_assert(offsetof(USART_TypeDef, TDR) == 0x28, "USART TDR");
_Static_assert(offsetof(SPI_TypeDef, RXDR) == 0x30, "SPI RXDR");
_Static_assert(offsetof(DMA_Channel_TypeDef, CTR1) == 0x40, "GPDMA CTR1");
_Static_assert(offsetof(DMA_Channel_TypeDef, CLLR) == 0x7C, "GPDMA CLLR");
_Static_assert(offsetof(ADC_TypeDef, DR) == 0x40, "ADC DR");
_Static_assert(offsetof(NVIC_TypeDef, IP) == 0x300, "NVIC IPR");

/* -------------------------------------------------------------- clocks */
void board_clock_init(void)
{
    uint32_t timeout;
    int use_hse = 1;

    /* Range 1 and the booster before anything runs faster than the
     * 4 MHz MSIS the chip resets on. */
    RCC->AHB3ENR |= RCC_AHB3ENR_PWREN;
    (void)RCC->AHB3ENR;
    PWR->VOSR = (PWR->VOSR & ~(PWR_VOSR_VOS_MASK | PWR_VOSR_BOOSTEN)) |
                PWR_VOSR_VOS_RANGE1 | PWR_VOSR_BOOSTEN;
    while (!(PWR->VOSR & PWR_VOSR_VOSRDY)) { }

    FLASH_R->ACR = (FLASH_R->ACR & ~0xFUL) | FLASH_ACR_LATENCY(BOARD_FLASH_WS) |
                   FLASH_ACR_PRFTEN;
    while ((FLASH_R->ACR & 0xF) != BOARD_FLASH_WS) { }

    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) { }

    RCC->CR |= RCC_CR_HSEON;
    for (timeout = 0; timeout < 2000000; timeout++)
        if (RCC->CR & RCC_CR_HSERDY) break;
    if (!(RCC->CR & RCC_CR_HSERDY)) {
        RCC->CR &= ~RCC_CR_HSEON;
        use_hse = 0;
    }

    RCC->CR &= ~RCC_CR_PLL1ON;
    while (RCC->CR & RCC_CR_PLL1RDY) { }

    /* The booster's clock is chosen with the PLL, so it is off while the
     * PLL is configured: 25 / 2 = 12.5 MHz from the crystal, 16 MHz from
     * HSI. */
    PWR->VOSR &= ~PWR_VOSR_BOOSTEN;
    if (use_hse) {
        RCC->PLL1CFGR = RCC_PLL1CFGR_SRC_HSE | RCC_PLL1CFGR_RGE_4_8 |
                        RCC_PLL1CFGR_M(5) | RCC_PLL1CFGR_MBOOST(1) |
                        RCC_PLL1CFGR_REN;
        RCC->PLL1DIVR = RCC_PLL1DIVR_N(64) | RCC_PLL1DIVR_P(2) |
                        RCC_PLL1DIVR_Q(2) | RCC_PLL1DIVR_R(2);
    } else {
        RCC->PLL1CFGR = RCC_PLL1CFGR_SRC_HSI | RCC_PLL1CFGR_RGE_4_8 |
                        RCC_PLL1CFGR_M(4) | RCC_PLL1CFGR_MBOOST(0) |
                        RCC_PLL1CFGR_REN;
        RCC->PLL1DIVR = RCC_PLL1DIVR_N(80) | RCC_PLL1DIVR_P(2) |
                        RCC_PLL1DIVR_Q(2) | RCC_PLL1DIVR_R(2);
    }
    PWR->VOSR |= PWR_VOSR_BOOSTEN;

    RCC->CR |= RCC_CR_PLL1ON;
    while (!(RCC->CR & RCC_CR_PLL1RDY)) { }

    /* Bus prescalers must be valid before SYSCLK ramps to 160 MHz. */
    RCC->CFGR2 = (RCC->CFGR2 & ~RCC_CFGR2_PRE_MASK) |
                 RCC_CFGR2_HPRE_DIV1 | RCC_CFGR2_PPRE1_DIV2 | RCC_CFGR2_PPRE2_DIV2;
    RCC->CFGR3 &= ~RCC_CFGR3_PPRE3_MASK;

    while (!(PWR->VOSR & PWR_VOSR_BOOSTRDY)) { }
    RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW_MASK) | RCC_CFGR1_SW_PLL;
    while ((RCC->CFGR1 & RCC_CFGR1_SWS_MASK) != RCC_CFGR1_SWS_PLL) { }

    if (use_hse)
        RCC->CR |= RCC_CR_CSSON;        /* trap a dying crystal        */

    /* Instruction fetch from flash through the cache. */
    ICACHE->CR |= ICACHE_CR_EN;

    g_clocks.clock_source = (uint8_t)use_hse;
    g_clocks.sysclk_hz = TARGET_HZ;
    g_clocks.hclk_hz   = TARGET_HZ;
    g_clocks.pclk1_hz  = TARGET_HZ / 2;
    g_clocks.pclk2_hz  = TARGET_HZ / 2;
}

/* ---------------------------------------------------------- console pins */
void board_uart_pins(void)
{
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR_USART2EN;      /* kernel clock: PCLK1 */
    (void)RCC->APB1ENR1;

    /* PA2, PA3 -> alternate function 7, push-pull, high speed. */
    GPIOA->MODER   = (GPIOA->MODER   & ~((3UL << 4) | (3UL << 6))) |
                     ((2UL << 4) | (2UL << 6));
    GPIOA->OTYPER &= ~((1UL << 2) | (1UL << 3));
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~((3UL << 4) | (3UL << 6))) |
                     ((3UL << 4) | (3UL << 6));
    GPIOA->PUPDR   = (GPIOA->PUPDR   & ~((3UL << 4) | (3UL << 6))) |
                     ((1UL << 4) | (1UL << 6));      /* pull-ups */
    GPIOA->AFR[0]  = (GPIOA->AFR[0] & ~((0xFUL << 8) | (0xFUL << 12))) |
                     ((7UL << 8) | (7UL << 12));
}

/* ---------------------------------------------------------- SD card power */
/* PA8 is the gate.  The level is latched before the pin becomes an
 * output, so the card never sees the gate float through the other state. */
void board_sd_power(int on) __attribute__((noinline, section(".text.board_sd_power")));
void board_sd_power(int on)
{
    GPIO_TypeDef *port = BOARD_SD_PWR_PORT;
    uint32_t pin = BOARD_SD_PWR_PIN;
    uint32_t pair = pin * 2U;
    int high = on ? BOARD_SD_PWR_ON : !BOARD_SD_PWR_ON;

    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    (void)RCC->AHB2ENR1;

    if (high) port->BSRR = (1UL << pin);
    else      port->BSRR = (1UL << (pin + 16));
    port->OTYPER  &= ~(1UL << pin);
    port->OSPEEDR  = (port->OSPEEDR & ~(3UL << pair)) | (1UL << pair);
    port->PUPDR   &= ~(3UL << pair);
    port->MODER    = (port->MODER & ~(3UL << pair)) | (1UL << pair);
}

/* ---------------------------------------------------------- SD card pins */
void board_spi_pins(void)
{
    board_sd_power(1);                  /* VDD on before any clock       */

    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    RCC->APB2ENR  |= RCC_APB2ENR_SPI1EN;        /* kernel clock: PCLK2 */
    (void)RCC->APB2ENR;

    /* PA5 = SCK, PA6 = MISO, PA7 = MOSI -> AF5 (SPI1). */
    GPIOA->MODER   = (GPIOA->MODER & ~((3UL << 10) | (3UL << 12) | (3UL << 14))) |
                     ((2UL << 10) | (2UL << 12) | (2UL << 14));
    GPIOA->OTYPER &= ~((1UL << 5) | (1UL << 6) | (1UL << 7));
    GPIOA->OSPEEDR |= (3UL << 10) | (3UL << 12) | (3UL << 14);
    GPIOA->PUPDR   = (GPIOA->PUPDR & ~((3UL << 10) | (3UL << 12) | (3UL << 14))) |
                     (1UL << 12);                       /* pull-up on MISO */
    GPIOA->AFR[0]  = (GPIOA->AFR[0] & ~((0xFUL << 20) | (0xFUL << 24) | (0xFUL << 28))) |
                     ((5UL << 20) | (5UL << 24) | (5UL << 28));

    /* PA4 as a plain push-pull output, idle high. */
    BOARD_SD_CS_PORT->BSRR = (1UL << BOARD_SD_CS_PIN);
    GPIOA->MODER   = (GPIOA->MODER & ~(3UL << (BOARD_SD_CS_PIN * 2))) |
                     (1UL << (BOARD_SD_CS_PIN * 2));
    GPIOA->OSPEEDR |= (3UL << (BOARD_SD_CS_PIN * 2));
}

/* Hand SCK, MISO and MOSI to a program's SPI.  MISO is pulled up so an
 * idle slave reads as high. */
void board_spi_mux(SPI_TypeDef *spi, int sck, int miso, int mosi, int af)
{
    int pins[3] = { sck, miso, mosi };

    if (spi == SPI2) {
        RCC->APB1ENR1 |= RCC_APB1ENR_SPI2EN;    /* kernel clock: PCLK1 */
        (void)RCC->APB1ENR1;
    }

    for (int i = 0; i < 3; i++) {
        int n = FREYA_PIN_NUM(pins[i]);
        uint32_t pair = (uint32_t)n * 2U;
        uint32_t idx  = (uint32_t)n >> 3;
        uint32_t sh   = ((uint32_t)n & 7U) * 4U;
        GPIO_TypeDef *port = board_gpio_port(FREYA_PIN_PORT(pins[i]));

        if (!port) continue;
        port->AFR[idx] = (port->AFR[idx] & ~(0xFUL << sh)) |
                         (((uint32_t)af & 0xFUL) << sh);
        port->MODER    = (port->MODER & ~(3UL << pair)) | (2UL << pair);
        port->OTYPER  &= ~(1UL << n);
        port->OSPEEDR  = (port->OSPEEDR & ~(3UL << pair)) | (3UL << pair);
        if (pins[i] == miso)
            port->PUPDR = (port->PUPDR & ~(3UL << pair)) | (1UL << pair);
        else
            port->PUPDR = (port->PUPDR & ~(3UL << pair));
    }
}

/* ---------------------------------------------------------- pins for programs */
/*
 * The U5 describes a pin the way the F4 does, in four registers, two bits
 * each.  A port's clock is turned on the first time a program asks for
 * one of its pins.
 */
GPIO_TypeDef *board_gpio_port(int port)
{
    static const uint32_t en[] = { RCC_AHB2ENR1_GPIOAEN, RCC_AHB2ENR1_GPIOBEN,
                                   RCC_AHB2ENR1_GPIOCEN };
    GPIO_TypeDef *const base[] = { GPIOA, GPIOB, GPIOC };

    if (port < 0 || port >= (int)ARRAY_SIZE(base)) return NULL;
    RCC->AHB2ENR1 |= en[port];
    (void)RCC->AHB2ENR1;
    return base[port];
}

void board_pin_mode(GPIO_TypeDef *port, int pin, int mode)
{
    uint32_t pair = (uint32_t)(pin * 2);
    uint32_t moder = 0, pupdr = 0;

    switch (mode) {
    case FREYA_PIN_IN_PULLUP:   pupdr = 1; break;
    case FREYA_PIN_IN_PULLDOWN: pupdr = 2; break;
    case FREYA_PIN_OUT:         moder = 1; break;
    case FREYA_PIN_OUT_OD:      moder = 1; pupdr = 1; break; /* ~40 kΩ, for I2C */
    case FREYA_PIN_ANALOG:      moder = 3; break;
    default:                    break;          /* floating input */
    }

    /* Already this output: leave the latch.  Writing the mode again
     * drops the pad, so toggle would set it once and then stick. */
    if (moder == 1 && ((port->MODER >> pair) & 3UL) == 1UL &&
        ((port->OTYPER >> pin) & 1UL) == (uint32_t)(mode == FREYA_PIN_OUT_OD))
        return;
    port->MODER   = (port->MODER   & ~(3UL << pair)) | (moder << pair);
    port->PUPDR   = (port->PUPDR   & ~(3UL << pair)) | (pupdr << pair);
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << pair)) | (1UL << pair);  /* medium */
    if (mode == FREYA_PIN_OUT_OD) port->OTYPER |=  (1UL << pin);
    else                          port->OTYPER &= ~(1UL << pin);
}

/*
 * The pull alone.  An input and an open-drain output take any of the
 * three.  A push-pull output would only spend current through it, a PWM
 * pin is the timer's, and an analog one must have none, so those are
 * refused.  FREYA_PULL_* are the PUPDR values themselves.
 */
int board_pin_pull(GPIO_TypeDef *port, int pin, int pull)
{
    uint32_t pair  = (uint32_t)(pin * 2);
    uint32_t moder = (port->MODER >> pair) & 3UL;

    if (moder >= 2UL || (moder == 1UL && !((port->OTYPER >> pin) & 1UL)))
        return FREYA_ERR_ARG;
    port->PUPDR = (port->PUPDR & ~(3UL << pair)) | ((uint32_t)pull << pair);
    return 0;
}

int board_pin_pull_get(GPIO_TypeDef *port, int pin)
{
    uint32_t pupdr = (port->PUPDR >> (pin * 2)) & 3UL;

    return (pupdr == 3UL) ? FREYA_PULL_NONE : (int)pupdr;   /* 3: reserved */
}

/* Hand a pin to a timer channel: the alternate function number the
 * channel uses, push-pull at the medium speed a plain output gets. */
void board_pin_af(GPIO_TypeDef *port, int pin, int af)
{
    uint32_t pair = (uint32_t)(pin * 2);
    uint32_t idx  = (uint32_t)pin >> 3;
    uint32_t sh   = ((uint32_t)pin & 7U) * 4U;

    port->AFR[idx] = (port->AFR[idx] & ~(0xFUL << sh)) |
                     (((uint32_t)af & 0xFUL) << sh);
    port->MODER    = (port->MODER    & ~(3UL << pair)) | (2UL << pair);
    port->OTYPER  &= ~(1UL << pin);
    port->PUPDR    = (port->PUPDR    & ~(3UL << pair));
    port->OSPEEDR  = (port->OSPEEDR  & ~(3UL << pair)) | (1UL << pair);
}

/* Route EXTI line 'pin' to this port.  The selector is in EXTI itself on
 * the U5, a byte per line, four lines per EXTICR word. */
void board_exti_select(int port, int pin)
{
    uint32_t idx = (uint32_t)pin >> 2;
    uint32_t sh  = ((uint32_t)pin & 3U) * 8U;

    EXTI->EXTICR[idx] = (EXTI->EXTICR[idx] & ~(0xFFUL << sh)) |
                        ((uint32_t)port << sh);
}

/* --------------------------------------------------------------- ADC */
/*
 * ADC1 wakes in deep power-down.  The first read leaves it, starts its
 * regulator, calibrates the offset and enables it; every read after that
 * is one conversion.  A channel has to be preselected in PCSEL as well as
 * named in the sequence.
 */
int board_adc_read(int channel)
{
    static int ready;
    uint32_t timeout;
    uint32_t shift;

    if (!ready) {
        RCC->AHB3ENR |= RCC_AHB3ENR_PWREN;
        PWR->SVMCR |= PWR_SVMCR_ASV;             /* VDDA is there */
        RCC->CCIPR3 &= ~RCC_CCIPR3_ADCDACSEL_MASK;    /* HCLK */
        RCC->AHB2ENR1 |= RCC_AHB2ENR1_ADC12EN;
        (void)RCC->AHB2ENR1;
        ADC_COMMON->CCR = (ADC_COMMON->CCR & ~ADC_CCR_PRESC_MASK) |
                          ADC_CCR_PRESC_DIV4;    /* 160 / 4 = 40 MHz */

        ADC1->CR = ADC_CR_ADVREGEN;              /* DEEPPWD cleared too */
        for (timeout = 100000; timeout && !(ADC1->ISR & ADC_ISR_LDORDY); timeout--) { }
        if (!(ADC1->ISR & ADC_ISR_LDORDY)) return FREYA_ERR_TIMEOUT;

        ADC1->CR |= ADC_CR_ADCAL;
        for (timeout = 1000000; timeout && (ADC1->CR & ADC_CR_ADCAL); timeout--) { }
        if (ADC1->CR & ADC_CR_ADCAL) return FREYA_ERR_TIMEOUT;

        ADC1->CFGR1 = (ADC1->CFGR1 & ~ADC_CFGR1_RES_MASK) | ADC_CFGR1_RES_12;
        ADC1->ISR = ADC_ISR_ADRDY;
        ADC1->CR |= ADC_CR_ADEN;
        for (timeout = 100000; timeout && !(ADC1->ISR & ADC_ISR_ADRDY); timeout--) { }
        if (!(ADC1->ISR & ADC_ISR_ADRDY)) return FREYA_ERR_TIMEOUT;
        ready = 1;
    }

    if (channel == BOARD_ADC_TEMP_CHANNEL || channel == BOARD_ADC_VREF_CHANNEL) {
        uint32_t want = (channel == BOARD_ADC_TEMP_CHANNEL) ? ADC_CCR_VSENSEEN
                                                            : ADC_CCR_VREFEN;
        if (!(ADC_COMMON->CCR & want)) {
            ADC_COMMON->CCR |= want;
            sys_delay_us(20);
        }
    }

    /* 814.5 ADC cycles is 20 us at 40 MHz, long enough for the internal
     * sensors and friendly to high-impedance inputs. */
    if (channel < 10) {
        shift = (uint32_t)channel * 3U;
        ADC1->SMPR1 = (ADC1->SMPR1 & ~(7UL << shift)) |
                      (ADC_SAMPLE_LONG << shift);
    } else {
        shift = ((uint32_t)channel - 10U) * 3U;
        ADC1->SMPR2 = (ADC1->SMPR2 & ~(7UL << shift)) |
                      (ADC_SAMPLE_LONG << shift);
    }
    ADC1->PCSEL = 1UL << channel;
    ADC1->SQR1 = (uint32_t)channel << 6;          /* one conversion */
    ADC1->ISR = ADC_ISR_EOC;
    ADC1->CR |= ADC_CR_ADSTART;

    for (timeout = 100000; timeout && !(ADC1->ISR & ADC_ISR_EOC); timeout--) { }
    if (!(ADC1->ISR & ADC_ISR_EOC)) return FREYA_ERR_TIMEOUT;
    return (int)(ADC1->DR & FREYA_ADC_MAX);
}

/* ----------------------------------------------------------------- LED */
void led_init(void)
{
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOCEN;
    (void)RCC->AHB2ENR1;
    led_set(0);
    LED_PORT->MODER = (LED_PORT->MODER & ~(3UL << (LED_PIN * 2))) |
                      (1UL << (LED_PIN * 2));          /* output      */
    LED_PORT->OSPEEDR &= ~(3UL << (LED_PIN * 2));
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

/* ------------------------------------------------------------ USB host */
#ifdef FREYA_USB
/* OTG_FS wants 48 MHz, which PLL1's 320 MHz VCO does not divide down to.
 * PLL2 makes it from the same 5 MHz (crystal / 5) or 4 MHz (HSI / 4)
 * input PLL1 uses: * 48 or * 60 = 240 MHz, / 5 = 48 MHz.  The HSI48
 * would do without a PLL, but a host has nothing to trim it against. */
int board_usb_init(void)
{
    uint32_t t;

    if (!(RCC->CR & RCC_CR_PLL2RDY)) {
        if (g_clocks.clock_source) {
            RCC->PLL2CFGR = RCC_PLL1CFGR_SRC_HSE | RCC_PLL1CFGR_RGE_4_8 |
                            RCC_PLL1CFGR_M(5) | RCC_PLL2CFGR_QEN;
            RCC->PLL2DIVR = RCC_PLL1DIVR_N(48) | RCC_PLL1DIVR_P(2) |
                            RCC_PLL1DIVR_Q(5) | RCC_PLL1DIVR_R(2);
        } else {
            RCC->PLL2CFGR = RCC_PLL1CFGR_SRC_HSI | RCC_PLL1CFGR_RGE_4_8 |
                            RCC_PLL1CFGR_M(4) | RCC_PLL2CFGR_QEN;
            RCC->PLL2DIVR = RCC_PLL1DIVR_N(60) | RCC_PLL1DIVR_P(2) |
                            RCC_PLL1DIVR_Q(5) | RCC_PLL1DIVR_R(2);
        }
        RCC->CR |= RCC_CR_PLL2ON;
        for (t = 0; t < 1000000; t++)
            if (RCC->CR & RCC_CR_PLL2RDY) break;
        if (!(RCC->CR & RCC_CR_PLL2RDY)) return -1;
    }
    RCC->CCIPR1 = (RCC->CCIPR1 & ~RCC_CCIPR1_ICLKSEL_MASK) |
                  RCC_CCIPR1_ICLKSEL_PLL2Q;

    PWR->SVMCR |= PWR_SVMCR_USV;        /* VDDUSB is the board's 3.3 V   */

    board_pin_af(GPIOA, 11, 10);        /* OTG_FS_DM */
    board_pin_af(GPIOA, 12, 10);        /* OTG_FS_DP */
    GPIOA->OSPEEDR |= (3UL << 22) | (3UL << 24);
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_OTGEN;
    (void)RCC->AHB2ENR1;
    return 0;
}

void board_usb_off(void)
{
    RCC->AHB2ENR1 &= ~RCC_AHB2ENR1_OTGEN;
}
#endif
