#include "all.h"

int freya_rsave[] = {
	R0, R1, R2, R3,
	-1
};

int freya_rclob[] = { -1 };

#define RGLOB (BIT(R4) | BIT(FP) | BIT(SP))

static int
freya_memargs(int op)
{
	(void)op;
	return 0;
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
