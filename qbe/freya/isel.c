#include "all.h"

static int
memarg(Ref *r, int op, Ins *i)
{
	if (!i)
		return 0;
	if (isload(op) || op == Ocall)
		return r == &i->arg[0];
	if (isstore(op))
		return r == &i->arg[1];
	return 0;
}

static void
fixarg(Ref *r, int k, Ins *i, Fn *fn)
{
	Ref r0, r1;
	int s, op;
	Con *c;

	if (k == Ks || k == Kd)
		err("freya: floating-point values are not supported");
	if (k < 0)
		return;
	/* Km, the class of an address, is Kl. Pointers are 32 bits. */
	if (k == Kl)
		k = Kw;
	r0 = r1 = *r;
	op = i ? i->op : Ocopy;
	switch (rtype(r0)) {
	case RCon:
		c = &fn->con[r0.val];
		if (c->type == CAddr && memarg(r, op, i))
			break;
		if (k == Kw && c->type == CBits)
			break;
		r1 = newtmp("isel", k, fn);
		emit(Ocopy, k, r1, r0, R);
		break;
	case RTmp:
		if (isreg(r0))
			break;
		s = fn->tmp[r0.val].slot;
		if (s != -1) {
			if (memarg(r, op, i)) {
				/* the slot itself is the memory; a plain
				 * RSlot address would mean a pointer that
				 * the spiller left in a slot */
				vgrow(&fn->mem, ++fn->nmem);
				fn->mem[fn->nmem - 1] = (Mem){
					.offset = {.type = CBits, .bits.i = 0},
					.base = SLOT(s), .index = R, .scale = 1};
				r1 = MEM(fn->nmem - 1);
				break;
			}
			r1 = newtmp("isel", k, fn);
			emit(Oaddr, k, r1, SLOT(s), R);
			break;
		}
		if (KBASE(fn->tmp[r0.val].cls) == 1)
			err("freya: floating-point values are not supported");
		break;
	}
	*r = r1;
}

/* The address of a load or store that is "base + constant" becomes an
 * indexed operand, off(rN) or off(r5) for a local, which the PDP-11
 * has for free.  The add itself goes when the address had no other
 * use.  Blocks are selected forwards and instructions backwards, so
 * a definition in this block has not been copied out yet and can be
 * turned into a nop; one in an earlier block just stays.
 *
 * The candidates are collected before any block is rewritten: idup()
 * frees the old instruction arrays and the def pointers of temporaries
 * in earlier blocks would dangle. */
typedef struct {
	Ref base;
	int32_t off;
	char ok;
} Fold;

static Fold *folds;
static uint nfolds;

static void
findfolds(Fn *fn)
{
	Tmp *t;
	Ins *d;
	Ref base, off;
	Con *c;
	uint n;

	nfolds = fn->ntmp;
	folds = emalloc(nfolds * sizeof folds[0]);
	for (n = Tmp0; n < nfolds; n++) {
		t = &fn->tmp[n];
		d = t->def;
		if (!d || d->op != Oadd)
			continue;
		if (rtype(d->arg[1]) == RCon && rtype(d->arg[0]) == RTmp) {
			base = d->arg[0];
			off = d->arg[1];
		} else if (rtype(d->arg[0]) == RCon && rtype(d->arg[1]) == RTmp) {
			base = d->arg[1];
			off = d->arg[0];
		} else
			continue;
		c = &fn->con[off.val];
		if (c->type != CBits || isreg(base))
			continue;
		if (c->bits.i < -0x7fffffff || c->bits.i > 0x7fffffff)
			continue;
		folds[n].base = base;
		folds[n].off = c->bits.i;
		folds[n].ok = 1;
	}
}

static void
foldaddr(Ref *r, Blk *b, Fn *fn)
{
	Ins *d;
	Ref base;
	Mem *m;
	Fold *fo;
	int s;

	if (rtype(*r) != RTmp || isreg(*r) || r->val >= nfolds)
		return;
	fo = &folds[r->val];
	if (!fo->ok)
		return;
	base = fo->base;
	s = fn->tmp[base.val].slot;
	vgrow(&fn->mem, ++fn->nmem);
	m = &fn->mem[fn->nmem - 1];
	m->offset = (Con){.type = CBits, .bits.i = fo->off};
	m->base = s != -1 ? SLOT(s) : base;
	m->index = R;
	m->scale = 1;
	d = fn->tmp[r->val].def;
	if (fn->tmp[r->val].nuse == 1 && d >= b->ins && d < &b->ins[b->nins]) {
		assert(d->op == Oadd && req(d->to, *r));
		*d = (Ins){.op = Onop};
	}
	*r = MEM(fn->nmem - 1);
}

static void
sel(Ins i, Blk *b, Fn *fn)
{
	Ins *i0;
	Ref r0, r1;
	int64_t sz;

	if (INRANGE(i.op, Oalloc, Oalloc1)) {
		fn->dynalloc = 1;
		if (rtype(i.arg[0]) == RCon) {
			sz = fn->con[i.arg[0].val].bits.i;
			if (sz < 0 || sz >= INT_MAX - 15)
				err("invalid alloc size %"PRId64, sz);
			sz = (sz + 15) & -16;
			emit(Osalloc, Kl, i.to, getcon(sz, fn), R);
		} else {
			r0 = newtmp("isel", Kw, fn);
			r1 = newtmp("isel", Kw, fn);
			emit(Osalloc, Kl, i.to, r0, R);
			emit(Oand, Kw, r0, r1, getcon(-16, fn));
			emit(Oadd, Kw, r1, i.arg[0], getcon(15, fn));
			i0 = curi;
			fixarg(&i0->arg[0], Kw, i0, fn);
			i0 = curi;
			fixarg(&i0->arg[1], Kw, i0, fn);
		}
		return;
	}
	if (KBASE(i.cls) == 1)
		err("freya: floating-point values are not supported");
	if (i.op == Onop)
		return;
	if (isload(i.op))
		foldaddr(&i.arg[0], b, fn);
	else if (isstore(i.op))
		foldaddr(&i.arg[1], b, fn);
	emiti(i);
	i0 = curi;
	fixarg(&i0->arg[0], argcls(&i, 0), i0, fn);
	fixarg(&i0->arg[1], argcls(&i, 1), i0, fn);
}

/* A jnz on a comparison used nowhere else becomes a compare that sets
 * the condition codes and a conditional branch; the comparison is
 * re-emitted as the last instruction of the block. */
static void
seljmp(Blk *b, Fn *fn)
{
	Ref r;
	Tmp *t;
	Ins *fi;
	int c, k;

	if (b->jmp.type != Jjnz)
		return;
	r = b->jmp.arg;
	if (rtype(r) == RTmp && !isreg(r) && b->s1 != b->s2) {
		t = &fn->tmp[r.val];
		fi = t->def;
		if (fi && fi >= b->ins && fi < &b->ins[b->nins]
		&& req(fi->to, r)
		&& t->nuse == 1 && iscmp(fi->op, &k, &c)
		&& (k == Kw || k == Kl) && c < NCmpI) {
			emit(Oxcmp, Kw, R, fi->arg[0], fi->arg[1]);
			fixarg(&curi->arg[0], Kw, curi, fn);
			fixarg(&curi->arg[1], Kw, curi, fn);
			*fi = (Ins){.op = Onop};
			b->jmp.type = Jjf + c;
			b->jmp.arg = R;
			return;
		}
	}
	fixarg(&b->jmp.arg, Kw, 0, fn);
}

void
freya_isel(Fn *fn)
{
	Blk *b, **sb;
	Ins *i;
	Phi *p;
	uint n, al;
	int64_t sz;

	b = fn->start;
	for (al = Oalloc, n = 4; al <= Oalloc1; al++, n *= 2)
		for (i = b->ins; i < &b->ins[b->nins]; i++)
			if (i->op == al) {
				if (rtype(i->arg[0]) != RCon)
					break;
				sz = fn->con[i->arg[0].val].bits.i;
				if (sz < 0 || sz >= INT_MAX - 15)
					err("invalid alloc size %"PRId64, sz);
				sz = (sz + n - 1) & -n;
				sz /= 4;
				if (sz > INT_MAX - fn->slot)
					die("alloc too large");
				fn->tmp[i->to.val].slot = fn->slot;
				fn->slot += sz;
				*i = (Ins){.op = Onop};
			}

	findfolds(fn);
	for (b = fn->start; b; b = b->link) {
		curi = &insb[NIns];
		for (sb = (Blk *[3]){b->s1, b->s2, 0}; *sb; sb++)
			for (p = (*sb)->phi; p; p = p->link) {
				if (p->cls == Ks || p->cls == Kd)
					err("freya: floating-point values are not supported");
				for (n = 0; p->blk[n] != b; n++)
					assert(n + 1 < p->narg);
				fixarg(&p->arg[n], p->cls, 0, fn);
			}
		seljmp(b, fn);
		for (i = &b->ins[b->nins]; i != b->ins;)
			sel(*--i, b, fn);
		idup(b, curi, &insb[NIns] - curi);
	}

	free(folds);
	folds = 0;
	nfolds = 0;

	if (debug['I']) {
		fprintf(stderr, "\n> After instruction selection:\n");
		printfn(fn, stderr);
	}
}
