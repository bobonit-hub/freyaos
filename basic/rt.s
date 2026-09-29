/* rt.s - what the BASIC image needs that C cannot say.
 *
 * The image is assembled with this file first, so _start is address 0
 * and the host needs no entry offset.  The host builds a C call frame
 * for main(heap_lo, heap_hi) and points the return address at a HALT.
 *
 * A system call is a TRAP whose number is in the low byte; the host
 * reads the arguments from R0-R2 and leaves the result in R0.  On the
 * way in the arguments are still C arguments on the stack: after
 * jsr pc the return address is at (sp) and the first argument at 4(sp).
 */
.text
.globl _start
_start:
	jmp @#main

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

/* int setjmp(jmp_buf b): b[0] = r5, b[1] = sp at the return address,
 * b[2] = the return address.  R0-R3 are caller-saved, so nothing else
 * survives a call and nothing else needs saving. */
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
