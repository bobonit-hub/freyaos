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
				r1 = SLOT(s);
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

static void
sel(Ins i, Fn *fn)
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
	if (i.op != Onop) {
		emiti(i);
		i0 = curi;
		fixarg(&i0->arg[0], argcls(&i, 0), i0, fn);
		fixarg(&i0->arg[1], argcls(&i, 1), i0, fn);
	}
}

static void
seljmp(Blk *b, Fn *fn)
{
	if (b->jmp.type == Jjnz)
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
			sel(*--i, fn);
		idup(b, curi, &insb[NIns] - curi);
	}

	if (debug['I']) {
		fprintf(stderr, "\n> After instruction selection:\n");
		printfn(fn, stderr);
	}
}
