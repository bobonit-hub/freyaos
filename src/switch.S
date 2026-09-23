/*
 * Freya - PendSV, the one place a thread is switched or aborted.
 *
 * Thread mode uses PSP.  PendSV runs on MSP, so the switch does not move
 * the stack this function itself is using.  The hardware has already
 * stacked the exception frame on the thread's stack.  Below that we keep
 * r4-r11, and on a core with an FPU the callee-saved s16-s31 when the
 * frame is the extended one (EXC_RETURN bit 4 clear).
 *
 * thread_preempt_kind() returns 1 to abort the run, 2 to switch, 0 to
 * leave the thread alone.  An abort must see the hardware frame, so it
 * is taken before any of those registers are stacked on top of it.
 */
    .syntax unified
    .thumb

    .section .text.PendSV_Handler, "ax", %progbits
    .global PendSV_Handler
    .thumb_func
    .type   PendSV_Handler, %function
PendSV_Handler:
    ldr     r1, =thread_exc_save
    str     lr, [r1]
    push    {lr}
    bl      thread_preempt_kind
    pop     {lr}
    cmp     r0, #1
    beq     .Labort
    cmp     r0, #2
    bne     .Lout

    mrs     r0, psp
#ifdef __ARM_FP
    tst     lr, #0x10
    it      eq
    vstmdbeq r0!, {s16-s31}
#endif
    stmdb   r0!, {r4-r11}
    bl      thread_switch
    ldr     r1, =thread_exc_restore
    ldr     lr, [r1]
    ldmia   r0!, {r4-r11}
#ifdef __ARM_FP
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
#endif
    msr     psp, r0
.Lout:
    bx      lr

.Labort:
    mrs     r0, psp
#ifdef __ARM_FP
    /* An extended frame has s0-s15, FPSCR and a pad under r0. */
    tst     lr, #0x10
    it      eq
    addeq   r0, r0, #72
#endif
    b       app_pendsv_handler
    .size   PendSV_Handler, . - PendSV_Handler
