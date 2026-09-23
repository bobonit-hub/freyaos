/*
 * Freya - board support for the WeAct STM32F411CEU6 "Black Pill":
 * clock tree, console and SD card pin mux, status LED.
 *
 *   HSE 25 MHz / M=25 -> 1 MHz -> * N=192 -> 192 MHz VCO -> / P=2 -> 96 MHz
 *   PLLQ = 4 gives the exact 48 MHz the USB/SDIO clock domain wants.
 * If the crystal does not start we fall back to HSI 16 MHz with M=16,
 * which produces the same 96 MHz SYSCLK.
 *
 *   HCLK  = 96 MHz, APB1 = 48 MHz, APB2 = 96 MHz
 */
#include "freya.h"

#define TARGET_HZ   96000000UL

#define LED_PORT    GPIOC
#define LED_PIN     13             /* active low on the Black Pill */

/* -------------------------------------------------------------- clocks */
void board_clock_init(void)
{
    uint32_t timeout;
    uint32_t pllm;
    int use_hse = 1;

    /* Voltage scale 1 is required above 84 MHz. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR = (PWR->CR & ~PWR_CR_VOS_MASK) | PWR_CR_VOS_SCALE1;

    /* Flash: 3 wait states at 96 MHz / 3.3 V, plus prefetch and caches. */
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

/* ---------------------------------------------------------- SD card power */
/* PA8 is the gate.  The level is latched before the pin becomes an
 * output, so the card never sees the gate float through the other state. */
/* Own section.  The Black Pill linker keeps it out of the 48 KiB image. */
void board_sd_power(int on) __attribute__((noinline, section(".text.board_sd_power")));
void board_sd_power(int on)
{
    GPIO_TypeDef *port = BOARD_SD_PWR_PORT;
    uint32_t pin = BOARD_SD_PWR_PIN;
    uint32_t pair = pin * 2U;
    int high = on ? BOARD_SD_PWR_ON : !BOARD_SD_PWR_ON;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    (void)RCC->AHB1ENR;

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

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
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
    GPIOA->MODER   = (GPIOA->MODER & ~(3UL << (BOARD_SD_CS_PIN * 2))) |
                     (1UL << (BOARD_SD_CS_PIN * 2));
    GPIOA->OSPEEDR |= (3UL << (BOARD_SD_CS_PIN * 2));
    BOARD_SD_CS_PORT->BSRR = (1UL << BOARD_SD_CS_PIN);
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
                                   RCC_AHB1ENR_GPIOCEN };
    GPIO_TypeDef *const base[] = { GPIOA, GPIOB, GPIOC };

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
                          ADC_CCR_ADCPRE_DIV4;       /* 96 / 4 = 24 MHz */
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
