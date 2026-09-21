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

/* ---------------------------------------------------------- SD card pins */
void board_spi_pins(void)
{
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
