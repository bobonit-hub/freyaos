/*
 * Freya - reset vector, interrupt vector table and C runtime bring-up
 * STM32H523xx, Cortex-M33 with FPU, TrustZone off
 */

    .syntax unified
    .cpu cortex-m33
    .fpu fpv5-sp-d16
    .thumb

    .global  g_pfnVectors
    .global  Reset_Handler

    .word  __etext
    .word  __data_start
    .word  __data_end
    .word  __bss_start
    .word  __bss_end

/* ------------------------------------------------------------------ */
    .section .isr_vector, "a", %progbits
    .type    g_pfnVectors, %object
g_pfnVectors:
    .word  __stack_top
    .word  Reset_Handler
    .word  NMI_Handler
    .word  HardFault_Handler
    .word  MemManage_Handler
    .word  BusFault_Handler
    .word  UsageFault_Handler
    .word  SecureFault_Handler
    .word  0
    .word  0
    .word  0
    .word  SVC_Handler
    .word  DebugMon_Handler
    .word  0
    .word  PendSV_Handler
    .word  SysTick_Handler

    /* External interrupts 0..132 */
    .word  WWDG_IRQHandler                   /*   0 */
    .word  PVD_AVD_IRQHandler
    .word  RTC_IRQHandler
    .word  RTC_S_IRQHandler
    .word  TAMP_IRQHandler
    .word  RAMCFG_IRQHandler
    .word  FLASH_IRQHandler
    .word  FLASH_S_IRQHandler
    .word  GTZC_IRQHandler
    .word  RCC_IRQHandler
    .word  RCC_S_IRQHandler                  /*  10 */
    .word  EXTI0_IRQHandler
    .word  EXTI1_IRQHandler
    .word  EXTI2_IRQHandler
    .word  EXTI3_IRQHandler
    .word  EXTI4_IRQHandler
    .word  EXTI5_IRQHandler
    .word  EXTI6_IRQHandler
    .word  EXTI7_IRQHandler
    .word  EXTI8_IRQHandler
    .word  EXTI9_IRQHandler                  /*  20 */
    .word  EXTI10_IRQHandler
    .word  EXTI11_IRQHandler
    .word  EXTI12_IRQHandler
    .word  EXTI13_IRQHandler
    .word  EXTI14_IRQHandler
    .word  EXTI15_IRQHandler
    .word  GPDMA1_Channel0_IRQHandler
    .word  GPDMA1_Channel1_IRQHandler
    .word  GPDMA1_Channel2_IRQHandler
    .word  GPDMA1_Channel3_IRQHandler        /*  30 */
    .word  GPDMA1_Channel4_IRQHandler
    .word  GPDMA1_Channel5_IRQHandler
    .word  GPDMA1_Channel6_IRQHandler
    .word  GPDMA1_Channel7_IRQHandler
    .word  IWDG_IRQHandler
    .word  0
    .word  ADC1_IRQHandler
    .word  DAC1_IRQHandler
    .word  FDCAN1_IT0_IRQHandler
    .word  FDCAN1_IT1_IRQHandler             /*  40 */
    .word  TIM1_BRK_IRQHandler
    .word  TIM1_UP_IRQHandler
    .word  TIM1_TRG_COM_IRQHandler
    .word  TIM1_CC_IRQHandler
    .word  TIM2_IRQHandler
    .word  TIM3_IRQHandler
    .word  TIM4_IRQHandler
    .word  TIM5_IRQHandler
    .word  TIM6_IRQHandler
    .word  TIM7_IRQHandler                   /*  50 */
    .word  I2C1_EV_IRQHandler
    .word  I2C1_ER_IRQHandler
    .word  I2C2_EV_IRQHandler
    .word  I2C2_ER_IRQHandler
    .word  SPI1_IRQHandler
    .word  SPI2_IRQHandler
    .word  SPI3_IRQHandler
    .word  USART1_IRQHandler
    .word  USART2_IRQHandler
    .word  USART3_IRQHandler                 /*  60 */
    .word  UART4_IRQHandler
    .word  UART5_IRQHandler
    .word  LPUART1_IRQHandler
    .word  LPTIM1_IRQHandler
    .word  TIM8_BRK_IRQHandler
    .word  TIM8_UP_IRQHandler
    .word  TIM8_TRG_COM_IRQHandler
    .word  TIM8_CC_IRQHandler
    .word  ADC2_IRQHandler
    .word  LPTIM2_IRQHandler                 /*  70 */
    .word  TIM15_IRQHandler
    .word  0
    .word  0
    .word  USB_DRD_FS_IRQHandler
    .word  CRS_IRQHandler
    .word  UCPD1_IRQHandler
    .word  FMC_IRQHandler
    .word  OCTOSPI1_IRQHandler
    .word  SDMMC1_IRQHandler
    .word  I2C3_EV_IRQHandler                /*  80 */
    .word  I2C3_ER_IRQHandler
    .word  SPI4_IRQHandler
    .word  0
    .word  0
    .word  USART6_IRQHandler
    .word  0
    .word  0
    .word  0
    .word  0
    .word  GPDMA2_Channel0_IRQHandler        /*  90 */
    .word  GPDMA2_Channel1_IRQHandler
    .word  GPDMA2_Channel2_IRQHandler
    .word  GPDMA2_Channel3_IRQHandler
    .word  GPDMA2_Channel4_IRQHandler
    .word  GPDMA2_Channel5_IRQHandler
    .word  GPDMA2_Channel6_IRQHandler
    .word  GPDMA2_Channel7_IRQHandler
    .word  0
    .word  0
    .word  0                                 /* 100 */
    .word  0
    .word  0
    .word  FPU_IRQHandler
    .word  ICACHE_IRQHandler
    .word  DCACHE1_IRQHandler
    .word  0
    .word  0
    .word  DCMI_PSSI_IRQHandler
    .word  FDCAN2_IT0_IRQHandler
    .word  FDCAN2_IT1_IRQHandler             /* 110 */
    .word  0
    .word  0
    .word  DTS_IRQHandler
    .word  RNG_IRQHandler
    .word  0
    .word  0
    .word  HASH_IRQHandler
    .word  PKA_IRQHandler
    .word  CEC_IRQHandler
    .word  TIM12_IRQHandler                  /* 120 */
    .word  0
    .word  0
    .word  I3C1_EV_IRQHandler
    .word  I3C1_ER_IRQHandler
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0                                 /* 130 */
    .word  I3C2_EV_IRQHandler
    .word  I3C2_ER_IRQHandler
    .size  g_pfnVectors, . - g_pfnVectors

/* ------------------------------------------------------------------ */
    .section .text.Reset_Handler
    .weak    Reset_Handler
    .type    Reset_Handler, %function
Reset_Handler:
    /* The boot ROM may leave the stack pointer anywhere - set it up first. */
    ldr   r0, =__stack_top
    mov   sp, r0

    /* Enable the FPU (CP10/CP11 full access) before any C code runs. */
    ldr   r0, =0xE000ED88
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)
    str   r1, [r0]
    dsb
    isb

    /* Copy .data from flash to SRAM. */
    ldr   r0, =__data_start
    ldr   r1, =__data_end
    ldr   r2, =__etext
    b     2f
1:  ldr   r3, [r2], #4
    str   r3, [r0], #4
2:  cmp   r0, r1
    bcc   1b

    /* Zero .bss. */
    movs  r3, #0
    ldr   r0, =__bss_start
    ldr   r1, =__bss_end
    b     4f
3:  str   r3, [r0], #4
4:  cmp   r0, r1
    bcc   3b

    /* Paint the stack so meminfo can report the high-water mark. */
    ldr   r0, =__stack_limit
    ldr   r1, =0xDEADBEEF
    mov   r2, sp
    sub   r2, r2, #64          /* keep clear of our own frame */
    b     6f
5:  str   r1, [r0], #4
6:  cmp   r0, r2
    bcc   5b

    /* Interrupts keep MSP, at the top of this region.  Thread mode,
     * the shell included, uses PSP, so a context switch never moves a
     * stack a handler is still running on. */
    ldr   r0, =__stack_top
    msr   msp, r0
    ldr   r0, =__thread_stack_top
    msr   psp, r0
    movs  r0, #2
    msr   control, r0
    isb

    bl    freya_main

    /* freya_main never returns; if it does, reset the MCU. */
    ldr   r0, =0xE000ED0C
    ldr   r1, =0x05FA0004
    str   r1, [r0]
    dsb
7:  b     7b
    .size Reset_Handler, . - Reset_Handler

/* ------------------------------------------------------------------ *
 * Fault handling
 *
 * Each of these captures the stack frame pointer of the interrupted
 * context and tail-calls into C.  Because we branch (never call) the
 * C routine, EXC_RETURN stays in LR and the C routine's return performs
 * the exception return - which lets it resume the thread at a different
 * PC by rewriting the stacked frame.
 * ------------------------------------------------------------------ */
    .section .text.fault_entries
    .thumb_func
    .global HardFault_Handler
HardFault_Handler:
    movs  r1, #3               /* FREYA_FAULT_HARD */
    b     fault_entry
    .thumb_func
    .global MemManage_Handler
MemManage_Handler:
    movs  r1, #4
    b     fault_entry
    .thumb_func
    .global BusFault_Handler
BusFault_Handler:
    movs  r1, #5
    b     fault_entry
    .thumb_func
    .global UsageFault_Handler
UsageFault_Handler:
    movs  r1, #6
    b     fault_entry
    .thumb_func
fault_entry:
    tst   lr, #4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    /* An extended (FPU) frame keeps r0-xPSR at the bottom, with s0-s15,
     * FPSCR and a pad stacked above them, so r0 points at it either way. */
    b     freya_fault_handler

/* PendSV lives in src/switch.S: it switches threads and aborts a run. */

/* ------------------------------------------------------------------ */
    .section .text.Default_Handler, "ax", %progbits
    .thumb_func
    .type  Default_Handler, %function
Default_Handler:
    b     .
    .size  Default_Handler, . - Default_Handler

    .macro def_irq_handler name
    .weak  \name
    .thumb_set \name, Default_Handler
    .endm

    def_irq_handler NMI_Handler
    def_irq_handler SVC_Handler
    def_irq_handler DebugMon_Handler
    def_irq_handler SecureFault_Handler
    def_irq_handler SysTick_Handler
    def_irq_handler WWDG_IRQHandler
    def_irq_handler PVD_AVD_IRQHandler
    def_irq_handler RTC_IRQHandler
    def_irq_handler RTC_S_IRQHandler
    def_irq_handler TAMP_IRQHandler
    def_irq_handler RAMCFG_IRQHandler
    def_irq_handler FLASH_IRQHandler
    def_irq_handler FLASH_S_IRQHandler
    def_irq_handler GTZC_IRQHandler
    def_irq_handler RCC_IRQHandler
    def_irq_handler RCC_S_IRQHandler
    def_irq_handler EXTI0_IRQHandler
    def_irq_handler EXTI1_IRQHandler
    def_irq_handler EXTI2_IRQHandler
    def_irq_handler EXTI3_IRQHandler
    def_irq_handler EXTI4_IRQHandler
    def_irq_handler EXTI5_IRQHandler
    def_irq_handler EXTI6_IRQHandler
    def_irq_handler EXTI7_IRQHandler
    def_irq_handler EXTI8_IRQHandler
    def_irq_handler EXTI9_IRQHandler
    def_irq_handler EXTI10_IRQHandler
    def_irq_handler EXTI11_IRQHandler
    def_irq_handler EXTI12_IRQHandler
    def_irq_handler EXTI13_IRQHandler
    def_irq_handler EXTI14_IRQHandler
    def_irq_handler EXTI15_IRQHandler
    def_irq_handler GPDMA1_Channel0_IRQHandler
    def_irq_handler GPDMA1_Channel1_IRQHandler
    def_irq_handler GPDMA1_Channel2_IRQHandler
    def_irq_handler GPDMA1_Channel3_IRQHandler
    def_irq_handler GPDMA1_Channel4_IRQHandler
    def_irq_handler GPDMA1_Channel5_IRQHandler
    def_irq_handler GPDMA1_Channel6_IRQHandler
    def_irq_handler GPDMA1_Channel7_IRQHandler
    def_irq_handler IWDG_IRQHandler
    def_irq_handler ADC1_IRQHandler
    def_irq_handler DAC1_IRQHandler
    def_irq_handler FDCAN1_IT0_IRQHandler
    def_irq_handler FDCAN1_IT1_IRQHandler
    def_irq_handler TIM1_BRK_IRQHandler
    def_irq_handler TIM1_UP_IRQHandler
    def_irq_handler TIM1_TRG_COM_IRQHandler
    def_irq_handler TIM1_CC_IRQHandler
    def_irq_handler TIM2_IRQHandler
    def_irq_handler TIM3_IRQHandler
    def_irq_handler TIM4_IRQHandler
    def_irq_handler TIM5_IRQHandler
    def_irq_handler TIM6_IRQHandler
    def_irq_handler TIM7_IRQHandler
    def_irq_handler I2C1_EV_IRQHandler
    def_irq_handler I2C1_ER_IRQHandler
    def_irq_handler I2C2_EV_IRQHandler
    def_irq_handler I2C2_ER_IRQHandler
    def_irq_handler SPI1_IRQHandler
    def_irq_handler SPI2_IRQHandler
    def_irq_handler SPI3_IRQHandler
    def_irq_handler USART1_IRQHandler
    def_irq_handler USART2_IRQHandler
    def_irq_handler USART3_IRQHandler
    def_irq_handler UART4_IRQHandler
    def_irq_handler UART5_IRQHandler
    def_irq_handler LPUART1_IRQHandler
    def_irq_handler LPTIM1_IRQHandler
    def_irq_handler TIM8_BRK_IRQHandler
    def_irq_handler TIM8_UP_IRQHandler
    def_irq_handler TIM8_TRG_COM_IRQHandler
    def_irq_handler TIM8_CC_IRQHandler
    def_irq_handler ADC2_IRQHandler
    def_irq_handler LPTIM2_IRQHandler
    def_irq_handler TIM15_IRQHandler
    def_irq_handler USB_DRD_FS_IRQHandler
    def_irq_handler CRS_IRQHandler
    def_irq_handler UCPD1_IRQHandler
    def_irq_handler FMC_IRQHandler
    def_irq_handler OCTOSPI1_IRQHandler
    def_irq_handler SDMMC1_IRQHandler
    def_irq_handler I2C3_EV_IRQHandler
    def_irq_handler I2C3_ER_IRQHandler
    def_irq_handler SPI4_IRQHandler
    def_irq_handler USART6_IRQHandler
    def_irq_handler GPDMA2_Channel0_IRQHandler
    def_irq_handler GPDMA2_Channel1_IRQHandler
    def_irq_handler GPDMA2_Channel2_IRQHandler
    def_irq_handler GPDMA2_Channel3_IRQHandler
    def_irq_handler GPDMA2_Channel4_IRQHandler
    def_irq_handler GPDMA2_Channel5_IRQHandler
    def_irq_handler GPDMA2_Channel6_IRQHandler
    def_irq_handler GPDMA2_Channel7_IRQHandler
    def_irq_handler FPU_IRQHandler
    def_irq_handler ICACHE_IRQHandler
    def_irq_handler DCACHE1_IRQHandler
    def_irq_handler DCMI_PSSI_IRQHandler
    def_irq_handler FDCAN2_IT0_IRQHandler
    def_irq_handler FDCAN2_IT1_IRQHandler
    def_irq_handler DTS_IRQHandler
    def_irq_handler RNG_IRQHandler
    def_irq_handler HASH_IRQHandler
    def_irq_handler PKA_IRQHandler
    def_irq_handler CEC_IRQHandler
    def_irq_handler TIM12_IRQHandler
    def_irq_handler I3C1_EV_IRQHandler
    def_irq_handler I3C1_ER_IRQHandler
    def_irq_handler I3C2_EV_IRQHandler
    def_irq_handler I3C2_ER_IRQHandler

    .end
