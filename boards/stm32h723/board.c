/*
 * Freya - board support for the WeAct MiniSTM32H723 (STM32H723VGT6):
 * clock tree, console, SD card and SPI flash pins, status LED.
 *
 *   HSE 25 MHz / M=5 -> 5 MHz -> * N=104 -> 520 MHz VCO -> / P=1 -> 520 MHz
 *                                                       -> / Q=6 -> 86.7 MHz
 * If the crystal does not start we fall back to HSI 64 MHz with M=16 and
 * N=130, which produces the same 520 MHz SYSCLK.
 *
 *   core = 520 MHz, HCLK (AXI, AHB) = 260 MHz, APB1..APB4 = 130 MHz,
 *   SPI1/SPI2 kernel (PLL1Q) = 86.7 MHz
 *
 * 520 MHz is the most the H723 runs at in voltage scale 0 without the
 * CPU_FREQ_BOOST option byte, which Freya leaves alone.  The supply is the
 * LDO, as on this board.  g_clocks.hclk_hz holds the core clock, because
 * SysTick and the microsecond delay count core cycles; the AXI and AHB
 * buses run at half of it.
 */
#include "freya.h"
#include <stddef.h>

#define TARGET_HZ   520000000UL

#define LED_PORT    GPIOE
#define LED_PIN     3              /* active high on this board */

/* The register header is written by hand from RM0468.  A field in the
 * wrong place is a silent fault, so the offsets that matter are pinned. */
_Static_assert(offsetof(RCC_TypeDef, PLLCKSELR) == 0x28, "RCC PLLCKSELR");
_Static_assert(offsetof(RCC_TypeDef, D3CCIPR) == 0x58, "RCC D3CCIPR");
_Static_assert(offsetof(RCC_TypeDef, CICR) == 0x68, "RCC CICR");
_Static_assert(offsetof(RCC_TypeDef, RSR) == 0xD0, "RCC RSR");
_Static_assert(offsetof(RCC_TypeDef, AHB4ENR) == 0xE0, "RCC AHB4ENR");
_Static_assert(offsetof(RCC_TypeDef, AHB1LPENR) == 0x100, "RCC AHB1LPENR");
_Static_assert(offsetof(RCC_TypeDef, APB1LENR) == 0xE8, "RCC APB1LENR");
_Static_assert(offsetof(RCC_TypeDef, APB4ENR) == 0xF4, "RCC APB4ENR");
_Static_assert(offsetof(PWR_TypeDef, D3CR) == 0x18, "PWR D3CR");
_Static_assert(offsetof(FLASH_TypeDef, CCR1) == 0x14, "FLASH CCR1");
_Static_assert(offsetof(SYSCFG_TypeDef, EXTICR) == 0x08, "SYSCFG EXTICR");
_Static_assert(offsetof(EXTI_TypeDef, IMR) == 0x80, "EXTI CPUIMR1");
_Static_assert(offsetof(EXTI_TypeDef, PR) == 0x88, "EXTI CPUPR1");
_Static_assert(offsetof(USART_TypeDef, TDR) == 0x28, "USART TDR");
_Static_assert(offsetof(SPI_TypeDef, RXDR) == 0x30, "SPI RXDR");
_Static_assert(offsetof(ADC_TypeDef, DR) == 0x40, "ADC DR");
_Static_assert(offsetof(ADC_Common_TypeDef, CCR) == 0x08, "ADC CCR");

/* -------------------------------------------------------------- clocks */
void board_clock_init(void)
{
    uint32_t timeout;
    int use_hse = 1;

    /* The supply is written once after reset: the LDO.  Then scale 0. */
    PWR->CR3 = (PWR->CR3 & ~(PWR_CR3_SCUEN | PWR_CR3_BYPASS)) | PWR_CR3_LDOEN;
    while (!(PWR->CSR1 & PWR_CSR1_ACTVOSRDY)) { }
    PWR->D3CR = (PWR->D3CR & ~PWR_D3CR_VOS_MASK) | PWR_D3CR_VOS0;
    while (!(PWR->D3CR & PWR_D3CR_VOSRDY)) { }

    /* Three wait states and the longest programming delay for a 260 MHz
     * AXI clock in scale 0. */
    FLASH_R->ACR = (FLASH_R->ACR & ~(0xFUL | FLASH_ACR_WRHIGHFREQ_MASK)) |
                   FLASH_ACR_LATENCY(BOARD_FLASH_WS) | FLASH_ACR_WRHIGHFREQ(3);
    while ((FLASH_R->ACR & 0xF) != BOARD_FLASH_WS) { }

    RCC->CR |= RCC_CR_HSEON;
    for (timeout = 0; timeout < 4000000; timeout++)
        if (RCC->CR & RCC_CR_HSERDY) break;
    if (!(RCC->CR & RCC_CR_HSERDY)) {
        RCC->CR &= ~RCC_CR_HSEON;
        use_hse = 0;
    }

    RCC->CR &= ~RCC_CR_PLL1ON;
    while (RCC->CR & RCC_CR_PLL1RDY) { }

    /* P is the core clock, Q the SPIs' kernel clock; R is not used. */
    RCC->PLLCKSELR = (RCC->PLLCKSELR & ~(RCC_PLLCKSELR_SRC_MASK | RCC_PLLCKSELR_DIVM1_MASK)) |
                     (use_hse ? RCC_PLLCKSELR_SRC_HSE | RCC_PLLCKSELR_DIVM1(5)
                              : RCC_PLLCKSELR_SRC_HSI | RCC_PLLCKSELR_DIVM1(16));
    RCC->PLLCFGR = (RCC->PLLCFGR & ~RCC_PLLCFGR_PLL1_MASK) |
                   RCC_PLLCFGR_PLL1RGE_4_8 | RCC_PLLCFGR_PLL1VCO_WIDE |
                   RCC_PLLCFGR_DIVP1EN | RCC_PLLCFGR_DIVQ1EN;
    RCC->PLL1DIVR = RCC_PLL1DIVR_N(use_hse ? 104 : 130) | RCC_PLL1DIVR_P(1) |
                    RCC_PLL1DIVR_Q(6) | RCC_PLL1DIVR_R(2);

    RCC->CR |= RCC_CR_PLL1ON;
    while (!(RCC->CR & RCC_CR_PLL1RDY)) { }

    /* Bus prescalers must be valid before SYSCLK ramps to 520 MHz. */
    RCC->D1CFGR = RCC_D1CFGR_D1CPRE_DIV1 | RCC_D1CFGR_HPRE_DIV2 |
                  RCC_D1CFGR_D1PPRE_DIV2;
    RCC->D2CFGR = RCC_D2CFGR_D2PPRE1_DIV2 | RCC_D2CFGR_D2PPRE2_DIV2;
    RCC->D3CFGR = RCC_D3CFGR_D3PPRE_DIV2;
    RCC->D2CCIP1R &= ~RCC_D2CCIP1R_SPI123SEL_MASK;   /* SPI1, SPI2: PLL1Q */

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL1;
    while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL1) { }

    if (use_hse)
        RCC->CR |= RCC_CR_CSSON;        /* trap a dying crystal        */

    /* Instruction fetch through the cache; the data cache stays off. */
    icache_enable();
    cyccnt_enable();                    /* sys_delay_us() counts cycles */

    g_clocks.clock_source = (uint8_t)use_hse;
    g_clocks.sysclk_hz = TARGET_HZ;
    g_clocks.hclk_hz   = TARGET_HZ;     /* the core clock, see above   */
    g_clocks.pclk1_hz  = TARGET_HZ / 4;
    g_clocks.pclk2_hz  = TARGET_HZ / 4;
}

/* ---------------------------------------------------------- console pins */
void board_uart_pins(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
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
 *
 * Slow is about 250 kHz for card identification; fast is as fast as the
 * pins toggle, which a 520 MHz core keeps under the card's 25 MHz.
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

/* -------------------------------------------------------- SPI flash pins */
/* SPI1 for the SPI NOR: SCK PB3, MISO PB4, MOSI PD7, all AF5, and chip
 * select PD6, idle high. */
void board_spi_pins(void)
{
    board_gpio_port(1);                 /* GPIOB */
    board_gpio_port(3);                 /* GPIOD */
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;         /* kernel clock: PLL1Q */
    (void)RCC->APB2ENR;

    BOARD_FLASH_CS_PORT->BSRR = (1UL << BOARD_FLASH_CS_PIN);
    board_pin_mode(BOARD_FLASH_CS_PORT, BOARD_FLASH_CS_PIN, FREYA_PIN_OUT);
    board_spi_mux(SPI1, FREYA_PB(3), FREYA_PB(4), FREYA_PIN(3, 7), 5);
}

/* Hand SCK, MISO and MOSI to an SPI: a program's, the ESP32-C6 link's or
 * the SPI flash's.  MISO is pulled up so an idle slave reads as high. */
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
 * The H7 describes a pin the way the F4 does, in four registers, two bits
 * each.  A port's clock is turned on the first time a program asks for
 * one of its pins.
 */
GPIO_TypeDef *board_gpio_port(int port)
{
    static const uint32_t en[] = { RCC_AHB4ENR_GPIOAEN, RCC_AHB4ENR_GPIOBEN,
                                   RCC_AHB4ENR_GPIOCEN, RCC_AHB4ENR_GPIODEN,
                                   RCC_AHB4ENR_GPIOEEN };
    GPIO_TypeDef *const base[] = { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE };

    if (port < 0 || port >= (int)ARRAY_SIZE(base)) return NULL;
    RCC->AHB4ENR |= en[port];
    (void)RCC->AHB4ENR;
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

/* Route EXTI line 'pin' to this port.  Four lines per EXTICR word, in
 * SYSCFG, as on the F4. */
void board_exti_select(int port, int pin)
{
    uint32_t idx = (uint32_t)pin >> 2;
    uint32_t sh  = ((uint32_t)pin & 3U) * 4U;

    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    (void)RCC->APB4ENR;
    SYSCFG->EXTICR[idx] = (SYSCFG->EXTICR[idx] & ~(0xFUL << sh)) |
                          ((uint32_t)port << sh);
}

/* --------------------------------------------------------------- ADC */
/*
 * ADC1 reads the pins and ADC3 the temperature sensor and Vref; a channel
 * with BOARD_ADC3 set is ADC3's.  Both run from per_ck, which is HSI
 * (64 MHz), divided by four.  Each wakes in deep power-down: the first read
 * leaves it, starts its regulator, calibrates and enables it, and every
 * read after that is one conversion.  ADC1 is the 16-bit converter, set
 * to 12 bits; ADC3 is 12 bits as it comes.
 */
static int adc_wake(ADC_TypeDef *adc, int is_adc1)
{
    uint32_t timeout;

    if (is_adc1) {
        RCC->AHB1ENR |= RCC_AHB1ENR_ADC12EN;
        (void)RCC->AHB1ENR;
        ADC12_COMMON->CCR = (ADC12_COMMON->CCR &
                             ~(ADC_CCR_PRESC_MASK | ADC_CCR_CKMODE_MASK)) |
                            ADC_CCR_PRESC_DIV4;
        adc->CR = ADC_CR_ADVREGEN | ADC_CR_BOOST_12_25;   /* DEEPPWD off */
        for (timeout = 100000; timeout && !(adc->ISR & ADC_ISR_LDORDY); timeout--) { }
        if (!(adc->ISR & ADC_ISR_LDORDY)) return FREYA_ERR_TIMEOUT;
        adc->CFGR = (adc->CFGR & ~ADC1_CFGR_RES_MASK) | ADC1_CFGR_RES_12;
        adc->CR |= ADC_CR_ADCALLIN | ADC_CR_ADCAL;
    } else {
        RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
        (void)RCC->AHB4ENR;
        ADC3_COMMON->CCR = (ADC3_COMMON->CCR &
                            ~(ADC_CCR_PRESC_MASK | ADC_CCR_CKMODE_MASK)) |
                           ADC_CCR_PRESC_DIV4;
        adc->CR = ADC_CR_ADVREGEN;                         /* DEEPPWD off */
        sys_delay_us(50);                   /* tADCVREG_STUP: no flag here */
        adc->CR |= ADC_CR_ADCAL;
    }
    for (timeout = 10000000; timeout && (adc->CR & ADC_CR_ADCAL); timeout--) { }
    if (adc->CR & ADC_CR_ADCAL) return FREYA_ERR_TIMEOUT;

    adc->ISR = ADC_ISR_ADRDY;
    adc->CR |= ADC_CR_ADEN;
    for (timeout = 100000; timeout && !(adc->ISR & ADC_ISR_ADRDY); timeout--) { }
    if (!(adc->ISR & ADC_ISR_ADRDY)) return FREYA_ERR_TIMEOUT;
    return 0;
}

int board_adc_read(int channel)
{
    static uint8_t ready[2];
    int is_adc1 = !(channel & BOARD_ADC3);
    ADC_TypeDef *adc = is_adc1 ? ADC1 : ADC3;
    uint32_t ch = (uint32_t)channel & 0x1FU;
    uint32_t timeout;
    uint32_t shift;

    if (!ready[is_adc1]) {
        int rc;

        RCC->D1CCIPR &= ~RCC_D1CCIPR_CKPERSEL_MASK;       /* per_ck: HSI */
        RCC->D3CCIPR = (RCC->D3CCIPR & ~RCC_D3CCIPR_ADCSEL_MASK) |
                       RCC_D3CCIPR_ADCSEL_PER;
        rc = adc_wake(adc, is_adc1);
        if (rc) return rc;
        ready[is_adc1] = 1;
    }

    if (channel == BOARD_ADC_TEMP_CHANNEL || channel == BOARD_ADC_VREF_CHANNEL) {
        uint32_t want = (channel == BOARD_ADC_TEMP_CHANNEL) ? ADC_CCR_TSEN
                                                            : ADC_CCR_VREFEN;
        if (!(ADC3_COMMON->CCR & want)) {
            ADC3_COMMON->CCR |= want;
            sys_delay_us(20);
        }
    }

    /* The longest sample time: long enough for the internal sensors and
     * friendly to high-impedance inputs. */
    if (ch < 10) {
        shift = ch * 3U;
        adc->SMPR1 = (adc->SMPR1 & ~(7UL << shift)) | (ADC_SAMPLE_LONG << shift);
    } else {
        shift = (ch - 10U) * 3U;
        adc->SMPR2 = (adc->SMPR2 & ~(7UL << shift)) | (ADC_SAMPLE_LONG << shift);
    }
    if (is_adc1) adc->PCSEL = 1UL << ch;
    adc->SQR1 = ch << 6;                          /* one conversion */
    adc->ISR = ADC_ISR_EOC;
    adc->CR |= ADC_CR_ADSTART;

    for (timeout = 1000000; timeout && !(adc->ISR & ADC_ISR_EOC); timeout--) { }
    if (!(adc->ISR & ADC_ISR_EOC)) return FREYA_ERR_TIMEOUT;
    return (int)(adc->DR & FREYA_ADC_MAX);
}

/* ----------------------------------------------------------------- LED */
void led_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOEEN;
    (void)RCC->AHB4ENR;
    led_set(0);
    LED_PORT->MODER = (LED_PORT->MODER & ~(3UL << (LED_PIN * 2))) |
                      (1UL << (LED_PIN * 2));          /* output      */
    LED_PORT->OSPEEDR &= ~(3UL << (LED_PIN * 2));
}

void led_set(int on)
{
    /* PE3 sources the LED: driving the pin high lights it. */
    LED_PORT->BSRR = on ? (1UL << LED_PIN) : (1UL << (LED_PIN + 16));
}

void led_toggle(void)
{
    LED_PORT->ODR ^= (1UL << LED_PIN);
}

/* ------------------------------------------------------------ USB host */
#ifdef FREYA_USB
/* OTG_HS on its embedded full speed PHY wants 48 MHz.  PLL3 makes it from
 * 5 MHz (crystal / 5) or 4 MHz (HSI / 16): * 96 or * 120 = 480 MHz,
 * / 10 = 48 MHz.  VDD33USB is the board's 3.3 V, so the USB regulator
 * stays off and only its level detector is turned on. */
int board_usb_init(void)
{
    uint32_t t;

    if (!(RCC->CR & RCC_CR_PLL3RDY)) {
        RCC->PLLCKSELR = (RCC->PLLCKSELR & ~RCC_PLLCKSELR_DIVM3_MASK) |
                         RCC_PLLCKSELR_DIVM3(g_clocks.clock_source ? 5 : 16);
        RCC->PLLCFGR = (RCC->PLLCFGR & ~RCC_PLLCFGR_PLL3_MASK) |
                       RCC_PLLCFGR_PLL3RGE_4_8 | RCC_PLLCFGR_DIVQ3EN;
        RCC->PLL3DIVR = RCC_PLL1DIVR_N(g_clocks.clock_source ? 96 : 120) |
                        RCC_PLL1DIVR_P(2) | RCC_PLL1DIVR_Q(10) |
                        RCC_PLL1DIVR_R(2);
        RCC->CR |= RCC_CR_PLL3ON;
        for (t = 0; t < 1000000; t++)
            if (RCC->CR & RCC_CR_PLL3RDY) break;
        if (!(RCC->CR & RCC_CR_PLL3RDY)) return -1;
    }
    RCC->D2CCIP2R = (RCC->D2CCIP2R & ~RCC_D2CCIP2R_USBSEL_MASK) |
                    RCC_D2CCIP2R_USBSEL_PLL3Q;

    PWR->CR3 |= PWR_CR3_USB33DEN;
    for (t = 0; t < 1000000; t++)
        if (PWR->CR3 & PWR_CR3_USB33RDY) break;
    if (!(PWR->CR3 & PWR_CR3_USB33RDY)) return -1;

    board_pin_af(GPIOA, 11, 10);        /* OTG_HS_DM */
    board_pin_af(GPIOA, 12, 10);        /* OTG_HS_DP */
    GPIOA->OSPEEDR |= (3UL << 22) | (3UL << 24);
    RCC->AHB1LPENR &= ~RCC_AHB1LPENR_USB1OTGHSULPILPEN;
    RCC->AHB1ENR |= RCC_AHB1ENR_USB1OTGHSEN;
    (void)RCC->AHB1ENR;
    return 0;
}

void board_usb_off(void)
{
    RCC->AHB1ENR &= ~RCC_AHB1ENR_USB1OTGHSEN;
}
#endif
