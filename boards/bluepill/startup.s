/*
 * Freya - reset vector, interrupt vector table and C runtime bring-up
 * STM32F103C8T6, Cortex-M3
 */

    .syntax unified
    .cpu cortex-m3
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

    /* External interrupts 0..42 (medium density) */
    .word  WWDG_IRQHandler                   /*  0 */
    .word  PVD_IRQHandler
    .word  TAMPER_IRQHandler
    .word  RTC_IRQHandler
    .word  FLASH_IRQHandler
    .word  RCC_IRQHandler
    .word  EXTI0_IRQHandler
    .word  EXTI1_IRQHandler
    .word  EXTI2_IRQHandler
    .word  EXTI3_IRQHandler
    .word  EXTI4_IRQHandler                  /* 10 */
    .word  DMA1_Channel1_IRQHandler
    .word  DMA1_Channel2_IRQHandler
    .word  DMA1_Channel3_IRQHandler
    .word  DMA1_Channel4_IRQHandler
    .word  DMA1_Channel5_IRQHandler
    .word  DMA1_Channel6_IRQHandler
    .word  DMA1_Channel7_IRQHandler
    .word  ADC1_2_IRQHandler
    .word  USB_HP_CAN1_TX_IRQHandler
    .word  USB_LP_CAN1_RX0_IRQHandler        /* 20 */
    .word  CAN1_RX1_IRQHandler
    .word  CAN1_SCE_IRQHandler
    .word  EXTI9_5_IRQHandler
    .word  TIM1_BRK_IRQHandler
    .word  TIM1_UP_IRQHandler
    .word  TIM1_TRG_COM_IRQHandler
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
    .word  USART3_IRQHandler
    .word  EXTI15_10_IRQHandler              /* 40 */
    .word  RTC_Alarm_IRQHandler
    .word  USBWakeUp_IRQHandler              /* 42 */
    .size  g_pfnVectors, . - g_pfnVectors

/* ------------------------------------------------------------------ */
    .section .text.Reset_Handler
    .weak    Reset_Handler
    .type    Reset_Handler, %function
Reset_Handler:
    /* The boot ROM may leave the stack pointer anywhere - set it up first. */
    ldr   r0, =__stack_top
    mov   sp, r0

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
    b     freya_fault_handler

/*
 * PendSV carries out a program abort.  It is configured at the lowest
 * priority, so when it runs no other handler is active and the frame on
 * top of the main stack belongs to the interrupted thread.
 */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
    mrs   r0, msp
    b     app_pendsv_handler

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
    def_irq_handler TAMPER_IRQHandler
    def_irq_handler RTC_IRQHandler
    def_irq_handler FLASH_IRQHandler
    def_irq_handler RCC_IRQHandler
    def_irq_handler EXTI0_IRQHandler
    def_irq_handler EXTI1_IRQHandler
    def_irq_handler EXTI2_IRQHandler
    def_irq_handler EXTI3_IRQHandler
    def_irq_handler EXTI4_IRQHandler
    def_irq_handler DMA1_Channel1_IRQHandler
    def_irq_handler DMA1_Channel2_IRQHandler
    def_irq_handler DMA1_Channel3_IRQHandler
    def_irq_handler DMA1_Channel4_IRQHandler
    def_irq_handler DMA1_Channel5_IRQHandler
    def_irq_handler DMA1_Channel6_IRQHandler
    def_irq_handler DMA1_Channel7_IRQHandler
    def_irq_handler ADC1_2_IRQHandler
    def_irq_handler USB_HP_CAN1_TX_IRQHandler
    def_irq_handler USB_LP_CAN1_RX0_IRQHandler
    def_irq_handler CAN1_RX1_IRQHandler
    def_irq_handler CAN1_SCE_IRQHandler
    def_irq_handler EXTI9_5_IRQHandler
    def_irq_handler TIM1_BRK_IRQHandler
    def_irq_handler TIM1_UP_IRQHandler
    def_irq_handler TIM1_TRG_COM_IRQHandler
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
    def_irq_handler USART3_IRQHandler
    def_irq_handler EXTI15_10_IRQHandler
    def_irq_handler RTC_Alarm_IRQHandler
    def_irq_handler USBWakeUp_IRQHandler

    .end
