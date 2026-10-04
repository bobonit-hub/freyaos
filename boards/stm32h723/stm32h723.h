/*
 * Freya - minimal STM32H723xx register definitions.
 * Only the peripherals Freya actually touches are described here; this
 * replaces CMSIS so the system stays dependency free.  The registers
 * below match RM0468.
 *
 * The H7 sits between the generations the other boards use.  GPIO, the
 * general purpose timers and the DMA streams are the F4's, EXTI works the
 * F4's way (SYSCFG picks the port, one pending register, lines 5..9 and
 * 10..15 share an interrupt), and the USART and the FIFO SPI are the
 * U5's blocks.  The clock tree, the power controller, the ADC and the
 * flash controller are its own, and the DMA requests go through a DMAMUX.
 */
#ifndef FREYA_STM32H723_H
#define FREYA_STM32H723_H

#include <stdint.h>

#define __IO volatile

/* ---------------------------------------------------------------- RCC */
/*
 * APB1LENR is also given the F4's name, APB1ENR, because the timer driver
 * turns TIM2..TIM4 on through it and their enable bits are the F4's bits
 * 0..2.
 */
typedef struct {
    __IO uint32_t CR;          /* 0x000 */
    __IO uint32_t HSICFGR;     /* 0x004 */
    __IO uint32_t CRRCR;       /* 0x008 */
    __IO uint32_t CSICFGR;     /* 0x00C */
    __IO uint32_t CFGR;        /* 0x010 */
    uint32_t      RES0;        /* 0x014 */
    __IO uint32_t D1CFGR;      /* 0x018 */
    __IO uint32_t D2CFGR;      /* 0x01C */
    __IO uint32_t D3CFGR;      /* 0x020 */
    uint32_t      RES1;        /* 0x024 */
    __IO uint32_t PLLCKSELR;   /* 0x028 */
    __IO uint32_t PLLCFGR;     /* 0x02C */
    __IO uint32_t PLL1DIVR;    /* 0x030 */
    __IO uint32_t PLL1FRACR;   /* 0x034 */
    __IO uint32_t PLL2DIVR;    /* 0x038 */
    __IO uint32_t PLL2FRACR;   /* 0x03C */
    __IO uint32_t PLL3DIVR;    /* 0x040 */
    __IO uint32_t PLL3FRACR;   /* 0x044 */
    uint32_t      RES2;        /* 0x048 */
    __IO uint32_t D1CCIPR;     /* 0x04C */
    __IO uint32_t D2CCIP1R;    /* 0x050 */
    __IO uint32_t D2CCIP2R;    /* 0x054 */
    __IO uint32_t D3CCIPR;     /* 0x058 */
    uint32_t      RES3;        /* 0x05C */
    __IO uint32_t CIER;        /* 0x060 */
    __IO uint32_t CIFR;        /* 0x064 */
    __IO uint32_t CICR;        /* 0x068 */
    uint32_t      RES4;        /* 0x06C */
    __IO uint32_t BDCR;        /* 0x070, the backup domain */
    __IO uint32_t CSR;         /* 0x074 */
    uint32_t      RES5[22];    /* 0x078 .. 0x0CC */
    __IO uint32_t RSR;         /* 0x0D0 */
    __IO uint32_t AHB3ENR;     /* 0x0D4 */
    __IO uint32_t AHB1ENR;     /* 0x0D8 */
    __IO uint32_t AHB2ENR;     /* 0x0DC */
    __IO uint32_t AHB4ENR;     /* 0x0E0 */
    __IO uint32_t APB3ENR;     /* 0x0E4 */
    union {
        __IO uint32_t APB1LENR; /* 0x0E8 */
        __IO uint32_t APB1ENR;
    };
    __IO uint32_t APB1HENR;    /* 0x0EC */
    __IO uint32_t APB2ENR;     /* 0x0F0 */
    __IO uint32_t APB4ENR;     /* 0x0F4 */
    uint32_t      RES6;        /* 0x0F8 */
    __IO uint32_t AHB3LPENR;   /* 0x0FC */
    __IO uint32_t AHB1LPENR;   /* 0x100 */
} RCC_TypeDef;

#define RCC                 ((RCC_TypeDef *)0x58024400UL)

#define RCC_CR_HSION        (1UL << 0)
#define RCC_CR_HSIRDY       (1UL << 2)
#define RCC_CR_HSIDIV_MASK  (3UL << 3)    /* 0: 64 MHz, the reset value  */
#define RCC_CR_HSEON        (1UL << 16)
#define RCC_CR_HSERDY       (1UL << 17)
#define RCC_CR_CSSON        (1UL << 19)   /* CSSHSEON                    */
#define RCC_CR_PLL1ON       (1UL << 24)
#define RCC_CR_PLL1RDY      (1UL << 25)
#define RCC_CR_PLL3ON       (1UL << 28)
#define RCC_CR_PLL3RDY      (1UL << 29)

#define RCC_CFGR_SW_MASK    (7UL << 0)
#define RCC_CFGR_SW_PLL1    (3UL << 0)
#define RCC_CFGR_SWS_MASK   (7UL << 3)
#define RCC_CFGR_SWS_PLL1   (3UL << 3)

/* The core runs at SYSCLK (D1CPRE /1); HCLK, the AXI and AHB clock, is
 * half that, and every APB bus half of HCLK. */
#define RCC_D1CFGR_HPRE_DIV2    (8UL << 0)
#define RCC_D1CFGR_D1PPRE_DIV2  (4UL << 4)
#define RCC_D1CFGR_D1CPRE_DIV1  (0UL << 8)
#define RCC_D2CFGR_D2PPRE1_DIV2 (4UL << 4)
#define RCC_D2CFGR_D2PPRE2_DIV2 (4UL << 8)
#define RCC_D3CFGR_D3PPRE_DIV2  (4UL << 4)

#define RCC_PLLCKSELR_SRC_HSI   (0UL << 0)
#define RCC_PLLCKSELR_SRC_HSE   (2UL << 0)
#define RCC_PLLCKSELR_SRC_MASK  (3UL << 0)
#define RCC_PLLCKSELR_DIVM1(m)  (((uint32_t)(m) & 0x3FUL) << 4)  /* not m-1 */
#define RCC_PLLCKSELR_DIVM1_MASK (0x3FUL << 4)
#define RCC_PLLCFGR_PLL1RGE_4_8 (2UL << 2)    /* input 4..8 MHz          */
#define RCC_PLLCFGR_PLL1VCO_WIDE (0UL << 1)
#define RCC_PLLCFGR_PLL1_MASK   0x0000000FUL
#define RCC_PLLCFGR_DIVP1EN     (1UL << 16)
#define RCC_PLLCFGR_DIVQ1EN     (1UL << 17)
#define RCC_PLL1DIVR_N(n)   (((uint32_t)(n) - 1U) & 0x1FFUL)
#define RCC_PLL1DIVR_P(n)   ((((uint32_t)(n) - 1U) & 0x7FUL) << 9)
#define RCC_PLL1DIVR_Q(n)   ((((uint32_t)(n) - 1U) & 0x7FUL) << 16)
#define RCC_PLL1DIVR_R(n)   ((((uint32_t)(n) - 1U) & 0x7FUL) << 24)
/* PLL3, for the USB clock: PLL1's fields, moved up. */
#define RCC_PLLCKSELR_DIVM3(m)  (((uint32_t)(m) & 0x3FUL) << 20) /* not m-1 */
#define RCC_PLLCKSELR_DIVM3_MASK (0x3FUL << 20)
#define RCC_PLLCFGR_PLL3_MASK   (0xFUL << 8)
#define RCC_PLLCFGR_PLL3RGE_4_8 (2UL << 10)
#define RCC_PLLCFGR_DIVQ3EN     (1UL << 23)
/* PLL3DIVR has PLL1DIVR's layout. */

/* D2CCIP2R.USBSEL: the 48 MHz clock of OTG_HS's full speed PHY. */
#define RCC_D2CCIP2R_USBSEL_MASK  (3UL << 20)
#define RCC_D2CCIP2R_USBSEL_PLL3Q (2UL << 20)

/* Kernel clocks.  SPI1/2/3 take PLL1Q and USART2 PCLK1, their reset
 * choices.  The ADCs take per_ck, which is HSI. */
#define RCC_D2CCIP1R_SPI123SEL_MASK (7UL << 12)
#define RCC_D1CCIPR_CKPERSEL_MASK   (3UL << 28)   /* 0: HSI             */
#define RCC_D3CCIPR_ADCSEL_MASK     (3UL << 16)
#define RCC_D3CCIPR_ADCSEL_PER      (2UL << 16)

#define RCC_CICR_CSSC       (1UL << 10)   /* HSECSSC                    */

#define RCC_AHB1ENR_DMA1EN  (1UL << 0)
#define RCC_AHB1ENR_ADC12EN (1UL << 5)
#define RCC_AHB1ENR_USB1OTGHSEN (1UL << 25)
/* The ULPI clock is for an external PHY.  Left on in sleep it stops the
 * embedded one, so it is turned off for WFI. */
#define RCC_AHB1LPENR_USB1OTGHSULPILPEN (1UL << 26)
#define RCC_AHB4ENR_GPIOAEN (1UL << 0)
#define RCC_AHB4ENR_GPIOBEN (1UL << 1)
#define RCC_AHB4ENR_GPIOCEN (1UL << 2)
#define RCC_AHB4ENR_GPIODEN (1UL << 3)
#define RCC_AHB4ENR_GPIOEEN (1UL << 4)
#define RCC_AHB4ENR_ADC3EN  (1UL << 24)
#define RCC_APB1ENR_TIM2EN  (1UL << 0)
#define RCC_APB1ENR_TIM3EN  (1UL << 1)
#define RCC_APB1ENR_TIM4EN  (1UL << 2)
#define RCC_APB1ENR_SPI2EN  (1UL << 14)
#define RCC_APB1ENR_USART2EN (1UL << 17)
#define RCC_APB2ENR_SPI1EN  (1UL << 12)
#define RCC_APB4ENR_SYSCFGEN (1UL << 1)
#define RCC_APB4ENR_RTCAPBEN (1UL << 16)

/* Reset flags live in RSR, at bits of their own; src/system.c reads them
 * through the BOARD_RSTF_* names in board.h. */
#define RCC_RSR_RMVF        (1UL << 16)

/* ---------------------------------------------------------------- PWR */
typedef struct {
    __IO uint32_t CR1;         /* 0x00 */
    __IO uint32_t CSR1;        /* 0x04 */
    __IO uint32_t CR2;         /* 0x08 */
    __IO uint32_t CR3;         /* 0x0C */
    __IO uint32_t CPUCR;       /* 0x10 */
    uint32_t      RES0;        /* 0x14 */
    __IO uint32_t D3CR;        /* 0x18 */
} PWR_TypeDef;

#define PWR                 ((PWR_TypeDef *)0x58024800UL)
#define PWR_CR1_DBP         (1UL << 8)    /* backup domain writable     */
#define PWR_CR3_BYPASS      (1UL << 0)
#define PWR_CR3_LDOEN       (1UL << 1)
#define PWR_CR3_SCUEN       (1UL << 2)    /* supply configuration lock   */
#define PWR_CR3_USB33DEN    (1UL << 24)   /* VDD33USB level detector     */
#define PWR_CR3_USB33RDY    (1UL << 26)
#define PWR_CSR1_ACTVOSRDY  (1UL << 13)
#define PWR_D3CR_VOS_MASK   (3UL << 14)
#define PWR_D3CR_VOS0       (3UL << 14)
#define PWR_D3CR_VOSRDY     (1UL << 13)

/* -------------------------------------------------------------- FLASH */
typedef struct {
    __IO uint32_t ACR;         /* 0x00 */
    __IO uint32_t KEYR1;       /* 0x04 */
    __IO uint32_t OPTKEYR;     /* 0x08 */
    __IO uint32_t CR1;         /* 0x0C */
    __IO uint32_t SR1;         /* 0x10 */
    __IO uint32_t CCR1;        /* 0x14 */
} FLASH_TypeDef;

#define FLASH_R             ((FLASH_TypeDef *)0x52002000UL)
#define FLASH_ACR_LATENCY(n) ((uint32_t)(n) & 0xF)
#define FLASH_ACR_WRHIGHFREQ(n) (((uint32_t)(n) & 3UL) << 4)
#define FLASH_ACR_WRHIGHFREQ_MASK (3UL << 4)

/* 1 MiB in one bank of eight 128 KiB sectors, programmed a flash word
 * (32 bytes) at a time.  Each flash word carries ECC and may be written
 * once between erases. */
#define FLASH_WORD          32U

#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL

/* Status in SR1; the flags are cleared through the same bits of CCR1. */
#define FLASH_SR_BSY        (1UL << 0)
#define FLASH_SR_WBNE       (1UL << 1)    /* write buffer not empty      */
#define FLASH_SR_QW         (1UL << 2)    /* a write is queued           */
#define FLASH_SR_EOP        (1UL << 16)
#define FLASH_SR_WRPERR     (1UL << 17)
#define FLASH_SR_PGSERR     (1UL << 18)
#define FLASH_SR_STRBERR    (1UL << 19)
#define FLASH_SR_INCERR     (1UL << 21)
#define FLASH_SR_OPERR      (1UL << 22)
#define FLASH_SR_ERRORS     (FLASH_SR_WRPERR | FLASH_SR_PGSERR | \
                             FLASH_SR_STRBERR | FLASH_SR_INCERR | \
                             FLASH_SR_OPERR)

#define FLASH_CR_LOCK       (1UL << 0)
#define FLASH_CR_PG         (1UL << 1)
#define FLASH_CR_SER        (1UL << 2)
#define FLASH_CR_PSIZE_X32  (2UL << 4)
#define FLASH_CR_START      (1UL << 7)
#define FLASH_CR_SNB(n)     (((uint32_t)(n) & 7UL) << 8)

/* --------------------------------------------------------------- GPIO */
/* The F4's layout, so board.c configures pins the way the Black Pill's
 * does. */
typedef struct {
    __IO uint32_t MODER;
    __IO uint32_t OTYPER;
    __IO uint32_t OSPEEDR;
    __IO uint32_t PUPDR;
    __IO uint32_t IDR;
    __IO uint32_t ODR;
    __IO uint32_t BSRR;
    __IO uint32_t LCKR;
    __IO uint32_t AFR[2];
} GPIO_TypeDef;

#define GPIOA               ((GPIO_TypeDef *)0x58020000UL)
#define GPIOB               ((GPIO_TypeDef *)0x58020400UL)
#define GPIOC               ((GPIO_TypeDef *)0x58020800UL)
#define GPIOD               ((GPIO_TypeDef *)0x58020C00UL)
#define GPIOE               ((GPIO_TypeDef *)0x58021000UL)

/* ---------------------------------------------------------------- ADC */
/*
 * ADC1 is the 16-bit converter, run at 12 bits, and reads the pins.  ADC3
 * is a 12-bit one and the only one wired to the temperature sensor and
 * Vref.  Both wake in deep power-down.  Only the registers both have, at
 * the same offsets, are described; PCSEL is ADC1's alone.
 */
typedef struct {
    __IO uint32_t ISR;         /* 0x00 */
    __IO uint32_t IER;         /* 0x04 */
    __IO uint32_t CR;          /* 0x08 */
    __IO uint32_t CFGR;        /* 0x0C */
    __IO uint32_t CFGR2;       /* 0x10 */
    __IO uint32_t SMPR1;       /* 0x14 */
    __IO uint32_t SMPR2;       /* 0x18 */
    __IO uint32_t PCSEL;       /* 0x1C, ADC1/2 only */
    __IO uint32_t TR1;         /* 0x20 */
    __IO uint32_t TR2;         /* 0x24 */
    __IO uint32_t TR3;         /* 0x28 */
    uint32_t      RES0;        /* 0x2C */
    __IO uint32_t SQR1;        /* 0x30 */
    __IO uint32_t SQR2;        /* 0x34 */
    __IO uint32_t SQR3;        /* 0x38 */
    __IO uint32_t SQR4;        /* 0x3C */
    __IO uint32_t DR;          /* 0x40 */
} ADC_TypeDef;

typedef struct {
    __IO uint32_t CSR;         /* 0x00 */
    uint32_t      RES0;        /* 0x04 */
    __IO uint32_t CCR;         /* 0x08 */
} ADC_Common_TypeDef;

#define ADC1                ((ADC_TypeDef *)0x40022000UL)
#define ADC12_COMMON        ((ADC_Common_TypeDef *)0x40022300UL)
#define ADC3                ((ADC_TypeDef *)0x58026000UL)
#define ADC3_COMMON         ((ADC_Common_TypeDef *)0x58026300UL)

#define ADC_ISR_ADRDY       (1UL << 0)
#define ADC_ISR_EOC         (1UL << 2)
#define ADC_ISR_LDORDY      (1UL << 12)   /* ADC1/2                      */
#define ADC_CR_ADEN         (1UL << 0)
#define ADC_CR_ADSTART      (1UL << 2)
#define ADC_CR_BOOST_12_25  (2UL << 8)    /* ADC1/2: for 12.5..25 MHz    */
#define ADC_CR_ADCALLIN     (1UL << 16)   /* ADC1/2: linearity too       */
#define ADC_CR_ADVREGEN     (1UL << 28)
#define ADC_CR_DEEPPWD      (1UL << 29)
#define ADC_CR_ADCAL        (1UL << 31)
#define ADC1_CFGR_RES_12    (2UL << 2)    /* ADC1/2 code for 12 bits     */
#define ADC1_CFGR_RES_MASK  (7UL << 2)
#define ADC_CCR_PRESC_DIV4  (2UL << 18)   /* 64 / 4 = 16 MHz             */
#define ADC_CCR_PRESC_MASK  (0xFUL << 18)
#define ADC_CCR_CKMODE_MASK (3UL << 16)   /* 0: the kernel clock, per_ck */
#define ADC_CCR_VREFEN      (1UL << 22)
#define ADC_CCR_TSEN        (1UL << 23)   /* VSENSEEN / VSENSESEL        */
#define ADC_SAMPLE_LONG     7UL           /* 810.5 / 640.5 cycles        */

/* ------------------------------------------------------- SYSCFG / EXTI */
/* Sixteen external interrupt lines, one per pin number; SYSCFG->EXTICR
 * decides which port's pin n drives line n, four bits a line, as on the
 * F4. */
typedef struct {
    uint32_t      RES0;        /* 0x00 */
    __IO uint32_t PMCR;        /* 0x04 */
    __IO uint32_t EXTICR[4];   /* 0x08 */
} SYSCFG_TypeDef;

#define SYSCFG              ((SYSCFG_TypeDef *)0x58000400UL)

/*
 * The H7's EXTI has a bank of trigger registers per thirty-two lines and
 * the core's own mask and pending registers further up.  Lines 0..15 are
 * all Freya uses, and they behave as the F4's: RTSR1, FTSR1, the core's
 * IMR1 and PR1 are given the F4's names, so src/gpio.c drives them
 * unchanged.
 */
typedef struct {
    __IO uint32_t RTSR;        /* 0x00, RTSR1 */
    __IO uint32_t FTSR;        /* 0x04, FTSR1 */
    __IO uint32_t SWIER;       /* 0x08, SWIER1 */
    uint32_t      RES0[29];    /* 0x0C .. 0x7C */
    __IO uint32_t IMR;         /* 0x80, CPUIMR1 */
    __IO uint32_t EMR;         /* 0x84, CPUEMR1 */
    __IO uint32_t PR;          /* 0x88, CPUPR1: write 1 to clear */
} EXTI_TypeDef;

#define EXTI                ((EXTI_TypeDef *)0x58000000UL)

/* -------------------------------------------------------------- USART */
typedef struct {
    __IO uint32_t CR1;         /* 0x00 */
    __IO uint32_t CR2;         /* 0x04 */
    __IO uint32_t CR3;         /* 0x08 */
    __IO uint32_t BRR;         /* 0x0C */
    __IO uint32_t GTPR;        /* 0x10 */
    __IO uint32_t RTOR;        /* 0x14 */
    __IO uint32_t RQR;         /* 0x18 */
    __IO uint32_t ISR;         /* 0x1C */
    __IO uint32_t ICR;         /* 0x20 */
    __IO uint32_t RDR;         /* 0x24 */
    __IO uint32_t TDR;         /* 0x28 */
    __IO uint32_t PRESC;       /* 0x2C */
} USART_TypeDef;

#define USART2              ((USART_TypeDef *)0x40004400UL)

/* FIFO off, so RXNE and TXE mean what they did on the F4. */
#define USART_ISR_PE        (1UL << 0)
#define USART_ISR_FE        (1UL << 1)
#define USART_ISR_NE        (1UL << 2)
#define USART_ISR_ORE       (1UL << 3)
#define USART_ISR_RXNE      (1UL << 5)
#define USART_ISR_TC        (1UL << 6)
#define USART_ISR_TXE       (1UL << 7)
#define USART_ICR_ERRORS    0x0FUL        /* PECF | FECF | NECF | ORECF */
#define USART_CR1_UE        (1UL << 0)
#define USART_CR1_RE        (1UL << 2)
#define USART_CR1_TE        (1UL << 3)
#define USART_CR1_RXNEIE    (1UL << 5)

/* ---------------------------------------------------------------- SPI */
/*
 * The FIFO SPI: configuration lives in CFG1 and CFG2, which may only be
 * written while SPE is clear, and a master transfers only after CSTART.
 * Data is a byte access to TXDR and RXDR - a word store would queue four
 * frames.  BSY is gone; TXC says the FIFO is empty and the bus is idle.
 */
typedef struct {
    __IO uint32_t CR1;         /* 0x00 */
    __IO uint32_t CR2;         /* 0x04 */
    __IO uint32_t CFG1;        /* 0x08 */
    __IO uint32_t CFG2;        /* 0x0C */
    __IO uint32_t IER;         /* 0x10 */
    __IO uint32_t SR;          /* 0x14 */
    __IO uint32_t IFCR;        /* 0x18 */
    __IO uint32_t AUTOCR;      /* 0x1C */
    __IO uint32_t TXDR;        /* 0x20 */
    uint32_t      RES0[3];     /* 0x24 */
    __IO uint32_t RXDR;        /* 0x30 */
} SPI_TypeDef;

#define SPI1                ((SPI_TypeDef *)0x40013000UL)
#define SPI2                ((SPI_TypeDef *)0x40003800UL)

#define SPI_TXDR8(s)        (*(__IO uint8_t *)&(s)->TXDR)
#define SPI_RXDR8(s)        (*(__IO uint8_t *)&(s)->RXDR)

#define SPI_CR1_SPE         (1UL << 0)
#define SPI_CR1_CSTART      (1UL << 9)
#define SPI_CR1_SSI         (1UL << 12)
#define SPI_CFG1_DSIZE_8    (7UL << 0)
#define SPI_CFG1_RXDMAEN    (1UL << 14)
#define SPI_CFG1_TXDMAEN    (1UL << 15)
#define SPI_CFG1_CRCSIZE_8  (7UL << 16)
#define SPI_CFG1_MBR_SHIFT  28            /* /2 << n, the F4's BR codes */
#define SPI_CFG1_MBR_MASK   (7UL << 28)
#define SPI_CFG2_CPHA       (1UL << 24)
#define SPI_CFG2_CPOL       (1UL << 25)
#define SPI_CFG2_MASTER     (1UL << 22)
#define SPI_CFG2_SSM        (1UL << 26)
#define SPI_CFG2_AFCNTR     (1UL << 31)   /* keep driving SCK when off  */
#define SPI_SR_RXP          (1UL << 0)
#define SPI_SR_TXP          (1UL << 1)
#define SPI_SR_TXC          (1UL << 12)
#define SPI_IFCR_ALL        0x0BF8UL

/* --------------------------------------------------------------- DMA */
/* The F4's DMA streams.  Which request a stream serves is not a channel
 * number in its CR here but the DMAMUX channel of the same number. */
typedef struct {
    __IO uint32_t CR;
    __IO uint32_t NDTR;
    __IO uint32_t PAR;
    __IO uint32_t M0AR;
    __IO uint32_t M1AR;
    __IO uint32_t FCR;
} DMA_Stream_TypeDef;

typedef struct {
    __IO uint32_t LISR;
    __IO uint32_t HISR;
    __IO uint32_t LIFCR;
    __IO uint32_t HIFCR;
    DMA_Stream_TypeDef STREAM[8];
} DMA_TypeDef;

#define DMA1                ((DMA_TypeDef *)0x40020000UL)
#define DMA1_Stream3        (&DMA1->STREAM[3])
#define DMA1_Stream4        (&DMA1->STREAM[4])
#define DMA_SxCR_EN         (1UL << 0)
#define DMA_SxCR_TEIE       (1UL << 2)
#define DMA_SxCR_TCIE       (1UL << 4)
#define DMA_SxCR_DIR_M2P    (1UL << 6)
#define DMA_SxCR_MINC       (1UL << 10)
#define DMA_SxCR_PL_HIGH    (2UL << 16)
#define DMA_LISR_TEIF3      (1UL << 25)
#define DMA_LISR_TCIF3      (1UL << 27)
#define DMA_LIFCR_CSTREAM3  (0x3DUL << 22)
#define DMA_HISR_TEIF4      (1UL << 3)
#define DMA_HISR_TCIF4      (1UL << 5)
#define DMA_HIFCR_CSTREAM4  0x3DUL

typedef struct {
    __IO uint32_t CCR;
} DMAMUX_Channel_TypeDef;

#define DMAMUX1_Channel3    ((DMAMUX_Channel_TypeDef *)0x4002080CUL)
#define DMAMUX1_Channel4    ((DMAMUX_Channel_TypeDef *)0x40020810UL)
#define DMA_REQUEST_SPI2_RX 39U
#define DMA_REQUEST_SPI2_TX 40U

/* -------------------------------------------------- general purpose timers */
/* TIM2..TIM4 keep the F4's layout for every field Freya uses. */
typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t SMCR;
    __IO uint32_t DIER;
    __IO uint32_t SR;
    __IO uint32_t EGR;
    __IO uint32_t CCMR1;
    __IO uint32_t CCMR2;
    __IO uint32_t CCER;
    __IO uint32_t CNT;
    __IO uint32_t PSC;
    __IO uint32_t ARR;
    __IO uint32_t RCR;               /* reserved on TIM2..TIM4          */
    __IO uint32_t CCR[4];            /* the four compare channels       */
} TIM_TypeDef;

#define TIM2                ((TIM_TypeDef *)0x40000000UL)
#define TIM3                ((TIM_TypeDef *)0x40000400UL)
#define TIM4                ((TIM_TypeDef *)0x40000800UL)

#define TIM_CR1_CEN         (1UL << 0)
#define TIM_CR1_UDIS        (1UL << 1)
#define TIM_CR1_URS         (1UL << 2)   /* only an overflow interrupts */
#define TIM_CR1_OPM         (1UL << 3)   /* one pulse: stop after it    */
#define TIM_CR1_ARPE        (1UL << 7)
#define TIM_DIER_UIE        (1UL << 0)
#define TIM_SR_UIF          (1UL << 0)
#define TIM_EGR_UG          (1UL << 0)   /* load PSC and ARR now        */

#define TIM_CCMR_PWM1       0x68UL
#define TIM_CCMR_SHIFT(ch)  ((((ch) - 1) & 1U) * 8U)
#define TIM_CCER_CCE(ch)    (1UL << (((ch) - 1) * 4))

/* ------------------------------------------------------- Cortex-M core */
/* The Cortex-M7's system control block. */
typedef struct {
    __IO uint32_t CPUID;
    __IO uint32_t ICSR;
    __IO uint32_t VTOR;
    __IO uint32_t AIRCR;
    __IO uint32_t SCR;
    __IO uint32_t CCR;
    __IO uint8_t  SHPR[12];
    __IO uint32_t SHCSR;
    __IO uint32_t CFSR;
    __IO uint32_t HFSR;
    __IO uint32_t DFSR;
    __IO uint32_t MMFAR;
    __IO uint32_t BFAR;
    __IO uint32_t AFSR;
} SCB_TypeDef;

#define SCB                 ((SCB_TypeDef *)0xE000ED00UL)
#define SCB_SHCSR_USGFAULTENA (1UL << 18)
#define SCB_SHCSR_BUSFAULTENA (1UL << 17)
#define SCB_SHCSR_MEMFAULTENA (1UL << 16)
#define SCB_CCR_DIV_0_TRP   (1UL << 4)
#define SCB_CCR_STKALIGN    (1UL << 9)
#define SCB_CCR_IC          (1UL << 17)  /* instruction cache on        */
#define SCB_AIRCR_SYSRESETREQ 0x05FA0004UL

typedef struct {
    __IO uint32_t CTRL;
    __IO uint32_t LOAD;
    __IO uint32_t VAL;
    __IO uint32_t CALIB;
} SysTick_TypeDef;

#define SysTick             ((SysTick_TypeDef *)0xE000E010UL)
#define SysTick_CTRL_ENABLE (1UL << 0)
#define SysTick_CTRL_TICKINT (1UL << 1)
#define SysTick_CTRL_CLKSRC (1UL << 2)

typedef struct {
    __IO uint32_t ISER[16];
    uint32_t      RES0[16];
    __IO uint32_t ICER[16];
    uint32_t      RES1[16];
    __IO uint32_t ISPR[16];
    uint32_t      RES2[16];
    __IO uint32_t ICPR[16];
    uint32_t      RES3[16];
    __IO uint32_t IABR[16];
    uint32_t      RES4[16];
    __IO uint32_t ITNS[16];
    uint32_t      RES5[16];
    __IO uint8_t  IP[480];
} NVIC_TypeDef;

#define NVIC                ((NVIC_TypeDef *)0xE000E100UL)
#define EXTI0_IRQn          6
#define EXTI1_IRQn          7
#define EXTI2_IRQn          8
#define EXTI3_IRQn          9
#define EXTI4_IRQn          10
#define DMA1_Stream3_IRQn   14
#define DMA1_Stream4_IRQn   15
#define EXTI9_5_IRQn        23
#define TIM2_IRQn           28
#define TIM3_IRQn           29
#define TIM4_IRQn           30
#define USART2_IRQn         38
#define EXTI15_10_IRQn      40

/* DBGMCU is on the debug bus; the identity words are in system memory. */
#define DBGMCU_IDCODE       (*(__IO uint32_t *)0x5C001000UL)
#define UID_BASE            0x1FF1E800UL
#define FLASHSIZE_BASE      0x1FF1E880UL

static inline void nvic_enable(int irq)
{
    NVIC->ISER[irq >> 5] = 1UL << (irq & 0x1F);
}

static inline void nvic_disable(int irq)
{
    NVIC->ICER[irq >> 5] = 1UL << (irq & 0x1F);
    NVIC->ICPR[irq >> 5] = 1UL << (irq & 0x1F);
}

static inline void nvic_set_priority(int irq, uint8_t prio)
{
    NVIC->IP[irq] = (uint8_t)(prio << 4);
}

static inline void __dsb(void)  { __asm volatile ("dsb 0xF" ::: "memory"); }
static inline void __isb(void)  { __asm volatile ("isb 0xF" ::: "memory"); }
static inline void __wfi(void)  { __asm volatile ("wfi"); }
static inline void __nop(void)  { __asm volatile ("nop"); }
static inline void irq_disable(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void irq_enable(void)  { __asm volatile ("cpsie i" ::: "memory"); }

static inline uint32_t irq_save(void)
{
    uint32_t pm;
    __asm volatile ("mrs %0, primask" : "=r"(pm));
    __asm volatile ("cpsid i" ::: "memory");
    return pm;
}

static inline void irq_restore(uint32_t pm)
{
    __asm volatile ("msr primask, %0" :: "r"(pm) : "memory");
}

/*
 * The Cortex-M7's instruction cache.  Code that was just written as data
 * - a program loaded into RAM, the flash routines copied there, a program
 * installed in flash - has to be invalidated in it before it is fetched.
 * The data cache is left off, so DMA sees what the core wrote.
 */
#define SCB_ICIALLU         (*(__IO uint32_t *)0xE000EF50UL)

static inline void icache_invalidate(void)
{
    __dsb();
    __isb();
    SCB_ICIALLU = 0;
    __dsb();
    __isb();
}

static inline void icache_enable(void)
{
    icache_invalidate();
    SCB->CCR |= SCB_CCR_IC;
    __dsb();
    __isb();
}

/*
 * The cycle counter, which times sys_delay_us() on this core: the M7
 * dual-issues, so a nop loop's cycles a pass are not a constant.  On the
 * M7 the DWT has a lock that has to be opened before it can be set up.
 */
#define DEMCR               (*(__IO uint32_t *)0xE000EDFCUL)
#define DEMCR_TRCENA        (1UL << 24)
#define DWT_CTRL            (*(__IO uint32_t *)0xE0001000UL)
#define DWT_CTRL_CYCCNTENA  (1UL << 0)
#define DWT_CYCCNT          (*(__IO uint32_t *)0xE0001004UL)
#define DWT_LAR             (*(__IO uint32_t *)0xE0001FB0UL)
#define DWT_LAR_KEY         0xC5ACCE55UL

static inline void cyccnt_enable(void)
{
    DEMCR |= DEMCR_TRCENA;
    DWT_LAR = DWT_LAR_KEY;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}

#endif /* FREYA_STM32H723_H */
