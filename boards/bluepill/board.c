/*
 * Freya - board support for the STM32F103C8T6 "Blue Pill":
 * clock tree, console and SD card pin mux, status LED.
 *
 * The F1 PLL has a single multiplier and no input divider, so the source
 * decides the result: the board's 8 MHz crystal times nine is 72 MHz, the
 * ceiling for this part.
 *
 *   HCLK = 72 MHz, APB1 = 36 MHz (its maximum), APB2 = 72 MHz
 *
 * Without a crystal the PLL can only be fed HSI/2 = 4 MHz and sixteen is
 * the largest multiplier, so the fallback runs at 64 MHz.  Nothing else in
 * the system needs changing for that: the SysTick reload, the USART
 * divisor and the microsecond delay all come from g_clocks.
 */
#include "freya.h"

#define HSE_SYSCLK_HZ   72000000UL      /* 8 MHz crystal   * 9  */
#define HSI_SYSCLK_HZ   64000000UL      /* HSI/2 = 4 MHz   * 16 */

#define LED_PORT        GPIOC
#define LED_PIN         13              /* active low on the Blue Pill */

/* -------------------------------------------------------------- clocks */
void board_clock_init(void)
{
    uint32_t timeout;
    uint32_t sysclk;
    uint32_t cfgr;
    int use_hse = 1;

    /* Flash: 2 wait states above 48 MHz, with the prefetch buffer on.
     * There is no data or instruction cache on this family. */
    FLASH_R->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY(BOARD_FLASH_WS);
    while ((FLASH_R->ACR & 0x7) != BOARD_FLASH_WS) { }

    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) { }

    /* Start from a known state in case a bootloader left the PLL running:
     * the multiplier and the PLL source can only be changed while it is
     * off, and it can only be turned off while SYSCLK is not driven by it. */
    RCC->CFGR &= ~RCC_CFGR_SW_MASK;
    while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != 0) { }
    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) { }

    RCC->CR |= RCC_CR_HSEON;
    for (timeout = 0; timeout < 2000000; timeout++)
        if (RCC->CR & RCC_CR_HSERDY) break;
    if (!(RCC->CR & RCC_CR_HSERDY)) {
        RCC->CR &= ~RCC_CR_HSEON;
        use_hse = 0;
    }

    /* APB1 has to stay at or below 36 MHz, hence the /2 prescaler; the ADC
     * prescaler is set to a legal value even though Freya never uses it. */
    cfgr = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1 |
           RCC_CFGR_ADCPRE_DIV6;

    if (use_hse) {
        sysclk = HSE_SYSCLK_HZ;
        cfgr |= RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLXTPRE_DIV1 | RCC_CFGR_PLLMUL(9);
    } else {
        sysclk = HSI_SYSCLK_HZ;
        cfgr |= RCC_CFGR_PLLMUL(16);        /* PLLSRC = 0: HSI/2 */
    }

    /* The prescalers must be valid before SYSCLK ramps up. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_CLKMASK) | cfgr;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) { }

    if (use_hse)
        RCC->CR |= RCC_CR_CSSON;        /* trap a dying crystal */

    g_clocks.clock_source = (uint8_t)use_hse;
    g_clocks.sysclk_hz = sysclk;
    g_clocks.hclk_hz   = sysclk;
    g_clocks.pclk1_hz  = sysclk / 2;
    g_clocks.pclk2_hz  = sysclk;
}

/* ---------------------------------------------------------- console pins */
void board_uart_pins(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

    /* USART2 is on PA2/PA3 with no remapping. TX is driven by the
     * peripheral, RX is an input held high so an unplugged adapter does
     * not look like a stream of break characters. */
    gpio_config(GPIOA, 2, GPIO_AF_PP_50M);
    GPIOA->BSRR = (1UL << 3);                   /* pull-up before input */
    gpio_config(GPIOA, 3, GPIO_IN_PULL);
}

/* ---------------------------------------------------------- SD card pins */
void board_spi_pins(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    /* PA5 = SCK, PA7 = MOSI driven by SPI1; PA6 = MISO is an input with a
     * pull-up so the bus reads as idle while no card answers. */
    gpio_config(GPIOA, 5, GPIO_AF_PP_50M);
    GPIOA->BSRR = (1UL << 6);
    gpio_config(GPIOA, 6, GPIO_IN_PULL);
    gpio_config(GPIOA, 7, GPIO_AF_PP_50M);

    /* PA4 as a plain push-pull output; raise it before it becomes one so
     * the card never sees a chip select glitch. */
    BOARD_SD_CS_PORT->BSRR = (1UL << BOARD_SD_CS_PIN);
    gpio_config(BOARD_SD_CS_PORT, BOARD_SD_CS_PIN, GPIO_OUT_PP_50M);
}

/* ----------------------------------------------------------------- LED */
void led_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    (void)RCC->APB2ENR;

    /* PC13 is fed from the backup domain pad and may only be driven at
     * 2 MHz with a couple of milliamps - which is all the LED needs. */
    LED_PORT->BSRR = (1UL << LED_PIN);          /* off before it drives */
    gpio_config(LED_PORT, LED_PIN, GPIO_OUT_PP_2M);
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
