# BASIC for Freya

A BASIC in the manner of DEC's BASIC-11, written in C, that builds two
ways. Compiled with cproc and QBE it is an image for the PDP-11 that
`src/vm.c` runs — eight 32-bit registers, 32-bit words, the PDP-11
opcodes — with the FP11 floating point done in software. Compiled with
GCC for the Cortex-M4F boards it is the native program `basic11`, whose
numbers are the C `float` on the FPU. The same source compiles for the
PC both ways for testing. Nothing here needs a C library; the
interpreter talks to whatever runs it through ten system calls.

```
$ build/basic/runbasic build/basic/basic.bin
BASIC-11 for the Freya VM
0 bytes of program, 56488 free

READY

10 FOR I=1 TO 3
20 PRINT I, SQR(I)
30 NEXT I
RUNNH
 1             1
 2             1.4142135623731
 3             1.73205080756888

READY

BYE
```

## Files

| file         | what it is                                                     |
|--------------|----------------------------------------------------------------|
| `basic.c`    | the interpreter, one translation unit that includes its arithmetic |
| `fp11.c/.h`  | the PDP-11 FP11 in software, from simh's `pdp11_fp.c`          |
| `fpnat.c/.h` | the same interface on the C `float`, for the native build      |
| `bas.h`      | the types, `setjmp` and the system call prototypes per target  |
| `rt.s`       | `_start` and the ten system calls of the VM image              |
| `runbasic.c` | runs the image on a PC with `src/vm.c`; serves the traps       |
| `hostrt.c`   | the same ten calls with stdio, for the PC builds               |
| `Makefile`   | builds the image, the PC interpreters and `runbasic`           |

Two Freya programs host it. `samples/basic11/main.c` includes `basic.c`
and is compiled by the top-level Makefile for the boards with an FPU:
it implements the ten calls on the Freya API, supplies `setjmp` and
`longjmp` for the Cortex-M4F, and takes the workspace from the heap.
`samples/basic/main.c` is the same host as `runbasic.c` written against
the Freya API, for a board whose heap can hold the VM image.

Three macros pick the build: `BAS_VM` for cproc, `BAS_HOST` for the PC
with a C library, neither for the bare-metal program; `BAS_FP11` takes
the FP11 arithmetic instead of `fpnat.c`. `BAS_BANNER` is the first
line printed.

## Building

```sh
make BOARD=blackpill samples                              # build/blackpill/samples/basic11.bin
make -C basic CPROC=/path/to/cproc-qbe QBE=/path/to/qbe   # build/basic/basic.bin
make -C basic host                                        # basic-host, basic-float, runbasic
make -C basic test                                        # tests/basic on all of them
```

The VM image needs the cproc and QBE builds described in
[`../qbe/README.md`](../qbe/README.md). cproc-qbe has no include path
option, so the C preprocessor runs first; `as.py` then turns the QBE
listing plus `rt.s` into `basic.bin`, which the VM runs from address 0.
`basic-host` is the PC build with the FP11 arithmetic, `basic-float`
with the `float` one; the first is what the VM image computes, the
second what `basic11` computes.

```
run basic11 [-m KiB] [program.bas]
runbasic [-m KiB] [-r program.bas] [-s] basic.bin
basic-host [-m KiB] [-r program.bas]
basic-float [-m KiB] [-r program.bas]
```

On the board `-m` asks for a workspace of that many KiB from the heap;
without it the program takes 32 KiB, or the largest multiple of 4 KiB
down to 12 that the heap can give. A program named on the command line
is loaded and run as though `OLD` and `RUN` had been typed; the
interpreter then goes on to `READY`. `BYE` returns to the shell. On the
PC `-m` is the size of the machine's memory, 256 KiB by default; on the
VM the image, the heap and a 16 KiB stack share it. `-r` loads a
program, runs it without the banner and the prompts and exits with 0
at END, or 1 after an error. `-s` prints the number of instructions
the VM executed.

The native program is 16 KiB of Thumb-2 code plus 11 KiB of static
data, the variable tables mostly, and runs from the shell's stack. The
VM image is about 100 KiB: an instruction of this machine is four
bytes and most operands are another four, and about 13 KiB of it is
the variable tables. That is more than the 56 KiB program region of
the boards Freya runs on today, so `samples/basic` reports that it has
too little memory there; it runs unchanged on a board with a larger
heap, and on the PC.

## The system calls

The interpreter is entered through `bas_main(heap, size, flags)`;
`flags & 1` is batch mode. It calls back through ten functions, `sys_exit`
to `sys_unlink` in `bas.h`, that the host defines. In the VM image they
are `TRAP n` with the arguments in R0–R2 and the result in R0, and
`main(heap_lo, heap_hi, flags)` is called with a normal C frame whose
return address is a HALT; the host finds `n` in the low byte of the word
before the PC the VM stopped at. In `basic11` they are ordinary
functions on the Freya API: `readline` reads the console with echo and
editing, `break` polls it for Ctrl-C, `open` and the rest are the file
calls, `ticks` the millisecond clock.

| n | call                    | notes                                   |
|---|-------------------------|-----------------------------------------|
| 0 | `exit(code)`            |                                         |
| 1 | `putc(c)`               | `\n` ends a line                        |
| 2 | `readline(buf, max)`    | length without the newline, −1 at EOF   |
| 3 | `break()`               | 1 once since the last call ^C was seen  |
| 4 | `open(path, mode)`      | mode 0 read, 1 write and create; fd/−1  |
| 5 | `close(fd)`             |                                         |
| 6 | `read(fd, buf, n)`      |                                         |
| 7 | `write(fd, buf, n)`     |                                         |
| 8 | `ticks()`               | milliseconds, seeds RANDOMIZE           |
| 9 | `unlink(path)`          | UNSAVE                                  |

## The language

Lines are numbered 1–65535 and kept tokenized; `LIST` reproduces them
from the tokens. Statements on a line are separated by `\` or `:`. A
`!` starts a comment, as does `REM`. Keywords and variable names are
folded to upper case outside quotes. A typed line without a number is
executed at once.

Variables are a letter and an optional digit: `A`, `B7` are floating,
`I%` integer, `S$` string. Arrays have one or two dimensions, `DIM
A(10,10)`, indexed from 0; an array used without `DIM` has 10 as each
bound. Numbers on the VM are the FP11's D format: a 56-bit fraction,
about 16 decimal digits, magnitudes up to about 1.7E38. They print with
up to 15 significant digits, a leading space for a positive value, and
in E notation when that is shorter, `1E+20`, `1E-06`. In `basic11`
they are the IEEE single of the FPU, a 24-bit fraction, about 7 decimal
digits, the same range of magnitudes; they print with up to 6
significant digits, as BASIC-11 did, `1.41421`, `.333333`, and in E
notation below 1E-5 and from 1E6, `1E+10`, `1.23457E+11`. Strings hold
up to 255 characters.

Statements: `LET` (optional), `PRINT` with `,` for the 14-column zones,
`;` to run items together and `TAB(n)`, `INPUT` with an optional prompt
string, `LINPUT` for a whole line, `IF ... THEN ... [ELSE ...]` where
either branch is a statement or a line number, `IF ... GOTO`, `FOR ...
TO ... [STEP ...]` / `NEXT`, `GOTO`, `GOSUB` / `RETURN`, `ON ... GOTO`
and `ON ... GOSUB`, `DIM`, `READ` / `DATA` / `RESTORE`, `DEF FNA(X,Y)=`
with up to four numeric parameters, `RANDOMIZE`, `STOP`, `END`, `REM`.

Files: `OPEN "name" FOR INPUT AS FILE #n`, `OPEN "name" FOR OUTPUT AS
FILE #n`, `PRINT #n`, `INPUT #n`, `LINPUT #n`, `IF END #n THEN line`,
`CLOSE #n` or `CLOSE`. Channels 1–6 are the program's, 7 is used by
`SAVE` and `OLD`, 0 is the terminal.

Operators: `^`, unary `-`, `* /`, `+ -` (`+` also joins strings), the
relations `= <> < <= > >=` (`==` is accepted for `=`), `NOT`, `AND`,
`OR`. A true relation is −1. `AND`, `OR` and `NOT` work on the integer
parts bitwise.

Functions: `ABS ATN COS EXP INT LOG LOG10 PI RND SGN SIN SQR TAN`,
`LEN ASC CHR$ POS(a$,b$,n) SEG$(a$,i,j) STR$ VAL TRM$ LEFT$(a$,n)
RIGHT$(a$,n) MID$(a$,i,n)` and `FN`. `RIGHT$(A$,N)` is BASIC-11's: the
characters from position N to the end, not the last N. `INT` is the
floor.

Commands: `RUN`, `RUNNH`, `LIST [line]`, `LISTNH`, `NEW`, `SCR`,
`OLD "file"`, `SAVE "file"`, `REPLACE "file"`, `UNSAVE "file"`,
`CLEAR`, `CONT`, `LENGTH`, `DEL line`, `BYE`. `SAVE` writes the listing
as text and `OLD` reads any text file of numbered lines, so programs
are plain files. Ctrl-C stops a running program with `STOP at line n`;
`CONT` goes on from there, as it does after a `STOP` statement.

Errors end the program and print `?Message at line n`, or `?Message`
when the statement was typed: `Syntax error`, `Undefined line number`,
`Illegal number`, `Division by zero`, `Overflow`, `Subscript out of
range`, `String too long`, `Out of memory`, `NEXT without FOR`, `RETURN
without GOSUB`, `Out of data`, `Illegal argument`, `Bad file`, `End of
file`, `Undefined function`, `Type mismatch`, `Too many nested loops`,
`Redimensioned array`, `Bad channel`, `Cannot continue`, `Line too
long`.

Not BASIC-11: no `ON ERROR`, no `CHAIN`, no virtual arrays, no
`PRINT USING`, no `DEF` with multiple lines, no integer-only
arithmetic (`I%` is stored truncated but computed in floating point),
no line editor beyond retyping a line and `DEL`. Compatibility with
programs written for BASIC-11 was not a goal.

## Inside

Program text lives at the bottom of the heap as records
`[len][line lo][line hi][tokens...][0]`. A keyword is one byte, 0x80
plus its index; a numeric constant is 0xFF followed by the bytes of the
number, eight on the VM and four natively, converted when the line is
entered, because parsing a number costs more than a hundred
multiplications and a loop body would pay it every time.
Line numbers after `GOTO`, `THEN` and the like stay text so `LIST` can
print them.

Variables are three fixed tables of 26×11 slots. Arrays are carved from
an arena above the program; `DIM` of an existing array is an error and
`CLEAR`, `RUN` and `NEW` reset the arena.

Strings are the flat pool above the arena. An entry is `[owner]
[capacity] data...`, where `owner` is the address of the descriptor
`{offset, length}` that holds it; a string variable or array element is
such a descriptor and nothing else points into the pool. When an
allocation does not fit, the pool is compacted in one pass: an entry is
alive exactly when its owner still points at it, so there is no free
list and no reference counting. Intermediate results of an expression
go to a 2 KiB scratch area that is emptied before every statement, so
compaction never has to know about them. `tests/basic/strings.bas`
runs the pool through many compactions; `basic-host -m 16 -r
tests/basic/strings.bas` makes them frequent.

The interpreter sees its numbers only through the `fp_*` functions of
`fp11.h` and `fpnat.h`, which agree on everything but the type behind
`fpac_t`. The FP11 one is simh's `pdp11_fp.c` reduced to what BASIC
needs: add, subtract, multiply, divide, compare, truncate, floor, the
conversions, and on top of them square root by Newton, exp and log by
their series after range reduction, sin and cos with the argument
reduced by a two-part π/2, atan by its series with argument halving,
and power as exp of log with integer powers done by squaring.

`fpnat.c` does the same on `float`, so on the M4F an add is one
instruction and the square root is `vsqrt`. It keeps BASIC's view of
arithmetic rather than IEEE's: a result that would be infinite or NaN
is an `Overflow` or `Illegal argument` error, a result that underflows
is 0, and there is no negative zero. The elementary functions are
short series after the usual reductions — exp by 2^k times a
polynomial on ±ln2/2, log by the atanh series of the fraction, sin and
cos by a four-piece π/2 that stays exact out to 2^31 quadrants, atan by
inversion and two halvings, power as 2^(y·log₂x) with the integer part
of the exponent split off in halves so the result is within a few
units in the last place. Reading and printing numbers goes through
64-bit integers, so `VAL` and a typed constant give the nearest float
and `PRINT` shows the 6 digits nearest the value; no libm or soft-float
is linked. `tests/host_fp11_test.c` and `tests/host_fpnat_test.c` run
290 000 and 330 000 comparisons of the two against libm.
