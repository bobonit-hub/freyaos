/*
 * Freya - WeAct MiniSTM32H723 core board (STM32H723VGT6).
 *
 * Every board Freya runs on supplies a header with this name holding the
 * chip's register definitions, the few strings the shell prints, and the
 * declarations of the bring-up hooks the generic drivers call.  src/freya.h
 * includes it and the Makefile puts the right boards/<board> directory on
 * the include path.
 *
 * The board is https://github.com/WeActStudio/WeActStudio.MiniSTM32H723:
 * a Cortex-M7 in an LQFP100 with a 25 MHz crystal, the LED on PE3, KEY on
 * PC13, a microSD slot on the SDMMC1 pins, an 8 MiB SPI NOR on SPI1
 * (PB3/PB4/PD7, chip select PD6), an 8 MiB OSPI NOR, an ST7735 LCD on SPI4
 * and a camera connector.  It is not the Black Pill's layout, but the
 * header pins Freya uses on the other boards - the console on PA2/PA3,
 * PWM, I2C, SPI2 and the ESP32-C6 link - are free here too, so a
 * program sees the same pin plan.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#include "stm32h723.h"

/* ------------------------------------------------------------ identity */
#define BOARD_NAME          "WeAct MiniSTM32H723 (STM32H723VGT6)"
#define BOARD_MCU           "STM32H723VGT6"
#define BOARD_CORE          "ARM Cortex-M7F"
#define BOARD_HSE_NAME      "HSE 25 MHz crystal"
#define BOARD_HSI_NAME      "HSI 64 MHz oscillator"
#define BOARD_CONSOLE_NAME  "USART2 921600 8N1 on PA2/PA3"
#define BOARD_LED_NAME      "PE3"
#define BOARD_FLASH_WS      3

/* ------------------------------------------------------ internal flash */
/* 1 MiB in eight 128 KiB sectors, programmed 32 bytes at a time.  The
 * map is the F4's: the kernel in sector 0, the system settings alone in
 * sector 1, the program region in sectors 2..6 and the kernel extension
 * in sector 7.  BOARD_FLASH_PAGE_SIZE is the erase unit. */
#define BOARD_FLASH_KIB         1024U
#define BOARD_FLASH_PAGE_SIZE   (128U * 1024U)

/* ------------------------------------------------- the generation of IP */
/* The USART and the SPI are the U5's blocks; EXTI is the F4's model. */
#define BOARD_USART_ISR     1           /* ISR/ICR/RDR/TDR, not SR/DR    */
#define BOARD_SPI_FIFO      1           /* CFG1/CFG2, CSTART, TXDR/RXDR  */

/* The SPIs run from PLL1Q, 520 / 6 = 86.7 MHz, not from an APB clock;
 * src/spi.c works a program's rates out from this. */
#define BOARD_SPI_KERNEL_HZ 86666666UL

/* The instruction cache is on: src/loader.c invalidates it after it
 * writes a program into RAM. */
#define BOARD_ICACHE        1

/* sys_delay_us() counts core cycles in the DWT, which board_clock_init()
 * starts: the M7 runs a nop loop at no fixed cycles a pass. */
#define BOARD_DELAY_CYCCNT  1

/* ---------------------------------------------------------- the card */
/*
 * The microSD slot is wired for SDMMC1, which no SPI peripheral can reach,
 * so the card is driven in SPI mode by the board itself, bit by bit, on
 * the same pins: CS is DAT3 (PC11), SCK is CLK (PC12), MOSI is CMD (PD2)
 * and MISO is DAT0 (PC8).  sdspi_*() live in board.c, and src/spi.c
 * gives SPI1 to the SPI flash instead.  The slot has no supply switch.
 */
#define BOARD_SD_BITBANG    1
#define BOARD_SD_CS_PORT    GPIOC
#define BOARD_SD_CS_PIN     11
#define BOARD_SPI_HAS_I2S   0

/* --------------------------------------------------------- SPI flash */
/* The 8 MiB SPI NOR has SPI1 to itself: SCK PB3, MISO PB4, MOSI PD7, all
 * AF5, and chip select PD6.  It is a LittleFS volume at /spi1, mounted
 * beside the card rather than instead of it. */
#define BOARD_SPIFLASH          1
#define BOARD_SPIFLASH_OWN_BUS  1
#define BOARD_FLASH_CS_PORT     GPIOD
#define BOARD_FLASH_CS_PIN      6
/* 86.7 MHz kernel clock: /256 = 338 kHz to identify, /4 = 21.7 MHz. */
#define BOARD_SPI_BR_SLOW   7
#define BOARD_SPI_BR_FAST   1

/* ------------------------------------------------- pins and interrupts */
/* Ports A to E.  Freya keeps the console (PA2/PA3), the card (PC8..PC12,
 * PD2) and the SPI flash (PB3, PB4, PD6, PD7).  PE3 is the LED, which a
 * program may drive as a pin or through api->led().  PB6 also selects the
 * OSPI NOR, which ignores it while its clock (PB2) is still. */
#define BOARD_PIN_PORTS     5
#ifdef FREYA_USB
/* USB=1 also keeps PA11 and PA12, the USB socket's D- and D+. */
#define BOARD_PIN_RESERVED  { 0x180CU, 0x0018U, 0x1F00U, 0x00C4U, 0x0000U }
#else
#define BOARD_PIN_RESERVED  { 0x000CU, 0x0018U, 0x1F00U, 0x00C4U, 0x0000U }
#endif

/* ------------------------------------------------------------ USB host */
/* USB=1: OTG_HS on its full speed PHY as the host, on PA11/PA12 (AF10), the board's
 * USB socket.  src/usbh.c drives it; board_usb_init() clocks it. */
#define BOARD_USB_OTG_BASE  0x40040000UL
#define BOARD_USB_IRQn      77
#define BOARD_USB_IRQ_HANDLER OTG_HS_IRQHandler

/* ------------------------------------------- timers a program may open */
/* TIM2..TIM4, all on APB1 and all clocked at twice PCLK1 (260 MHz)
 * because the prescaler is not 1.  Each entry is { registers, IRQ,
 * APB1LENR bit } in the order src/timer.c hands them out and names their
 * handlers.  At 260 MHz the longest period that fits the 32-bit tick
 * count is 16.5 s. */
#define BOARD_TIMER_LIST \
    { { TIM2, TIM2_IRQn, RCC_APB1ENR_TIM2EN }, \
      { TIM3, TIM3_IRQn, RCC_APB1ENR_TIM3EN }, \
      { TIM4, TIM4_IRQn, RCC_APB1ENR_TIM4EN } }
#define BOARD_TIMER_COUNT   3
#define BOARD_TIMER_NAMES   { "TIM2", "TIM3", "TIM4" }

/* ------------------------------------------- PWM outputs a program may open */
/*
 * The pins those timers can drive: { pin, timer index in the list above,
 * channel 1..4, alternate function }.  The Black Pill's eight pins and
 * alternate functions: AF1 for TIM2, AF2 for TIM3 and TIM4.  PB7 is also
 * the camera's VSYNC and PB8/PB9 its I2C, which matter only with a camera
 * fitted.
 */
#define BOARD_PWM_MAP \
    { { FREYA_PA(0), 0, 1, 1 }, { FREYA_PA(1), 0, 2, 1 },  \
      { FREYA_PB(0), 1, 3, 2 }, { FREYA_PB(1), 1, 4, 2 },  \
      { FREYA_PB(6), 2, 1, 2 }, { FREYA_PB(7), 2, 2, 2 },  \
      { FREYA_PB(8), 2, 3, 2 }, { FREYA_PB(9), 2, 4, 2 } }

/* ----------------------------------------------- I2C a program may open */
/* Each bus is { SCL pin, SDA pin }.  The master drives them as open-drain
 * GPIO.  Bus 1 is PB6/PB7, the pair every board has, and bus 2 PB10/PB11,
 * as on the Blue Pill: the LQFP100 bonds PB11. */
#define BOARD_I2C_MAP \
    { { FREYA_PB(6), FREYA_PB(7) }, \
      { FREYA_PB(10), FREYA_PB(11) } }
#define BOARD_I2C_NAMES { "I2C1", "I2C2" }

/* ----------------------------------------------- SPI a program may open */
/* SPI2 on PB13..PB15, AF5: { regs, APB number, SCK, MISO, MOSI,
 * alternate function }.  Chip select is not in the map; a program drives
 * that pin itself. */
#define BOARD_SPI_MAP \
    { { SPI2, 1, FREYA_PB(13), FREYA_PB(14), FREYA_PB(15), 5 } }
#define BOARD_SPI_NAMES { "SPI2" }

/* ESP32-C6 network coprocessor on SPI2, wired as on the Black Pill.  The
 * transfers use DMA1 streams 3 (receive) and 4 (transmit), routed to SPI2
 * by DMAMUX1.  SPI2 is returned to the public SPI API when the link is
 * closed. */
#define BOARD_NET_SUPPORTED  1
#define BOARD_NET_SPI        SPI2
#define BOARD_NET_SPI_BUS    1
#define BOARD_NET_SCK        FREYA_PB(13)
#define BOARD_NET_MISO       FREYA_PB(14)
#define BOARD_NET_MOSI       FREYA_PB(15)
#define BOARD_NET_CS         FREYA_PB(12)
#define BOARD_NET_READY      FREYA_PB(10)
#define BOARD_NET_SPI_AF     5
#define BOARD_NET_SPI_BR     1
#define BOARD_ESP_LINK       1
#define BOARD_ESP_CS         BOARD_NET_CS
#define BOARD_ESP_READY      BOARD_NET_READY
#define BOARD_ESP_SPI_AF     BOARD_NET_SPI_AF
#define BOARD_ESP_DMAMUX     1          /* F4 streams behind a DMAMUX    */
/* 86.7 MHz / 4 = 21.7 MHz, the F4 boards' rate. */
#define BOARD_ESP_SPI_BR     1

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

/* --------------------------------------------------------------- reset */
/* The H7 keeps its reset flags in RSR, at bits of its own. */
#define BOARD_RESET_SR      (RCC->RSR)
#define BOARD_RESET_RMVF    RCC_RSR_RMVF
#define BOARD_RSTF_LPWR     (1UL << 30)
#define BOARD_RSTF_WWDG     (1UL << 28)
#define BOARD_RSTF_IWDG     (1UL << 26)
#define BOARD_RSTF_SOFTWARE (1UL << 24)
#define BOARD_RSTF_POWER_ON (1UL << 23)
#define BOARD_RSTF_PIN      (1UL << 22)
#define BOARD_RSTF_BROWNOUT (1UL << 21)

/* --------------------------------------------------------------- ADC */
/* ADC1 reads the pins; the PC2/PC3 analog pads are left out.  The
 * temperature sensor and Vref are ADC3's channels 17 and 18, which
 * board_adc_read() tells apart by BOARD_ADC3. */
#define BOARD_ADC3          0x40
#define BOARD_ADC_MAP \
    { { FREYA_PA(0), 16 }, { FREYA_PA(1), 17 }, \
      { FREYA_PB(0), 9 }, { FREYA_PB(1), 5 }, \
      { FREYA_PC(0), 10 }, { FREYA_PC(1), 11 }, \
      { FREYA_PC(4), 4 }, { FREYA_PC(5), 8 } }
#define BOARD_ADC_TEMP_CHANNEL  (BOARD_ADC3 | 17)
#define BOARD_ADC_VREF_CHANNEL  (BOARD_ADC3 | 18)

/* --------------------------------------------------------------- RTC */
/* The calendar RTC, on the 32.768 kHz crystal, for RTC=internal
 * (src/rtc.c): its base, where the H7 keeps CR, and what has to be turned on
 * before its registers and the backup domain can be written. */
#define BOARD_RTC_INTERNAL  1
#define BOARD_RTC_BASE      0x58004000UL
#define BOARD_RTC_CR_OFF    0x08
#ifdef FREYA_RTC_INTERNAL
static inline void board_rtc_access(void)
{
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    (void)RCC->APB4ENR;
    PWR->CR1 |= PWR_CR1_DBP;
}
#endif

/* --------------------------------------------------------------- hooks */
void board_clock_init(void);            /* clock tree, fills g_clocks    */
void board_uart_pins(void);             /* console pins and USART clock  */
void board_spi_pins(void);              /* SPI flash pins, SPI1 and CS   */
void board_sd_power(int on);            /* the slot has no switch        */
void board_spi_mux(SPI_TypeDef *spi, int sck, int miso, int mosi, int af);

/* Pins for programs: the register layout is the chip's, so the generic
 * driver in src/gpio.c asks the board to configure and to route. */
GPIO_TypeDef *board_gpio_port(int port);          /* NULL: no such port  */
void          board_pin_mode(GPIO_TypeDef *port, int pin, int mode);
int           board_pin_pull(GPIO_TypeDef *port, int pin, int pull);
int           board_pin_pull_get(GPIO_TypeDef *port, int pin);  /* FREYA_PULL_* */
void          board_exti_select(int port, int pin);
void          board_pin_af(GPIO_TypeDef *port, int pin, int af);  /* PWM  */
int           board_adc_read(int channel);

/* led_init(), led_set() and led_toggle() are declared in freya.h and
 * implemented per board. */

#endif /* FREYA_BOARD_H */
