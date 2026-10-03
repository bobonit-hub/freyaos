/*
 * Freya - reset vector, interrupt vector table and C runtime bring-up
 * STM32H723xx, Cortex-M7 with FPU
 */

    .syntax unified
    .cpu cortex-m7
    .fpu fpv5-d16
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

    /* External interrupts 0..162 */
    .word  WWDG_IRQHandler                   /*   0 */
    .word  PVD_AVD_IRQHandler
    .word  TAMP_STAMP_IRQHandler
    .word  RTC_WKUP_IRQHandler
    .word  FLASH_IRQHandler
    .word  RCC_IRQHandler
    .word  EXTI0_IRQHandler
    .word  EXTI1_IRQHandler
    .word  EXTI2_IRQHandler
    .word  EXTI3_IRQHandler
    .word  EXTI4_IRQHandler                  /*  10 */
    .word  DMA1_Stream0_IRQHandler
    .word  DMA1_Stream1_IRQHandler
    .word  DMA1_Stream2_IRQHandler
    .word  DMA1_Stream3_IRQHandler
    .word  DMA1_Stream4_IRQHandler
    .word  DMA1_Stream5_IRQHandler
    .word  DMA1_Stream6_IRQHandler
    .word  ADC_IRQHandler
    .word  FDCAN1_IT0_IRQHandler
    .word  FDCAN2_IT0_IRQHandler             /*  20 */
    .word  FDCAN1_IT1_IRQHandler
    .word  FDCAN2_IT1_IRQHandler
    .word  EXTI9_5_IRQHandler
    .word  TIM1_BRK_IRQHandler
    .word  TIM1_UP_IRQHandler
    .word  TIM1_TRG_COM_IRQHandler
    .word  TIM1_CC_IRQHandler
    .word  TIM2_IRQHandler
    .word  TIM3_IRQHandler
    .word  TIM4_IRQHandler                   /*  30 */
    .word  I2C1_EV_IRQHandler
    .word  I2C1_ER_IRQHandler
    .word  I2C2_EV_IRQHandler
    .word  I2C2_ER_IRQHandler
    .word  SPI1_IRQHandler
    .word  SPI2_IRQHandler
    .word  USART1_IRQHandler
    .word  USART2_IRQHandler
    .word  USART3_IRQHandler
    .word  EXTI15_10_IRQHandler              /*  40 */
    .word  RTC_Alarm_IRQHandler
    .word  0
    .word  TIM8_BRK_TIM12_IRQHandler
    .word  TIM8_UP_TIM13_IRQHandler
    .word  TIM8_TRG_COM_TIM14_IRQHandler
    .word  TIM8_CC_IRQHandler
    .word  DMA1_Stream7_IRQHandler
    .word  FMC_IRQHandler
    .word  SDMMC1_IRQHandler
    .word  TIM5_IRQHandler                   /*  50 */
    .word  SPI3_IRQHandler
    .word  UART4_IRQHandler
    .word  UART5_IRQHandler
    .word  TIM6_DAC_IRQHandler
    .word  TIM7_IRQHandler
    .word  DMA2_Stream0_IRQHandler
    .word  DMA2_Stream1_IRQHandler
    .word  DMA2_Stream2_IRQHandler
    .word  DMA2_Stream3_IRQHandler
    .word  DMA2_Stream4_IRQHandler           /*  60 */
    .word  ETH_IRQHandler
    .word  ETH_WKUP_IRQHandler
    .word  FDCAN_CAL_IRQHandler
    .word  0
    .word  0
    .word  0
    .word  0
    .word  DMA2_Stream5_IRQHandler
    .word  DMA2_Stream6_IRQHandler
    .word  DMA2_Stream7_IRQHandler           /*  70 */
    .word  USART6_IRQHandler
    .word  I2C3_EV_IRQHandler
    .word  I2C3_ER_IRQHandler
    .word  OTG_HS_EP1_OUT_IRQHandler
    .word  OTG_HS_EP1_IN_IRQHandler
    .word  OTG_HS_WKUP_IRQHandler
    .word  OTG_HS_IRQHandler
    .word  DCMI_PSSI_IRQHandler
    .word  0
    .word  RNG_IRQHandler                    /*  80 */
    .word  FPU_IRQHandler
    .word  UART7_IRQHandler
    .word  UART8_IRQHandler
    .word  SPI4_IRQHandler
    .word  SPI5_IRQHandler
    .word  SPI6_IRQHandler
    .word  SAI1_IRQHandler
    .word  LTDC_IRQHandler
    .word  LTDC_ER_IRQHandler
    .word  DMA2D_IRQHandler                  /*  90 */
    .word  0
    .word  OCTOSPI1_IRQHandler
    .word  LPTIM1_IRQHandler
    .word  CEC_IRQHandler
    .word  I2C4_EV_IRQHandler
    .word  I2C4_ER_IRQHandler
    .word  SPDIF_RX_IRQHandler
    .word  0
    .word  0
    .word  0                                 /* 100 */
    .word  0
    .word  DMAMUX1_OVR_IRQHandler
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  0
    .word  DFSDM1_FLT0_IRQHandler            /* 110 */
    .word  DFSDM1_FLT1_IRQHandler
    .word  DFSDM1_FLT2_IRQHandler
    .word  DFSDM1_FLT3_IRQHandler
    .word  0
    .word  SWPMI1_IRQHandler
    .word  TIM15_IRQHandler
    .word  TIM16_IRQHandler
    .word  TIM17_IRQHandler
    .word  MDIOS_WKUP_IRQHandler
    .word  MDIOS_IRQHandler                  /* 120 */
    .word  0
    .word  MDMA_IRQHandler
    .word  0
    .word  SDMMC2_IRQHandler
    .word  HSEM1_IRQHandler
    .word  0
    .word  ADC3_IRQHandler
    .word  DMAMUX2_OVR_IRQHandler
    .word  BDMA_Channel0_IRQHandler
    .word  BDMA_Channel1_IRQHandler          /* 130 */
    .word  BDMA_Channel2_IRQHandler
    .word  BDMA_Channel3_IRQHandler
    .word  BDMA_Channel4_IRQHandler
    .word  BDMA_Channel5_IRQHandler
    .word  BDMA_Channel6_IRQHandler
    .word  BDMA_Channel7_IRQHandler
    .word  COMP_IRQHandler
    .word  LPTIM2_IRQHandler
    .word  LPTIM3_IRQHandler
    .word  LPTIM4_IRQHandler                 /* 140 */
    .word  LPTIM5_IRQHandler
    .word  LPUART1_IRQHandler
    .word  0
    .word  CRS_IRQHandler
    .word  ECC_IRQHandler
    .word  SAI4_IRQHandler
    .word  DTS_IRQHandler
    .word  0
    .word  WAKEUP_PIN_IRQHandler
    .word  OCTOSPI2_IRQHandler               /* 150 */
    .word  0
    .word  0
    .word  FMAC_IRQHandler
    .word  CORDIC_IRQHandler
    .word  UART9_IRQHandler
    .word  USART10_IRQHandler
    .word  I2C5_EV_IRQHandler
    .word  I2C5_ER_IRQHandler
    .word  FDCAN3_IT0_IRQHandler
    .word  FDCAN3_IT1_IRQHandler             /* 160 */
    .word  TIM23_IRQHandler
    .word  TIM24_IRQHandler
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

    /* AXI SRAM keeps ECC per 64-bit word, and a word nobody wrote since
     * power-on reads back as an uncorrectable error.  Write all of it,
     * eight bytes at a time, before anything reads it. */
    ldr   r0, =__ram_start
    ldr   r1, =__ram_end
    movs  r2, #0
    movs  r3, #0
8:  strd  r2, r3, [r0], #8
    cmp   r0, r1
    bcc   8b
    dsb

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
    def_irq_handler SysTick_Handler
    def_irq_handler WWDG_IRQHandler
    def_irq_handler PVD_AVD_IRQHandler
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
    def_irq_handler FDCAN1_IT0_IRQHandler
    def_irq_handler FDCAN2_IT0_IRQHandler
    def_irq_handler FDCAN1_IT1_IRQHandler
    def_irq_handler FDCAN2_IT1_IRQHandler
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
    def_irq_handler TIM8_BRK_TIM12_IRQHandler
    def_irq_handler TIM8_UP_TIM13_IRQHandler
    def_irq_handler TIM8_TRG_COM_TIM14_IRQHandler
    def_irq_handler TIM8_CC_IRQHandler
    def_irq_handler DMA1_Stream7_IRQHandler
    def_irq_handler FMC_IRQHandler
    def_irq_handler SDMMC1_IRQHandler
    def_irq_handler TIM5_IRQHandler
    def_irq_handler SPI3_IRQHandler
    def_irq_handler UART4_IRQHandler
    def_irq_handler UART5_IRQHandler
    def_irq_handler TIM6_DAC_IRQHandler
    def_irq_handler TIM7_IRQHandler
    def_irq_handler DMA2_Stream0_IRQHandler
    def_irq_handler DMA2_Stream1_IRQHandler
    def_irq_handler DMA2_Stream2_IRQHandler
    def_irq_handler DMA2_Stream3_IRQHandler
    def_irq_handler DMA2_Stream4_IRQHandler
    def_irq_handler ETH_IRQHandler
    def_irq_handler ETH_WKUP_IRQHandler
    def_irq_handler FDCAN_CAL_IRQHandler
    def_irq_handler DMA2_Stream5_IRQHandler
    def_irq_handler DMA2_Stream6_IRQHandler
    def_irq_handler DMA2_Stream7_IRQHandler
    def_irq_handler USART6_IRQHandler
    def_irq_handler I2C3_EV_IRQHandler
    def_irq_handler I2C3_ER_IRQHandler
    def_irq_handler OTG_HS_EP1_OUT_IRQHandler
    def_irq_handler OTG_HS_EP1_IN_IRQHandler
    def_irq_handler OTG_HS_WKUP_IRQHandler
    def_irq_handler OTG_HS_IRQHandler
    def_irq_handler DCMI_PSSI_IRQHandler
    def_irq_handler RNG_IRQHandler
    def_irq_handler FPU_IRQHandler
    def_irq_handler UART7_IRQHandler
    def_irq_handler UART8_IRQHandler
    def_irq_handler SPI4_IRQHandler
    def_irq_handler SPI5_IRQHandler
    def_irq_handler SPI6_IRQHandler
    def_irq_handler SAI1_IRQHandler
    def_irq_handler LTDC_IRQHandler
    def_irq_handler LTDC_ER_IRQHandler
    def_irq_handler DMA2D_IRQHandler
    def_irq_handler OCTOSPI1_IRQHandler
    def_irq_handler LPTIM1_IRQHandler
    def_irq_handler CEC_IRQHandler
    def_irq_handler I2C4_EV_IRQHandler
    def_irq_handler I2C4_ER_IRQHandler
    def_irq_handler SPDIF_RX_IRQHandler
    def_irq_handler DMAMUX1_OVR_IRQHandler
    def_irq_handler DFSDM1_FLT0_IRQHandler
    def_irq_handler DFSDM1_FLT1_IRQHandler
    def_irq_handler DFSDM1_FLT2_IRQHandler
    def_irq_handler DFSDM1_FLT3_IRQHandler
    def_irq_handler SWPMI1_IRQHandler
    def_irq_handler TIM15_IRQHandler
    def_irq_handler TIM16_IRQHandler
    def_irq_handler TIM17_IRQHandler
    def_irq_handler MDIOS_WKUP_IRQHandler
    def_irq_handler MDIOS_IRQHandler
    def_irq_handler MDMA_IRQHandler
    def_irq_handler SDMMC2_IRQHandler
    def_irq_handler HSEM1_IRQHandler
    def_irq_handler ADC3_IRQHandler
    def_irq_handler DMAMUX2_OVR_IRQHandler
    def_irq_handler BDMA_Channel0_IRQHandler
    def_irq_handler BDMA_Channel1_IRQHandler
    def_irq_handler BDMA_Channel2_IRQHandler
    def_irq_handler BDMA_Channel3_IRQHandler
    def_irq_handler BDMA_Channel4_IRQHandler
    def_irq_handler BDMA_Channel5_IRQHandler
    def_irq_handler BDMA_Channel6_IRQHandler
    def_irq_handler BDMA_Channel7_IRQHandler
    def_irq_handler COMP_IRQHandler
    def_irq_handler LPTIM2_IRQHandler
    def_irq_handler LPTIM3_IRQHandler
    def_irq_handler LPTIM4_IRQHandler
    def_irq_handler LPTIM5_IRQHandler
    def_irq_handler LPUART1_IRQHandler
    def_irq_handler CRS_IRQHandler
    def_irq_handler ECC_IRQHandler
    def_irq_handler SAI4_IRQHandler
    def_irq_handler DTS_IRQHandler
    def_irq_handler WAKEUP_PIN_IRQHandler
    def_irq_handler OCTOSPI2_IRQHandler
    def_irq_handler FMAC_IRQHandler
    def_irq_handler CORDIC_IRQHandler
    def_irq_handler UART9_IRQHandler
    def_irq_handler USART10_IRQHandler
    def_irq_handler I2C5_EV_IRQHandler
    def_irq_handler I2C5_ER_IRQHandler
    def_irq_handler FDCAN3_IT0_IRQHandler
    def_irq_handler FDCAN3_IT1_IRQHandler
    def_irq_handler TIM23_IRQHandler
    def_irq_handler TIM24_IRQHandler

    .end
