/* vmrt.s - what the BASIC image needs that C cannot say.
 *
 * The image is assembled with this file first, so _start is address 0.
 * The program running the machine builds a C call frame for
 * bas_main(heap, size, flags) - the return address, then the three
 * arguments - and points the return address at a HALT.
 *
 * A system call of bas.h is a TRAP whose number is in the low byte;
 * the host reads the arguments from R0-R2 and leaves the result in R0.
 * On the way in they are still C arguments on the stack: after jsr pc
 * the return address is at (sp) and the first argument at 4(sp).  The
 * numbers are vmsys.h's, in the same order.
 */
.text
.globl _start
_start:
	jmp @#bas_main

.globl sys_exit
sys_exit:
	mov 4(sp), r0
	trap 0
	rts pc

.globl sys_putc
sys_putc:
	mov 4(sp), r0
	trap 1
	rts pc

.globl sys_readline
sys_readline:
	mov 4(sp), r0
	mov 8(sp), r1
	mov 12(sp), r2
	trap 2
	rts pc

.globl sys_break
sys_break:
	trap 3
	rts pc

.globl sys_open
sys_open:
	mov 4(sp), r0
	mov 8(sp), r1
	trap 4
	rts pc

.globl sys_close
sys_close:
	mov 4(sp), r0
	trap 5
	rts pc

.globl sys_read
sys_read:
	mov 4(sp), r0
	mov 8(sp), r1
	mov 12(sp), r2
	trap 6
	rts pc

.globl sys_write
sys_write:
	mov 4(sp), r0
	mov 8(sp), r1
	mov 12(sp), r2
	trap 7
	rts pc

.globl sys_ticks
sys_ticks:
	trap 8
	rts pc

.globl sys_unlink
sys_unlink:
	mov 4(sp), r0
	trap 9
	rts pc

.globl sys_clock
sys_clock:
	mov 4(sp), r0
	trap 10
	rts pc

.globl sys_sleep
sys_sleep:
	mov 4(sp), r0
	trap 11
	rts pc

.globl sys_inkey
sys_inkey:
	trap 12
	rts pc

.globl sys_flash_save
sys_flash_save:
	mov 4(sp), r0
	mov 8(sp), r1
	trap 13
	rts pc

.globl sys_autostart
sys_autostart:
	mov 4(sp), r0
	trap 14
	rts pc

.globl sys_pin_mode
sys_pin_mode:
	mov 4(sp), r0
	mov 8(sp), r1
	trap 15
	rts pc

.globl sys_pin_read
sys_pin_read:
	mov 4(sp), r0
	trap 16
	rts pc

.globl sys_pin_write
sys_pin_write:
	mov 4(sp), r0
	mov 8(sp), r1
	trap 17
	rts pc

.globl sys_pin_toggle
sys_pin_toggle:
	mov 4(sp), r0
	trap 18
	rts pc

.globl sys_pwm
sys_pwm:
	mov 4(sp), r0
	mov 8(sp), r1
	mov 12(sp), r2
	trap 19
	rts pc

.globl sys_adc
sys_adc:
	mov 4(sp), r0
	trap 20
	rts pc

.globl sys_pin_pull
sys_pin_pull:
	mov 4(sp), r0
	mov 8(sp), r1
	trap 21
	rts pc

.globl sys_pin_pull_get
sys_pin_pull_get:
	mov 4(sp), r0
	trap 22
	rts pc

/* int setjmp(jmp_buf b): b[0] = r5, b[1] = sp at the return address,
 * b[2] = the return address.  R0-R4 are not kept across a call, so
 * nothing else survives one and nothing else needs saving. */
.globl setjmp
setjmp:
	mov 4(sp), r0
	mov r5, (r0)
	mov sp, 4(r0)
	mov (sp), 8(r0)
	clr r0
	rts pc

/* void longjmp(jmp_buf b, int val): back into setjmp's caller, which
 * then pops its own argument as if setjmp had just returned val. */
.globl longjmp
longjmp:
	mov 4(sp), r0
	mov 8(sp), r1
	mov (r0), r5
	mov 4(r0), sp
	mov 8(r0), (sp)
	mov r1, r0
	bne 1f
	inc r0
1:
	rts pc
