/* armrt.h - what basic.c needs on an ARM core that C cannot say.
 *
 * Included by samples/basic11/main.c, the program for the board, and by
 * armrt.c, which runs the same code under qemu-arm for the tests, so
 * both have the one copy.
 */

/*
 * setjmp and longjmp for the interpreter's error exit.  r4-r11, sp and
 * lr, and s16-s31 when the code is hard-float, because the compiler may
 * keep a value in the callee-saved half of the FPU across a call.  The
 * Blue Pill's Cortex-M3 has no FPU and is built soft-float, so there
 * the core registers are all of it.  Plain Thumb-2 and VFP, as the
 * kernel's own src/setjmp.s is.
 */
#ifdef __ARM_FP
#define VFP_SAVE    "    vstmia  r0!, {s16-s31}\n"
#define VFP_RESTORE "    vldmia  r0!, {s16-s31}\n"
#else
#define VFP_SAVE    ""
#define VFP_RESTORE ""
#endif
__asm__(
    ".syntax unified\n"
    ".thumb\n"
    ".section .text.setjmp,\"ax\",%progbits\n"
    ".thumb_func\n"
    ".global setjmp\n"
    ".type setjmp, %function\n"
    "setjmp:\n"                             /* r0 = buffer */
    "    mov     r2, sp\n"
    "    stmia   r0!, {r2, r4-r11, lr}\n"
    VFP_SAVE
    "    movs    r0, #0\n"
    "    bx      lr\n"
    ".size setjmp, . - setjmp\n"
    ".section .text.longjmp,\"ax\",%progbits\n"
    ".thumb_func\n"
    ".global longjmp\n"
    ".type longjmp, %function\n"
    "longjmp:\n"                            /* r0 = buffer, r1 = value */
    "    ldmia   r0!, {r2, r4-r11, lr}\n"
    VFP_RESTORE
    "    mov     sp, r2\n"
    "    movs    r0, r1\n"
    "    it      eq\n"
    "    moveq   r0, #1\n"                  /* setjmp must never return 0 twice */
    "    bx      lr\n"
    ".size longjmp, . - longjmp\n"
);

/* bas_main(heap, size, flags) on the stack whose top is 'top'.  The
 * interpreter leaves through sys_exit(), which does not come back: on
 * the board it unwinds to the kernel, with the kernel's own stack
 * pointer.  A return would come back here, to the caller's stack. */
int bas_main_on(uint8_t *heap, uint32_t size, uint32_t flags, void *top);
__asm__(
    ".syntax unified\n"
    ".thumb\n"
    ".section .text.bas_main_on,\"ax\",%progbits\n"
    ".thumb_func\n"
    ".global bas_main_on\n"
    ".type bas_main_on, %function\n"
    "bas_main_on:\n"
    "    push    {r4, lr}\n"
    "    mov     r4, sp\n"
    "    mov     sp, r3\n"
    "    bl      bas_main\n"
    "    mov     sp, r4\n"
    "    pop     {r4, pc}\n"
    ".size bas_main_on, . - bas_main_on\n"
);
