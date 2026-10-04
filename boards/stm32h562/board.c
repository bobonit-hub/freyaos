/*
 * Freya - board support for the WeAct STM32H5 64-pin core board with the
 * STM32H562RGT6: clock tree, console, SD card, status LED.
 *
 *   HSE 8 MHz / M=4 -> 2 MHz -> * N=250 -> 500 MHz VCO -> / P=2 -> 250 MHz
 *                                                      -> / Q=5 -> 100 MHz
 * If the crystal does not start we fall back to HSI, which comes out of
 * reset at 64 / 2 = 32 MHz; M=16 gives the same 2 MHz and so the same
 * 250 MHz SYSCLK.
 *
 *   HCLK = APB1 = APB2 = APB3 = 250 MHz, SPI2 kernel (PLL1Q) 100 MHz
 *
 * 250 MHz needs voltage scale 0, five flash wait states and the longer
 * programming delay.  The SPIs take PLL1Q, their reset choice of kernel
 * clock, which lets every bus run at full speed and still gives a
 * program's SPI2 a slow tap of 390.6 kHz.
 *
 * Everything here but the card, the LED and port D is the STM32H523's:
 * the two chips share RM0481 and a die generation.
 */
#include "freya.h"
#include <stddef.h>

#define TARGET_HZ   250000000UL

#define LED_PORT    GPIOB
#define LED_PIN     2              /* active high on this board */

/* The register header is written by hand from RM0481.  A field in the
 * wrong place is a silent fault, so the offsets that matter are pinned. */
_Static_assert(offsetof(RCC_TypeDef, CFGR1) == 0x1C, "RCC CFGR1");
_Static_assert(offsetof(RCC_TypeDef, PLL1DIVR) == 0x34, "RCC PLL1DIVR");
_Static_assert(offsetof(RCC_TypeDef, CICR) == 0x58, "RCC CICR");
_Static_assert(offsetof(RCC_TypeDef, AHB1ENR) == 0x88, "RCC AHB1ENR");
_Static_assert(offsetof(RCC_TypeDef, APB1LENR) == 0x9C, "RCC APB1LENR");
_Static_assert(offsetof(RCC_TypeDef, APB2ENR) == 0xA4, "RCC APB2ENR");
_Static_assert(offsetof(RCC_TypeDef, CCIPR3) == 0xE0, "RCC CCIPR3");
_Static_assert(offsetof(RCC_TypeDef, CCIPR5) == 0xE8, "RCC CCIPR5");
_Static_assert(offsetof(RCC_TypeDef, RSR) == 0xF4, "RCC RSR");
_Static_assert(offsetof(PWR_TypeDef, VOSSR) == 0x14, "PWR VOSSR");
_Static_assert(offsetof(FLASH_TypeDef, NSCCR) == 0x30, "FLASH NSCCR");
_Static_assert(offsetof(EXTI_TypeDef, EXTICR) == 0x60, "EXTI EXTICR");
_Static_assert(offsetof(EXTI_TypeDef, IMR1) == 0x80, "EXTI IMR1");
_Static_assert(offsetof(USART_TypeDef, TDR) == 0x28, "USART TDR");
_Static_assert(offsetof(SPI_TypeDef, RXDR) == 0x30, "SPI RXDR");
_Static_assert(offsetof(DMA_Channel_TypeDef, CTR1) == 0x40, "GPDMA CTR1");
_Static_assert(offsetof(ADC_TypeDef, DR) == 0x40, "ADC DR");
_Static_assert(offsetof(ADC_Common_TypeDef, CCR) == 0x08, "ADC CCR");
_Static_assert(offsetof(NVIC_TypeDef, IP) == 0x300, "NVIC IPR");

/* -------------------------------------------------------------- clocks */
void board_clock_init(void)
{
    uint32_t timeout;
    int use_hse = 1;

    /* Scale 0 before anything runs faster than the 32 MHz HSI the chip
     * resets on. */
    PWR->VOSCR = (PWR->VOSCR & ~PWR_VOSCR_VOS_MASK) | PWR_VOSCR_VOS0;
    while (!(PWR->VOSSR & PWR_VOSSR_VOSRDY)) { }

    FLASH_R->ACR = (FLASH_R->ACR & ~(0xFUL | FLASH_ACR_WRHIGHFREQ_MASK)) |
                   FLASH_ACR_LATENCY(BOARD_FLASH_WS) | FLASH_ACR_WRHIGHFREQ_2 |
                   FLASH_ACR_PRFTEN;
    while ((FLASH_R->ACR & 0xF) != BOARD_FLASH_WS) { }

    RCC->CR |= RCC_CR_HSEON;
    for (timeout = 0; timeout < 2000000; timeout++)
        if (RCC->CR & RCC_CR_HSERDY) break;
    if (!(RCC->CR & RCC_CR_HSERDY)) {
        RCC->CR &= ~RCC_CR_HSEON;
        use_hse = 0;
    }

    RCC->CR &= ~RCC_CR_PLL1ON;
    while (RCC->CR & RCC_CR_PLL1RDY) { }

    /* P is SYSCLK and Q the SPIs' kernel clock; R is not used. */
    RCC->PLL1CFGR = (use_hse ? RCC_PLL1CFGR_SRC_HSE | RCC_PLL1CFGR_M(4)
                             : RCC_PLL1CFGR_SRC_HSI | RCC_PLL1CFGR_M(16)) |
                    RCC_PLL1CFGR_RGE_2_4 | RCC_PLL1CFGR_VCO_WIDE |
                    RCC_PLL1CFGR_PEN | RCC_PLL1CFGR_QEN;
    RCC->PLL1DIVR = RCC_PLL1DIVR_N(250) | RCC_PLL1DIVR_P(2) |
                    RCC_PLL1DIVR_Q(5) | RCC_PLL1DIVR_R(2);

    RCC->CR |= RCC_CR_PLL1ON;
    while (!(RCC->CR & RCC_CR_PLL1RDY)) { }

    /* Every bus at HCLK, set before SYSCLK ramps to 250 MHz. */
    RCC->CFGR2 = (RCC->CFGR2 & ~RCC_CFGR2_PRE_MASK) | RCC_CFGR2_DIV1;
    RCC->CCIPR3 &= ~RCC_CCIPR3_SPI12SEL_MASK;   /* SPI1, SPI2: PLL1Q */

    RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW_MASK) | RCC_CFGR1_SW_PLL;
    while ((RCC->CFGR1 & RCC_CFGR1_SWS_MASK) != RCC_CFGR1_SWS_PLL) { }

    if (use_hse)
        RCC->CR |= RCC_CR_CSSON;        /* trap a dying crystal        */

    /* Instruction fetch from flash through the cache. */
    ICACHE->CR |= ICACHE_CR_EN;

    g_clocks.clock_source = (uint8_t)use_hse;
    g_clocks.sysclk_hz = TARGET_HZ;
    g_clocks.hclk_hz   = TARGET_HZ;
    g_clocks.pclk1_hz  = TARGET_HZ;
    g_clocks.pclk2_hz  = TARGET_HZ;
}

/* ---------------------------------------------------------- console pins */
void board_uart_pins(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1LENR |= RCC_APB1ENR_USART2EN;      /* kernel clock: PCLK1 */
    (void)RCC->APB1LENR;

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

/* ---------------------------------------------------------- the card */
/*
 * The microSD slot is wired for SDMMC1.  Freya speaks to it in SPI mode
 * on the same pins, toggled by hand: CS on DAT3 (PC11), SCK on CLK (PC12),
 * MOSI on CMD (PD2) and MISO on DAT0 (PC8).  DAT1 and DAT2 (PC9, PC10)
 * are only held up.  Mode 0: MOSI is set while SCK is low, MISO is read
 * just after SCK rises, which is half a bit after the card changed it.
 * This is the MiniSTM32H723's slot, on the same pins.
 *
 * Slow is about 250 kHz for card identification; fast is as fast as the
 * pins toggle, which at 250 MHz is well under the card's 25 MHz.
 */
#define SD_CS_BIT       11              /* PC11 */
#define SD_SCK_BIT      12              /* PC12 */
#define SD_MOSI_BIT     2               /* PD2  */
#define SD_MISO_BIT     8               /* PC8  */

static uint8_t s_sd_fast;

static void sd_pin(GPIO_TypeDef *port, int pin, int mode, int fast)
{
    uint32_t pair = (uint32_t)pin * 2U;

    board_pin_mode(port, pin, mode);
    if (fast) port->OSPEEDR |= (3UL << pair);
}

void sdspi_init(void)
{
    board_gpio_port(2);                 /* GPIOC */
    board_gpio_port(3);                 /* GPIOD */

    GPIOC->BSRR = (1UL << SD_CS_BIT) | (1UL << (SD_SCK_BIT + 16));
    GPIOD->BSRR = (1UL << SD_MOSI_BIT);
    sd_pin(GPIOC, SD_CS_BIT, FREYA_PIN_OUT, 1);
    sd_pin(GPIOC, SD_SCK_BIT, FREYA_PIN_OUT, 1);
    sd_pin(GPIOD, SD_MOSI_BIT, FREYA_PIN_OUT, 1);
    sd_pin(GPIOC, SD_MISO_BIT, FREYA_PIN_IN_PULLUP, 0);
    sd_pin(GPIOC, 9, FREYA_PIN_IN_PULLUP, 0);    /* DAT1 */
    sd_pin(GPIOC, 10, FREYA_PIN_IN_PULLUP, 0);   /* DAT2 */
    s_sd_fast = 0;
}

void sdspi_set_speed(int fast)
{
    s_sd_fast = (uint8_t)(fast ? 1 : 0);
}

void sdspi_cs(int low)
{
    GPIOC->BSRR = low ? (1UL << (SD_CS_BIT + 16)) : (1UL << SD_CS_BIT);
}

uint8_t sdspi_xfer(uint8_t v)
{
    uint32_t in = 0;

    for (int b = 7; b >= 0; b--) {
        GPIOD->BSRR = ((v >> b) & 1U) ? (1UL << SD_MOSI_BIT)
                                      : (1UL << (SD_MOSI_BIT + 16));
        if (!s_sd_fast) sys_delay_us(2);
        GPIOC->BSRR = 1UL << SD_SCK_BIT;
        if (!s_sd_fast) sys_delay_us(2);
        else (void)GPIOC->IDR;          /* hold SCK high a bus round trip */
        in = (in << 1) | ((GPIOC->IDR >> SD_MISO_BIT) & 1U);
        GPIOC->BSRR = 1UL << (SD_SCK_BIT + 16);
    }
    return (uint8_t)in;
}

void sdspi_write(const uint8_t *buf, uint32_t len)
{
    while (len--) (void)sdspi_xfer(*buf++);
}

void sdspi_read(uint8_t *buf, uint32_t len)
{
    while (len--) *buf++ = sdspi_xfer(0xFF);
}

/* Let go of the slot's pins.  The slot has no supply switch, so this is
 * all 'power sd off' can do: the card stays powered, but nothing drives
 * it. */
void sdspi_quiesce(void) __attribute__((noinline, section(".text.sdspi_quiesce")));
void sdspi_quiesce(void)
{
    board_gpio_port(2);
    board_gpio_port(3);
    for (int n = 8; n <= 12; n++)
        board_pin_mode(GPIOC, n, FREYA_PIN_ANALOG);
    board_pin_mode(GPIOD, SD_MOSI_BIT, FREYA_PIN_ANALOG);
}

void board_sd_power(int on) __attribute__((noinline, section(".text.board_sd_power")));
void board_sd_power(int on)
{
    (void)on;                           /* no switch on this board */
}

/* Hand SCK, MISO and MOSI to a program's SPI.  MISO is pulled up so an
 * idle slave reads as high. */
void board_spi_mux(SPI_TypeDef *spi, int sck, int miso, int mosi, int af)
{
    int pins[3] = { sck, miso, mosi };

    if (spi == SPI2) {
        RCC->APB1LENR |= RCC_APB1ENR_SPI2EN;    /* kernel clock: PLL1Q */
        (void)RCC->APB1LENR;
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
 * The H5 describes a pin the way the F4 does, in four registers, two bits
 * each.  A port's clock is turned on the first time a program asks for
 * one of its pins.
 */
GPIO_TypeDef *board_gpio_port(int port)
{
    static const uint32_t en[] = { RCC_AHB2ENR_GPIOAEN, RCC_AHB2ENR_GPIOBEN,
                                   RCC_AHB2ENR_GPIOCEN, RCC_AHB2ENR_GPIODEN };
    GPIO_TypeDef *const base[] = { GPIOA, GPIOB, GPIOC, GPIOD };

    if (port < 0 || port >= (int)ARRAY_SIZE(base)) return NULL;
    RCC->AHB2ENR |= en[port];
    (void)RCC->AHB2ENR;
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
 * the H5, a byte per line, four lines per EXTICR word. */
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
 * regulator and waits out its start-up time (the H5 has no ready flag for
 * it), calibrates the offset and enables it; every read after that is one
 * conversion.
 */
int board_adc_read(int channel)
{
    static int ready;
    uint32_t timeout;
    uint32_t shift;

    if (!ready) {
        RCC->CCIPR5 &= ~RCC_CCIPR5_ADCDACSEL_MASK;    /* HCLK */
        RCC->AHB2ENR |= RCC_AHB2ENR_ADCEN;
        (void)RCC->AHB2ENR;
        ADC_COMMON->CCR = (ADC_COMMON->CCR &
                           ~(ADC_CCR_PRESC_MASK | ADC_CCR_CKMODE_MASK)) |
                          ADC_CCR_PRESC_DIV4;    /* 250 / 4 = 62.5 MHz */

        ADC1->CR = ADC_CR_ADVREGEN;              /* DEEPPWD cleared too */
        sys_delay_us(50);                        /* tADCVREG_STUP: 20 us */

        ADC1->CR |= ADC_CR_ADCAL;
        for (timeout = 1000000; timeout && (ADC1->CR & ADC_CR_ADCAL); timeout--) { }
        if (ADC1->CR & ADC_CR_ADCAL) return FREYA_ERR_TIMEOUT;

        ADC1->ISR = ADC_ISR_ADRDY;               /* 12 bits: CFGR's reset */
        ADC1->CR |= ADC_CR_ADEN;
        for (timeout = 100000; timeout && !(ADC1->ISR & ADC_ISR_ADRDY); timeout--) { }
        if (!(ADC1->ISR & ADC_ISR_ADRDY)) return FREYA_ERR_TIMEOUT;
        ready = 1;
    }

    if (channel == BOARD_ADC_TEMP_CHANNEL || channel == BOARD_ADC_VREF_CHANNEL) {
        uint32_t want = (channel == BOARD_ADC_TEMP_CHANNEL) ? ADC_CCR_TSEN
                                                            : ADC_CCR_VREFEN;
        if (!(ADC_COMMON->CCR & want)) {
            ADC_COMMON->CCR |= want;
            sys_delay_us(20);
        }
    }

    /* 640.5 ADC cycles is 10 us at 62.5 MHz, long enough for the
     * internal sensors and friendly to high-impedance inputs. */
    if (channel < 10) {
        shift = (uint32_t)channel * 3U;
        ADC1->SMPR1 = (ADC1->SMPR1 & ~(7UL << shift)) |
                      (ADC_SAMPLE_LONG << shift);
    } else {
        shift = ((uint32_t)channel - 10U) * 3U;
        ADC1->SMPR2 = (ADC1->SMPR2 & ~(7UL << shift)) |
                      (ADC_SAMPLE_LONG << shift);
    }
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
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    (void)RCC->AHB2ENR;
    led_set(0);
    LED_PORT->MODER = (LED_PORT->MODER & ~(3UL << (LED_PIN * 2))) |
                      (1UL << (LED_PIN * 2));          /* output      */
    LED_PORT->OSPEEDR &= ~(3UL << (LED_PIN * 2));
}

void led_set(int on)
{
    /* PB2 sources the LED: driving the pin high lights it. */
    LED_PORT->BSRR = on ? (1UL << LED_PIN) : (1UL << (LED_PIN + 16));
}

void led_toggle(void)
{
    LED_PORT->ODR ^= (1UL << LED_PIN);
}
