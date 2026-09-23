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
/* 512 KiB, well above the 128 KiB every supported board has.  The F4
 * erases in unequal sectors (16/16/16/16/64/128/128/128 KiB) and programs
 * 32-bit words.  BOARD_FLASH_PAGE_SIZE is the program region's erase unit
 * (sector 4), used only for install progress; the driver walks the real
 * sector map. */
#define BOARD_FLASH_KIB         512U
#define BOARD_FLASH_PAGE_SIZE   (64U * 1024U)

/* ---------------------------------------------------------- SD on SPI1 */
#define BOARD_SD_CS_PORT    GPIOA
#define BOARD_SD_CS_PIN     4
#define BOARD_SPI_HAS_I2S   1           /* SPI1 has the I2S registers    */

/* PA8 is the gate of a P-channel MOSFET that feeds the socket.  Low
 * applies VDD.  A pull-down on the gate keeps the card powered while
 * the pin is still an input, which is how reset leaves it. */
#define BOARD_SD_PWR_PORT   GPIOA
#define BOARD_SD_PWR_PIN    8
#define BOARD_SD_PWR_ON     0

/* Card identification has to sit in the 100-400 kHz window; the data rate
 * is whatever the card and the wiring stand.  PCLK2 is 96 MHz here. */
#define BOARD_SPI_BR_SLOW   7           /* /256 = 375 kHz                */
#define BOARD_SPI_BR_FAST   2           /* /8   =  12 MHz                */

/* ------------------------------------------------- pins and interrupts */
/* The ports a program may name, and within them the pins Freya keeps for
 * itself: the console on PA2/PA3, the card on PA4..PA7 and the socket's
 * power switch on PA8.  PC13 is the LED, which a program may drive as a
 * pin or through api->led(). */
#define BOARD_PIN_PORTS     3                       /* GPIOA, GPIOB, GPIOC */
#define BOARD_PIN_RESERVED  { 0x01FCU, 0x0000U, 0x0000U }

/* ------------------------------------------- timers a program may open */
/* TIM2..TIM4, all on APB1 and all clocked at twice PCLK1 because the
 * prescaler is not 1.  Each entry is { registers, IRQ, APB1ENR bit } in
 * the order src/timer.c hands them out and names their handlers.  TIM1,
 * TIM5 and TIM9..TIM11 are left alone. */
#define BOARD_TIMER_LIST \
    { { TIM2, TIM2_IRQn, RCC_APB1ENR_TIM2EN }, \
      { TIM3, TIM3_IRQn, RCC_APB1ENR_TIM3EN }, \
      { TIM4, TIM4_IRQn, RCC_APB1ENR_TIM4EN } }
#define BOARD_TIMER_COUNT   3
#define BOARD_TIMER_NAMES   { "TIM2", "TIM3", "TIM4" }

/* ------------------------------------------- PWM outputs a program may open */
/*
 * The pins those timers can drive: { pin, timer index in the list above,
 * channel 1..4, alternate function }.  The F4 reaches several pins per
 * channel and the F1 reaches one, so the list is the intersection - the
 * eight pins that mean the same thing on both boards with no remapping,
 * and none of them a pin Freya keeps (BOARD_PIN_RESERVED; 'make test'
 * checks that).  PA0 is also the Black Pill's KEY button.
 */
#define BOARD_PWM_MAP \
    { { FREYA_PA(0), 0, 1, 1 }, { FREYA_PA(1), 0, 2, 1 },  \
      { FREYA_PB(0), 1, 3, 2 }, { FREYA_PB(1), 1, 4, 2 },  \
      { FREYA_PB(6), 2, 1, 2 }, { FREYA_PB(7), 2, 2, 2 },  \
      { FREYA_PB(8), 2, 3, 2 }, { FREYA_PB(9), 2, 4, 2 } }

/* ----------------------------------------------- I2C a program may open */
/* Each bus is { SCL pin, SDA pin }.  The master drives them as open-drain
 * GPIO.  Bus 1 is PB6/PB7, the pair every board has.  This package does
 * not bond PB11, so bus 2's SDA is PB9 instead of the Blue Pill's PB11;
 * PB9 is also a PWM pin, and can be only one. */
#define BOARD_I2C_MAP \
    { { FREYA_PB(6), FREYA_PB(7) }, \
      { FREYA_PB(10), FREYA_PB(9) } }
#define BOARD_I2C_NAMES { "I2C1", "I2C2" }

/* ----------------------------------------------- SPI a program may open */
/* The card keeps SPI1.  What a program gets is SPI2, the controller both
 * boards bond to the same three pins: { regs, APB number, SCK, MISO,
 * MOSI, alternate function }.  The F1 has no alternate function number
 * and ignores the last field.  Chip select is not in the map; a program
 * drives that pin itself. */
#define BOARD_SPI_MAP \
    { { SPI2, 1, FREYA_PB(13), FREYA_PB(14), FREYA_PB(15), 5 } }
#define BOARD_SPI_NAMES { "SPI2" }

/* --------------------------------------------------------------- hooks */
void board_clock_init(void);            /* clock tree, fills g_clocks    */
void board_uart_pins(void);             /* console pins and USART clock  */
void board_spi_pins(void);              /* SD card pins, SPI and CS      */
void board_sd_power(int on);            /* socket VDD, through PA8       */
void board_spi_mux(SPI_TypeDef *spi, int sck, int miso, int mosi, int af);

/* Pins for programs: the register layout is the chip's, so the generic
 * driver in src/gpio.c asks the board to configure and to route. */
GPIO_TypeDef *board_gpio_port(int port);          /* NULL: no such port  */
void          board_pin_mode(GPIO_TypeDef *port, int pin, int mode);
void          board_exti_select(int port, int pin);
void          board_pin_af(GPIO_TypeDef *port, int pin, int af);  /* PWM  */

/* led_init(), led_set() and led_toggle() are declared in freya.h and
 * implemented per board. */

#endif /* FREYA_BOARD_H */
