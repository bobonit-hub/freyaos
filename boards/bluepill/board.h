/*
 * Freya - STM32F103C8T6 "Blue Pill".
 *
 * Every board Freya runs on supplies a header with this name holding the
 * chip's register definitions, the few strings the shell prints, and the
 * declarations of the bring-up hooks the generic drivers call.  src/freya.h
 * includes it and the Makefile puts the right boards/<board> directory on
 * the include path.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#include "stm32f103.h"

/* ------------------------------------------------------------ identity */
#define BOARD_NAME          "STM32F103C8T6 \"Blue Pill\""
#define BOARD_MCU           "STM32F103C8T6"
#define BOARD_CORE          "ARM Cortex-M3"
#define BOARD_HSE_NAME      "HSE 8 MHz crystal"
#define BOARD_HSI_NAME      "HSI 8 MHz oscillator"
#define BOARD_CONSOLE_NAME  "USART2 921600 8N1 on PA2/PA3"
#define BOARD_LED_NAME      "PC13"
#define BOARD_FLASH_WS      2

/* ------------------------------------------------------ internal flash */
/* The F1 erases in 1 KiB pages and programs halfwords.  The auto-start
 * slot and the program image share page 39, so flash_erase() restores
 * whichever of the two a write did not cover. */
#define BOARD_FLASH_PAGE_SIZE   1024U

/* ---------------------------------------------------------- SD on SPI1 */
#define BOARD_SD_CS_PORT    GPIOA
#define BOARD_SD_CS_PIN     4
#define BOARD_SPI_HAS_I2S   0           /* no I2S on SPI1 of this line   */

/* Card identification has to sit in the 100-400 kHz window; the data rate
 * is whatever the card and the wiring stand.  PCLK2 is 72 MHz here. */
#define BOARD_SPI_BR_SLOW   7           /* /256 = 281 kHz                */
#define BOARD_SPI_BR_FAST   2           /* /8   =   9 MHz                */

/* --------------------------------------------------------------- hooks */
void board_clock_init(void);            /* clock tree, fills g_clocks    */
void board_uart_pins(void);             /* console pins and USART clock  */
void board_spi_pins(void);              /* SD card pins, SPI and CS      */

/* led_init(), led_set() and led_toggle() are declared in freya.h and
 * implemented per board. */

#endif /* FREYA_BOARD_H */
