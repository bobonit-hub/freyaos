/*
 * Freya - SPI1 master used by the SD card slot.
 *
 * The board wires SCK, MISO, MOSI and the software driven chip select in
 * board_spi_pins() and names the two bus dividers: card identification has
 * to run inside the 100-400 kHz window the spec demands, and the bus moves
 * up to BOARD_SPI_BR_FAST once the card is in data transfer mode.
 */
#include "freya.h"

#define CS_PORT     BOARD_SD_CS_PORT
#define CS_PIN      BOARD_SD_CS_PIN

void spi_init(void)
{
    board_spi_pins();

    SPI1->CR1 = 0;
#if BOARD_SPI_HAS_I2S
    SPI1->I2SCFGR = 0;                  /* SPI mode, not I2S */
#endif
    SPI1->CR2 = 0;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                (BOARD_SPI_BR_SLOW << SPI_CR1_BR_SHIFT);   /* mode 0 */
    SPI1->CR1 |= SPI_CR1_SPE;
}

void spi_set_speed(int fast)
{
    uint32_t br = fast ? (uint32_t)BOARD_SPI_BR_FAST : (uint32_t)BOARD_SPI_BR_SLOW;

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
