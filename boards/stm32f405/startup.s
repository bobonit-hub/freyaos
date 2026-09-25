/*
 * Freya - reset vector, interrupt vector table and C runtime bring-up
 * STM32F405xx, Cortex-M4F
 */

    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
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
    .word  0
    .word  0
    .word  0
    .word  0
    .word  SVC_Handler
    .word  DebugMon_Handler
    .word  0
    .word  PendSV_Handler
    .word  SysTick_Handler

    /* External interrupts 0..81.  Slots the F405 fills with CAN, USART3,
     * TIM8, FSMC, UART4/5 and Ethernet are left at 0: Freya never enables
     * those peripherals, and the IRQ numbers it does use (EXTI, TIM2..4,
     * USART2) sit at the same positions as on the F411. */
    .word  WWDG_IRQHandler                   /*  0 */
    .word  PVD_IRQHandler
    .word  TAMP_STAMP_IRQHandler
    .word  RTC_WKUP_IRQHandler
    .word  FLASH_IRQHandler
    .word  RCC_IRQHandler
    .word  EXTI0_IRQHandler
    .word  EXTI1_IRQHandler
    .word  EXTI2_IRQHandler
    .word  EXTI3_IRQHandler
    .word  EXTI4_IRQHandler                  /* 10 */
    .word  DMA1_Stream0_IRQHandler
    .word  DMA1_Stream1_IRQHandler
    .word  DMA1_Stream2_IRQHandler
    .word  DMA1_Stream3_IRQHandler
    .word  DMA1_Stream4_IRQHandler
    .word  DMA1_Stream5_IRQHandler
    .word  DMA1_Stream6_IRQHandler
    .word  ADC_IRQHandler
    .word  0
    .word  0                                 /* 20 */
    .word  0
    .word  0
    .word  EXTI9_5_IRQHandler
    .word  TIM1_BRK_TIM9_IRQHandler
    .word  TIM1_UP_TIM10_IRQHandler
    .word  TIM1_TRG_COM_TIM11_IRQHandler
    .word  TIM1_CC_IRQHandler
    .word  TIM2_IRQHandler
    .word  TIM3_IRQHandler
    .word  TIM4_IRQHandler                   /* 30 */
    .word  I2C1_EV_IRQHandler
    .word  I2C1_ER_IRQHandler
    .word  I2C2_EV_IRQHandler
    .word  I2C2_ER_IRQHandler
    .word  SPI1_IRQHandler
    .word  SPI2_IRQHandler
    .word  USART1_IRQHandler
    .word  USART2_IRQHandler
    .word  0
    .word  EXTI15_10_IRQHandler              /* 40 */
    .word  RTC_Alarm_IRQHandler
    .word  OTG_FS_WKUP_IRQHandler
    .word  0
    .word  0
    .word  0
    .word  0
    .word  DMA1_Stream7_IRQHandler
    .word  0
    .word  SDIO_IRQHandler
    .word  TIM5_IRQHandler                   /* 50 */
    .word  SPI3_IRQHandler
    .word  0
    .word  0
    .word  DMA2_Stream0_IRQHandler
    .word  DMA2_Stream1_IRQHandler
    .word  DMA2_Stream2_IRQHandler
    .word  DMA2_Stream3_IRQHandler
    .word  DMA2_Stream4_IRQHandler
    .word  0
    .word  0                                 /* 60 */
    .word  0
    .word  0
    .word  OTG_FS_IRQHandler
    .word  DMA2_Stream5_IRQHandler
    .word  DMA2_Stream6_IRQHandler
    .word  DMA2_Stream7_IRQHandler
    .word  USART6_IRQHandler
    .word  I2C3_EV_IRQHandler
    .word  I2C3_ER_IRQHandler
    .word  0                                 /* 70 */
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0                                 /* 80 */
    .word  FPU_IRQHandler                    /* 81 */
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
    /* A thread that used the FPU has s0-s15 and two words under r0. */
    tst   lr, #0x10
    it    eq
    addeq r0, r0, #72
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
    def_irq_handler SysTick_Handler
    def_irq_handler WWDG_IRQHandler
    def_irq_handler PVD_IRQHandler
    def_irq_handler TAMP_STAMP_IRQHandler
    def_irq_handler RTC_WKUP_IRQHandler
    def_irq_handler FLASH_IRQHandler
    def_irq_handler RCC_IRQHandler
    def_irq_handler EXTI0_IRQHandler
    def_irq_handler EXTI1_IRQHandler
    def_irq_handler EXTI2_IRQHandler
    def_irq_handler EXTI3_IRQHandler
    def_irq_handler EXTI4_IRQHandler
    def_irq_handler DMA1_Stream0_IRQHandler
    def_irq_handler DMA1_Stream1_IRQHandler
    def_irq_handler DMA1_Stream2_IRQHandler
    def_irq_handler DMA1_Stream3_IRQHandler
    def_irq_handler DMA1_Stream4_IRQHandler
    def_irq_handler DMA1_Stream5_IRQHandler
    def_irq_handler DMA1_Stream6_IRQHandler
    def_irq_handler ADC_IRQHandler
    def_irq_handler EXTI9_5_IRQHandler
    def_irq_handler TIM1_BRK_TIM9_IRQHandler
    def_irq_handler TIM1_UP_TIM10_IRQHandler
    def_irq_handler TIM1_TRG_COM_TIM11_IRQHandler
    def_irq_handler TIM1_CC_IRQHandler
    def_irq_handler TIM2_IRQHandler
    def_irq_handler TIM3_IRQHandler
    def_irq_handler TIM4_IRQHandler
    def_irq_handler I2C1_EV_IRQHandler
    def_irq_handler I2C1_ER_IRQHandler
    def_irq_handler I2C2_EV_IRQHandler
    def_irq_handler I2C2_ER_IRQHandler
    def_irq_handler SPI1_IRQHandler
    def_irq_handler SPI2_IRQHandler
    def_irq_handler USART1_IRQHandler
    def_irq_handler USART2_IRQHandler
    def_irq_handler EXTI15_10_IRQHandler
    def_irq_handler RTC_Alarm_IRQHandler
    def_irq_handler OTG_FS_WKUP_IRQHandler
    def_irq_handler DMA1_Stream7_IRQHandler
    def_irq_handler SDIO_IRQHandler
    def_irq_handler TIM5_IRQHandler
    def_irq_handler SPI3_IRQHandler
    def_irq_handler DMA2_Stream0_IRQHandler
    def_irq_handler DMA2_Stream1_IRQHandler
    def_irq_handler DMA2_Stream2_IRQHandler
    def_irq_handler DMA2_Stream3_IRQHandler
    def_irq_handler DMA2_Stream4_IRQHandler
    def_irq_handler OTG_FS_IRQHandler
    def_irq_handler DMA2_Stream5_IRQHandler
    def_irq_handler DMA2_Stream6_IRQHandler
    def_irq_handler DMA2_Stream7_IRQHandler
    def_irq_handler USART6_IRQHandler
    def_irq_handler I2C3_EV_IRQHandler
    def_irq_handler I2C3_ER_IRQHandler
    def_irq_handler FPU_IRQHandler

    .end
