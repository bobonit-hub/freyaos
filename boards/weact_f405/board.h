/*
 * Freya - WeAct STM32F4 64-pin core board, with the STM32F405RGT6.
 *
 * Every board Freya runs on supplies a header with this name holding the
 * chip's register definitions, the few strings the shell prints, and the
 * declarations of the bring-up hooks the generic drivers call.  src/freya.h
 * includes it and the Makefile puts the right boards/<board> directory on
 * the include path.
 *
 * The board is https://github.com/WeActStudio/WeActStudio.STM32F4_64Pin_CoreBoard
 * (schematic V1.1) with the STM32F405RGT6 fitted: an 8 MHz crystal, a
 * 32.768 kHz crystal, the LED on PB2, KEY on PC13, USB-C on PA11/PA12 and
 * a microSD slot on the SDIO pins, always powered, its card-detect switch
 * on PA8 through 10 kOhm.  It is the STM32F405 board in everything but the
 * pins of the card and the LED: the same clock tree, memory map and
 * console on PA2/PA3, so freya_api.h treats FREYA_BOARD_WEACT_F405 as
 * FREYA_BOARD_STM32F405.  On the F405 fitting PB9 and PB11 are plain
 * pins, not VCAP, so a program sees the Black Pill's pin plan.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#include "stm32f405.h"

/* ------------------------------------------------------------ identity */
#define BOARD_NAME          "WeAct STM32F4 64-pin"
#define BOARD_MCU           "STM32F405RGT6"
#define BOARD_CORE          "ARM Cortex-M4F"
#define BOARD_HSE_NAME      "HSE 8 MHz crystal"
#define BOARD_HSI_NAME      "HSI 16 MHz oscillator"
#define BOARD_CONSOLE_NAME  "USART2 921600 8N1 on PA2/PA3"
#define BOARD_LED_NAME      "PB2"
#define BOARD_FLASH_WS      5

/* ------------------------------------------------------ internal flash */
/* Every STM32F405 has 1 MiB, twelve sectors; the kernel extension is the
 * last of them, so the map needs all of it.  The F4 erases in unequal
 * sectors (16/16/16/16/64, then 128 KiB) and programs 32-bit words.
 * BOARD_FLASH_PAGE_SIZE is the erase unit of the program region's first
 * sector (4), used for install progress and as the block of the shell's
 * flash_write(); the driver walks the real sector map, and the two blocks
 * inside each 128 KiB sector share an erase. */
#define BOARD_FLASH_KIB         1024U
#define BOARD_FLASH_PAGE_SIZE   (64U * 1024U)

/* ------------------------------------------------------- SD on the SDIO pins */
/*
 * The microSD slot is wired for the SDIO peripheral, which no SPI
 * peripheral can reach, so the card is driven in SPI mode by the board
 * itself, bit by bit, on the same pins, as on the STM32H562 board of the
 * same family: CS is DAT3 (PC11), SCK is CLK (PC12), MOSI is CMD (PD2)
 * and MISO is DAT0 (PC8); DAT1 (PC9) and DAT2 (PC10) are held up.
 * sdspi_*() live in board.c.  The slot has no supply switch, and the
 * board no SPI flash, so SPI1 is left alone.  PA8, the card-detect
 * switch, is not read: a missing card simply does not answer.
 */
#define BOARD_SD_BITBANG    1
#define BOARD_SD_CS_PORT    GPIOC
#define BOARD_SD_CS_PIN     11
#define BOARD_SPI_HAS_I2S   1           /* SPI1 has the I2S registers    */

/* ------------------------------------------------- pins and interrupts */
/* Ports A to D; the LQFP64 bonds PD2 alone of port D.  Freya keeps the
 * console (PA2/PA3) and the card (PC8..PC12, PD2).  PB2 is the LED, which
 * a program may drive as a pin or through api->led(); PC13 is KEY, which
 * pulls the pin high when pressed; PA8 reads the card-detect switch. */
#define BOARD_PIN_PORTS     4               /* GPIOA, GPIOB, GPIOC, GPIOD */
#ifdef FREYA_USB
/* USB=1 also keeps PA11 and PA12, the USB socket's D- and D+. */
#define BOARD_PIN_RESERVED  { 0x180CU, 0x0000U, 0x1F00U, 0x0004U }
#else
#define BOARD_PIN_RESERVED  { 0x000CU, 0x0000U, 0x1F00U, 0x0004U }
#endif

/* ------------------------------------------------------------ USB host */
/* USB=1: OTG_FS as the host, on PA11/PA12 (AF10), the board's
 * USB socket.  src/usbh.c drives it; board_usb_init() clocks it.  The
 * F4's core is the older one, with NOVBUSSENS in GCCFG. */
#define BOARD_USB_OTG_BASE  0x50000000UL
#define BOARD_USB_OTG_V1    1
#define BOARD_USB_IRQn      67
#define BOARD_USB_IRQ_HANDLER OTG_FS_IRQHandler

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
 * GPIO.  Bus 1 is PB6/PB7, the pair every board has, and bus 2 is
 * PB10/PB11, which this family bonds. */
#define BOARD_I2C_MAP \
    { { FREYA_PB(6), FREYA_PB(7) }, \
      { FREYA_PB(10), FREYA_PB(11) } }
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
/* ADC1 channels shared with the other ports and not kept by Freya.
 * The internal temperature sensor is channel 18; Vref is 17. */
#define BOARD_ADC_MAP \
    { { FREYA_PA(0), 0 }, { FREYA_PA(1), 1 }, \
      { FREYA_PB(0), 8 }, { FREYA_PB(1), 9 }, \
      { FREYA_PC(0), 10 }, { FREYA_PC(1), 11 }, \
      { FREYA_PC(2), 12 }, { FREYA_PC(3), 13 }, \
      { FREYA_PC(4), 14 }, { FREYA_PC(5), 15 } }
#define BOARD_ADC_TEMP_CHANNEL  18
#define BOARD_ADC_VREF_CHANNEL  17

/* --------------------------------------------------------------- RTC */
/* The calendar RTC, on the 32.768 kHz crystal, for RTC=internal
 * (src/rtc.c): its base, where the F4 keeps CR, and what has to be turned on
 * before its registers and the backup domain can be written. */
#define BOARD_RTC_INTERNAL  1
#define BOARD_RTC_BASE      0x40002800UL
#define BOARD_RTC_CR_OFF    0x08
#ifdef FREYA_RTC_INTERNAL
static inline void board_rtc_access(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_DBP;
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
