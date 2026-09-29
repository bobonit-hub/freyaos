#include "all.h"

int freya_rsave[] = {
	R0, R1, R2, R3,
	-1
};

int freya_rclob[] = { -1 };

#define RGLOB (BIT(R4) | BIT(FP) | BIT(SP))

/* How many operands may stay in a stack slot instead of a register.
 * The PDP-11 reads memory operands everywhere, but a slot in the
 * address of a load or store means the slot itself (the ABI and the
 * spiller use them so), and the emitter wants registers for swaps and
 * address-taking. */
static int
freya_memargs(int op)
{
	if (isload(op) || isstore(op) || op == Ocall || op == Oswap || op == Oaddr)
		return 0;
	if (op == Oxcmp || INRANGE(op, Ocmpw, Ocmpw1))
		return 2;
	return 1;
}

Target T_freya = {
	.name = "freya",
	.gpr0 = R0,
	.ngpr = NGPR,
	.fpr0 = -1,
	.nfpr = 0,
	.rglob = RGLOB,
	.nrglob = 3,
	.rsave = freya_rsave,
	.nrsave = {NGPS, NFPS},
	.retregs = freya_retregs,
	.argregs = freya_argregs,
	.memargs = freya_memargs,
	.abi0 = elimsb,
	.abi1 = freya_abi,
	.isel = freya_isel,
	.emitfn = freya_emitfn,
	.emitfin = freya_emitfin,
	.asloc = ".L",
	.assym = "",
	.cansel = 0,
};

MAKESURE(freya_rsave_ok, sizeof freya_rsave == (NGPS + NFPS + 1) * sizeof(int));
MAKESURE(freya_rclob_ok, sizeof freya_rclob == (NCLR + 1) * sizeof(int));
