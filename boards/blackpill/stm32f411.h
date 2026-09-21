/*
 * Freya - minimal STM32F411xE register definitions.
 * Only the peripherals Freya actually touches are described here; this
 * replaces CMSIS so the system stays dependency free.
 */
#ifndef FREYA_STM32F411_H
#define FREYA_STM32F411_H

#include <stdint.h>

#define __IO volatile

/* ---------------------------------------------------------------- RCC */
typedef struct {
    __IO uint32_t CR;          /* 0x00 */
    __IO uint32_t PLLCFGR;     /* 0x04 */
    __IO uint32_t CFGR;        /* 0x08 */
    __IO uint32_t CIR;         /* 0x0C */
    __IO uint32_t AHB1RSTR;    /* 0x10 */
    __IO uint32_t AHB2RSTR;    /* 0x14 */
    __IO uint32_t AHB3RSTR;    /* 0x18 */
    uint32_t      RES0;        /* 0x1C */
    __IO uint32_t APB1RSTR;    /* 0x20 */
    __IO uint32_t APB2RSTR;    /* 0x24 */
    uint32_t      RES1[2];     /* 0x28 */
    __IO uint32_t AHB1ENR;     /* 0x30 */
    __IO uint32_t AHB2ENR;     /* 0x34 */
    __IO uint32_t AHB3ENR;     /* 0x38 */
    uint32_t      RES2;        /* 0x3C */
    __IO uint32_t APB1ENR;     /* 0x40 */
    __IO uint32_t APB2ENR;     /* 0x44 */
    uint32_t      RES3[2];     /* 0x48 */
    __IO uint32_t AHB1LPENR;   /* 0x50 */
    __IO uint32_t AHB2LPENR;   /* 0x54 */
    __IO uint32_t AHB3LPENR;   /* 0x58 */
    uint32_t      RES4;        /* 0x5C */
    __IO uint32_t APB1LPENR;   /* 0x60 */
    __IO uint32_t APB2LPENR;   /* 0x64 */
    uint32_t      RES5[2];     /* 0x68 */
    __IO uint32_t BDCR;        /* 0x70 */
    __IO uint32_t CSR;         /* 0x74 */
    uint32_t      RES6[2];     /* 0x78 */
    __IO uint32_t SSCGR;       /* 0x80 */
    __IO uint32_t PLLI2SCFGR;  /* 0x84 */
    uint32_t      RES7;        /* 0x88 */
    __IO uint32_t DCKCFGR;     /* 0x8C */
} RCC_TypeDef;

#define RCC_BASE            0x40023800UL
#define RCC                 ((RCC_TypeDef *)RCC_BASE)

#define RCC_CR_HSION        (1UL << 0)
#define RCC_CR_HSIRDY       (1UL << 1)
#define RCC_CR_HSEON        (1UL << 16)
#define RCC_CR_HSERDY       (1UL << 17)
#define RCC_CR_HSEBYP       (1UL << 18)
#define RCC_CR_CSSON        (1UL << 19)
#define RCC_CR_PLLON        (1UL << 24)
#define RCC_CR_PLLRDY       (1UL << 25)

#define RCC_CFGR_SW_MASK    (3UL << 0)
#define RCC_CFGR_SW_PLL     (2UL << 0)
#define RCC_CFGR_SWS_MASK   (3UL << 2)
#define RCC_CFGR_SWS_PLL    (2UL << 2)
#define RCC_CFGR_HPRE_DIV1  (0UL << 4)
#define RCC_CFGR_PPRE1_DIV2 (4UL << 10)
#define RCC_CFGR_PPRE2_DIV1 (0UL << 13)

#define RCC_PLLCFGR_SRC_HSE (1UL << 22)

#define RCC_AHB1ENR_GPIOAEN (1UL << 0)
#define RCC_AHB1ENR_GPIOBEN (1UL << 1)
#define RCC_AHB1ENR_GPIOCEN (1UL << 2)
#define RCC_APB1ENR_USART2EN (1UL << 17)
#define RCC_APB1ENR_PWREN   (1UL << 28)
#define RCC_APB2ENR_SPI1EN  (1UL << 12)
#define RCC_APB2ENR_SYSCFGEN (1UL << 14)

#define RCC_CSR_RMVF        (1UL << 24)

/* ---------------------------------------------------------------- PWR */
typedef struct {
    __IO uint32_t CR;
    __IO uint32_t CSR;
} PWR_TypeDef;

#define PWR_BASE            0x40007000UL
#define PWR                 ((PWR_TypeDef *)PWR_BASE)
#define PWR_CR_VOS_SCALE1   (3UL << 14)
#define PWR_CR_VOS_MASK     (3UL << 14)
#define PWR_CSR_VOSRDY      (1UL << 14)

/* -------------------------------------------------------------- FLASH */
typedef struct {
    __IO uint32_t ACR;
    __IO uint32_t KEYR;
    __IO uint32_t OPTKEYR;
    __IO uint32_t SR;
    __IO uint32_t CR;
    __IO uint32_t OPTCR;
} FLASH_TypeDef;

#define FLASH_R_BASE        0x40023C00UL
#define FLASH_R             ((FLASH_TypeDef *)FLASH_R_BASE)
#define FLASH_ACR_LATENCY(n) ((uint32_t)(n) & 0xF)
#define FLASH_ACR_PRFTEN    (1UL << 8)
#define FLASH_ACR_ICEN      (1UL << 9)
#define FLASH_ACR_DCEN      (1UL << 10)

/* --------------------------------------------------------------- GPIO */
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

#define GPIOA               ((GPIO_TypeDef *)0x40020000UL)
#define GPIOB               ((GPIO_TypeDef *)0x40020400UL)
#define GPIOC               ((GPIO_TypeDef *)0x40020800UL)

/* -------------------------------------------------------------- USART */
typedef struct {
    __IO uint32_t SR;
    __IO uint32_t DR;
    __IO uint32_t BRR;
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t CR3;
    __IO uint32_t GTPR;
} USART_TypeDef;

#define USART1              ((USART_TypeDef *)0x40011000UL)
#define USART2              ((USART_TypeDef *)0x40004400UL)
#define USART6              ((USART_TypeDef *)0x40011400UL)

#define USART_SR_PE         (1UL << 0)
#define USART_SR_FE         (1UL << 1)
#define USART_SR_NE         (1UL << 2)
#define USART_SR_ORE        (1UL << 3)
#define USART_SR_RXNE       (1UL << 5)
#define USART_SR_TC         (1UL << 6)
#define USART_SR_TXE        (1UL << 7)
#define USART_CR1_RXNEIE    (1UL << 5)
#define USART_CR1_TE        (1UL << 3)
#define USART_CR1_RE        (1UL << 2)
#define USART_CR1_UE        (1UL << 13)

/* ---------------------------------------------------------------- SPI */
typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t SR;
    __IO uint32_t DR;
    __IO uint32_t CRCPR;
    __IO uint32_t RXCRCR;
    __IO uint32_t TXCRCR;
    __IO uint32_t I2SCFGR;
    __IO uint32_t I2SPR;
} SPI_TypeDef;

#define SPI1                ((SPI_TypeDef *)0x40013000UL)

#define SPI_CR1_CPHA        (1UL << 0)
#define SPI_CR1_CPOL        (1UL << 1)
#define SPI_CR1_MSTR        (1UL << 2)
#define SPI_CR1_BR_MASK     (7UL << 3)
#define SPI_CR1_BR_SHIFT    3
#define SPI_CR1_SPE         (1UL << 6)
#define SPI_CR1_SSI         (1UL << 8)
#define SPI_CR1_SSM         (1UL << 9)
#define SPI_SR_RXNE         (1UL << 0)
#define SPI_SR_TXE          (1UL << 1)
#define SPI_SR_BSY          (1UL << 7)

/* ------------------------------------------------------- Cortex-M core */
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
    __IO uint32_t ISER[8];
    uint32_t      RES0[24];
    __IO uint32_t ICER[8];
    uint32_t      RES1[24];
    __IO uint32_t ISPR[8];
    uint32_t      RES2[24];
    __IO uint32_t ICPR[8];
    uint32_t      RES3[24];
    __IO uint32_t IABR[8];
    uint32_t      RES4[56];
    __IO uint8_t  IP[240];
} NVIC_TypeDef;

#define NVIC                ((NVIC_TypeDef *)0xE000E100UL)
#define USART2_IRQn         38

#define DBGMCU_IDCODE       (*(__IO uint32_t *)0xE0042000UL)
#define UID_BASE            0x1FFF7A10UL
#define FLASHSIZE_BASE      0x1FFF7A22UL

static inline void nvic_enable(int irq)
{
    NVIC->ISER[irq >> 5] = 1UL << (irq & 0x1F);
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

#endif /* FREYA_STM32F411_H */
