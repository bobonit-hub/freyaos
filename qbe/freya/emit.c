#include "all.h"

static char *rname[] = {
	[R0] = "r0", "r1", "r2", "r3",
	[R4] = "r4",
	[FP] = "r5",
	[SP] = "sp",
};

static int need_div, need_rem, need_udiv, need_urem;
static int need_shl, need_shr, need_sar;
static int lbl;

static int64_t
slot(Ref r, Fn *fn)
{
	int s;

	s = rsval(r);
	if (s < 0)
		return 4 * (1 - s);          /* -1 -> 8(fp): first argument */
	return -4 * (int64_t)(fn->slot - s);
}

static void
val(char *buf, size_t n, Ref r, Fn *fn)
{
	Con *c;

	switch (rtype(r)) {
	case RTmp:
		snprintf(buf, n, "%s", rname[r.val]);
		return;
	case RCon:
		c = &fn->con[r.val];
		if (c->type == CBits) {
			snprintf(buf, n, "#%"PRId64, (int64_t)(int32_t)c->bits.i);
			return;
		}
		if (c->type == CAddr) {
			if (c->bits.i)
				snprintf(buf, n, "#%s%+"PRId64, str(c->sym.id), c->bits.i);
			else
				snprintf(buf, n, "#%s", str(c->sym.id));
			return;
		}
		break;
	case RSlot:
		/* A spilled temporary; every instruction that takes a source
		 * operand can read it from its stack slot. */
		snprintf(buf, n, "%"PRId64"(r5)", slot(r, fn));
		return;
	}
	die("bad operand");
}

static void
memstr(char *buf, size_t n, Ref r, Fn *fn, int add)
{
	Con *c;
	Mem *m;
	int64_t o;

	switch (rtype(r)) {
	case RTmp:
		if (add)
			snprintf(buf, n, "%d(%s)", add, rname[r.val]);
		else
			snprintf(buf, n, "(%s)", rname[r.val]);
		return;
	case RSlot:
		o = slot(r, fn) + add;
		snprintf(buf, n, "%"PRId64"(r5)", o);
		return;
	case RCon:
		c = &fn->con[r.val];
		assert(c->type == CAddr);
		o = c->bits.i + add;
		if (o)
			snprintf(buf, n, "@#%s%+"PRId64, str(c->sym.id), o);
		else
			snprintf(buf, n, "@#%s", str(c->sym.id));
		return;
	case RMem:
		/* base + constant, folded by isel */
		m = &fn->mem[r.val];
		o = m->offset.bits.i + add;
		if (rtype(m->base) == RSlot) {
			snprintf(buf, n, "%"PRId64"(r5)", o + slot(m->base, fn));
			return;
		}
		assert(isreg(m->base));
		if (o)
			snprintf(buf, n, "%"PRId64"(%s)", o, rname[m->base.val]);
		else
			snprintf(buf, n, "(%s)", rname[m->base.val]);
		return;
	}
	die("bad address");
}

static int
isreg_(Ref r, int reg)
{
	return rtype(r) == RTmp && r.val == reg;
}

/* A result computed in r4 (saved on the stack first) goes to dst. */
static void
deliver(FILE *f, int dst)
{
	if (dst != R4) {
		fprintf(f, "\tmov r4, %s\n", rname[dst]);
		fprintf(f, "\tmov (sp)+, r4\n");
	} else
		fprintf(f, "\tadd #4, sp\n");
}

/* dst = a <op> b, with <op> a two-operand instruction (dst = dst <op>
 * src).  The operands may be registers, constants or stack slots; dst
 * is a register that may be one of them. */
static void
binop(FILE *f, char *op, int dst, Ref a, Ref b, Fn *fn, int commutes)
{
	char as[80], bs[80];

	val(as, sizeof as, a, fn);
	val(bs, sizeof bs, b, fn);
	if (isreg_(a, dst)) {
		fprintf(f, "\t%s %s, %s\n", op, bs, rname[dst]);
		return;
	}
	if (isreg_(b, dst)) {
		if (commutes) {
			fprintf(f, "\t%s %s, %s\n", op, as, rname[dst]);
			return;
		}
		/* dst = a - dst */
		fprintf(f, "\tneg %s\n", rname[dst]);
		fprintf(f, "\tadd %s, %s\n", as, rname[dst]);
		return;
	}
	fprintf(f, "\tmov %s, %s\n", as, rname[dst]);
	fprintf(f, "\t%s %s, %s\n", op, bs, rname[dst]);
}

/* dst = a & b: bic clears the bits of its source, so the complement
 * of b is made on the stack first, before dst may be written. */
static void
emitand(FILE *f, int dst, Ref a, Ref b, Fn *fn)
{
	char as[80], bs[80];

	val(as, sizeof as, a, fn);
	val(bs, sizeof bs, b, fn);
	fprintf(f, "\tmov %s, -(sp)\n", bs);
	fprintf(f, "\tcom (sp)\n");
	if (!isreg_(a, dst))
		fprintf(f, "\tmov %s, %s\n", as, rname[dst]);
	fprintf(f, "\tbic (sp)+, %s\n", rname[dst]);
}

/* xor wants a register source. */
static void
emitxor(FILE *f, int dst, Ref a, Ref b, Fn *fn)
{
	char as[80], bs[80];

	val(as, sizeof as, a, fn);
	val(bs, sizeof bs, b, fn);
	if (rtype(a) == RTmp && !isreg_(a, dst)) {
		if (!isreg_(b, dst))
			fprintf(f, "\tmov %s, %s\n", bs, rname[dst]);
		fprintf(f, "\txor %s, %s\n", rname[a.val], rname[dst]);
		return;
	}
	if (rtype(b) == RTmp && !isreg_(b, dst)) {
		if (!isreg_(a, dst))
			fprintf(f, "\tmov %s, %s\n", as, rname[dst]);
		fprintf(f, "\txor %s, %s\n", rname[b.val], rname[dst]);
		return;
	}
	fprintf(f, "\tmov r4, -(sp)\n");
	fprintf(f, "\tmov %s, r4\n", as);
	if (!isreg_(b, dst))
		fprintf(f, "\tmov %s, %s\n", bs, rname[dst]);
	fprintf(f, "\txor r4, %s\n", rname[dst]);
	fprintf(f, "\tmov (sp)+, r4\n");
}

/* mul into an odd register leaves the low 32 bits there; into an
 * even one it would also write the register after it. */
static void
emitmul(FILE *f, int dst, Ref a, Ref b, Fn *fn)
{
	char as[80], bs[80];

	val(as, sizeof as, a, fn);
	val(bs, sizeof bs, b, fn);
	if ((dst - R0) & 1) {
		if (isreg_(a, dst))
			fprintf(f, "\tmul %s, %s\n", bs, rname[dst]);
		else if (isreg_(b, dst))
			fprintf(f, "\tmul %s, %s\n", as, rname[dst]);
		else {
			fprintf(f, "\tmov %s, %s\n", as, rname[dst]);
			fprintf(f, "\tmul %s, %s\n", bs, rname[dst]);
		}
		return;
	}
	fprintf(f, "\tmov r3, -(sp)\n");
	if (isreg_(b, R3)) {
		fprintf(f, "\tmov %s, r3\n", as);
		fprintf(f, "\tmul (sp), r3\n");
	} else {
		fprintf(f, "\tmov %s, r3\n", as);
		fprintf(f, "\tmul %s, r3\n", bs);
	}
	fprintf(f, "\tmov r3, %s\n", rname[dst]);
	fprintf(f, "\tmov (sp)+, r3\n");
}

/* Helpers take (r0, r1) and return in r0, with a remainder in r1. */
static void
call2(FILE *f, int dst, Ref a, Ref b, Fn *fn, char *helper, int rem)
{
	char as[80], bs[80];

	val(as, sizeof as, a, fn);
	val(bs, sizeof bs, b, fn);
	fprintf(f, "\tmov r4, -(sp)\n");
	fprintf(f, "\tmov %s, -(sp)\n", as);
	fprintf(f, "\tmov %s, -(sp)\n", bs);
	fprintf(f, "\tmov r0, -(sp)\n");
	fprintf(f, "\tmov r1, -(sp)\n");
	fprintf(f, "\tmov r2, -(sp)\n");
	fprintf(f, "\tmov r3, -(sp)\n");
	fprintf(f, "\tmov 20(sp), r0\n");
	fprintf(f, "\tmov 16(sp), r1\n");
	fprintf(f, "\tjsr pc, @#%s\n", helper);
	fprintf(f, "\tmov %s, r4\n", rem ? "r1" : "r0");
	fprintf(f, "\tmov (sp)+, r3\n");
	fprintf(f, "\tmov (sp)+, r2\n");
	fprintf(f, "\tmov (sp)+, r1\n");
	fprintf(f, "\tmov (sp)+, r0\n");
	fprintf(f, "\tadd #8, sp\n");
	deliver(f, dst);
}

static void
emitshift(FILE *f, int dst, Ref a, Ref b, Fn *fn, int kind)
{
	char as[80];
	int64_t n;
	uint32_t mask;
	char *helper;

	if (isconbits(fn, b, &n)) {
		n &= 31;
		val(as, sizeof as, a, fn);
		if (!isreg_(a, dst))
			fprintf(f, "\tmov %s, %s\n", as, rname[dst]);
		if (n) {
			if (kind == 0)
				fprintf(f, "\tash #%"PRId64", %s\n", n, rname[dst]);
			else
				fprintf(f, "\tash #-%"PRId64", %s\n", n, rname[dst]);
			if (kind == 1) {
				mask = 0xffffffffu << (32 - (int)n);
				fprintf(f, "\tbic #%"PRId32", %s\n", (int32_t)mask, rname[dst]);
			}
		}
		return;
	}
	if (kind == 0) {
		helper = "__freya_shl";
		need_shl = 1;
	} else if (kind == 1) {
		helper = "__freya_shr";
		need_shr = 1;
	} else {
		helper = "__freya_sar";
		need_sar = 1;
	}
	call2(f, dst, a, b, fn, helper, 0);
}

static char *
condbr(int op)
{
	switch (op) {
	case Oceqw: return "beq";
	case Ocnew: return "bne";
	case Ocsgew: return "bge";
	case Ocsgtw: return "bgt";
	case Ocslew: return "ble";
	case Ocsltw: return "blt";
	case Ocugew: return "bhis";
	case Ocugtw: return "bhi";
	case Oculew: return "blos";
	case Ocultw: return "blo";
	default:
		err("freya: unsupported comparison");
		return 0;
	}
}

static void
emitcmp(FILE *f, Ins *i, Fn *fn)
{
	char as[80], bs[80];
	int id;

	val(as, sizeof as, i->arg[0], fn);
	val(bs, sizeof bs, i->arg[1], fn);
	id = lbl;
	lbl += 2;
	fprintf(f, "\tcmp %s, %s\n", as, bs);
	fprintf(f, "\t%s .Lc%d\n", condbr(i->op), id);
	fprintf(f, "\tclr %s\n", rname[i->to.val]);
	fprintf(f, "\tbr .Lc%d\n", id + 1);
	fprintf(f, ".Lc%d:\n", id);
	fprintf(f, "\tmov #1, %s\n", rname[i->to.val]);
	fprintf(f, ".Lc%d:\n", id + 1);
}

static void
emitload(FILE *f, Ins *i, Fn *fn, int kind)
{
	char m0[80], m1[80];
	int dst;

	dst = i->to.val;
	memstr(m0, sizeof m0, i->arg[0], fn, 0);
	/* movb into a register sign-extends the byte through it, so a
	 * signed load is the one instruction and an unsigned load clears
	 * what the sign filled in. */
	if (kind == 0 || kind == 1) {
		fprintf(f, "\tmovb %s, %s\n", m0, rname[dst]);
		if (kind == 1)
			fprintf(f, "\tbic #-256, %s\n", rname[dst]);
		return;
	}
	if (kind == 2 || kind == 3) {
		memstr(m1, sizeof m1, i->arg[0], fn, 1);
		fprintf(f, "\tmov r4, -(sp)\n");
		fprintf(f, "\tmov #0, -(sp)\n");
		fprintf(f, "\tmovb %s, (sp)\n", m0);
		fprintf(f, "\tmovb %s, r4\n", m1);
		fprintf(f, "\tbic #-256, r4\n");
		fprintf(f, "\tash #8, r4\n");
		fprintf(f, "\tbis r4, (sp)\n");
		fprintf(f, "\tmov (sp)+, r4\n");
		if (kind == 2) {
			fprintf(f, "\tash #16, r4\n");
			fprintf(f, "\tash #-16, r4\n");
		}
		deliver(f, dst);
		return;
	}
	fprintf(f, "\tmov %s, %s\n", m0, rname[dst]);
}

static void
emitstore(FILE *f, Ins *i, Fn *fn, int width)
{
	char src[80], m0[80], m1[80];

	val(src, sizeof src, i->arg[0], fn);
	memstr(m0, sizeof m0, i->arg[1], fn, 0);
	if (width == 1) {
		fprintf(f, "\tmovb %s, %s\n", src, m0);
		return;
	}
	if (width == 2) {
		memstr(m1, sizeof m1, i->arg[1], fn, 1);
		fprintf(f, "\tmovb %s, %s\n", src, m0);
		fprintf(f, "\tmov r4, -(sp)\n");
		fprintf(f, "\tmov %s, r4\n", src);
		fprintf(f, "\tash #-8, r4\n");
		fprintf(f, "\tbic #-256, r4\n");
		fprintf(f, "\tmovb r4, %s\n", m1);
		fprintf(f, "\tmov (sp)+, r4\n");
		return;
	}
	fprintf(f, "\tmov %s, %s\n", src, m0);
}

static void
emitins(Ins *i, Fn *fn, FILE *f)
{
	char buf[80], buf2[80];
	Con *c;
	int64_t s;

	switch (i->op) {
	case Onop:
		break;
	case Odbgloc:
		emitdbgloc(i->arg[0].val, i->arg[1].val, f);
		break;
	case Oadd: binop(f, "add", i->to.val, i->arg[0], i->arg[1], fn, 1); break;
	case Osub: binop(f, "sub", i->to.val, i->arg[0], i->arg[1], fn, 0); break;
	case Oor:  binop(f, "bis", i->to.val, i->arg[0], i->arg[1], fn, 1); break;
	case Oand: emitand(f, i->to.val, i->arg[0], i->arg[1], fn); break;
	case Oxor: emitxor(f, i->to.val, i->arg[0], i->arg[1], fn); break;
	case Omul: emitmul(f, i->to.val, i->arg[0], i->arg[1], fn); break;
	case Odiv:
		need_div = 1;
		call2(f, i->to.val, i->arg[0], i->arg[1], fn, "__freya_div", 0);
		break;
	case Orem:
		need_rem = 1;
		call2(f, i->to.val, i->arg[0], i->arg[1], fn, "__freya_rem", 0);
		break;
	case Oudiv:
		need_udiv = 1;
		call2(f, i->to.val, i->arg[0], i->arg[1], fn, "__freya_udiv", 0);
		break;
	case Ourem:
		need_urem = 1;
		need_udiv = 1;
		call2(f, i->to.val, i->arg[0], i->arg[1], fn, "__freya_urem", 0);
		break;
	case Oshl: emitshift(f, i->to.val, i->arg[0], i->arg[1], fn, 0); break;
	case Oshr: emitshift(f, i->to.val, i->arg[0], i->arg[1], fn, 1); break;
	case Osar: emitshift(f, i->to.val, i->arg[0], i->arg[1], fn, 2); break;
	case Oneg:
		val(buf, sizeof buf, i->arg[0], fn);
		if (!isreg_(i->arg[0], i->to.val))
			fprintf(f, "\tmov %s, %s\n", buf, rname[i->to.val]);
		fprintf(f, "\tneg %s\n", rname[i->to.val]);
		break;
	case Oextsb:
	case Oextub:
	case Oextsh:
	case Oextuh:
	case Oextsw:
	case Oextuw:
		/* A word widened to an address is the same 32 bits. */
		val(buf, sizeof buf, i->arg[0], fn);
		if (!(rtype(i->arg[0]) == RTmp && i->arg[0].val == i->to.val))
			fprintf(f, "\tmov %s, %s\n", buf, rname[i->to.val]);
		if (i->op == Oextsw || i->op == Oextuw)
			break;
		if (i->op == Oextub)
			fprintf(f, "\tbic #-256, %s\n", rname[i->to.val]);
		else if (i->op == Oextuh)
			fprintf(f, "\tbic #-65536, %s\n", rname[i->to.val]);
		else if (i->op == Oextsb) {
			fprintf(f, "\tash #24, %s\n", rname[i->to.val]);
			fprintf(f, "\tash #-24, %s\n", rname[i->to.val]);
		} else {
			fprintf(f, "\tash #16, %s\n", rname[i->to.val]);
			fprintf(f, "\tash #-16, %s\n", rname[i->to.val]);
		}
		break;
	case Oceqw: case Ocnew: case Ocsgew: case Ocsgtw:
	case Ocslew: case Ocsltw: case Ocugew: case Ocugtw:
	case Oculew: case Ocultw:
		emitcmp(f, i, fn);
		break;
	case Oxcmp:
		/* only the condition codes; the branch follows */
		val(buf, sizeof buf, i->arg[0], fn);
		val(buf2, sizeof buf2, i->arg[1], fn);
		fprintf(f, "\tcmp %s, %s\n", buf, buf2);
		break;
	case Oloadsb: emitload(f, i, fn, 0); break;
	case Oloadub: emitload(f, i, fn, 1); break;
	case Oloadsh: emitload(f, i, fn, 2); break;
	case Oloaduh: emitload(f, i, fn, 3); break;
	case Oload: case Oloadsw: case Oloaduw:
		emitload(f, i, fn, 4);
		break;
	case Ostoreb: emitstore(f, i, fn, 1); break;
	case Ostoreh: emitstore(f, i, fn, 2); break;
	case Ostorew: emitstore(f, i, fn, 4); break;
	case Ostorel: emitstore(f, i, fn, 4); break; /* a spilled address */
	case Ocopy:
		if (req(i->to, i->arg[0]))
			break;
		if (!isreg(i->to)) {
			char dst[80];

			assert(rtype(i->to) == RSlot);
			val(buf, sizeof buf, i->arg[0], fn);
			memstr(dst, sizeof dst, i->to, fn, 0);
			fprintf(f, "\tmov %s, %s\n", buf, dst);
			break;
		}
		switch (rtype(i->arg[0])) {
		case RCon:
			c = &fn->con[i->arg[0].val];
			if (c->type == CBits)
				fprintf(f, "\tmov #%"PRId64", %s\n",
					(int64_t)(int32_t)c->bits.i, rname[i->to.val]);
			else if (c->type == CAddr) {
				fprintf(f, "\tmov #");
				fputs(str(c->sym.id), f);
				if (c->bits.i)
					fprintf(f, "%+"PRId64, c->bits.i);
				fprintf(f, ", %s\n", rname[i->to.val]);
			} else
				die("bad copy");
			break;
		case RSlot:
			memstr(buf, sizeof buf, i->arg[0], fn, 0);
			fprintf(f, "\tmov %s, %s\n", buf, rname[i->to.val]);
			break;
		default:
			fprintf(f, "\tmov %s, %s\n", rname[i->arg[0].val], rname[i->to.val]);
			break;
		}
		break;
	case Oswap:
		fprintf(f, "\tmov %s, r4\n", rname[i->arg[0].val]);
		fprintf(f, "\tmov %s, %s\n", rname[i->arg[1].val], rname[i->arg[0].val]);
		fprintf(f, "\tmov r4, %s\n", rname[i->arg[1].val]);
		break;
	case Oaddr:
		assert(rtype(i->arg[0]) == RSlot);
		s = slot(i->arg[0], fn);
		fprintf(f, "\tmov r5, %s\n", rname[i->to.val]);
		if (s)
			fprintf(f, "\tadd #%"PRId64", %s\n", s, rname[i->to.val]);
		break;
	case Osalloc:
		val(buf, sizeof buf, i->arg[0], fn);
		fprintf(f, "\tsub %s, sp\n", buf);
		if (!req(i->to, R))
			fprintf(f, "\tmov sp, %s\n", rname[i->to.val]);
		break;
	case Ocall:
		if (rtype(i->arg[0]) == RCon) {
			c = &fn->con[i->arg[0].val];
			if (c->type != CAddr || c->sym.type != SGlo || c->bits.i)
				die("invalid call argument");
			fprintf(f, "\tjsr pc, @#%s\n", str(c->sym.id));
		} else if (rtype(i->arg[0]) == RTmp) {
			fprintf(f, "\tjsr pc, (%s)\n", rname[i->arg[0].val]);
		} else
			die("invalid call argument");
		break;
	default:
		err("freya: cannot emit %s", optab[i->op].name);
	}
}

static void
helper(FILE *f, char *name, char *body)
{
	fprintf(f, ".text\n.globl %s\n%s:\n%s", name, name, body);
}

static char *
condname(int c)
{
	switch (c) {
	case Cieq: return "eq";
	case Cine: return "ne";
	case Cisge: return "ge";
	case Cisgt: return "gt";
	case Cisle: return "le";
	case Cislt: return "lt";
	case Ciuge: return "his";
	case Ciugt: return "hi";
	case Ciule: return "los";
	case Ciult: return "lo";
	}
	die("bad condition");
	return 0;
}

/* Does instruction i write register r? */
static int
writesreg(Ins *i, int r)
{
	if (i->op == Ocall)
		return 1;
	if (i->op == Oswap)
		return isreg_(i->arg[0], r) || isreg_(i->arg[1], r);
	return isreg_(i->to, r);
}

/* Is it safe to move the compare xc past the instructions after it,
 * to just before the branch?  Only if none of them writes one of its
 * operands or the memory it reads. */
static int
canmove(Ins *xc, Ins *end)
{
	Ins *i;
	int n;

	for (i = xc + 1; i < end; i++) {
		if (isstore(i->op) || i->op == Ocall)
			return 0;
		for (n = 0; n < 2; n++)
			if (rtype(xc->arg[n]) == RTmp && writesreg(i, xc->arg[n].val))
				return 0;
	}
	return 1;
}

void
freya_emitfn(Fn *fn, FILE *f)
{
	static int id0;
	int lbln, neg, frame, c, stackcond;
	char sbuf[80], sbuf2[80];
	Blk *b, *s;
	Ins *i, *xc;

	emitfnlnk(fn->name, &fn->lnk, f);
	fprintf(f, "\tmov r5, -(sp)\n");
	fprintf(f, "\tmov sp, r5\n");
	frame = 4 * fn->slot;
	if (frame)
		fprintf(f, "\tsub #%d, sp\n", frame);

	for (lbln = 0, b = fn->start; b; b = b->link) {
		if (lbln || b->npred > 1)
			fprintf(f, ".L%d:\n", id0 + b->id);
		xc = 0;
		stackcond = 0;
		for (i = b->ins; i != &b->ins[b->nins]; i++) {
			if (i->op == Oxcmp && b->jmp.type >= Jjf && b->jmp.type <= Jjf1) {
				/* The spiller may have put reloads between
				 * the compare and the branch, and mov sets
				 * the condition codes.  Delay the compare
				 * when that is safe; otherwise keep the
				 * outcome on the stack until the branch. */
				if (canmove(i, &b->ins[b->nins])) {
					xc = i;
					continue;
				}
				c = b->jmp.type - Jjf;
				emitins(i, fn, f);
				fprintf(f, "\tb%s .Lc%d\n", condname(c), lbl);
				fprintf(f, "\tclr -(sp)\n");
				fprintf(f, "\tbr .Lc%d\n", lbl + 1);
				fprintf(f, ".Lc%d:\n", lbl);
				fprintf(f, "\tmov #1, -(sp)\n");
				fprintf(f, ".Lc%d:\n", lbl + 1);
				lbl += 2;
				stackcond = 1;
				continue;
			}
			emitins(i, fn, f);
		}
		lbln = 1;
		switch (b->jmp.type) {
		case Jhlt:
			fprintf(f, "\thalt\n");
			break;
		case Jret0:
			fprintf(f, "\tmov r5, sp\n");
			fprintf(f, "\tmov (sp)+, r5\n");
			fprintf(f, "\trts pc\n");
			break;
		case Jjmp:
		Jmp:
			if (b->s1 != b->link)
				fprintf(f, "\tbr .L%d\n", id0 + b->s1->id);
			else
				lbln = 0;
			break;
		case Jjnz:
			neg = 0;
			if (b->link == b->s2) {
				s = b->s1;
				b->s1 = b->s2;
				b->s2 = s;
				neg = 1;
			}
			/* The spiller may leave the condition in a stack
			 * slot when four registers are not enough; tst
			 * reads memory as happily as a register. */
			if (rtype(b->jmp.arg) == RSlot) {
				memstr(sbuf, sizeof sbuf, b->jmp.arg, fn, 0);
				fprintf(f, "\ttst %s\n", sbuf);
			} else {
				assert(isreg(b->jmp.arg));
				fprintf(f, "\ttst %s\n", rname[b->jmp.arg.val]);
			}
			fprintf(f, "\tb%s .L%d\n",
				neg ? "ne" : "eq", id0 + b->s2->id);
			goto Jmp;
		default:
			if (b->jmp.type < Jjf || b->jmp.type > Jjf1)
				die("unhandled jump");
			/* jump to s1 when the condition holds, else s2;
			 * the branch goes to s2, so when s2 is the block
			 * that follows the two are swapped and the branch
			 * is taken on the condition itself. */
			c = b->jmp.type - Jjf;
			neg = 1;
			if (b->link == b->s2) {
				s = b->s1;
				b->s1 = b->s2;
				b->s2 = s;
				neg = 0;
			} else
				c = cmpneg(c);
			if (stackcond) {
				/* the stack holds 1 when the condition held */
				fprintf(f, "\ttst (sp)+\n");
				fprintf(f, "\tb%s .L%d\n", neg ? "eq" : "ne",
					id0 + b->s2->id);
				goto Jmp;
			}
			if (xc) {
				val(sbuf, sizeof sbuf, xc->arg[0], fn);
				val(sbuf2, sizeof sbuf2, xc->arg[1], fn);
				fprintf(f, "\tcmp %s, %s\n", sbuf, sbuf2);
			}
			fprintf(f, "\tb%s .L%d\n", condname(c), id0 + b->s2->id);
			goto Jmp;
		}
	}
	id0 += fn->nblk;
	elf_emitfnfin(fn->name, f);
}

void
freya_emitfin(FILE *f)
{
	if (need_div)
		helper(f, "__freya_div",
			"\tmov r1, -(sp)\n"
			"\tmov r0, r1\n"
			"\tmov r0, r4\n"
			"\tash #-31, r0\n"
			"\tdiv (sp)+, r0\n"
			"\trts pc\n");
	if (need_rem)
		helper(f, "__freya_rem",
			"\tmov r1, -(sp)\n"
			"\tmov r0, r1\n"
			"\tmov r0, r4\n"
			"\tash #-31, r0\n"
			"\tdiv (sp)+, r0\n"
			"\tmov r1, r0\n"
			"\trts pc\n");
	if (need_udiv)
		helper(f, "__freya_udiv",
			"\tclr r2\n"
			"\tclr r3\n"
			"\tmov #32, -(sp)\n"
			"1:\n"
			"\tasl r2\n"
			"\tasl r0\n"
			"\trol r3\n"
			"\tcmp r3, r1\n"
			"\tblo 2f\n"
			"\tsub r1, r3\n"
			"\tinc r2\n"
			"2:\n"
			"\tdec (sp)\n"
			"\tbne 1b\n"
			"\tadd #4, sp\n"
			"\tmov r2, r0\n"
			"\tmov r3, r1\n"
			"\trts pc\n");
	if (need_urem)
		helper(f, "__freya_urem",
			"\tjsr pc, @#__freya_udiv\n"
			"\tmov r1, r0\n"
			"\trts pc\n");
	if (need_shl)
		helper(f, "__freya_shl",
			"\tbic #-32, r1\n"
			"\ttst r1\n"
			"\tbeq 2f\n"
			"1:\n"
			"\tasl r0\n"
			"\tdec r1\n"
			"\tbne 1b\n"
			"2:\n"
			"\trts pc\n");
	if (need_shr)
		helper(f, "__freya_shr",
			"\tbic #-32, r1\n"
			"\ttst r1\n"
			"\tbeq 2f\n"
			"1:\n"
			"\tclc\n"
			"\tror r0\n"
			"\tdec r1\n"
			"\tbne 1b\n"
			"2:\n"
			"\trts pc\n");
	if (need_sar)
		helper(f, "__freya_sar",
			"\tbic #-32, r1\n"
			"\ttst r1\n"
			"\tbeq 2f\n"
			"1:\n"
			"\tasr r0\n"
			"\tdec r1\n"
			"\tbne 1b\n"
			"2:\n"
			"\trts pc\n");
	elf_emitfin(f);
}
