/*
 * Freya - minimal STM32U585xx register definitions.
 * Only the peripherals Freya actually touches are described here; this
 * replaces CMSIS so the system stays dependency free.  The registers
 * below match RM0456, at their non-secure addresses: Freya runs with
 * TrustZone off (TZEN = 0), which is how the chip ships.
 *
 * The U5 is a newer generation than the F1 and F4 the other boards use.
 * GPIO and the general purpose timers keep their old layout, so
 * src/gpio.c, src/timer.c and src/pwm.c drive them unchanged.  The
 * USART, SPI, EXTI, DMA, ADC and flash controllers are new designs; the
 * board header turns on the code paths in src/ that speak them
 * (BOARD_USART_ISR, BOARD_SPI_FIFO, BOARD_EXTI_SPLIT, BOARD_ESP_GPDMA).
 */
#ifndef FREYA_STM32U585_H
#define FREYA_STM32U585_H

#include <stdint.h>

#define __IO volatile

/* ---------------------------------------------------------------- RCC */
/*
 * The U5 splits the old enable registers in two.  APB1ENR1 is also given
 * the F4's name, APB1ENR, because the timer driver turns TIM2..TIM4 on
 * through it and their enable bits are the F4's bits 0..2.
 */
typedef struct {
    __IO uint32_t CR;          /* 0x00 */
    uint32_t      RES0;        /* 0x04 */
    __IO uint32_t ICSCR1;      /* 0x08 */
    __IO uint32_t ICSCR2;      /* 0x0C */
    __IO uint32_t ICSCR3;      /* 0x10 */
    __IO uint32_t CRRCR;       /* 0x14 */
    uint32_t      RES1;        /* 0x18 */
    __IO uint32_t CFGR1;       /* 0x1C */
    __IO uint32_t CFGR2;       /* 0x20 */
    __IO uint32_t CFGR3;       /* 0x24 */
    __IO uint32_t PLL1CFGR;    /* 0x28 */
    __IO uint32_t PLL2CFGR;    /* 0x2C */
    __IO uint32_t PLL3CFGR;    /* 0x30 */
    __IO uint32_t PLL1DIVR;    /* 0x34 */
    __IO uint32_t PLL1FRACR;   /* 0x38 */
    __IO uint32_t PLL2DIVR;    /* 0x3C */
    __IO uint32_t PLL2FRACR;   /* 0x40 */
    __IO uint32_t PLL3DIVR;    /* 0x44 */
    __IO uint32_t PLL3FRACR;   /* 0x48 */
    uint32_t      RES2;        /* 0x4C */
    __IO uint32_t CIER;        /* 0x50 */
    __IO uint32_t CIFR;        /* 0x54 */
    __IO uint32_t CICR;        /* 0x58 */
    uint32_t      RES3;        /* 0x5C */
    __IO uint32_t AHB1RSTR;    /* 0x60 */
    __IO uint32_t AHB2RSTR1;   /* 0x64 */
    __IO uint32_t AHB2RSTR2;   /* 0x68 */
    __IO uint32_t AHB3RSTR;    /* 0x6C */
    uint32_t      RES4;        /* 0x70 */
    __IO uint32_t APB1RSTR1;   /* 0x74 */
    __IO uint32_t APB1RSTR2;   /* 0x78 */
    __IO uint32_t APB2RSTR;    /* 0x7C */
    __IO uint32_t APB3RSTR;    /* 0x80 */
    uint32_t      RES5;        /* 0x84 */
    __IO uint32_t AHB1ENR;     /* 0x88 */
    __IO uint32_t AHB2ENR1;    /* 0x8C */
    __IO uint32_t AHB2ENR2;    /* 0x90 */
    __IO uint32_t AHB3ENR;     /* 0x94 */
    uint32_t      RES6;        /* 0x98 */
    union {
        __IO uint32_t APB1ENR1; /* 0x9C */
        __IO uint32_t APB1ENR;
    };
    __IO uint32_t APB1ENR2;    /* 0xA0 */
    __IO uint32_t APB2ENR;     /* 0xA4 */
    __IO uint32_t APB3ENR;     /* 0xA8 */
    uint32_t      RES7[13];    /* 0xAC .. 0xDC */
    __IO uint32_t CCIPR1;      /* 0xE0 */
    __IO uint32_t CCIPR2;      /* 0xE4 */
    __IO uint32_t CCIPR3;      /* 0xE8 */
    uint32_t      RES8;        /* 0xEC */
    __IO uint32_t BDCR;        /* 0xF0 */
    __IO uint32_t CSR;         /* 0xF4 */
} RCC_TypeDef;

#define RCC                 ((RCC_TypeDef *)0x46020C00UL)

#define RCC_CR_MSISON       (1UL << 0)
#define RCC_CR_HSION        (1UL << 8)
#define RCC_CR_HSIRDY       (1UL << 10)
#define RCC_CR_HSEON        (1UL << 16)
#define RCC_CR_HSERDY       (1UL << 17)
#define RCC_CR_CSSON        (1UL << 19)
#define RCC_CR_PLL1ON       (1UL << 24)
#define RCC_CR_PLL1RDY      (1UL << 25)

#define RCC_CFGR1_SW_MASK   (3UL << 0)
#define RCC_CFGR1_SW_PLL    (3UL << 0)
#define RCC_CFGR1_SWS_MASK  (3UL << 2)
#define RCC_CFGR1_SWS_PLL   (3UL << 2)
#define RCC_CFGR2_HPRE_DIV1 (0UL << 0)
#define RCC_CFGR2_PPRE1_DIV2 (4UL << 4)
#define RCC_CFGR2_PPRE2_DIV2 (4UL << 8)
#define RCC_CFGR2_PRE_MASK  0x0000077FUL
#define RCC_CFGR3_PPRE3_MASK (7UL << 4)

#define RCC_PLL1CFGR_SRC_HSI (2UL << 0)
#define RCC_PLL1CFGR_SRC_HSE (3UL << 0)
#define RCC_PLL1CFGR_RGE_4_8 (0UL << 2)  /* input 4..8 MHz            */
#define RCC_PLL1CFGR_M(n)   ((((uint32_t)(n) - 1U) & 0xFUL) << 8)
#define RCC_PLL1CFGR_MBOOST(code) (((uint32_t)(code) & 0xFUL) << 12)
#define RCC_PLL1CFGR_REN    (1UL << 18)
#define RCC_PLL1DIVR_N(n)   (((uint32_t)(n) - 1U) & 0x1FFUL)
#define RCC_PLL1DIVR_P(n)   ((((uint32_t)(n) - 1U) & 0x7FUL) << 9)
#define RCC_PLL1DIVR_Q(n)   ((((uint32_t)(n) - 1U) & 0x7FUL) << 16)
#define RCC_PLL1DIVR_R(n)   ((((uint32_t)(n) - 1U) & 0x7FUL) << 24)

#define RCC_CICR_CSSC       (1UL << 10)

#define RCC_AHB1ENR_GPDMA1EN (1UL << 0)
#define RCC_AHB2ENR1_GPIOAEN (1UL << 0)
#define RCC_AHB2ENR1_GPIOBEN (1UL << 1)
#define RCC_AHB2ENR1_GPIOCEN (1UL << 2)
#define RCC_AHB2ENR1_ADC12EN (1UL << 10)
#define RCC_AHB3ENR_PWREN   (1UL << 2)
#define RCC_APB1ENR_TIM2EN  (1UL << 0)
#define RCC_APB1ENR_TIM3EN  (1UL << 1)
#define RCC_APB1ENR_TIM4EN  (1UL << 2)
#define RCC_APB1ENR_SPI2EN  (1UL << 14)
#define RCC_APB1ENR_USART2EN (1UL << 17)
#define RCC_APB2ENR_SPI1EN  (1UL << 12)
#define RCC_APB3ENR_RTCAPBEN (1UL << 21)

/* The kernel clock of ADC1: HCLK, divided again in ADC12_COMMON->CCR. */
#define RCC_CCIPR3_ADCDACSEL_MASK (7UL << 12)

/* Reset flags.  BORRSTF (27) is set by a power-on as well, which is where
 * src/system.c looks for one.  Bit 25 is the option byte loader here, not
 * the F4's brown-out, so a reset after an option byte change is reported
 * as a brown-out. */
#define RCC_CSR_RMVF        (1UL << 23)

/* ---------------------------------------------------------------- PWR */
typedef struct {
    __IO uint32_t CR1;         /* 0x00 */
    __IO uint32_t CR2;         /* 0x04 */
    __IO uint32_t CR3;         /* 0x08 */
    __IO uint32_t VOSR;        /* 0x0C */
    __IO uint32_t SVMCR;       /* 0x10 */
    __IO uint32_t WUCR1;       /* 0x14 */
    __IO uint32_t WUCR2;       /* 0x18 */
    __IO uint32_t WUCR3;       /* 0x1C */
    __IO uint32_t BDCR1;       /* 0x20 */
    __IO uint32_t BDCR2;       /* 0x24 */
    __IO uint32_t DBPR;        /* 0x28 */
} PWR_TypeDef;

#define PWR                 ((PWR_TypeDef *)0x46020800UL)
#define PWR_VOSR_BOOSTRDY   (1UL << 14)
#define PWR_VOSR_VOSRDY     (1UL << 15)
#define PWR_VOSR_VOS_MASK   (3UL << 16)
#define PWR_VOSR_VOS_RANGE1 (3UL << 16)
#define PWR_VOSR_BOOSTEN    (1UL << 18)
#define PWR_SVMCR_ASV       (1UL << 30)  /* VDDA valid: the ADC may run */
#define PWR_DBPR_DBP        (1UL << 0)   /* backup domain writable      */

/* -------------------------------------------------------------- FLASH */
typedef struct {
    __IO uint32_t ACR;         /* 0x00 */
    uint32_t      RES0;        /* 0x04 */
    __IO uint32_t NSKEYR;      /* 0x08 */
    __IO uint32_t SECKEYR;     /* 0x0C */
    __IO uint32_t OPTKEYR;     /* 0x10 */
    uint32_t      RES1;        /* 0x14 */
    __IO uint32_t PDKEY1R;     /* 0x18 */
    __IO uint32_t PDKEY2R;     /* 0x1C */
    __IO uint32_t NSSR;        /* 0x20 */
    __IO uint32_t SECSR;       /* 0x24 */
    __IO uint32_t NSCR;        /* 0x28 */
    __IO uint32_t SECCR;       /* 0x2C */
    __IO uint32_t ECCR;        /* 0x30 */
    __IO uint32_t OPSR;        /* 0x34 */
    uint32_t      RES2[2];     /* 0x38 */
    __IO uint32_t OPTR;        /* 0x40 */
} FLASH_TypeDef;

#define FLASH_R             ((FLASH_TypeDef *)0x40022000UL)
#define FLASH_ACR_LATENCY(n) ((uint32_t)(n) & 0xF)
#define FLASH_ACR_PRFTEN    (1UL << 8)

/* 2 MiB in two banks of 1 MiB, 8 KiB pages, programmed a quad-word
 * (16 bytes) at a time.  Each quad-word carries ECC and may be written
 * once between erases. */
#define FLASH_BANK2_BASE    0x08100000UL
#define FLASH_QUADWORD      16U

#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL

#define FLASH_SR_EOP        (1UL << 0)
#define FLASH_SR_OPERR      (1UL << 1)
#define FLASH_SR_PROGERR    (1UL << 3)
#define FLASH_SR_WRPERR     (1UL << 4)
#define FLASH_SR_PGAERR     (1UL << 5)
#define FLASH_SR_SIZERR     (1UL << 6)
#define FLASH_SR_PGSERR     (1UL << 7)
#define FLASH_SR_OPTWERR    (1UL << 13)
#define FLASH_SR_BSY        (1UL << 16)
#define FLASH_SR_WDW        (1UL << 17)
#define FLASH_SR_ERRORS     (FLASH_SR_OPERR | FLASH_SR_PROGERR | \
                             FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                             FLASH_SR_SIZERR | FLASH_SR_PGSERR | \
                             FLASH_SR_OPTWERR)

#define FLASH_CR_PG         (1UL << 0)
#define FLASH_CR_PER        (1UL << 1)
#define FLASH_CR_PNB(n)     (((uint32_t)(n) & 0x7FUL) << 3)
#define FLASH_CR_PNB_MASK   (0x7FUL << 3)
#define FLASH_CR_BKER       (1UL << 11)
#define FLASH_CR_STRT       (1UL << 16)
#define FLASH_CR_LOCK       (1UL << 31)

/* ------------------------------------------------------------- ICACHE */
/* Caches instruction fetch from flash.  It is invalidated after the
 * program region is written, before the new code is fetched. */
typedef struct {
    __IO uint32_t CR;
    __IO uint32_t SR;
    __IO uint32_t IER;
    __IO uint32_t FCR;
} ICACHE_TypeDef;

#define ICACHE              ((ICACHE_TypeDef *)0x40030400UL)
#define ICACHE_CR_EN        (1UL << 0)
#define ICACHE_CR_CACHEINV  (1UL << 1)
#define ICACHE_SR_BUSYF     (1UL << 0)
#define ICACHE_SR_BSYENDF   (1UL << 1)
#define ICACHE_FCR_CBSYENDF (1UL << 1)

/* --------------------------------------------------------------- GPIO */
/* The F4's layout, so board.c configures pins the way the Black Pill's
 * does.  BRR, HSLVR and SECCFGR follow AFR and are not used. */
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

#define GPIOA               ((GPIO_TypeDef *)0x42020000UL)
#define GPIOB               ((GPIO_TypeDef *)0x42020400UL)
#define GPIOC               ((GPIO_TypeDef *)0x42020800UL)

/* ---------------------------------------------------------------- ADC */
/* ADC1 is the 14-bit converter.  Freya runs it at 12 bits, the width
 * every other board returns. */
typedef struct {
    __IO uint32_t ISR;         /* 0x00 */
    __IO uint32_t IER;         /* 0x04 */
    __IO uint32_t CR;          /* 0x08 */
    __IO uint32_t CFGR1;       /* 0x0C */
    __IO uint32_t CFGR2;       /* 0x10 */
    __IO uint32_t SMPR1;       /* 0x14 */
    __IO uint32_t SMPR2;       /* 0x18 */
    __IO uint32_t PCSEL;       /* 0x1C */
    __IO uint32_t AWD1TR;      /* 0x20 */
    __IO uint32_t AWD2TR;      /* 0x24 */
    __IO uint32_t CHSELR;      /* 0x28 */
    __IO uint32_t AWD3TR;      /* 0x2C */
    __IO uint32_t SQR1;        /* 0x30 */
    __IO uint32_t SQR2;        /* 0x34 */
    __IO uint32_t SQR3;        /* 0x38 */
    __IO uint32_t SQR4;        /* 0x3C */
    __IO uint32_t DR;          /* 0x40 */
} ADC_TypeDef;

typedef struct {
    __IO uint32_t CCR;
} ADC_Common_TypeDef;

#define ADC1                ((ADC_TypeDef *)0x42028000UL)
#define ADC_COMMON          ((ADC_Common_TypeDef *)0x42028308UL)

#define ADC_ISR_ADRDY       (1UL << 0)
#define ADC_ISR_EOC         (1UL << 2)
#define ADC_ISR_LDORDY      (1UL << 12)
#define ADC_CR_ADEN         (1UL << 0)
#define ADC_CR_ADSTART      (1UL << 2)
#define ADC_CR_ADVREGEN     (1UL << 28)
#define ADC_CR_DEEPPWD      (1UL << 29)
#define ADC_CR_ADCAL        (1UL << 31)
#define ADC_CFGR1_RES_12    (1UL << 2)
#define ADC_CFGR1_RES_MASK  (3UL << 2)
#define ADC_CCR_PRESC_DIV4  (2UL << 18)   /* 160 / 4 = 40 MHz          */
#define ADC_CCR_PRESC_MASK  (0xFUL << 18)
#define ADC_CCR_VREFEN      (1UL << 22)
#define ADC_CCR_VSENSEEN    (1UL << 23)
#define ADC_SAMPLE_LONG     7UL           /* 814.5 cycles              */

/* --------------------------------------------------------------- EXTI */
/*
 * Sixteen external interrupt lines, one per pin number, each with an
 * interrupt of its own.  The port a line listens to is chosen here, in
 * EXTICR, not in SYSCFG; a rising and a falling edge set separate pending
 * bits, each cleared by writing it.
 */
typedef struct {
    __IO uint32_t RTSR1;       /* 0x00 */
    __IO uint32_t FTSR1;       /* 0x04 */
    __IO uint32_t SWIER1;      /* 0x08 */
    __IO uint32_t RPR1;        /* 0x0C */
    __IO uint32_t FPR1;        /* 0x10 */
    __IO uint32_t SECCFGR1;    /* 0x14 */
    __IO uint32_t PRIVCFGR1;   /* 0x18 */
    uint32_t      RES0[17];    /* 0x1C */
    __IO uint32_t EXTICR[4];   /* 0x60 */
    __IO uint32_t LOCKR;       /* 0x70 */
    uint32_t      RES1[3];     /* 0x74 */
    __IO uint32_t IMR1;        /* 0x80 */
    __IO uint32_t EMR1;        /* 0x84 */
} EXTI_TypeDef;

#define EXTI                ((EXTI_TypeDef *)0x46022000UL)

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

/* -------------------------------------------------------------- GPDMA */
/* One channel of GPDMA1, used in its simplest form: a single block, no
 * linked list.  SPI2 receives on channel 0 and transmits on channel 1. */
typedef struct {
    __IO uint32_t CLBAR;       /* 0x00 */
    uint32_t      RES0[2];     /* 0x04 */
    __IO uint32_t CFCR;        /* 0x0C */
    __IO uint32_t CSR;         /* 0x10 */
    __IO uint32_t CCR;         /* 0x14 */
    uint32_t      RES1[10];    /* 0x18 */
    __IO uint32_t CTR1;        /* 0x40 */
    __IO uint32_t CTR2;        /* 0x44 */
    __IO uint32_t CBR1;        /* 0x48 */
    __IO uint32_t CSAR;        /* 0x4C */
    __IO uint32_t CDAR;        /* 0x50 */
    __IO uint32_t CTR3;        /* 0x54 */
    __IO uint32_t CBR2;        /* 0x58 */
    uint32_t      RES2[8];     /* 0x5C */
    __IO uint32_t CLLR;        /* 0x7C */
} DMA_Channel_TypeDef;

#define GPDMA1_Channel0     ((DMA_Channel_TypeDef *)0x40020050UL)
#define GPDMA1_Channel1     ((DMA_Channel_TypeDef *)0x400200D0UL)

#define DMA_CCR_EN          (1UL << 0)
#define DMA_CCR_RESET       (1UL << 1)
#define DMA_CCR_SUSP        (1UL << 2)
#define DMA_CCR_TCIE        (1UL << 8)
#define DMA_CCR_DTEIE       (1UL << 10)
#define DMA_CCR_ULEIE       (1UL << 11)
#define DMA_CCR_USEIE       (1UL << 12)
#define DMA_CCR_PRIO_HIGH   (3UL << 22)
#define DMA_CSR_IDLEF       (1UL << 0)
#define DMA_CSR_TCF         (1UL << 8)
#define DMA_CSR_ERRORS      (7UL << 10)   /* DTEF | ULEF | USEF          */
#define DMA_CFCR_ALL        (0x7FUL << 8)
#define DMA_CTR1_SINC       (1UL << 3)
#define DMA_CTR1_DINC       (1UL << 19)
#define DMA_CTR2_DREQ       (1UL << 10)   /* the request is the target's */
#define GPDMA1_REQUEST_SPI2_RX 8U
#define GPDMA1_REQUEST_SPI2_TX 9U

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
/* The Cortex-M33's system control block keeps the v7-M layout for every
 * register Freya reads. */
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
#define SCB_CCR_STKALIGN    (1UL << 9)   /* RES1 on v8-M: a no-op       */
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
#define EXTI0_IRQn          11            /* EXTI n is 11 + n           */
#define EXTI1_IRQn          12
#define EXTI2_IRQn          13
#define EXTI3_IRQn          14
#define EXTI4_IRQn          15
#define GPDMA1_Channel0_IRQn 29
#define GPDMA1_Channel1_IRQn 30
#define TIM2_IRQn           45
#define TIM3_IRQn           46
#define TIM4_IRQn           47
#define USART2_IRQn         62

/* The DBGMCU block moved with the v8-M debug map.  The identity words
 * are in the system memory's engineering area. */
#define DBGMCU_IDCODE       (*(__IO uint32_t *)0xE0044000UL)
#define UID_BASE            0x0BFA0700UL
#define FLASHSIZE_BASE      0x0BFA07A0UL

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

#endif /* FREYA_STM32U585_H */
