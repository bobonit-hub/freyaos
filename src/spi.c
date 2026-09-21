/*
 * Freya - SPI1 master used by the SD card slot.
 *
 *   PA5 = SCK, PA6 = MISO, PA7 = MOSI, PA4 = CS (software driven)
 *
 * SPI1 lives on APB2 (96 MHz).  Card identification runs at /256
 * (375 kHz, inside the 100-400 kHz window the spec demands) and switches
 * to /8 (12 MHz) once the card is in data transfer mode.
 */
#include "freya.h"

#define CS_PORT     GPIOA
#define CS_PIN      4

void spi_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    /* PA5/PA6/PA7 -> AF5 (SPI1). */
    GPIOA->MODER   = (GPIOA->MODER & ~((3UL << 10) | (3UL << 12) | (3UL << 14))) |
                     ((2UL << 10) | (2UL << 12) | (2UL << 14));
    GPIOA->OTYPER &= ~((1UL << 5) | (1UL << 6) | (1UL << 7));
    GPIOA->OSPEEDR |= (3UL << 10) | (3UL << 12) | (3UL << 14);
    GPIOA->PUPDR   = (GPIOA->PUPDR & ~((3UL << 10) | (3UL << 12) | (3UL << 14))) |
                     (1UL << 12);                       /* pull-up on MISO */
    GPIOA->AFR[0]  = (GPIOA->AFR[0] & ~((0xFUL << 20) | (0xFUL << 24) | (0xFUL << 28))) |
                     ((5UL << 20) | (5UL << 24) | (5UL << 28));

    /* PA4 as a plain push-pull output, idle high. */
    GPIOA->MODER   = (GPIOA->MODER & ~(3UL << (CS_PIN * 2))) | (1UL << (CS_PIN * 2));
    GPIOA->OSPEEDR |= (3UL << (CS_PIN * 2));
    CS_PORT->BSRR = (1UL << CS_PIN);

    SPI1->CR1 = 0;
    SPI1->I2SCFGR = 0;
    SPI1->CR2 = 0;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                (7UL << SPI_CR1_BR_SHIFT);              /* mode 0, /256 */
    SPI1->CR1 |= SPI_CR1_SPE;
}

void spi_set_speed(int fast)
{
    uint32_t br = fast ? 2UL : 7UL;     /* /8 = 12 MHz, /256 = 375 kHz */

    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_BR_MASK) | (br << SPI_CR1_BR_SHIFT);
    SPI1->CR1 |= SPI_CR1_SPE;
}

void spi_cs(int low)
{
    if (low) CS_PORT->BSRR = (1UL << (CS_PIN + 16));
    else     CS_PORT->BSRR = (1UL << CS_PIN);
}

uint8_t spi_xfer(uint8_t v)
{
    while (!(SPI1->SR & SPI_SR_TXE)) { }
    SPI1->DR = v;
    while (!(SPI1->SR & SPI_SR_RXNE)) { }
    return (uint8_t)(SPI1->DR & 0xFF);
}

void spi_write(const uint8_t *buf, uint32_t len)
{
    while (len--) (void)spi_xfer(*buf++);
}

void spi_read(uint8_t *buf, uint32_t len)
{
    while (len--) *buf++ = spi_xfer(0xFF);
}
