/*
 * Freya - WeAct AT32F403ACGU7 "Black Pill 2".
 *
 * Every board Freya runs on supplies a header with this name holding the
 * chip's register definitions, the few strings the shell prints, and the
 * declarations of the bring-up hooks the generic drivers call.  src/freya.h
 * includes it and the Makefile puts the right boards/<board> directory on
 * the include path.
 *
 * The board is WeAct's Black Pill outline and pinout with an Artery
 * AT32F403A on it: https://github.com/WeActStudio/WeActStudio.BlackPill.
 * The chip's peripherals are the STM32F103's, so the pin plan is the Blue
 * Pill's; the core is a Cortex-M4F, so the feature set is the F4 boards'.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#include "at32f403a.h"

/* ------------------------------------------------------------ identity */
#define BOARD_NAME          "WeAct AT32F403ACGU7 \"Black Pill 2\""
#define BOARD_MCU           "AT32F403ACGU7"
#define BOARD_CORE          "ARM Cortex-M4F"
#define BOARD_HSE_NAME      "HEXT 8 MHz crystal"
#define BOARD_HSI_NAME      "HICK 8 MHz oscillator"
#define BOARD_CONSOLE_NAME  "USART2 921600 8N1 on PA2/PA3"
#define BOARD_LED_NAME      "PC13"
#define BOARD_FLASH_WS      0           /* none to set; see at32f403a.h  */

/* ------------------------------------------------------ internal flash */
/* 1 MiB in 2 KiB pages, in two banks of 512 KiB, halfword programming.
 * The first 256 KiB read with no wait states and the rest is slower,
 * which is why the kernel extension sits there and not at the top.
 * System settings own the page at 0x0800C000, which holds nothing else. */
#define BOARD_FLASH_KIB         1024U
#define BOARD_FLASH_PAGE_SIZE   2048U

/* ---------------------------------------------------------- SD on SPI1 */
#define BOARD_SD_CS_PORT    GPIOA
#define BOARD_SD_CS_PIN     4
#define BOARD_SPI_HAS_I2S   0           /* SPI1's I2S is not used here   */

/* PA8 is the gate of a P-channel MOSFET that feeds the socket.  Low
 * applies VDD.  A pull-down on the gate keeps the card powered while
 * the pin is still an input, which is how reset leaves it.  The board's
 * empty SOP-8 footprint also uses PA8, as its chip select: fit the card
 * socket or a flash chip, not both. */
#define BOARD_SD_PWR_PORT   GPIOA
#define BOARD_SD_PWR_PIN    8
#define BOARD_SD_PWR_ON     0

/* Card identification has to sit in the 100-400 kHz window; the data rate
 * is whatever the card and the wiring stand.  PCLK2 is 60 MHz here, kept
 * at a quarter of HCLK so that /256 is still inside that window. */
#define BOARD_SPI_BR_SLOW   7           /* /256 = 234 kHz                */
#define BOARD_SPI_BR_FAST   1           /* /4   =  15 MHz                */

/* ------------------------------------------------- pins and interrupts */
/* The ports a program may name, and within them the pins Freya keeps for
 * itself: the console on PA2/PA3, the card on PA4..PA7 and the socket's
 * power switch on PA8.  PC13 is the LED, which a program may drive as a
 * pin or through api->led(). */
#define BOARD_PIN_PORTS     3                       /* GPIOA, GPIOB, GPIOC */
#define BOARD_PIN_RESERVED  { 0x01FCU, 0x0000U, 0x0000U }

/* ------------------------------------------- timers a program may open */
/* TIM2..TIM4, all on APB1 and all clocked at twice PCLK1 (240 MHz)
 * because the prescaler is not 1.  Each entry is { registers, IRQ,
 * APB1ENR bit } in the order src/timer.c hands them out and names their
 * handlers.  TIM1 and the rest are left alone. */
#define BOARD_TIMER_LIST \
    { { TIM2, TIM2_IRQn, RCC_APB1ENR_TIM2EN }, \
      { TIM3, TIM3_IRQn, RCC_APB1ENR_TIM3EN }, \
      { TIM4, TIM4_IRQn, RCC_APB1ENR_TIM4EN } }
#define BOARD_TIMER_COUNT   3
#define BOARD_TIMER_NAMES   { "TIM2", "TIM3", "TIM4" }

/* ------------------------------------------- PWM outputs a program may open */
/*
 * The pins those timers can drive: { pin, timer index in the list above,
 * channel 1..4, alternate function }.  As on the F103 a pin belongs to one
 * peripheral and there are no alternate function numbers, so the last
 * field is zero and these are the default mappings.  They are the same
 * eight pins as on the other boards, and none of them is a pin Freya
 * keeps (BOARD_PIN_RESERVED; 'make test' checks that).  PA0 is also the
 * board's KEY button.
 */
#define BOARD_PWM_MAP \
    { { FREYA_PA(0), 0, 1, 0 }, { FREYA_PA(1), 0, 2, 0 },  \
      { FREYA_PB(0), 1, 3, 0 }, { FREYA_PB(1), 1, 4, 0 },  \
      { FREYA_PB(6), 2, 1, 0 }, { FREYA_PB(7), 2, 2, 0 },  \
      { FREYA_PB(8), 2, 3, 0 }, { FREYA_PB(9), 2, 4, 0 } }

/* ----------------------------------------------- I2C a program may open */
/* Each bus is { SCL pin, SDA pin }.  The master drives them as open-drain
 * GPIO.  Bus 1 is PB6/PB7, the pair every board has, and bus 2 is
 * PB10/PB11 as on the Blue Pill: this package bonds PB11.  An output
 * cannot turn its pull-up on, so the lines need external resistors. */
#define BOARD_I2C_MAP \
    { { FREYA_PB(6), FREYA_PB(7) }, \
      { FREYA_PB(10), FREYA_PB(11) } }
#define BOARD_I2C_NAMES { "I2C1", "I2C2" }

/* ----------------------------------------------- SPI a program may open */
/* The card keeps SPI1.  What a program gets is SPI2 on PB13..PB15 with no
 * remap: { regs, APB number, SCK, MISO, MOSI, alternate function }.  This
 * chip has no alternate function number and ignores the last field.
 * Chip select is not in the map; a program drives that pin itself. */
#define BOARD_SPI_MAP \
    { { SPI2, 1, FREYA_PB(13), FREYA_PB(14), FREYA_PB(15), 5 } }
#define BOARD_SPI_NAMES { "SPI2" }

/* ESP32-C6 network coprocessor on SPI2, wired as on the Black Pill.  The
 * transfers use DMA1 channels 4 and 5.  SPI2 is returned to the public
 * SPI API when the link is closed. */
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
#define BOARD_ESP_DMA_CHANNELS 1        /* F103 channels, not F4 streams */
/* PCLK1 is 120 MHz, so /2 would be 60 MHz; /8 is 15 MHz, below the
 * 21-24 MHz the F4 boards run the link at. */
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
/* ADC1 channels common to the supported boards and not kept by Freya.
 * The internal temperature sensor is channel 16, as on the F103; Vref
 * is 17. */
#define BOARD_ADC_MAP \
    { { FREYA_PA(0), 0 }, { FREYA_PA(1), 1 }, \
      { FREYA_PB(0), 8 }, { FREYA_PB(1), 9 }, \
      { FREYA_PC(0), 10 }, { FREYA_PC(1), 11 }, \
      { FREYA_PC(2), 12 }, { FREYA_PC(3), 13 }, \
      { FREYA_PC(4), 14 }, { FREYA_PC(5), 15 } }
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
