# BASIC for Freya

A BASIC in the manner of DEC's BASIC-11, written in C. Compiled with
GCC for the Cortex-M4F boards it is the native program `basic11`,
whose numbers are the C `float` on the FPU; the same source compiles
for the PC, with the same arithmetic, for testing. Nothing here needs
a C library; the interpreter talks to whatever runs it through ten
system calls.

```
$ build/basic/basic-host
BASIC-11 for Freya
0 bytes of program, 104032 free

READY

10 FOR I=1 TO 3
20 PRINT I, SQR(I)
30 NEXT I
RUNNH
 1             1
 2             1.41421
 3             1.73205

READY

BYE
```

## Files

| file         | what it is                                                     |
|--------------|----------------------------------------------------------------|
| `basic.c`    | the interpreter, one translation unit that includes its arithmetic |
| `fpnat.c/.h` | BASIC's numbers as the C `float`                               |
| `bas.h`      | the types, `setjmp` and the system call prototypes per target  |
| `hostrt.c`   | the ten calls with stdio, for the PC build                     |
| `Makefile`   | builds the PC interpreter                                      |

`samples/basic11/main.c` is the Freya program that hosts it. It
includes `basic.c` and is compiled by the top-level Makefile for the
boards with an FPU: it implements the ten calls on the Freya API,
supplies `setjmp` and `longjmp` for the Cortex-M4F, and takes the
workspace from the heap.

`BAS_HOST` picks the PC build, with a C library; without it the source
is the bare-metal program. `BAS_BANNER` is the first line printed.

## Building

```sh
make BOARD=blackpill samples   # build/blackpill/samples/basic11.bin
make -C basic                  # build/basic/basic-host
make -C basic test             # tests/basic on the host build
```

`basic-host` computes what `basic11` computes, so the expected outputs
under `tests/basic` are what the board prints.

```
run basic11 [-m KiB] [program.bas]
basic-host [-m KiB] [-r program.bas]
```

On the board `-m` asks for a workspace of that many KiB from the heap;
without it the program takes 32 KiB, or the largest multiple of 4 KiB
down to 12 that the heap can give. A program named on the command line
is loaded and run as though `OLD` and `RUN` had been typed; the
interpreter then goes on to `READY`. `BYE` returns to the shell. On the
PC `-m` is the size of the machine's memory, 256 KiB by default. `-r`
loads a program, runs it without the banner and the prompts and exits
with 0 at END, or 1 after an error.

The native program is 18 KiB of Thumb-2 code plus 12 KiB of static
data, the variable tables mostly, and runs from the shell's stack.

## The system calls

The interpreter is entered through `bas_main(heap, size, flags)`;
`flags & 1` is batch mode. It calls back through twelve functions,
`sys_exit` to `sys_sleep` in `bas.h`, that the host defines. In
`basic11` they are ordinary functions on the Freya API: `readline`
reads the console with echo and editing, `break` polls it for Ctrl-C,
`open` and the rest are the file calls, `ticks` the millisecond clock,
`clock` and `sleep` the calendar and the wait.

| call                    | notes                                   |
|-------------------------|-----------------------------------------|
| `exit(code)`            |                                         |
| `putc(c)`               | `\n` ends a line                        |
| `readline(buf, max)`    | length without the newline, −1 at EOF   |
| `break()`               | 1 once since the last call ^C was seen  |
| `open(path, mode)`      | mode 0 read, 1 write and create; fd/−1  |
| `close(fd)`             |                                         |
| `read(fd, buf, n)`      |                                         |
| `write(fd, buf, n)`     |                                         |
| `ticks()`               | milliseconds since start, seeds RANDOMIZE |
| `unlink(path)`          | UNSAVE                                  |
| `clock(f)`              | six fields of civil time, −1 with no clock |
| `sleep(ms)`             | called in pieces, so ^C is still seen   |

`clock` is `rtc_get()` of the Freya API, which reads the count the
kernel keeps from SysTick — the `date` command sets it, and on a board
built with `RTC=ds3231` the battery-backed chip is read into it at
boot. A kernel too old for that call, and the Blue Pill, which had no
room for it, report that there is no clock; `DATE$` and `TIME$` then
give `?No clock`.

## The language

Lines are numbered 1–65535 and kept tokenized; `LIST` reproduces them
from the tokens. Statements on a line are separated by `\` or `:`. A
`!` starts a comment, as does `REM`. Keywords and variable names are
folded to upper case outside quotes. A typed line without a number is
executed at once.

Variables are a letter and an optional digit: `A`, `B7` are floating,
`I%` integer, `S$` string. Arrays have one or two dimensions, `DIM
A(10,10)`, indexed from 0; an array used without `DIM` has 10 as each
bound. Numbers are the IEEE single of the FPU: a 24-bit fraction,
about 7 decimal digits, magnitudes up to about 3.4E38. They print with
up to 6 significant digits, as BASIC-11 did, a leading space for a
positive value, `1.41421`, `.333333`, and in E notation below 1E-5 and
from 1E6, `1E+10`, `1.23457E+11`. Strings hold up to 255 characters.

Statements: `LET` (optional), `PRINT` with `,` for the 14-column zones,
`;` to run items together and `TAB(n)`, `INPUT` with an optional prompt
string, `LINPUT` for a whole line, `IF ... THEN ... [ELSE ...]` where
either branch is a statement or a line number, `IF ... GOTO`, `FOR ...
TO ... [STEP ...]` / `NEXT`, `GOTO`, `GOSUB` / `RETURN`, `ON ... GOTO`
and `ON ... GOSUB`, `DIM`, `READ` / `DATA` / `RESTORE`, `DEF` and
`FNEND`, `RANDOMIZE`, `SLEEP`, `STOP`, `END`, `REM`.

A function is `FN` and a name, `FNA`, `FNMAX`, `FNPAD$`, and takes up
to four parameters. `DEF FNMAX(A,B) = ...` is the one-statement form.
Where no `=` follows the parameters the definition is the statements up
to its `FNEND`, and the value is what the body assigns to the
function's own name:

```
10 DEF FNMAX(A,B)
20   IF A > B THEN FNMAX = A ELSE FNMAX = B
30 FNEND
40 PRINT FNMAX(3,7)
```

A body may do anything a program may, including calling functions and
itself, six calls deep. Reaching an `FNEND` returns, so `IF ... THEN
FNEND` is how a body leaves early; the block itself ends at the first
`FNEND` that is not in the `THEN` or the `ELSE` part of an `IF`. A
function whose name ends in `$`
returns a string and one that does not returns a number, 0 or `""`
when the body assigns nothing. Only the parameters are local: they are
ordinary variables, saved when the call starts and put back when it
ends. Definitions may sit anywhere among the lines that call them,
since reaching a `DEF` skips over it.

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
RIGHT$(a$,n) MID$(a$,i,n)`, `DATE$ TIME$ TIME` and `FN`.
`RIGHT$(A$,N)` is BASIC-11's: the characters from position N to the
end, not the last N. `INT` is the floor.

The clock: `DATE$` is `YYYY-MM-DD` and `TIME$` is `HH:MM:SS`, both ten
and eight characters and both read when they are evaluated, so a
program that wants one instant should take a copy. Neither takes an
argument and neither sets the clock; the shell's `date` command does
that. `TIME` is a number, the milliseconds since the board came up.
It is exact while it is below 2^24, which is the first four hours and
three quarters; after that the count is still right but a BASIC number
cannot keep every millisecond of it and it steps in 2, then 4, and so
on. `SLEEP n` waits n milliseconds and is a statement, not a function;
Ctrl-C stops the program during the wait, as it does between
statements, and a negative n is `?Illegal argument`.

```
10 PRINT "started "; DATE$; " "; TIME$
20 T = TIME
30 FOR I = 1 TO 3
40   PRINT TIME$
50   SLEEP 1000
60 NEXT I
70 PRINT "took"; TIME - T; "ms"
```

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
long`, `DEF without FNEND`, `No clock`.

Not BASIC-11: `DATE$`, `TIME$` and `TIME` take no argument, where
BASIC-11 wanted one and counted `TIME` in seconds since midnight;
`SLEEP` is new. No `ON ERROR`, no `CHAIN`, no virtual arrays, no
`PRINT USING`, no `FNEXIT` (`IF ... THEN FNEND` does as much), no
integer-only arithmetic (`I%` is stored truncated but computed in
floating point), no line editor beyond retyping a line and `DEL`.
Compatibility with programs written for BASIC-11 was not a goal.

## Inside

Program text lives at the bottom of the heap as records
`[len][line lo][line hi][tokens...][0]`. A keyword is one byte, 0x80
plus its index; a numeric constant is 0xFF followed by the four bytes
of the number, converted when the line is
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

The `DEF`s of a program are found in one pass over the text and kept in
a table of 26 that is thrown away whenever a line is entered or
deleted; a definition is a few pointers into the tokens, the name among
them, so a name is compared where it stands and needs no storage. The
pass is also what settles where a `DEF ... FNEND` block ends, since a
conditional `FNEND` is an early exit and only the first unconditional
one closes it. An `FNEND` past that belongs to no definition, and the
program saying so is better than a definition growing over the lines
between.

Calling such a definition runs a statement loop of its own, because the
call is in the middle of an expression and cannot be returned from: the
loop ends at an `FNEND`, and anything else that would end it — an
error, Ctrl-C, `END`, the last line of the program — leaves through the
`longjmp` that unwinds the expression as well. The body gets a scratch
area above what the caller holds there, since its statements empty the
one they are given. Two things in an expression can reach the pool and
compact it, binding a string parameter and the statements of a body, so
a string value that has to outlive either is copied to the scratch area
first; a program with no definition that can do it copies nothing.
`tests/basic/deffn.bas` at `-m 20` compacts the pool inside a body
while the caller holds a string in it.

The interpreter sees its numbers only through the `fp_*` functions of
`fpnat.h`, never the type behind `fpac_t`: add, subtract, multiply,
divide, compare, truncate, floor, the conversions, and on top of them
square root and the elementary functions.

`fpnat.c` implements them on `float`, so on the M4F an add is one
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
is linked. `tests/host_fpnat_test.c` runs 330 000 comparisons of it
against libm.
