/*
 * Freya - WeAct STM32F411CEU6 "Black Pill".
 *
 * Every board Freya runs on supplies a header with this name holding the
 * chip's register definitions, the few strings the shell prints, and the
 * declarations of the bring-up hooks the generic drivers call.  src/freya.h
 * includes it and the Makefile puts the right boards/<board> directory on
 * the include path.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#include "stm32f411.h"

/* ------------------------------------------------------------ identity */
#define BOARD_NAME          "WeAct STM32F411CEU6 \"Black Pill\""
#define BOARD_MCU           "STM32F411CEU6"
#define BOARD_CORE          "ARM Cortex-M4F"
#define BOARD_HSE_NAME      "HSE 25 MHz crystal"
#define BOARD_HSI_NAME      "HSI 16 MHz oscillator"
#define BOARD_CONSOLE_NAME  "USART2 921600 8N1 on PA2/PA3"
#define BOARD_LED_NAME      "PC13"
#define BOARD_FLASH_WS      3

/* ------------------------------------------------------ internal flash */
/* The F4 erases in unequal sectors (16/16/16/16/64/128/128/128 KiB) and
 * programs 32-bit words.  BOARD_FLASH_PAGE_SIZE is the program region's
 * erase unit (sector 4), used only for install progress; the driver walks
 * the real sector map. */
#define BOARD_FLASH_PAGE_SIZE   (64U * 1024U)

/* ---------------------------------------------------------- SD on SPI1 */
#define BOARD_SD_CS_PORT    GPIOA
#define BOARD_SD_CS_PIN     4
#define BOARD_SPI_HAS_I2S   1           /* SPI1 has the I2S registers    */

/* Card identification has to sit in the 100-400 kHz window; the data rate
 * is whatever the card and the wiring stand.  PCLK2 is 96 MHz here. */
#define BOARD_SPI_BR_SLOW   7           /* /256 = 375 kHz                */
#define BOARD_SPI_BR_FAST   2           /* /8   =  12 MHz                */

/* --------------------------------------------------------------- hooks */
void board_clock_init(void);            /* clock tree, fills g_clocks    */
void board_uart_pins(void);             /* console pins and USART clock  */
void board_spi_pins(void);              /* SD card pins, SPI and CS      */

/* led_init(), led_set() and led_toggle() are declared in freya.h and
 * implemented per board. */

#endif /* FREYA_BOARD_H */
