/*
 * Freya - WeAct STM32H523CET6 core board.
 *
 * Every board Freya runs on supplies a header with this name holding the
 * chip's register definitions, the few strings the shell prints, and the
 * declarations of the bring-up hooks the generic drivers call.  src/freya.h
 * includes it and the Makefile puts the right boards/<board> directory on
 * the include path.
 *
 * The board is https://github.com/WeActStudio/WeActStudio.STM32H523CoreBoard,
 * the LQFP48 (CxTx) one: the Black Pill's pinout with a Cortex-M33 at
 * 250 MHz on it, an 8 MHz crystal, the LED on PC13, KEY on PA0 and an
 * empty SOP-8 footprint for a SPI NOR chip on SPI1 (PA4..PA7).  So the
 * pin plan is the Black Pill's.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#include "stm32h523.h"

/* ------------------------------------------------------------ identity */
#define BOARD_NAME          "WeAct STM32H523CET6"
#define BOARD_MCU           "STM32H523CET6"
#define BOARD_CORE          "ARM Cortex-M33F"
#define BOARD_HSE_NAME      "HSE 8 MHz crystal"
#define BOARD_HSI_NAME      "HSI 32 MHz oscillator"
#define BOARD_CONSOLE_NAME  "USART2 921600 8N1 on PA2/PA3"
#define BOARD_LED_NAME      "PC13"
#define BOARD_FLASH_WS      5

/* ------------------------------------------------------ internal flash */
/* 512 KiB in 8 KiB sectors, two banks of 256 KiB, quad-word programming.
 * The system settings own the sector at 0x0800C000, which holds nothing
 * else. */
#define BOARD_FLASH_KIB         512U
#define BOARD_FLASH_PAGE_SIZE   8192U

/* ------------------------------------------------- the generation of IP */
/* Peripherals the H5 shares with the U5: src/ has a code path for each. */
#define BOARD_USART_ISR     1           /* ISR/ICR/RDR/TDR, not SR/DR    */
#define BOARD_SPI_FIFO      1           /* CFG1/CFG2, CSTART, TXDR/RXDR  */
#define BOARD_EXTI_SPLIT    1           /* RPR1/FPR1, one IRQ per line   */

/* ---------------------------------------------------------- SD on SPI1 */
#define BOARD_SD_CS_PORT    GPIOA
#define BOARD_SD_CS_PIN     4
#define BOARD_SPI_HAS_I2S   0           /* the FIFO SPI has no I2S part  */

/* PA8 is the gate of a P-channel MOSFET that feeds the socket.  Low
 * applies VDD.  A pull-down on the gate keeps the card powered while
 * the pin is still an input, which is how reset leaves it. */
#define BOARD_SD_PWR_PORT   GPIOA
#define BOARD_SD_PWR_PIN    8
#define BOARD_SD_PWR_ON     0

/* Card identification has to sit in the 100-400 kHz window; the data rate
 * is whatever the card and the wiring stand.  The SPIs do not divide an
 * APB clock here: their kernel clock is PLL1Q, set to 100 MHz so that /256
 * is inside that window.  src/spi.c works a program's rates out from
 * BOARD_SPI_KERNEL_HZ instead of PCLK. */
#define BOARD_SPI_KERNEL_HZ 100000000UL
#define BOARD_SPI_BR_SLOW   7           /* /256 = 390.6 kHz              */
#define BOARD_SPI_BR_FAST   2           /* /8   =  12.5 MHz              */

/* The SOP-8 footprint on the back is a SPI NOR chip on SPI1, chip select
 * PA4 like the card: fit one or the other.  It is a LittleFS volume at
 * /spi1, as on the Black Pill. */
#define BOARD_SPIFLASH      1

/* ------------------------------------------------- pins and interrupts */
/* The ports a program may name, and within them the pins Freya keeps for
 * itself: the console on PA2/PA3, the card on PA4..PA7 and the socket's
 * power switch on PA8.  PC13 is the LED, which a program may drive as a
 * pin or through api->led(). */
#define BOARD_PIN_PORTS     3                       /* GPIOA, GPIOB, GPIOC */
#define BOARD_PIN_RESERVED  { 0x01FCU, 0x0000U, 0x0000U }

/* ------------------------------------------- timers a program may open */
/* TIM2..TIM4, all on APB1 and clocked at PCLK1 (250 MHz) because the
 * prescaler is 1.  Each entry is { registers, IRQ, APB1LENR bit } in the
 * order src/timer.c hands them out and names their handlers.  TIM1, TIM5
 * and the rest are left alone.  At 250 MHz the longest period that fits
 * the 32-bit tick count is 17.1 s. */
#define BOARD_TIMER_LIST \
    { { TIM2, TIM2_IRQn, RCC_APB1ENR_TIM2EN }, \
      { TIM3, TIM3_IRQn, RCC_APB1ENR_TIM3EN }, \
      { TIM4, TIM4_IRQn, RCC_APB1ENR_TIM4EN } }
#define BOARD_TIMER_COUNT   3
#define BOARD_TIMER_NAMES   { "TIM2", "TIM3", "TIM4" }

/* ------------------------------------------- PWM outputs a program may open */
/*
 * The pins those timers can drive: { pin, timer index in the list above,
 * channel 1..4, alternate function }.  The same eight pins and the same
 * alternate functions as the Black Pill: AF1 for TIM2, AF2 for TIM3 and
 * TIM4, as on the H5 as well.  None of them is a pin Freya keeps (BOARD_PIN_RESERVED; 'make
 * test' checks that).  PA0 is also the board's KEY button.
 */
#define BOARD_PWM_MAP \
    { { FREYA_PA(0), 0, 1, 1 }, { FREYA_PA(1), 0, 2, 1 },  \
      { FREYA_PB(0), 1, 3, 2 }, { FREYA_PB(1), 1, 4, 2 },  \
      { FREYA_PB(6), 2, 1, 2 }, { FREYA_PB(7), 2, 2, 2 },  \
      { FREYA_PB(8), 2, 3, 2 }, { FREYA_PB(9), 2, 4, 2 } }

/* ----------------------------------------------- I2C a program may open */
/* Each bus is { SCL pin, SDA pin }.  The master drives them as open-drain
 * GPIO.  Bus 1 is PB6/PB7, the pair every board has.  The LQFP48, like
 * the F411's package, does not bond PB11, so bus 2's SDA is PB9, as on the Black
 * Pill; PB9 is also a PWM pin, and can be only one. */
#define BOARD_I2C_MAP \
    { { FREYA_PB(6), FREYA_PB(7) }, \
      { FREYA_PB(10), FREYA_PB(9) } }
#define BOARD_I2C_NAMES { "I2C1", "I2C2" }

/* ----------------------------------------------- SPI a program may open */
/* The card keeps SPI1.  What a program gets is SPI2 on PB13..PB15, AF5:
 * { regs, APB number, SCK, MISO, MOSI, alternate function }.  Chip select
 * is not in the map; a program drives that pin itself. */
#define BOARD_SPI_MAP \
    { { SPI2, 1, FREYA_PB(13), FREYA_PB(14), FREYA_PB(15), 5 } }
#define BOARD_SPI_NAMES { "SPI2" }

/* ESP32-C6 network coprocessor on SPI2, wired as on the Black Pill.  The
 * transfers use GPDMA1 channels 0 (receive) and 1 (transmit).  SPI2 is
 * returned to the public SPI API when the link is closed. */
#define BOARD_NET_SUPPORTED  1
#define BOARD_NET_SPI        SPI2
#define BOARD_NET_SPI_BUS    1
#define BOARD_NET_SCK        FREYA_PB(13)
#define BOARD_NET_MISO       FREYA_PB(14)
#define BOARD_NET_MOSI       FREYA_PB(15)
#define BOARD_NET_CS         FREYA_PB(12)
#define BOARD_NET_READY      FREYA_PB(10)
#define BOARD_NET_SPI_AF     5
#define BOARD_NET_SPI_BR     2
#define BOARD_ESP_LINK       1
#define BOARD_ESP_CS         BOARD_NET_CS
#define BOARD_ESP_READY      BOARD_NET_READY
#define BOARD_ESP_SPI_AF     BOARD_NET_SPI_AF
#define BOARD_ESP_GPDMA      1          /* GPDMA channels, not streams   */
/* The SPI kernel clock is 100 MHz: /8 is 12.5 MHz, the next tap up
 * (25 MHz) is past the 21-24 MHz of the F4s. */
#define BOARD_ESP_SPI_BR     2

/* heatshrink LZSS in the kernel extension: compress() and decompress(). */
#define BOARD_COMPRESS       1

/* Ascon-AEAD128 in the kernel extension.  The key comes from the PC. */
#define BOARD_AEAD           1

/* The 32-bit PDP-11 in the kernel extension: vm_reset(), vm_step()
 * and vm_run(). */
#define BOARD_VM             1

/* The civil clock in the program table: rtc_get() and rtc_set(). */
#define BOARD_RTC_API        1

/* Float values in the shell: 3.5, float(), sin(), cos() and pi(). */
#define BOARD_SHELL_FLOAT    1

/* --------------------------------------------------------------- ADC */
/* ADC1 channels on the same pins as the other boards, numbered the H5's
 * way.  The internal temperature sensor is channel 16; Vref is 17. */
#define BOARD_ADC_MAP \
    { { FREYA_PA(0), 0 }, { FREYA_PA(1), 1 }, \
      { FREYA_PB(0), 9 }, { FREYA_PB(1), 5 }, \
      { FREYA_PC(0), 10 }, { FREYA_PC(1), 11 }, \
      { FREYA_PC(2), 12 }, { FREYA_PC(3), 13 }, \
      { FREYA_PC(4), 4 }, { FREYA_PC(5), 8 } }
#define BOARD_ADC_TEMP_CHANNEL  16
#define BOARD_ADC_VREF_CHANNEL  17

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
int           board_adc_read(int channel);

/* led_init(), led_set() and led_toggle() are declared in freya.h and
 * implemented per board. */

#endif /* FREYA_BOARD_H */
