#include "../all.h"

/* Freya VM: a PDP-11 with 32-bit registers and 32-bit words.
 *
 * R0–R3 are the allocatable registers, all caller-saved.
 * R4 is reserved for the emitter. R5 is the frame pointer,
 * R6 the stack pointer and R7 the program counter.
 */

enum FreyaReg {
	R0 = RXX + 1, R1, R2, R3,
	R4,			/* scratch, not allocated */
	FP,			/* R5 */
	SP,			/* R6 */

	NGPR = SP - R0 + 1,
	NGPS = R3 - R0 + 1,
	NFPS = 0,
	NCLR = 0,
};
MAKESURE(freya_reg_not_tmp, SP < (int)Tmp0);

/* targ.c */
extern int freya_rsave[];
extern int freya_rclob[];

/* abi.c */
bits freya_retregs(Ref, int[2]);
bits freya_argregs(Ref, int[2]);
void freya_abi(Fn *);

/* isel.c */
void freya_isel(Fn *);

/* emit.c */
void freya_emitfn(Fn *, FILE *);
void freya_emitfin(FILE *);
