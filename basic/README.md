# BASIC for Freya

A BASIC in the manner of DEC's BASIC-11, written in C. One source,
built two ways for the boards:

- **`basic11`**, native ARM32: compiled with GCC for the Cortex-M, its
  numbers the C `float` on the FPU of the boards that have one, and on
  the Blue Pill, whose Cortex-M3 has none, in the soft-float helpers of
  `src/softfp.c`.
- **`basic11.vm`**, for the Freya virtual machine: compiled with cproc
  and QBE for the 32-bit PDP-11 of the kernel's `vm_run()`, its numbers
  the same IEEE floats computed with integers, and run on a board by
  `basic11vm` (see [The virtual machine](#the-virtual-machine)).

The two print the same thing for the same program, digit for digit;
[One BASIC, two builds](#one-basic-two-builds) says how that is kept.
The same source also compiles for the PC, with the same arithmetic, for
testing. Nothing here needs a C library; the interpreter talks to
whatever runs it through a few system calls.

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
| `fpnat.c/.h` | BASIC's numbers as IEEE single floats, on primitives that are the C `float` or `fpsoft.c` |
| `fpsoft.c`   | IEEE single with 32-bit integers only, the VM build's primitives |
| `bas.h`      | the types, `setjmp` and the system call prototypes per target  |
| `hostrt.c`   | the calls with stdio, for the PC build and `runbasic`          |
| `pretend.c`  | a pretend board's pins, for the builds that run on the PC      |
| `armrt.h`    | `setjmp`, `longjmp` and `bas_main_on()` for the Cortex-M       |
| `vmrt.s`     | `_start`, a `TRAP` per system call, `setjmp` and `longjmp` for the VM |
| `vmsys.h`    | the `TRAP` numbers of the system calls                         |
| `runbasic.c` | runs `basic11.vm` on the PC, on `src/vm.c`                     |
| `armrt.c`    | runs the board's code on the PC under `qemu-arm`, for the tests |
| `Makefile`   | builds the PC interpreter, the VM image and the two test runners |

`samples/basic11/main.c` is the Freya program that hosts it. It
includes `basic.c` and is compiled by the top-level Makefile for every
board: it takes the workspace and the interpreter's stack from the heap,
or on the Blue Pill from its own RAM window (see [The Blue
Pill](#the-blue-pill)), and the system calls are
`samples/basic11/sys.c`, on the Freya API. `samples/basic11vm/main.c`
loads `basic11.vm` and serves its system calls with the same `sys.c`.

`BAS_HOST` picks the PC build, with a C library, and `BAS_VM` the image
for the virtual machine; with neither the source is the bare-metal
program. `BAS_SOFTFLOAT` makes the numbers integers (`fpsoft.c`), which
`BAS_VM` needs. `BAS_BANNER` is the first line printed. `BAS_SMALL` is
the build for a few KiB of RAM, and `BAS_STACK_FLOOR` the lowest
address the stack may reach.

## Building

```sh
make BOARD=blackpill samples   # build/blackpill/samples/basic11.bin
make BOARD=bluepill samples    # build/bluepill/samples/basic11.xip.bin
make BOARD=stm32h723 samples   # ... and build/stm32h723/samples/basic11vm.bin
make -C basic                  # build/basic/basic-host
make -C basic vm               # build/basic/basic11.vm and runbasic
make -C basic arm              # build/basic/basic-arm, for qemu-arm
make -C basic test             # tests/basic on every build there is
```

`make -C basic vm` needs cproc and QBE with the Freya target, which
[../qbe/README.md](../qbe/README.md) says how to build; `CPROC=` and
`QBE=` point at them when they are not on the `PATH`.

`basic-host` computes what `basic11` computes, so the expected outputs
under `tests/basic` are what the board prints — except for
`pins.bas`, whose pins are the pretend ones of `pretend.c`. The tests
run every program on `basic-host`, then on `basic11.vm` under
`runbasic` when it has been built, then on the board's own code under
`qemu-arm` when that is installed, and each has to print the same
bytes.

```
run basic11 [-m KiB] [program.bas | -e text]
run basic11vm [-m KiB] [-i image] [program.bas | -e text]
basic-host [-m KiB] [-r program.bas]
runbasic [-m KiB] [-r program.bas] [-s] basic11.vm
```

On the board `-m` asks for a workspace of that many KiB from the heap;
without it the program takes 32 KiB, or the largest multiple of 4 KiB
down to 12 that the heap can give with the interpreter's stack. A program named on the command line
is loaded and run as though `OLD` and `RUN` had been typed, without the
banner and the prompts, and `basic11` exits when the program ends, with
1 after an error and 0 otherwise. `-e text` does the same with the
program's lines given as the text, one per line, carriage returns
ignored; it is how a kernel built with `BASIC=` starts its program.
Without either the interpreter takes commands until `BYE`, which
returns to the shell. On the
PC `-m` is the size of the machine's memory, 256 KiB by default. `-r`
loads a program, runs it without the banner and the prompts and exits
with 0 at END, or 1 after an error. `runbasic -s` reports how many
instructions the machine ran and how deep its stack went.

The native program is 21 KiB of Thumb-2 code plus 12 KiB of static
data, the variable tables mostly. The interpreter runs on a stack of its
own, 12 KiB taken from the heap with the workspace, not on the shell's:
that is 6 KiB, with the program window right under it, and a program
run from flash has its code copied to the top of that window, where an
expression nested deep enough would write over it. The nesting limit
below keeps the stack under 8 KiB, which `make -C basic arm` and
`basic-arm -s` measured; `BAS_STACK_FLOOR` stops the interpreter with
`?Out of memory` 1.5 KiB short of the bottom should anything ever go
deeper.

## The Blue Pill

The Blue Pill's Cortex-M3 has no FPU, and a program there has a 9 KiB
RAM window and no heap to speak of. `basic11` is built for it all the
same, with three differences.

- **Numbers.** It is compiled soft-float, so every `float` operation
  is a call into `src/softfp.c`, which the Makefile links into each
  Blue Pill program. The square root is worked out from the bits
  (`sqrt_bits()` in `fpnat.c`), correctly rounded; it agrees with the
  FPU on every non-negative float. The results are the same as on the
  FPU boards, only slower.
- **Memory** (`BAS_SMALL`). The variables are not three tables of all
  286 names of each kind but records in the arena, made when a name is
  first used and cleared with the rest of the arena. The other tables
  are halved: 8 `FOR` and 8 `DO` loops, 16 `GOSUB`s, 4 keys, 16 `DEF`s
  of which 6 may be calling one another, 32-byte channel buffers, 1 KiB
  of scratch for strings. The program starts no threads and owns the
  whole window (`NOTHREADS` and `WHOLE_WINDOW` in the Makefile). Its own
  data is under 3 KiB, and the workspace for the program, its arrays
  and its strings is the rest, about 5 KiB, less the top kilobyte. `-m`
  is refused.
- **The stack.** The program stack is the shell's 2560 bytes, directly
  above the window, and it may grow down into that top kilobyte.
  `BAS_STACK_FLOOR` stops the interpreter 384 bytes short of the bottom
  with `?Out of memory`, instead of letting it run over the workspace.
  That allows about seven built-in functions nested inside one another,
  a dozen levels of parentheses, or a `DEF` that calls itself four
  deep, short of the 16 levels the other boards allow.

It is 23 KiB of code, so it is built only as `basic11.xip.bin`, and
runs from the program flash where `install` or `make flash PROGRAM=`
puts it.

### A program built in

`make BASIC=prog.bas` builds the text of `prog.bas` into the kernel
extension. At boot the autorun program, `basic11` unless `PROGRAM=`
says otherwise, is started as `basic11 -e TEXT`, so it runs that
program. `BASIC=` also sets `PROGRAM=basic11`, when it is not set, and
`AUTOSTART=1`. With the shell left out the board does nothing else, and
starts the program again a second after it ends:

```sh
make BOARD=bluepill NOSHELL=1 flash BASIC=prog.bas
```

The text has to be plain ASCII, and is at most what is free in the
kernel extension: about 6 KiB on a Blue Pill kernel with the shell,
about 47 KiB on one without. A change to the file rebuilds the kernel.
`docs/building.md` has the rest of `NOSHELL=1`.

### A program saved in flash

`FSAVE` keeps the program in the program flash region, after the image
of `basic11` itself, and `AUTOSTART` turns the auto-start flag on, so
the next boot starts `basic11 -e TEXT` with that program:

```
10 X = PIN("PC13","OUT")
20 X = PIN("PC13","TOGGLE") \ SLEEP 500 \ GOTO 20
AUTOSTART
FSAVE
```

A running program cannot write flash: the kernel's flash routines run
from the program's RAM window. So `AUTOSTART [ON | OFF]` only asks,
and the kernel sets the flag when `basic11` ends; `FSAVE` asks the
same of the listing and ends `basic11` at once, like `BYE`, which
also clears the variables. The kernel then writes the text and prints
what it did, and the shell comes back. `FSAVE` with no program
removes the saved text. `uninstall` in the shell removes it with the
rest of the region, and a text is only handed to the program it was
saved for.

`basic11` has to have been started from flash (`runflash`, or the boot);
run from the card it answers `?Not run from flash`. The text starts at
the first erase unit after the image, so there is room for anything on
the other boards. On the Blue Pill `basic11` fills all but the last
1.3 KiB of its region and the text goes there, straight after the
image: about 1.3 KiB of listing, `?No room in flash` past that.
`BASIC=` still takes larger programs. A saved text is used before the
one `BASIC=` built in.

## The virtual machine

`basic11.vm` is `basic.c` compiled by cproc and QBE for the 32-bit
PDP-11 of [docs/vm.md](../docs/vm.md) and assembled by `qbe/as.py`,
with `vmrt.s` first so that address 0 jumps to `bas_main()`. It is
128 KiB: 115 KiB of code, since every instruction of that machine and
most operands are four bytes, and the static data of the native
program. On a board, `basic11vm` runs it:

```
copy build/basic/basic11.vm to the card's root, then
> run basic11vm
BASIC-11 for Freya
```

`basic11vm` is 3 KiB. The machine's memory is the program window from
the end of `basic11vm`'s own variables up to the window's end, or up to
its code when the loader copied that to the top: the image, then the
workspace bas_main() is given, then 20 KiB of stack for the machine,
then a `HALT` that `bas_main()` would return to. The workspace is
`-m`, or 32 KiB when there is room and the most there is in steps of
4 KiB down to 12 otherwise. So only the boards with a window of 168
KiB or more build it: the STM32U585, the STM32H562 and the STM32H723,
which give it 32 KiB, and the STM32H523, which has room for 16. The
F4 boards' 56 KiB windows and the Blue Pill's 7 KiB cannot hold the
image, and the Blue Pill has no machine at all.

Each system call is a `TRAP`, its number from `vmsys.h` in the low
byte, with the arguments in R0–R2; `basic11vm` checks that every
address the image hands it is inside the machine's memory and calls the
function of `samples/basic11/sys.c` that `basic11` calls, so the
console, the files, the clock, the pins and Ctrl-C behave the same. It
runs the machine 20 000 instructions at a time and stops it, with a
message, if its stack ever reached the workspace; the deepest nesting
takes 12 KiB of it (`runbasic -s`). It is slower than `basic11`, by as
much as an interpreter running on an interpreter is. `FSAVE` and
`AUTOSTART` answer `?Not run from flash`, as `basic11` run from the card
does.

On the PC, `runbasic` runs the image the same way, on `src/vm.c`, and
serves the calls with the functions of `hostrt.c` that `basic-host`
uses, so the tests compare the two byte for byte.

## One BASIC, two builds

The language, the limits and the messages are `basic.c`'s, the same
file in both. What could still make them differ, and what keeps them
apart:

- **The numbers.** `fpnat.c` is written on a handful of primitives,
  `f_add()`, `f_mul()`, `f_div()`, the comparisons, the conversions,
  the square root: the C operators on the FPU, `fpsoft.c` on integers.
  Each rounds once, to nearest, as IEEE says, so the exponential, the
  logarithm, the sines, the power and the decimal conversions, which
  are made of them, come out the same bit for bit. The decimal
  conversions need 64-bit integers, which the VM's compiler has not
  got, so both builds use two 32-bit halves. The board build is
  compiled `-ffp-contract=off`: GCC would otherwise fuse a multiply and
  an add into one `vfma` with one rounding, and the series would print
  a different last digit for about one argument in three hundred.
- **The stack.** An expression may nest 16 deep — each parenthesis,
  argument of a function, sign and power is a level, and the body of
  a `DEF` goes on from the level of its call — and the 17th is `?Out of
  memory`, the same in both, since a limit that came from the size of
  the stack would come at a different depth in each.
- **The system calls.** On a board both builds call the same
  `samples/basic11/sys.c`, and on the PC the same `hostrt.c`.

What does differ is the speed, and so what a program that counts
against the clock counts. The free memory `LENGTH` and the banner
report is the same for the same workspace; it differs only where the
workspace does, as on the STM32H523, which has room for 16 KiB beside
the image.

`tests/host_fpsoft_test.c` builds `fpnat.c` both ways and runs two
million operations through each, which have to give the same bits; it
also checks `fpsoft.c` against the PC's FPU directly, the square root
over every 61st float. `tests/basic` runs every program on the PC
build, on the image and on the board's code under `qemu-arm`, against
one expected output; `math.bas` prints 2 000 values of the functions,
enough that a build with fused multiply-adds fails it.

## The system calls

The interpreter is entered through `bas_main(heap, size, flags)`;
`flags & 1` is batch mode. It calls back through the functions
`sys_exit` to `sys_adc` in `bas.h`, that the host defines. In
`basic11` they are ordinary functions on the Freya API, and
`basic11vm` calls the same ones for its image's `TRAP`s: `readline`
reads the console with echo and editing, `break` polls it for Ctrl-C,
`open` and the rest are the file calls, `ticks` the millisecond clock,
`clock` and `sleep` the calendar and the wait, `inkey` takes what
`kbhit` says is there, and the pin calls are `pin_mode()` and the rest
of the pin, PWM and ADC calls.

| call                    | notes                                   |
|-------------------------|-----------------------------------------|
| `exit(code)`            |                                         |
| `putc(c)`               | `\n` ends a line                        |
| `readline(buf, max, running)` | length without the newline, −1 at EOF; `running` a program's INPUT, not a command |
| `break()`               | 1 once since the last call ^C was seen  |
| `open(path, mode)`      | mode 0 read, 1 write and create; fd/−1  |
| `close(fd)`             |                                         |
| `read(fd, buf, n)`      |                                         |
| `write(fd, buf, n)`     |                                         |
| `ticks()`               | milliseconds since start, seeds RANDOMIZE |
| `unlink(path)`          | UNSAVE                                  |
| `clock(f)`              | six fields of civil time, −1 with no clock |
| `sleep(ms)`             | called in pieces, so ^C is still seen   |
| `inkey()`               | a key typed, or −1 at once; INKEY$       |
| `pin_mode(pin, mode)`   | the pin is port×16+number, `FREYA_PIN()`'s packing |
| `pin_read(pin)`         | 0 or 1                                  |
| `pin_write(pin, level)` |                                         |
| `pin_toggle(pin)`       |                                         |
| `pin_pull(pin, pull)`   | `SYS_PULL_NONE`, `_UP`, `_DOWN`; the mode stays |
| `pin_pull_get(pin)`     | the pull now, `SYS_PULL_*`              |
| `pwm(pin, hz, duty)`    | duty in ten-thousandths; hz 0 stops the channel |
| `adc(source)`           | a pin, or `SYS_ADC_TEMP`, `SYS_ADC_VREF`; 0..4095 |
| `flash_save(text, n)`   | FSAVE; `SYS_EARG` when it does not fit  |
| `autostart(on)`         | AUTOSTART                               |

The pin calls answer with the value, or with `SYS_EPIN`, `SYS_EBUSY`,
`SYS_EARG` or `SYS_EIO`, which are `Bad pin`, `Pin in use`, `Illegal
argument` and `Device error` to the program. The first three are the
`FREYA_ERR_*` codes of the same names; everything else the API can
answer, and a kernel too old to have the call at all, is `SYS_EIO`.
The PC builds have no pins, so `pretend.c` pretends: ports A, B and C of
16 pins, each an input reading 0 until it is driven, PA2 and PA3 kept
back as the console's are on the boards, PWM on ports A and B, a pull
that follows the F4's (each mode sets the one it names, open drain a
pull-up), and an ADC that answers 2048 from a pin, 1000 from `TEMP` and 1500 from
`VREF`. That is enough for `tests/basic/pins.bas` to check what the
interpreter does with the names, the modes and the values.

`clock` is `rtc_get()` of the Freya API, which reads the count the
kernel keeps from SysTick — the `date` command sets it, and on a board
built with `RTC=ds3231` the battery-backed chip is read into it at
boot. A kernel too old for that call, and the Blue Pill, which had no
room for it, report that there is no clock; `DATE$` and `TIME$` then
give `?No clock`.

## The language

Lines are numbered 1–65535 and kept tokenized; `LIST` reproduces them
from the tokens. Statements on a line are separated by `\` or `:`. A
`!` starts a comment, as does `REM`, and a `!` comment may follow a
statement on its line. Keywords and variable names are
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
TO ... [STEP ...]` / `NEXT`, `DO [WHILE ...|UNTIL ...]` / `LOOP [WHILE
...|UNTIL ...]`, `GOTO`, `GOSUB` / `RETURN`, `ON ... GOTO` and `ON ...
GOSUB`, `ON TIMER(ms) GOSUB` / `TIMER ON|OFF`, `ON KEY(p$) GOSUB` /
`KEY(p$) ON|OFF`, `DIM`, `READ` / `DATA` / `RESTORE`, `DEF` and
`FNEND`, `RANDOMIZE`, `SLEEP`, `STOP`, `END`, `REM`.

`DO` and `LOOP` enclose a loop that runs until a condition says
otherwise: `DO WHILE c` and `DO UNTIL c` test before each pass, `LOOP
WHILE c` and `LOOP UNTIL c` after it, and a `DO` / `LOOP` with no
condition on either end runs until a `GOTO` leaves it. Sixteen may be
nested, with `FOR` loops among them. A jump out of a `DO` loop and back
to its `DO` starts it over.

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
RIGHT$(a$,n) MID$(a$,i,n)`, `DATE$ TIME$ TIME INKEY$`, `PIN PULL PWM ADC`
and `FN`.
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

The pins: `PIN`, `PULL`, `PWM` and `ADC` are the shell's `pin`, `pull`,
`pwm` and `adc` as functions, on the same kernel calls, so a pin Freya keeps for itself
is refused here for the same reason. A pin is named as the shell names
one, `"PB0"`, `"pb0"` or `"B0"`, and the name may be computed. `PIN(P$)`
reads the pin, 0 or 1. `PIN(P$, L)` makes it a push-pull output, drives
it to L — 0 for 0, anything else for 1, so a relation will do — and
`PIN(P$, "TOGGLE")` flips it. `PIN(P$, M$)` sets its mode, `"IN"`,
`"UP"`, `"DOWN"`, `"OUT"`, `"OD"` or `"ANALOG"`, in either case, and
`PIN(P$, M$, L)` sets the mode and then drives it. Whatever it did, the
value is what the pin reads afterwards, which is what it really is.
`PULL(P$)` is the pin's pull resistor, 0 for none, 1 up, 2 down, and
`PULL(P$, M$)` sets it first to `"NONE"`, `"UP"` or `"DOWN"`, in either
case, leaving the mode as it is. That works on an input or an
open-drain pin: `PIN(P$, "OD")` turns the pin's pull-up on with it, and
`PULL(P$, "NONE")` turns it off again, leaving the line to its external
resistor. A push-pull, PWM or analog pin is `?Illegal argument`. The
Blue Pill and the Black Pill 2 pull an input only, and a pull-up on an
open-drain pin there is `?Device error`.
`ADC(S$)` is one raw 12-bit conversion, 0 to 4095, from a pin, or from
`"TEMP"` or `"VREF"`, the chip's own sources; the pin is left in analog
mode. `PWM(P$, HZ, D)` starts the channel on the pin at HZ hertz with a
duty cycle of D percent, which may be fractional, `7.5`, and is HZ;
calling it again changes the channel. `PWM(P$)` stops it, puts the pin
back to an input and is 0. Nothing else stops a channel: it runs on
after `END` until `PWM(P$)` or `BYE`. The channels of one timer share
a frequency, and which pins have a channel and where the ADC reaches
are the board's; [docs/console-commands.md](../docs/console-commands.md)
and [docs/adc.md](../docs/adc.md) say. There is no `DAC`: neither board's
chip has one that Freya drives.

```
10 X = PIN("PB5", "OUT")
20 FOR I = 1 TO 10
30   X = PIN("PB5", "TOGGLE")
40   SLEEP 200
50 NEXT I
60 PRINT "PA0 reads"; ADC("PA0"); "of 4095"
70 X = PWM("PB6", 1000, 25)
80 IF PIN("PB0", "UP") = 0 THEN X = PWM("PB6")
90 X = PIN("PB7", "OD") + PULL("PB7", "NONE")
```

A pin that is not one, or is one the kernel keeps, is `?Bad pin`; a
pin or a timer that is taken is `?Pin in use`; a frequency or duty
cycle out of range, a mode, pull or action word that is not one, a
pull on a pin that cannot have one, and stopping a channel that is not running are `?Illegal argument`; a
conversion that did not finish, or a kernel too old to have these
calls, is `?Device error`.

Events: a program can have a subroutine called every so many
milliseconds, and one called when a button is pressed, while it goes
on with its own work. `ON TIMER(ms) GOSUB line` names the subroutine
and the period, and `TIMER ON` starts the ticks; `TIMER OFF` stops
them, and `TIMER ON` again starts a fresh period. `ON KEY(p$) GOSUB
line` names the subroutine for a key, which is a pin with a button on
it, `"PA0"` for the Black Pill's; `KEY(p$) ON` makes the pin an input
with a pull-up and watches it for a press, which is the pin going low,
and `KEY(p$) OFF` stops watching. A button that pulls the pin high
instead is `ON KEY(p$, 1) GOSUB line`, and gets a pull-down. Eight
keys may be defined. `INKEY$` is the key typed at the console since the
last one was taken, as a string of one character, or `""` when none
was; it never waits, and it is how the main loop is told to end.

```
10 ON TIMER(500) GOSUB 1000
20 TIMER ON
30 ON KEY("PA0") GOSUB 2000
40 KEY("PA0") ON
50 DO
60   PRINT "."; \ SLEEP 100          ! the main work
70 LOOP UNTIL INKEY$ = "q"
80 END
1000 PRINT "tick"
1010 RETURN
2000 PRINT "PA0 pressed"
2010 RETURN
```

The sources are looked at between statements, and during a `SLEEP`,
which goes on to its end when the subroutine returns; an `INPUT` that
is waiting for a line does not see them. A subroutine is entered as by
a `GOSUB` and ends with `RETURN`. While it runs its own source is held,
and a tick or a press that arrived meanwhile is delivered once when it
has returned; another source's subroutine may still be called inside
it. Between two calls of a subroutine the program always gets one
statement of its own in, so a subroutine slower than its period slows
the program down rather than stopping it. A press is the pin reading
the pressed level for 20 ms without a break, so the bounce of a switch
is one press and a key held down is one press; there is no repeat.
`RUN` and `NEW` forget the definitions and switch everything off. `ON
TIMER` needs a period above 0 and a line that exists; `TIMER ON` with
no `ON TIMER`, or `KEY(p$) ON` for a key with no `ON KEY`, is refused.
On the PC, `INKEY$` sees the keys once the line they are on is entered,
as the terminal is line-buffered.

Commands: `RUN`, `RUNNH`, `LIST [line]`, `LISTNH`, `NEW`, `SCR`,
`OLD "file"`, `SAVE "file"`, `REPLACE "file"`, `UNSAVE "file"`,
`CLEAR`, `CONT`, `LENGTH`, `DEL line`, `BYE`, `FSAVE`, `AUTOSTART [ON |
OFF]`. `SAVE` writes the listing
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
long`, `DEF without FNEND`, `No clock`, `Bad pin`, `Pin in use`,
`Device error`, `LOOP without DO`, `DO without LOOP`, `Too many keys`,
`Undefined key`, `Not run from flash`, `No room in flash`.

Not BASIC-11: `DATE$`, `TIME$` and `TIME` take no argument, where
BASIC-11 wanted one and counted `TIME` in seconds since midnight;
`SLEEP`, `PIN`, `PULL`, `PWM`, `ADC`, `DO` / `LOOP`, `INKEY$`, `FSAVE`,
`AUTOSTART` and the `TIMER` and `KEY` events are new. No `ON ERROR`, no `CHAIN`, no virtual arrays, no
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

The events are polled, not interrupts: after every statement of a
running program, and after every 20 ms slice of a `SLEEP`, the
statement loop reads the clock and the key pins. The timer is a
deadline that moves on by its period; a key is a two-level debouncer,
the newest sample and the level it has held for 20 ms, and the press
is the moment the held level becomes the pressed one. A source that
has fired is pending until its subroutine can be called, which is done
by pushing a `GOSUB` frame that returns to the boundary the loop was
at — a separator, the end of the line, the target of a jump — and
marking the source busy until that frame's `RETURN` pops it. A `SLEEP`
that is interrupted is returned to as a statement, with the frame
carrying the deadline it had reached and a flag that makes it go on
from there rather than start over; the interrupting subroutine may
`SLEEP` itself without disturbing that. After a `RETURN` from a
subroutine nothing is dispatched until the program has executed a
statement, so a subroutine slower than its period cannot starve the
program. `tests/basic/events.bas` runs a timer, a key pressed by
driving its pin, the bounce, a subroutine slower than its period, and
the `DO` loops; its counts depend on the machine, so it prints ranges.

The interpreter sees its numbers only through the `fp_*` functions of
`fpnat.h`, never the type behind `fpac_t`: add, subtract, multiply,
divide, compare, truncate, floor, the conversions, and on top of them
square root and the elementary functions.

`fpnat.c` implements them on `float`, so on the M4F an add is one
instruction and the square root is `vsqrt`; with `BAS_SOFTFLOAT` on
the integers of `fpsoft.c`, the same operations to the same bits. It keeps BASIC's view of
arithmetic rather than IEEE's: a result that would be infinite or NaN
is an `Overflow` or `Illegal argument` error, a result that underflows
is 0, and there is no negative zero. The elementary functions are
short series after the usual reductions — exp by 2^k times a
polynomial on ±ln2/2, log by the atanh series of the fraction, sin and
cos by a four-piece π/2 that stays exact out to 2^31 quadrants, atan by
inversion and two halvings, power as 2^(y·log₂x) with the integer part
of the exponent split off in halves so the result is within a few
units in the last place. Reading and printing numbers goes through
64-bit integers, kept as two 32-bit halves, so `VAL` and a typed
constant give the nearest float and `PRINT` shows the 6 digits nearest
the value; no libm or soft-float is linked. `tests/host_fpnat_test.c`
runs 330 000 comparisons of it against libm.
