/*
 * Freya - non local jump used to unwind out of a user program.
 *
 * Buffer layout (10 words): sp, r4-r11, lr.
 * Callee saved FP registers (s16-s31) are not preserved: a program that
 * is aborted never resumes, and the kernel itself is built soft-float
 * clean for these paths.
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .section .text.freya_setjmp
    .thumb_func
    .global freya_setjmp
    .type   freya_setjmp, %function
freya_setjmp:                       /* r0 = buffer */
    mov     r2, sp
    stmia   r0!, {r2, r4-r11, lr}
    movs    r0, #0
    bx      lr
    .size   freya_setjmp, . - freya_setjmp

    .section .text.freya_longjmp
    .thumb_func
    .global freya_longjmp
    .type   freya_longjmp, %function
freya_longjmp:                      /* r0 = buffer, r1 = value */
    ldmia   r0!, {r2, r4-r11, lr}
    mov     sp, r2
    movs    r0, r1
    it      eq
    moveq   r0, #1                  /* setjmp must never return 0 twice */
    bx      lr
    .size   freya_longjmp, . - freya_longjmp

    .end
