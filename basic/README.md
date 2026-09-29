# BASIC for the Freya virtual machine

A BASIC in the manner of DEC's BASIC-11, written in C and compiled with
cproc and QBE for the PDP-11 that `src/vm.c` runs: eight 32-bit
registers, 32-bit words, the PDP-11 opcodes. The same source compiles
natively for testing. Nothing here needs a C library; the interpreter
talks to whatever runs the machine through ten TRAP instructions.

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

| file         | what it is                                                    |
|--------------|---------------------------------------------------------------|
| `basic.c`    | the interpreter, one translation unit that includes `fp11.c`  |
| `fp11.c/.h`  | the PDP-11 FP11 in software, from simh's `pdp11_fp.c`         |
| `bas.h`      | the fixed-width types and the system call prototypes          |
| `rt.s`       | `_start` and the ten system calls, assembled in front of the C |
| `runbasic.c` | runs the image on a PC with `src/vm.c`; serves the traps      |
| `hostrt.c`   | the same ten calls with stdio, for the native build           |
| `Makefile`   | builds the image, the native interpreter and `runbasic`       |

`samples/basic/main.c` is the same host as `runbasic.c` written against
the Freya API, for a board whose heap can hold the machine.

## Building

The image needs the cproc and QBE builds described in
[`../qbe/README.md`](../qbe/README.md). cproc-qbe has no include path
option, so the C preprocessor runs first; `as.py` then turns the QBE
listing plus `rt.s` into `basic.bin`, which the VM runs from address 0.

```sh
make -C basic CPROC=/path/to/cproc-qbe QBE=/path/to/qbe   # build/basic/basic.bin
make -C basic host                                        # basic-host and runbasic
make -C basic test                                        # tests/basic on both
```

```
runbasic [-m KiB] [-r program.bas] [-s] basic.bin
basic-host [-m KiB] [-r program.bas]
```

`-m` is the size of the machine's memory, 256 KiB by default; on the
VM the image, the heap and a 16 KiB stack share it. `-r` loads a
program, runs it without the banner and the prompts and exits with 0
at END, or 1 after an error. `-s` prints the number of instructions
the VM executed.

The image is about 100 KiB: an instruction of this machine is four
bytes and most operands are another four, and about 13 KiB of it is
the variable tables. That is more than the 56 KiB program region of
the boards Freya runs on today, so `samples/basic` reports that it has
too little memory there; it runs unchanged on a board with a larger
heap, and on the PC.

## The system calls

`main(heap_lo, heap_hi, flags)` is called with a normal C frame whose
return address is a HALT; `flags & 1` is batch mode. A system call is
`TRAP n` with the arguments in R0–R2 and the result in R0. The host
finds `n` in the low byte of the word before the PC the VM stopped at.

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
bound. Numbers are the FP11's D format: a 56-bit fraction, about 16
decimal digits, magnitudes up to about 1.7E38. They print with up to
15 significant digits, a leading space for a positive value, and in E
notation when that is shorter, `1E+20`, `1E-06`. Strings hold up to 255
characters.

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
plus its index; a numeric constant is 0xFF followed by its eight bytes,
converted when the line is entered, because parsing a number costs more
than a hundred multiplications and a loop body would pay it every time.
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

The floating point is simh's `pdp11_fp.c` reduced to what BASIC needs:
add, subtract, multiply, divide, compare, truncate, floor, the
conversions, and on top of them square root by Newton, exp and log by
their series after range reduction, sin and cos with the argument
reduced by a two-part π/2, atan by its series with argument halving,
and power as exp of log with integer powers done by squaring.
`tests/host_fp11_test.c` runs 290 000 comparisons against libm.
