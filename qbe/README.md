# QBE and cproc for the Freya VM

The Freya virtual machine is a PDP-11 whose eight registers and whose
words are 32 bits. A byte is still 8 bits. `freya/` is a QBE target for
that machine, and `cproc.patch` teaches cproc the matching ILP32 C
types. The assembler in `as.py` turns the listing into an image the VM
can run from address 0.

## Language

`int`, `long` and pointers are 32 bits. `long long` is rejected.
Floating point is rejected. QBE still spells an address temporary `l`,
because that is the class its memory operands use; on this target that
temporary holds 32 bits. A pointer in memory is a 32-bit word.

`movb` into a register sign-extends, so a `signed char` load is that
one instruction and an `unsigned char` load is `movb` then
`bic #-256`. A 16-bit load is two byte loads joined with `ash` and
`bis`, through r4 and a word of stack.

## Calling convention

Arguments are 32-bit words on the stack, pushed by the caller. An
aggregate is a pointer to a copy the caller made. A structure result is
written through a pointer passed as the first argument. The scalar
result is in R0.

R5 is the frame pointer, R6 the stack pointer and R7 the program
counter. R0–R3 are caller-saved. R4 is reserved for the code generator.
`jsr pc, dst` calls and `rts pc` returns. After the prologue the first
argument is at `8(r5)`.

## Building

Copy `freya/` into a QBE tree and apply `qbe.patch` there. Apply
`cproc.patch` in a cproc tree, then build both. `./configure --target=freya`
in cproc selects this machine. There is no system linker for the VM, so
the useful pipeline stops at the image:

```sh
cproc-qbe -t freya prog.c | qbe -t freya | python3 qbe/as.py -o prog.bin --entry main
```

`--entry` prints the offset of that symbol. `runvm.c` loads the image
and halts when the entry function returns. Link it with `src/vm.c` the
same way `tests/host_vm_test.c` is linked. The value left in R0 is the
function result.

`--map file` writes every symbol with its offset, one `%08x name` per
line, which is what a fault address is looked up in. Several `.s`
files can be concatenated before assembling; `basic/rt.s` in front of
the compiler output is how the BASIC image gets its `_start` at 0 and
its system calls.

A branch whose target is out of the 8-bit word range is relaxed into
`jmp @#target`, and the assembler iterates until no offset changes,
since a long branch moves everything after it. Block comments may
span lines, and string data uses the octal escapes QBE writes.

## Code generation

Address arithmetic that is a temporary plus a constant is folded into
the load or store as an indexed operand, `off(rN)`, or `off(r5)` for
a local; the add itself is dropped when the address had no other use.
The candidates are collected before any block is rewritten, because
instruction selection frees the old instruction arrays as it goes and
the `def` pointers of temporaries defined in earlier blocks would
dangle.

A `jnz` on a comparison that has no other use becomes `cmp` and a
conditional branch. The compare is emitted right before the branch
when nothing between them stores, calls or writes its operands;
otherwise it stays where it was and leaves a word on the stack that
`tst (sp)+` reads at the branch.

Binary operations take one operand from memory, so a temporary the
register allocator spilled is used from its slot without a reload;
loads and stores are the exception, since a slot in their address
position means the slot itself, which is how the ABI and the spiller
use them. R4 is scratch for the sequences that need a register of
their own: multiplication into an even register, exclusive or, and
the 16-bit accesses.

Every call is followed by a copy from R0 because the spiller and the
register allocator use that copy as the place where temporaries live
across the call are saved; a call without a result gets a copy to
nowhere. A call with no arguments leaves SP alone.

## Known limits

Casting a pointer to `unsigned long` and back is not supported; cast
to `uint32_t` (which is `unsigned int`). `cproc.patch` inserts the
`extuw` a pointer needs when it comes from a 32-bit integer or goes to
one, but `unsigned long` is the class QBE spells structure member
access in and is left alone.
