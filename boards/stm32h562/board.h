/*
 * Freya - WeAct STM32H5 64-pin core board (STM32H562RGT6).
 *
 * Every board Freya runs on supplies a header with this name holding the
 * chip's register definitions, the few strings the shell prints, and the
 * declarations of the bring-up hooks the generic drivers call.  src/freya.h
 * includes it and the Makefile puts the right boards/<board> directory on
 * the include path.
 *
 * The board is https://github.com/WeActStudio/WeActStudio.STM32H5_64Pin_CoreBoard
 * with the STM32H562RGT6 fitted: a Cortex-M33 at 250 MHz in an LQFP64, an
 * 8 MHz crystal, a 32.768 kHz crystal, the LED on PB2, KEY on PC13 and a
 * microSD slot on the SDMMC1 pins.  There is no SPI NOR footprint.  The
 * H562 spends the pads the F4 parts of the same board call PB9 and PB11
 * on its VCAP capacitors, so neither pin exists here.  Otherwise the
 * header pins Freya uses on the other boards - the console on PA2/PA3,
 * PWM, I2C, SPI2 and the ESP32-C6 link - are free, so a program sees the
 * Black Pill's pin plan less those two pins.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#include "stm32h562.h"

/* ------------------------------------------------------------ identity */
#define BOARD_NAME          "WeAct STM32H562RGT6"
#define BOARD_MCU           "STM32H562RGT6"
#define BOARD_CORE          "ARM Cortex-M33F"
#define BOARD_HSE_NAME      "HSE 8 MHz crystal"
#define BOARD_HSI_NAME      "HSI 32 MHz oscillator"
#define BOARD_CONSOLE_NAME  "USART2 921600 8N1 on PA2/PA3"
#define BOARD_LED_NAME      "PB2"
#define BOARD_FLASH_WS      5

/* ------------------------------------------------------ internal flash */
/* 1 MiB in 8 KiB sectors, two banks of 512 KiB, quad-word programming.
 * The system settings own the sector at 0x0800C000, which holds nothing
 * else. */
#define BOARD_FLASH_KIB         1024U
#define BOARD_FLASH_PAGE_SIZE   8192U

/* ------------------------------------------------- the generation of IP */
/* Peripherals the H5 shares with the U5: src/ has a code path for each. */
#define BOARD_USART_ISR     1           /* ISR/ICR/RDR/TDR, not SR/DR    */
#define BOARD_SPI_FIFO      1           /* CFG1/CFG2, CSTART, TXDR/RXDR  */
#define BOARD_EXTI_SPLIT    1           /* RPR1/FPR1, one IRQ per line   */

/* ---------------------------------------------------------- the card */
/*
 * The microSD slot is wired for SDMMC1, which no SPI peripheral can reach,
 * so the card is driven in SPI mode by the board itself, bit by bit, on
 * the same pins: CS is DAT3 (PC11), SCK is CLK (PC12), MOSI is CMD (PD2)
 * and MISO is DAT0 (PC8).  sdspi_*() live in board.c.  The slot has no
 * supply switch, and the board no SPI flash, so SPI1 is left alone.
 */
#define BOARD_SD_BITBANG    1
#define BOARD_SD_CS_PORT    GPIOC
#define BOARD_SD_CS_PIN     11
#define BOARD_SPI_HAS_I2S   0           /* the FIFO SPI has no I2S part  */

/* The SPIs do not divide an APB clock here: their kernel clock is PLL1Q,
 * 100 MHz.  src/spi.c works a program's rates out from
 * BOARD_SPI_KERNEL_HZ instead of PCLK. */
#define BOARD_SPI_KERNEL_HZ 100000000UL

/* ------------------------------------------------- pins and interrupts */
/* Ports A to D; the LQFP64 bonds PD2 alone of port D.  Freya keeps the
 * console (PA2/PA3) and the card (PC8..PC12, PD2).  PB2 is the LED, which
 * a program may drive as a pin or through api->led(); PC13 is KEY, which
 * pulls the pin high when pressed. */
#define BOARD_PIN_PORTS     4               /* GPIOA, GPIOB, GPIOC, GPIOD */
#define BOARD_PIN_RESERVED  { 0x000CU, 0x0000U, 0x1F00U, 0x0004U }

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
 * channel 1..4, alternate function }.  The Black Pill's alternate
 * functions: AF1 for TIM2, AF2 for TIM3 and TIM4.  Its eighth pin, PB9,
 * is a VCAP pad on the H562, so TIM4 channel 4 is gone and PB5 drives
 * TIM3 channel 2 in its place.  None of them is a pin Freya keeps
 * (BOARD_PIN_RESERVED; 'make test' checks that).
 */
#define BOARD_PWM_MAP \
    { { FREYA_PA(0), 0, 1, 1 }, { FREYA_PA(1), 0, 2, 1 },  \
      { FREYA_PB(0), 1, 3, 2 }, { FREYA_PB(1), 1, 4, 2 },  \
      { FREYA_PB(6), 2, 1, 2 }, { FREYA_PB(7), 2, 2, 2 },  \
      { FREYA_PB(8), 2, 3, 2 }, { FREYA_PB(5), 1, 2, 2 } }

/* ----------------------------------------------- I2C a program may open */
/* Each bus is { SCL pin, SDA pin }.  The master drives them as open-drain
 * GPIO.  Bus 1 is PB6/PB7, the pair every board has.  PB11 and PB9, the
 * SDA of bus 2 on the other boards, are both VCAP pads on the H562, so
 * bus 2's SDA is PB3 here: free on the header, and nothing else's. */
#define BOARD_I2C_MAP \
    { { FREYA_PB(6), FREYA_PB(7) }, \
      { FREYA_PB(10), FREYA_PB(3) } }
#define BOARD_I2C_NAMES { "I2C1", "I2C2" }

/* ----------------------------------------------- SPI a program may open */
/* What a program gets is SPI2 on PB13..PB15, AF5:
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
 * way, as on the STM32H523.  The internal temperature sensor is channel
 * 16; Vref is 17. */
#define BOARD_ADC_MAP \
    { { FREYA_PA(0), 0 }, { FREYA_PA(1), 1 }, \
      { FREYA_PB(0), 9 }, { FREYA_PB(1), 5 }, \
      { FREYA_PC(0), 10 }, { FREYA_PC(1), 11 }, \
      { FREYA_PC(2), 12 }, { FREYA_PC(3), 13 }, \
      { FREYA_PC(4), 4 }, { FREYA_PC(5), 8 } }
#define BOARD_ADC_TEMP_CHANNEL  16
#define BOARD_ADC_VREF_CHANNEL  17

/* --------------------------------------------------------------- RTC */
/* The calendar RTC, on the 32.768 kHz crystal, for RTC=internal
 * (src/rtc.c): its base, where the H5 keeps CR, and what has to be turned on
 * before its registers and the backup domain can be written. */
#define BOARD_RTC_INTERNAL  1
#define BOARD_RTC_BASE      0x44007800UL
#define BOARD_RTC_CR_OFF    0x18
#ifdef FREYA_RTC_INTERNAL
static inline void board_rtc_access(void)
{
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN;
    (void)RCC->APB3ENR;
    PWR->DBPCR |= PWR_DBPCR_DBP;
}
#endif

/* --------------------------------------------------------------- hooks */
void board_clock_init(void);            /* clock tree, fills g_clocks    */
void board_uart_pins(void);             /* console pins and USART clock  */
void board_sd_power(int on);            /* the slot has no switch        */
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
