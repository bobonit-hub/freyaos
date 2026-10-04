/*
 * Freya - board support for the WeAct STM32F4 64-pin core board with the
 * STM32F405RGT6: clock tree, console pins, the microSD slot driven in SPI
 * mode on its SDIO pins, status LED on PB2.  The clock tree is the
 * STM32F405 board's:
 *
 *   HSE 8 MHz / M=8 -> 1 MHz -> * N=336 -> 336 MHz VCO -> / P=2 -> 168 MHz
 *   PLLQ = 7 gives the exact 48 MHz the USB/SDIO clock domain wants.
 * If the crystal does not start we fall back to HSI 16 MHz with M=16,
 * which produces the same 168 MHz SYSCLK.
 *
 *   HCLK  = 168 MHz, APB1 = 42 MHz, APB2 = 84 MHz
 *   TIM2..TIM4 clock at 2 * APB1 = 84 MHz, because the APB1 prescaler
 *   is not 1.
 */
#include "freya.h"

#define TARGET_HZ   168000000UL

#define LED_PORT    GPIOB
#define LED_PIN     2              /* blue, active high               */

/* The microSD slot, wired for SDIO (schematic V1.1, U4). */
#define SD_CS_BIT       11              /* PC11, DAT3 */
#define SD_SCK_BIT      12              /* PC12, CLK  */
#define SD_MOSI_BIT     2               /* PD2,  CMD  */
#define SD_MISO_BIT     8               /* PC8,  DAT0 */

/* -------------------------------------------------------------- clocks */
void board_clock_init(void)
{
    uint32_t timeout;
    uint32_t pllm;
    int use_hse = 1;

    /* Voltage scale 1 is required above 144 MHz.  The regulator has to
     * arrive there before SYSCLK is raised. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR = (PWR->CR & ~PWR_CR_VOS_MASK) | PWR_CR_VOS_SCALE1;
    while (!(PWR->CSR & PWR_CSR_VOSRDY)) { }

    /* Flash: 5 wait states at 168 MHz / 3.3 V, plus prefetch and caches. */
    FLASH_R->ACR = FLASH_ACR_LATENCY(BOARD_FLASH_WS) | FLASH_ACR_PRFTEN |
                   FLASH_ACR_ICEN | FLASH_ACR_DCEN;
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

    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) { }

    pllm = use_hse ? 8U : 16U;          /* both give a 1 MHz PLL input */
    RCC->PLLCFGR = pllm |
                   (336UL << 6) |                 /* PLLN = 336        */
                   (0UL << 16) |                  /* PLLP = 2          */
                   (use_hse ? RCC_PLLCFGR_SRC_HSE : 0) |
                   (7UL << 24);                   /* PLLQ = 7 -> 48MHz */

    /* Bus prescalers must be valid before SYSCLK ramps to 168 MHz.
     * APB1 must stay at or below 42 MHz, APB2 at or below 84. */
    RCC->CFGR = (RCC->CFGR & ~(0x0000FFF0UL)) |
                RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) { }

    if (use_hse)
        RCC->CR |= RCC_CR_CSSON;        /* trap a dying crystal        */

    g_clocks.clock_source = (uint8_t)use_hse;
    g_clocks.sysclk_hz = TARGET_HZ;
    g_clocks.hclk_hz   = TARGET_HZ;
    g_clocks.pclk1_hz  = TARGET_HZ / 4;
    g_clocks.pclk2_hz  = TARGET_HZ / 2;
}

/* ---------------------------------------------------------- console pins */
void board_uart_pins(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

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

/* ---------------------------------------------------------- SD card */
/*
 * The slot's pins are the SDIO peripheral's, so the card is spoken to in
 * SPI mode by toggling them, as on the STM32H562 board of the same family.
 * Slow mode, for identification, holds each half clock 2 us: about
 * 250 kHz, inside the 100-400 kHz the spec asks for.  Fast mode runs as
 * fast as the GPIO goes.
 */
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
    sd_pin(GPIOC, 10, FREYA_PIN_IN_PULLUP, 0);   /* DAT2, 100k on the board too */
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
 * idle slave reads as high.  The driver is the fast one: this bus is
 * allowed up to PCLK/2. */
void board_spi_mux(SPI_TypeDef *spi, int sck, int miso, int mosi, int af)
{
    int pins[3] = { sck, miso, mosi };

    if (spi == SPI2) {
        RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
        (void)RCC->APB1ENR;
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
 * The F4 describes a pin in four registers, two bits each: MODER picks
 * input, output or alternate function, OTYPER push-pull or open drain,
 * PUPDR the pull.  A port's clock is turned on the first time a program
 * asks for one of its pins, which is also why this returns the base
 * address rather than letting src/gpio.c index a table of its own.
 */
GPIO_TypeDef *board_gpio_port(int port)
{
    static const uint32_t en[] = { RCC_AHB1ENR_GPIOAEN, RCC_AHB1ENR_GPIOBEN,
                                   RCC_AHB1ENR_GPIOCEN, RCC_AHB1ENR_GPIODEN };
    GPIO_TypeDef *const base[] = { GPIOA, GPIOB, GPIOC, GPIOD };

    if (port < 0 || port >= (int)ARRAY_SIZE(base)) return NULL;
    RCC->AHB1ENR |= en[port];
    (void)RCC->AHB1ENR;
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
 * Hand a pin to a peripheral - a timer channel, in the one place this is
 * called from.  The F4 names the peripheral with a number in AFR, so the
 * caller supplies the one its channel uses, and the pin is driven
 * push-pull at the same medium speed a plain output gets.
 */
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

/* Route EXTI line 'pin' to this port.  Four lines per EXTICR word. */
void board_exti_select(int port, int pin)
{
    uint32_t idx = (uint32_t)pin >> 2;
    uint32_t sh  = ((uint32_t)pin & 3U) * 4U;

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    SYSCFG->EXTICR[idx] = (SYSCFG->EXTICR[idx] & ~(0xFUL << sh)) |
                          ((uint32_t)port << sh);
}

/* --------------------------------------------------------------- ADC */
int board_adc_read(int channel)
{
    static int ready;
    uint32_t timeout;
    uint32_t shift;

    if (!ready) {
        RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
        (void)RCC->APB2ENR;
        ADC_COMMON->CCR = (ADC_COMMON->CCR & ~ADC_CCR_ADCPRE_MASK) |
                          ADC_CCR_ADCPRE_DIV4;       /* 84 / 4 = 21 MHz */
        ADC1->CR1 = 0;
        ADC1->CR2 = ADC_CR2_ADON;
        ready = 1;
    }

    if (channel == BOARD_ADC_TEMP_CHANNEL || channel == BOARD_ADC_VREF_CHANNEL) {
        if (!(ADC_COMMON->CCR & ADC_CCR_TSVREFE)) {
            ADC_COMMON->CCR |= ADC_CCR_TSVREFE;
            sys_delay_us(10);
        }
    }

    /* 480 ADC cycles is 20 us at the selected clock, long enough for
     * the internal sensors and friendly to high-impedance inputs. */
    if (channel < 10) {
        shift = (uint32_t)channel * 3U;
        ADC1->SMPR2 = (ADC1->SMPR2 & ~(7UL << shift)) |
                      (ADC_SAMPLE_LONG << shift);
    } else {
        shift = ((uint32_t)channel - 10U) * 3U;
        ADC1->SMPR1 = (ADC1->SMPR1 & ~(7UL << shift)) |
                      (ADC_SAMPLE_LONG << shift);
    }
    ADC1->SQR1 &= ~(0xFUL << 20);        /* one conversion */
    ADC1->SQR3 = (uint32_t)channel;
    ADC1->SR = 0;
    ADC1->CR2 |= ADC_CR2_SWSTART;

    for (timeout = 100000; timeout && !(ADC1->SR & ADC_SR_EOC); timeout--) { }
    if (!(ADC1->SR & ADC_SR_EOC)) return FREYA_ERR_TIMEOUT;
    return (int)(ADC1->DR & FREYA_ADC_MAX);
}

/* ----------------------------------------------------------------- LED */
void led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    (void)RCC->AHB1ENR;
    LED_PORT->MODER = (LED_PORT->MODER & ~(3UL << (LED_PIN * 2))) |
                      (1UL << (LED_PIN * 2));          /* output      */
    LED_PORT->OSPEEDR &= ~(3UL << (LED_PIN * 2));
    led_set(0);
}

void led_set(int on)
{
    /* PB2 drives the LED through 5.1 kOhm to ground: high lights it. */
    LED_PORT->BSRR = on ? (1UL << LED_PIN) : (1UL << (LED_PIN + 16));
}

void led_toggle(void)
{
    LED_PORT->ODR ^= (1UL << LED_PIN);
}

/* ------------------------------------------------------------ USB host */
#ifdef FREYA_USB
#define RCC_AHB2ENR_OTGFSEN (1UL << 7)

/* OTG_FS takes its 48 MHz from PLLQ, which board_clock_init() already
 * set.  From the crystal it is exact; on the HSI fallback it is only as
 * good as the HSI, and a stick may not keep up with that. */
int board_usb_init(void)
{
    board_pin_af(GPIOA, 11, 10);        /* OTG_FS_DM */
    board_pin_af(GPIOA, 12, 10);        /* OTG_FS_DP */
    GPIOA->OSPEEDR |= (3UL << 22) | (3UL << 24);
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    (void)RCC->AHB2ENR;
    return 0;
}

void board_usb_off(void)
{
    RCC->AHB2ENR &= ~RCC_AHB2ENR_OTGFSEN;
}
#endif
