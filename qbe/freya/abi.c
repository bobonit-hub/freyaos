#include "all.h"

/* ILP32, stack arguments, result in R0.
 *
 * Every argument is a 32-bit word on the stack. An aggregate is a
 * pointer to a caller-made copy. A structure return value is written
 * through a pointer passed as the first argument. Variadic arguments
 * are ordinary stack words; va_list is a pointer to the next one.
 *
 * After the prologue, 8(fp) is the first argument. fp is R5.
 */

typedef struct Insl Insl;
typedef struct Params Params;

struct Insl {
	Ins i;
	Insl *link;
};

struct Params {
	int stk; /* slot index of the first variadic word */
};

static void
stkblob(Ref r, Typ *t, Fn *fn, Insl **ilp)
{
	Insl *il;
	int al;
	uint64_t sz;

	il = alloc(sizeof *il);
	al = t->align - 2; /* NAlign == 3 */
	if (al < 0)
		al = 0;
	sz = (t->size + 3) & ~3u;
	il->i = (Ins){Oalloc + al, Kw, r, {getcon(sz, fn)}};
	il->link = *ilp;
	*ilp = il;
}

static void
stackstore(Ref val, Ref base, int off, Fn *fn)
{
	Ref a;

	a = newtmp("abi", Kw, fn);
	emit(Ostorew, Kw, R, val, a);
	emit(Oadd, Kw, a, base, getcon(off, fn));
}

bits
freya_retregs(Ref r, int p[2])
{
	int ngp;

	assert(rtype(r) == RCall);
	ngp = r.val & 3;
	if (p) {
		p[0] = ngp;
		p[1] = 0;
	}
	return ngp ? BIT(R0) : 0;
}

bits
freya_argregs(Ref r, int p[2])
{
	(void)r;
	if (p) {
		p[0] = 0;
		p[1] = 0;
	}
	return 0;
}

static void
selret2(Blk *b, Fn *fn)
{
	int j;
	Ref r;

	j = b->jmp.type;
	if (!isret(j) || j == Jret0)
		return;
	r = b->jmp.arg;
	b->jmp.type = Jret0;
	if (j == Jretc) {
		emit(Oblit1, 0, R, INT(typ[fn->retty].size), R);
		emit(Oblit0, 0, R, r, fn->retr);
		b->jmp.arg = CALL(0);
		return;
	}
	if (j != Jretw && j != Jretl && !(j >= Jretsb && j <= Jretuh))
		err("freya: only integer results are supported");
	emit(Ocopy, Kw, TMP(R0), r, R);
	b->jmp.arg = CALL(1);
}

static Params
selpar(Fn *fn, Ins *i0, Ins *i1)
{
	Ins *i;
	int s;

	curi = &insb[NIns];
	s = 1;
	if (fn->retty >= 0) {
		fn->retr = newtmp("abi", Kw, fn);
		emit(Oload, Kw, fn->retr, SLOT(-s), R);
		s++;
	}
	for (i = i0; i < i1; i++) {
		switch (i->op) {
		case Opar:
		case Oparc:
			if (i->op == Opar && i->cls != Kw && i->cls != Kl)
				err("freya: only integer parameters are supported");
			emit(Oload, Kw, i->to, SLOT(-s), R);
			s++;
			break;
		case Opare:
			err("freya: env parameters are not supported");
		case Oargv:
			break;
		default:
			die("unreachable");
		}
	}
	return (Params){.stk = s};
}

static void
selvaarg(Fn *fn, Ins *i)
{
	Ref loc, nxt;

	if (i->cls != Kw && i->cls != Kl)
		err("freya: va_arg of a floating value");
	loc = newtmp("abi", Kw, fn);
	nxt = newtmp("abi", Kw, fn);
	emit(Ostorew, Kw, R, nxt, i->arg[0]);
	emit(Oadd, Kw, nxt, loc, getcon(4, fn));
	emit(Oload, Kw, i->to, loc, R);
	emit(Oload, Kw, loc, i->arg[0], R);
}

static void
selvastart(Fn *fn, Params p, Ref ap)
{
	Ref r;

	r = newtmp("abi", Kw, fn);
	emit(Ostorew, Kw, R, r, ap);
	emit(Oaddr, Kw, r, SLOT(-p.stk), R);
}

static void
selcall(Fn *fn, Ins *i0, Ins *i1, Insl **ilp)
{
	Ins *i;
	Ref sp, ptr;
	int off, stk, cty, retptr;
	Typ *t;

	/* arg[1] is a type when the call returns a struct, else R. */
	retptr = !req(i1->arg[1], R);
	stk = retptr ? 4 : 0;
	for (i = i0; i < i1; i++) {
		if (i->op == Oargv)
			continue;
		if (i->op == Oarge)
			err("freya: env arguments are not supported");
		if (i->op == Oarg && i->cls != Kw && i->cls != Kl)
			err("freya: only integer arguments are supported");
		stk += 4;
	}
	cty = 0;
	if (stk)
		emit(Osalloc, Kl, R, getcon(-(int64_t)stk, fn), R);

	if (retptr) {
		t = &typ[i1->arg[1].val];
		stkblob(i1->to, t, fn, ilp);
	} else if (!req(i1->to, R)) {
		if (i1->cls != Kw && i1->cls != Kl)
			err("freya: only integer results are supported");
		emit(Ocopy, Kw, i1->to, TMP(R0), R);
		cty = 1;
	}

	emit(Ocall, 0, R, i1->arg[0], CALL(cty));

	off = 0;
	sp = newtmp("abi", Kw, fn);
	if (retptr) {
		stackstore(i1->to, sp, off, fn);
		off += 4;
	}
	for (i = i0; i < i1; i++) {
		if (i->op == Oargv)
			continue;
		if (i->op == Oargc) {
			t = &typ[i->arg[0].val];
			ptr = newtmp("abi", Kw, fn);
			stkblob(ptr, t, fn, ilp);
			stackstore(ptr, sp, off, fn);
			emit(Oblit1, 0, R, INT(t->size), R);
			emit(Oblit0, 0, R, i->arg[1], ptr);
		} else {
			stackstore(i->arg[0], sp, off, fn);
		}
		off += 4;
	}
	if (stk)
		emit(Osalloc, Kl, sp, getcon(stk, fn), R);
	else
		emit(Ocopy, Kw, sp, TMP(SP), R);
}

void
freya_abi(Fn *fn)
{
	Blk *b;
	Ins *i, *i0;
	Insl *il;
	int n0, n1, ioff;
	Params p;

	for (b = fn->start; b; b = b->link)
		b->visit = 0;

	for (b = fn->start, i = b->ins; i < &b->ins[b->nins]; i++)
		if (!ispar(i->op))
			break;
	p = selpar(fn, b->ins, i);
	n0 = &insb[NIns] - curi;
	ioff = i - b->ins;
	n1 = b->nins - ioff;
	vgrow(&b->ins, n0 + n1);
	icpy(b->ins + n0, b->ins + ioff, n1);
	icpy(b->ins, curi, n0);
	b->nins = n0 + n1;

	il = 0;
	b = fn->start;
	do {
		if (!(b = b->link))
			b = fn->start;
		if (b->visit)
			continue;
		curi = &insb[NIns];
		selret2(b, fn);
		for (i = &b->ins[b->nins]; i != b->ins;)
			switch ((--i)->op) {
			default:
				if (i->op != Osalloc && KBASE(i->cls) == 1)
					err("freya: 64-bit and floating-point values are not supported");
				emiti(*i);
				break;
			case Ocall:
				for (i0 = i; i0 > b->ins; i0--)
					if (!isarg((i0 - 1)->op))
						break;
				selcall(fn, i0, i, &il);
				i = i0;
				break;
			case Ovastart:
				selvastart(fn, p, i->arg[0]);
				break;
			case Ovaarg:
				selvaarg(fn, i);
				break;
			case Oarg:
			case Oargc:
				die("unreachable");
			}
		if (b == fn->start)
			for (; il; il = il->link)
				emiti(il->i);
		idup(b, curi, &insb[NIns] - curi);
	} while (b != fn->start);

	if (debug['A']) {
		fprintf(stderr, "\n> After ABI lowering:\n");
		printfn(fn, stderr);
	}
}
