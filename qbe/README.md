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
