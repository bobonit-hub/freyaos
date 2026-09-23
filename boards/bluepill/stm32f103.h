/*
 * Freya - minimal STM32F103xB register definitions.
 * Only the peripherals Freya actually touches are described here; this
 * replaces CMSIS so the system stays dependency free.
 *
 * The F1 is an older design than the F4: the RCC has a single PLL
 * multiplier instead of the M/N/P divider chain, the flash controller has
 * no caches, and GPIO pins are configured through two four-bit-per-pin
 * registers (CRL, CRH) rather than MODER/OTYPER/OSPEEDR/PUPDR/AFR.  The
 * core registers are the same, minus the FPU.
 */
#ifndef FREYA_STM32F103_H
#define FREYA_STM32F103_H

#include <stdint.h>

#define __IO volatile

/* ---------------------------------------------------------------- RCC */
typedef struct {
    __IO uint32_t CR;          /* 0x00 */
    __IO uint32_t CFGR;        /* 0x04 */
    __IO uint32_t CIR;         /* 0x08 */
    __IO uint32_t APB2RSTR;    /* 0x0C */
    __IO uint32_t APB1RSTR;    /* 0x10 */
    __IO uint32_t AHBENR;      /* 0x14 */
    __IO uint32_t APB2ENR;     /* 0x18 */
    __IO uint32_t APB1ENR;     /* 0x1C */
    __IO uint32_t BDCR;        /* 0x20 */
    __IO uint32_t CSR;         /* 0x24 */
} RCC_TypeDef;

#define RCC_BASE            0x40021000UL
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
#define RCC_CFGR_PPRE1_DIV2 (4UL << 8)
#define RCC_CFGR_PPRE2_DIV1 (0UL << 11)
#define RCC_CFGR_ADCPRE_DIV6 (2UL << 14)
#define RCC_CFGR_PLLSRC_HSE (1UL << 16)
#define RCC_CFGR_PLLXTPRE_DIV1 (0UL << 17)
#define RCC_CFGR_PLLMUL(n)  (((uint32_t)(n) - 2UL) << 18)   /* n = 2..16 */
#define RCC_CFGR_CLKMASK    0x003FFFF0UL   /* HPRE..PLLMUL, all we set  */

#define RCC_APB2ENR_AFIOEN  (1UL << 0)
#define RCC_APB2ENR_IOPAEN  (1UL << 2)
#define RCC_APB2ENR_IOPBEN  (1UL << 3)
#define RCC_APB2ENR_IOPCEN  (1UL << 4)
#define RCC_APB2ENR_ADC1EN  (1UL << 9)
#define RCC_APB2ENR_SPI1EN  (1UL << 12)
#define RCC_APB2ENR_USART1EN (1UL << 14)
#define RCC_APB1ENR_TIM2EN  (1UL << 0)
#define RCC_APB1ENR_TIM3EN  (1UL << 1)
#define RCC_APB1ENR_TIM4EN  (1UL << 2)
#define RCC_APB1ENR_I2C1EN  (1UL << 21)
#define RCC_APB1ENR_I2C2EN  (1UL << 22)
#define RCC_APB1ENR_SPI2EN  (1UL << 14)
#define RCC_APB1ENR_USART2EN (1UL << 17)
#define RCC_APB1ENR_PWREN   (1UL << 28)

#define RCC_CSR_RMVF        (1UL << 24)

/* -------------------------------------------------------------- FLASH */
typedef struct {
    __IO uint32_t ACR;
    __IO uint32_t KEYR;
    __IO uint32_t OPTKEYR;
    __IO uint32_t SR;
    __IO uint32_t CR;
    __IO uint32_t AR;
    uint32_t      RES0;
    __IO uint32_t OBR;
    __IO uint32_t WRPR;
} FLASH_TypeDef;

#define FLASH_R_BASE        0x40022000UL
#define FLASH_R             ((FLASH_TypeDef *)FLASH_R_BASE)
#define FLASH_ACR_LATENCY(n) ((uint32_t)(n) & 0x7)
#define FLASH_ACR_PRFTBE    (1UL << 4)

/* The embedded flash programming interface.  The F1 erases in 1 KiB pages
 * and programs a halfword at a time; there is no byte or word write. */
#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL

#define FLASH_SR_BSY        (1UL << 0)
#define FLASH_SR_PGERR      (1UL << 2)
#define FLASH_SR_WRPRTERR   (1UL << 4)
#define FLASH_SR_EOP        (1UL << 5)

#define FLASH_CR_PG         (1UL << 0)
#define FLASH_CR_PER        (1UL << 1)
#define FLASH_CR_MER        (1UL << 2)
#define FLASH_CR_STRT       (1UL << 6)
#define FLASH_CR_LOCK       (1UL << 7)

/* --------------------------------------------------------------- GPIO */
typedef struct {
    __IO uint32_t CRL;         /* pins 0..7,  four bits each */
    __IO uint32_t CRH;         /* pins 8..15, four bits each */
    __IO uint32_t IDR;
    __IO uint32_t ODR;
    __IO uint32_t BSRR;        /* set in 0..15, reset in 16..31 */
    __IO uint32_t BRR;
    __IO uint32_t LCKR;
} GPIO_TypeDef;

#define GPIOA               ((GPIO_TypeDef *)0x40010800UL)
#define GPIOB               ((GPIO_TypeDef *)0x40010C00UL)
#define GPIOC               ((GPIO_TypeDef *)0x40011000UL)
#define GPIOD               ((GPIO_TypeDef *)0x40011400UL)

/*
 * The CRL/CRH nibble for one pin: CNF in the top two bits, MODE in the
 * bottom two.  MODE 00 is input, 01/10/11 are outputs capped at 10, 2 and
 * 50 MHz.  For a pulled input the direction comes from ODR: 1 = pull-up.
 */
#define GPIO_IN_ANALOG      0x0
#define GPIO_IN_FLOATING    0x4
#define GPIO_IN_PULL        0x8
#define GPIO_OUT_PP_2M      0x2
#define GPIO_OUT_PP_50M     0x3
#define GPIO_OUT_OD_50M     0x7
#define GPIO_AF_PP_50M      0xB
#define GPIO_AF_OD_50M      0xF

static inline void gpio_config(GPIO_TypeDef *port, int pin, uint32_t cfg)
{
    __IO uint32_t *cr = (pin < 8) ? &port->CRL : &port->CRH;
    int shift = (pin & 7) * 4;

    *cr = (*cr & ~(0xFUL << shift)) | ((cfg & 0xFUL) << shift);
}

/* ---------------------------------------------------------------- ADC */
typedef struct {
    __IO uint32_t SR;
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t SMPR1;
    __IO uint32_t SMPR2;
    __IO uint32_t JOFR[4];
    __IO uint32_t HTR;
    __IO uint32_t LTR;
    __IO uint32_t SQR1;
    __IO uint32_t SQR2;
    __IO uint32_t SQR3;
    __IO uint32_t JSQR;
    __IO uint32_t JDR[4];
    __IO uint32_t DR;
} ADC_TypeDef;

#define ADC1                ((ADC_TypeDef *)0x40012400UL)

#define ADC_SR_EOC          (1UL << 1)
#define ADC_CR2_ADON        (1UL << 0)
#define ADC_CR2_CAL         (1UL << 2)
#define ADC_CR2_RSTCAL      (1UL << 3)
#define ADC_CR2_EXTSEL_SW   (7UL << 17)
#define ADC_CR2_EXTTRIG     (1UL << 20)
#define ADC_CR2_SWSTART     (1UL << 22)
#define ADC_CR2_TSVREFE     (1UL << 23)
#define ADC_SAMPLE_LONG     7UL

/* ---------------------------------------------------------- AFIO / EXTI */
/* Sixteen external interrupt lines, one per pin number; AFIO->EXTICR
 * decides which port's pin n drives line n.  The F4 does the same job in
 * SYSCFG, which is the only difference between the two EXTI drivers. */
typedef struct {
    __IO uint32_t EVCR;
    __IO uint32_t MAPR;
    __IO uint32_t EXTICR[4];
    uint32_t      RES0;
    __IO uint32_t MAPR2;
} AFIO_TypeDef;

#define AFIO                ((AFIO_TypeDef *)0x40010000UL)

typedef struct {
    __IO uint32_t IMR;
    __IO uint32_t EMR;
    __IO uint32_t RTSR;
    __IO uint32_t FTSR;
    __IO uint32_t SWIER;
    __IO uint32_t PR;          /* write 1 to clear */
} EXTI_TypeDef;

#define EXTI                ((EXTI_TypeDef *)0x40010400UL)

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

#define USART1              ((USART_TypeDef *)0x40013800UL)
#define USART2              ((USART_TypeDef *)0x40004400UL)
#define USART3              ((USART_TypeDef *)0x40004800UL)

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
} SPI_TypeDef;

#define SPI1                ((SPI_TypeDef *)0x40013000UL)
#define SPI2                ((SPI_TypeDef *)0x40003800UL)

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

/* --------------------------------------------------------------- I2C */
/* The original STM32 I2C.  The baud rate is CCR, a transfer is one byte
 * at a time, and a two-byte read is a special case of the ACK bit. */
typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t OAR1;
    __IO uint32_t OAR2;
    __IO uint32_t DR;
    __IO uint32_t SR1;
    __IO uint32_t SR2;
    __IO uint32_t CCR;
    __IO uint32_t TRISE;
} I2C_TypeDef;

#define I2C1                ((I2C_TypeDef *)0x40005400UL)
#define I2C2                ((I2C_TypeDef *)0x40005800UL)

#define I2C_CR1_PE          (1UL << 0)
#define I2C_CR1_START       (1UL << 8)
#define I2C_CR1_STOP        (1UL << 9)
#define I2C_CR1_ACK         (1UL << 10)
#define I2C_CR1_POS         (1UL << 11)

#define I2C_SR1_SB          (1UL << 0)
#define I2C_SR1_ADDR        (1UL << 1)
#define I2C_SR1_BTF         (1UL << 2)
#define I2C_SR1_RXNE        (1UL << 6)
#define I2C_SR1_TXE         (1UL << 7)
#define I2C_SR1_BERR        (1UL << 8)
#define I2C_SR1_ARLO        (1UL << 9)
#define I2C_SR1_AF          (1UL << 10)

#define I2C_SR2_BUSY        (1UL << 1)

#define I2C_CCR_FS          (1UL << 15)
#define I2C_OAR1_FIXED      (1UL << 14)   /* the manual says keep this set */

/* -------------------------------------------------- general purpose timers */
/* Only the fields a periodic interrupt and a PWM output need are
 * described.  TIM2..TIM4 live at the same addresses, with the same
 * layout, on the F1 and the F4 alike, which is why src/timer.c and
 * src/pwm.c are board independent. */
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

/* One channel of CCMR1 (channels 1 and 2) or CCMR2 (3 and 4) is a byte:
 * PWM mode 1 - the output is active while CNT is below CCR - with the
 * compare value preloaded, so a duty cycle written mid-period takes
 * effect at the next one rather than cutting the pulse short. */
#define TIM_CCMR_PWM1       0x68UL
#define TIM_CCMR_SHIFT(ch)  ((((ch) - 1) & 1U) * 8U)
#define TIM_CCER_CCE(ch)    (1UL << (((ch) - 1) * 4))

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
#define SCB_CCR_STKALIGN    (1UL << 9)   /* zero out of reset on Cortex-M3 */
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
#define EXTI0_IRQn          6
#define EXTI1_IRQn          7
#define EXTI2_IRQn          8
#define EXTI3_IRQn          9
#define EXTI4_IRQn          10
#define EXTI9_5_IRQn        23
#define TIM2_IRQn           28
#define TIM3_IRQn           29
#define TIM4_IRQn           30
#define USART2_IRQn         38
#define EXTI15_10_IRQn      40

#define DBGMCU_IDCODE       (*(__IO uint32_t *)0xE0042000UL)
#define UID_BASE            0x1FFFF7E8UL
#define FLASHSIZE_BASE      0x1FFFF7E0UL

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
    NVIC->IP[irq] = (uint8_t)(prio << 4);   /* four priority bits */
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

#endif /* FREYA_STM32F103_H */
