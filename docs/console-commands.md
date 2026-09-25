# Console commands

The shell on USART2 is the same program on both boards.  `help` lists
whatever the running image was compiled with; after the Black Pill gained
a program flash region that is the full set on both.  The language — values,
expressions, `if`, `loop`, variables and functions — is written out in
[shell.md](shell.md).

## Blue Pill commands

Every command Freya implements.  The Black Pill now has the same list.

| Command | What it does |
|---|---|
| `help(["command"])` | list commands, or describe one |
| `sysinfo()` | CPU, unique id, clocks, reset cause, uptime, log level, auto-start and ram-dump flags, card, filesystem |
| `meminfo()` | flash and RAM usage: .data, .bss, heap, program region, stack |
| `mount()` | initialise the card and mount it on `/`, or the Black Pill SPI flash (LittleFS) on `/spi1` |
| `power(["sd" [, "on"\|"off"]])` | show the socket supply, or switch it |
| `ls(["-l"] [, "path"])` | list a directory; `-l` adds sizes, dates and attributes |
| `cd(["path"])`, `pwd()` | move around |
| `mkdir("dir" [, ...])` | create directories |
| `rm(["-r",] "path" [, ...])` | remove files, empty directories, or whole trees |
| `rename("old", "new")` | rename or move a file or directory (no data copy) |
| `download("file" [, "--raw"\|"--size", bytes])` | receive a file over XMODEM; `--size` stores that many bytes and drops the padding |
| `upload("file")` | send a file over XMODEM; the first line gives the exact size |
| `cat("file")` | print a file |
| `write("file", value [, ...])` | append a line to a file |
| `hexdump("file" [, offset [, length]])` | dump a file in hex |
| `flashdump(["file"])` | write internal flash to a file on the card (default `/freya.flash`) |
| `df()` | capacity, free and used space |
| `load("file"\|"@flash")` | load a program image into RAM, or bind the flash image |
| `run(["file"\|"@flash" [, arg ...]])` | run the loaded program |
| `runflash([arg [, ...]])` | run the program stored in internal flash |
| `stop(["thread"])` | stop the program, or one thread by name |
| `threads()` | list threads: id, priority, state, name |
| `status()` | exit status of the last command and the last program |
| `install("file")` | write a program, or a shell script, into internal flash |
| `saveflash(["file"])` | copy the installed program or script from flash onto the card |
| `uninstall()` | erase the program flash region |
| `autostart(["on"\|"off"])` | run the flash program or script automatically at boot |
| `ramdump(["on"\|"off"])` | write SRAM to `/freya.ram` after a BusFault (default off) |
| `date(["YYYY-MM-DD HH:MM:SS"])` | show or set the clock used for file timestamps |
| `loglevel(["level"])` | show or set the file log level (`off`/`error`/`warn`/`info`/`debug`, or `0`..`4`) |
| `pin(["pin" [, "mode"\|level [, level]]])` | list pins, or read or drive one |
| `pwm(["pin" [, hz [, duty]]])` | list the PWM channels, or start and stop one |
| `adc(["pin"\|"temp"\|"vref"])` | take one raw 12-bit ADC sample |
| `i2c([bus [, hz\|"off"\|"scan"\|addr, ...]])` | list the I2C buses, or open, scan and talk to one |
| `spi([bus [, hz\|"off"\|"x", ...]])` | list the SPI buses, or open one and shift bytes |
| `w1(["pin"\|"off"\|"search"\|...])` | list open 1-Wire pins, or open one and talk to it |
| `crypt(["key", "nonce", "hex"])` | XTEA-CTR: the same call encrypts and decrypts |
| `sleep(ms)` | wait that many milliseconds; Ctrl-C returns early |
| `yield()` | let a script thread run |
| `source("file"\|"@flash")` | run a shell script from a file, or from program flash |
| `set [<name> [, <name>]... <expr>]` | list variables, or store one or more values |
| `unset <name>` | remove a variable |
| `fn [<name>]` | list functions, or define one up to `end` |
| `return <expr> [, <expr>]...` | leave the function with those values |
| `if <command>` ... `else` ... `end` | run the following commands when that command's status is 0 |
| `loop <count>` ... `end` | repeat the commands up to `end` |
| `uptime()`, `led(...)`, `echo(...)`, `clear()`, `reboot()` | the usual small change |

## Socket power

`power` is `api->power()`. `power sd off` closes every open file, unmounts,
releases PA4..PA7 and drives PA8 high, which opens the VDD switch described
in [sd-slot.txt](sd-slot.txt). `power sd on` drives PA8 low and waits for
the rail; the card is not identified again until `mount`. `mount` itself
turns the rail on when it was off. A program does the same with
`api->power(FREYA_PWR_SD, 0)` and `api->power(FREYA_PWR_SD, 1)`. The call
returns the state it found. A kernel from before this call is detected
with `FREYA_API_HAS(api, power)`.

## Pins and PWM at the prompt

`pin` and `pwm` are the pin and PWM service calls with a prompt in front of
them — the same `src/gpio.c` and `src/pwm.c` a program reaches through
`api->pin_mode()` and `api->pwm_open()`, so a pin Freya keeps is refused here
for the same reason, and what works at the prompt works in a program.

A pin is named the way `FREYA_PB(0)` is written: `PB0`, `pb0` and `B0` are the
same pin. `pin PB0` reads it; a mode word (`in`, `up`, `down`, `out`, `od`,
`analog`) sets it; `0`, `1` or `toggle` drives it, making the pin a push-pull
output first if no mode was given. Either way the command finishes by reading
the pin back, so what it prints is what the pin really is:

```

`adc PA0` takes one analog conversion and leaves PA0 in analog mode.
`adc temp` and `adc vref` read the internal sources. Values are raw counts
from 0 to 4095; [adc.md](adc.md) describes the pin set, conflicts, and
conversion to voltage.
freya: pin PB5 out
PB5 = 0
freya: pin PB5 1
PB5 = 1
freya: pin PB0 up
PB0 = 1
```

`pwm` with no arguments lists the board's eight channels, which timer and
channel each pin is, and what each is doing:

```
freya: pwm PB6 1000 25
PB6  TIM4 CH1  1000 Hz 25.00%
freya: pwm
  PA0  TIM2 CH1  off
  PA1  TIM2 CH2  off
  PB0  TIM3 CH3  off
  PB1  TIM3 CH4  off
  PB6  TIM4 CH1  1000 Hz 25.00%
  PB7  TIM4 CH2  off
  PB8  TIM4 CH3  off
  PB9  TIM4 CH4  off
the channels of one timer share its frequency
usage: pwm [<pin> <hz> <duty%> | <pin> off]
```

A duty cycle is a percentage and may have a decimal point: `7.5` is what a
servo sits at. A channel started here keeps running — that is the point of it
— until `pwm PB6 off`, which also puts the pin back to an input. A channel a
*program* opened is closed when its run ends instead.

Frequencies are 1 Hz to 1 MHz, the channels of one timer share one frequency,
and a timer driving pins is not one a program can open with `timer_open()`.
[docs/interrupts.md](interrupts.md) has the rest.

## I2C at the prompt

`i2c` is the I2C service calls with a prompt in front of them — the same
`src/i2c.c` a program reaches through `api->i2c_open()` and
`api->i2c_transfer()`. With no arguments it lists the buses and the pins:

```
freya: i2c
  1  I2C1  SCL PB6  SDA PB7  off
  2  I2C2  SCL PB10  SDA PB11  off
pull SCL and SDA up to 3.3 V
usage: i2c [<bus> <hz> | <bus> off | <bus> scan | <bus> <addr> [w <byte>...] [r <n>]]
```

Bus 2's pins are the board's: PB10/PB11 on the Blue Pill, PB10/PB9 on the
Black Pill. The rest of the command is opening a speed, scanning, a write, a
read, or a write then a read, and closing again. [docs/i2c.md](i2c.md) has
the worked transcript and the reasons a call is refused.

## SPI at the prompt

`spi` is the SPI service calls with a prompt in front of them — the same
`src/spi.c` a program reaches through `api->spi_open()` and
`api->spi_transfer()`. With no arguments it lists the buses and the pins:

```
freya: spi
  1  SPI2  PB13 PB14 PB15  off
chip select is a pin you drive
usage: spi [<bus> <hz> [mode] | <bus> off | <bus> x <byte>...]
```

The bus is SPI2 on both boards. The card keeps SPI1. The rest of the
command is opening a speed and a mode, shifting bytes, and closing
again. Chip select is `pin`, around the shift.
[docs/spi.md](spi.md) has the worked transcript and the reasons a call
is refused.

## 1-Wire at the prompt

`w1` is the 1-Wire service calls with a prompt in front of them — the same
`src/w1.c` a program reaches through `api->w1_open()` and `api->w1_search()`.
With no arguments it lists the pins that are open:

```
freya: w1 PB12
PB12  1-Wire
freya: w1
  PB12
pull the data pin up to 3.3 V
usage: w1 [<pin> | <pin> off | <pin> reset | <pin> search]
```

The pin is named the way `pin` names one. `search` prints every ROM and
`off` puts the pin back to an input. A pin a *program* opened is closed
when its run ends instead. Reading a thermometer is `samples/w1`.
[docs/w1.md](w1.md) has the worked transcript.

## crypt at the prompt

`crypt` is the cipher with a prompt in front of it — the same `src/crypt.c`
a program reaches through `api->crypt()`. With no arguments it names the
cipher and prints the usage. Otherwise the key is 32 hex digits, the nonce
is 16 and the data is one hex word, with no `0x`. The same command decrypts:

```
freya: crypt 000102030405060708090a0b0c0d0e0f 4142434445464748 0000000000000000
497df3d072612cb5
```

A file is `samples/crypt`. [docs/crypt.md](crypt.md) is the call.

Ctrl-C stops a running program and every thread it created. It also
stops a `sleep`, a `loop` or a `wait`, and every script thread, and throws
away a script that is still being typed. `stop` with no name does the same
when a program is running, and unloads it otherwise;
`stop <name>` stops that thread and leaves the run going. A script thread
is stopped the same way, by the name of its function. `threads` lists
both. Ctrl-U clears the input line, and the up and down cursor keys walk
the command history. While a program runs, only `threads`, `stop` and
`help` are read from the console; anything else waits until the run ends.

`$?` anywhere in a line becomes the exit status of the previous command: 0 when
it worked, 1 when it failed, 127 for a word that is not a command, and for
`run` the status of the program — its own code, 130 after Ctrl-C, or 131..134
after a fault. `$name` becomes that variable the same way. Both are expanded
before the line is split, so `echo $?` and `write /runs.txt $?` both work,
and `status` prints the same numbers with the reason and the run time beside
them.

## Scripts

`;` separates commands on one line. A new line separates them the same
way, and quotes hide a semicolon, so `echo "a;b"` is one command.

`if` runs the command written after it. When that command's status is
0, the commands up to `else` or `end` run. Otherwise they are skipped,
and the commands between `else` and `end` run if an `else` was written.
`loop` repeats the commands up to `end` the number of times given.
`break` leaves the innermost loop and continues after its `end`. A
`break` outside a loop is refused before anything runs.
`sleep` waits that many milliseconds. A count is a decimal number, at
most 1000000. `$?` may be the count: it is read when the loop starts.
While it waits, a script thread that is ready runs. `yield` does that
and does not wait. The calls are in [shell.md](shell.md).

```
freya: if echo hi
> echo yes
> else
> echo no
> end
hi
yes
freya: loop 3; echo tick; sleep 200; end
tick
tick
tick
```

The prompt changes to `>` while a block is still open, and Ctrl-C on
that prompt throws the lines away. Those lines together have to stay
within 159 characters. Nothing runs until the block is closed, so a
missing `end` or an `else` in the wrong place does not half-run the
commands. A branch that is skipped does not change `$?`. `$?` is
expanded when each command runs, so a loop sees a new value every pass.
Blocks nest, eight deep.

## Variables

`set` with no name lists the variables. `set <name> <expr>` stores the
value of the expression under that name, and the value's type is whatever
the expression produced: an integer, a byte, a bool, empty, none, a float, a string of at most 31
characters, an auto array, or a dict. There are eight of them. A name is a letter or `_` and then
letters, digits or `_`, at most seven characters. `unset <name>` removes one. `$name` in a later command is
the value as text, so `echo $n` and `loop $n` both work. A name that is not
set is an error.

An integer is a decimal or `0x` hex literal. Hex keeps all 32 bits, so
the high bit is the sign and `0xFFFFFFFF` is `-1`. A byte is that literal
with a trailing `b` and is 0 to 255 (`65b`). A hex byte is `byte(0x41)`.
`true` and `false` are the bool values. `empty` is the empty
value, and `none` is a different value with no number. A float has a decimal point.
A string is written in quotes. `+ - * /` work on numbers; a byte is promoted
to an integer, and if either side is
a float the result is a float, and integer `/` truncates. An integer or a
byte also
has `%`, `~`, `&`, `|`, `^`, `<<` and `>>` (the shifts are logical, and the
count is 0..31). `+` concatenates when either side is a string, rendering
the other side as text. A string written next to further values is a format:
`%d`, `%u`, `%x`, `%s`, `%f` and `%%`, each value filling one conversion.

`array(10, 20)` stores an auto array. Every element is the same type.
`$a[i]` reads one, and `set a[i] <expr>` writes one. An index past the
end grows the array, up to 8 elements, and fills the gap with zero.
`dict("b", 2, "a", 1)` stores a dict. Keys are one type, values are
one type, and the keys stay sorted. `$d["a"]` reads a pair and
`set d[k] <expr>` inserts or replaces one. There are at most 8 pairs.
`len` is the count. `min` and `max` are the least and greatest
element, and `sort` returns the array in that order. `set b $a` copies. Four arrays and dicts may exist
at once. `unset`, or storing something else over the name, frees the
cells. The full rules are in [shell.md](shell.md).

```
freya: set n 1 + 2 * 3
freya: set x 7.5 / 2
freya: set s "%d %s" $n "items"
freya: echo $s
7 items
```

`==` and `/=` compare two values and leave 1 or 0. Numbers match by value,
so `1 == 1.0` and `1 == 1b` are true; a string matches only the same text,
a bool matches only a bool, `empty` matches only `empty`, and `none`
matches only `none`. `<`, `>` and
`><` compare numbers only: less, greater, and not equal. A string, a bool, `empty`, or `none`
on either side is an error. When the command after `if` contains one of these, that
is the condition: true runs the commands up to `else` or `end`, false runs
the `else`. `if true`, `if false`, and `if bool(...)` are conditions too.
Anything else after `if` is still a command, and the branch is
chosen from its status.

```
freya: if $n == 7
> echo yes
> else
> echo no
> end
yes
```

## Functions

`fn` with no name lists the functions. `fn <name>` ... `end` defines
one. The body is not run at the definition. A name is the same shape as
a variable, and there are four of them. Defining the same name again
replaces the body. The body is at most 127 characters. A longer one is
`function too long`. The body is kept on the heap, so it can also be
refused with `out of memory`.

A call is an expression, `name(arg, ...)`, with 0 to 32 arguments. Each
argument is an expression. `return` leaves from anywhere in the body
with 1 to 32 values, separated by commas, each keeping its type. A call
used as one value yields the first of those, or 0 when the body has no
`return`. `set a, b name(...)` stores the first values, one per name.
Inside the body `$0` is how many arguments were passed and `$1` .. `$32`
are those values. An argument that was not passed is an error. `$?` and
`$name` still mean what they mean outside.

```
freya: fn add
> return $1 + $2
> end
freya: set n add(2, 3)
freya: echo $n
5
freya: fn
add
```

`return` outside a function is refused before anything runs, the way
`break` is refused outside a loop. A call may call another function.
Deep or very wide calls are refused rather than grown without limit.

`int`, `float`, `byte`, `bool`, `str` and `hex` convert, and cannot be defined with `fn`.
Each of those takes one value. `empty()`, `none()`, `true()` and `false()` take none. `int` makes an integer: a byte is widened, a bool is 0 or 1, a float is truncated toward
zero, and a string is a decimal integer, a `0x` hex integer, or a decimal
float that is then truncated. `byte` makes a value from 0 to 255; a float
is truncated first, and a value outside that range is `integer overflow`.
`float` makes a float from an integer, a byte, or
from decimal text (a `0x` string is an integer first). `str` renders any
value as text, the same way `$name` does; `true` is the text `true`,
`empty` is `empty`, and `none` is `none`. `bool` makes a bool: zero is
`false`, any other number is `true`, and the strings are `true` and `false`.
`hex` of a number is lowercase
hex text with no `0x` (`hex(-1)` is `"ffffffff"`); `hex` of a string
parses hex digits, with an optional sign and an optional `0x`, back to
an integer. A value that is not of that form is `not a number`, and one
that does not fit is `integer overflow`. A byte literal is `65b`.
The empty value is written `empty`. The none value is written `none`.
A bool is written `true` or `false`. `if true` and `if false` are
conditions.

```
freya: set n int("0x10")
freya: set x float($n)
freya: set s hex($n)
freya: echo $s
10
freya: set n hex($s)
freya: echo $n
16
```

`match`, `find` and `gsub` search a string with a Lua pattern. `%d`
is a digit and `%a` is a letter; an uppercase class is the complement.
`*` `+` `-` and `?` repeat, `^` and `$` anchor, and `()` captures.
`match` returns the match, or the captures, or `none`. `find` returns
the start and the end, counting from 1. `gsub` returns the new string
and how many replacements it made. The full rules are in
[shell.md](shell.md).

`rand()` returns the next value from the ANSI C 1989 example
generator: `state = state * 1103515245 + 12345` in 32 bits, then
`(state / 65536) % 32768`, so the result is an integer from 0 to 32767.
The state starts at 1. `srand(seed)` replaces it with that integer,
which may be negative (`srand(-1)` stores the bit pattern), and returns
0. The same seed repeats the same sequence.

```
freya: set n srand(1)
freya: set n rand()
freya: echo $n
16838
freya: set n rand()
freya: echo $n
5758
```

`pi()` is the constant 3.14159265 and takes no argument. `sin(angle)`
and `cos(angle)` take one integer, byte, or float, in radians, and return a
float. An angle past about a million, or one that is not a number, is
`not a number`.

```
freya: set x sin(pi() / 2)
freya: echo $x
1
freya: set x cos(pi())
freya: echo $x
-1
```

The names that cannot be defined with `fn` are listed in
[shell.md](shell.md). A pin is a
string in the same form as the `pin` command (`"PB0"`, `"pb0"`, `"B0"`).
`get` reads it and returns 0 or 1. `set` makes it a push-pull output,
writes 0 or 1, and returns the level read back. `adc` takes one raw
sample and returns that count, 0 to 4095; `"temp"` and `"vref"` are the
internal sources. `pwm(pin, hz, duty)` starts a channel and returns the
rate; the duty is a percent and may be a float (`7.5`). `pwm(pin)`
stops it and returns 0.

```
freya: set n set("PB5", 1)
freya: echo get("PB5")
1
freya: set n adc("PA0")
freya: set n pwm("PB6", 1000, 25)
```

A comment is one line and starts with `#`. Spaces or tabs may come
before it, and a `#` after a space starts one too. The comment runs to
the end of the line, so a semicolon there does not start another
command. Quotes hide the hash, so `echo "a # b"` prints the hash. A
line that is only a comment is skipped.

`source <file>` reads a script from the card and runs it. The file is
plain text, at most 1024 bytes, and each command is still one line of
at most 159 characters. `source @flash` runs the script stored in the
program flash region, which needs no card. A short flash script is
copied into RAM first, so the script may `install` or `uninstall`
without erasing the text it is still reading; a longer one is read
from the flash as it runs. `source` inside a script is allowed, three
deep including the line that started it. Ctrl-C stops the script the
same way it stops a `loop`.

```
freya: source /blink.sh
PB5 = 1
freya: install /blink.sh
install: console input is dropped while flash is busy
  erasing 1 page ... writing ... ok
installed script /blink.sh at 0x0800c080: 24 B in 1 page
freya: source @flash
PB5 = 1
```

`install` of a text file stores that script in the program flash region,
in place of a program image. `saveflash` copies it back (default
`/script.sh`). `uninstall` erases it. `runflash` on a script says to
use `source @flash`. With `autostart on`, the next boot runs the script
when `/autorun.bin` is absent. `make flash SCRIPT=boot.sh AUTOSTART=1`
packs that script with the flag already on.

## Commands that were Blue Pill only

These lived behind `FREYA_APP_FLASH_ADDR`, which only the Blue Pill defined.
They are implemented on both boards now (Black Pill: 128-byte slot at the
end of sector 3, 64 KiB program in sector 4).  Acceptance: type any of
them at `freya:` on either module.

| Command | Why it was missing on the Black Pill |
|---|---|
| `runflash [args...]` | no program flash region |
| `install <file>` | no F4 flash programmer |
| `saveflash [file]` | nothing to copy out of flash |
| `uninstall` | nothing to erase |
| `autostart [on\|off]` | no auto-start slot |
| `ramdump [on\|off]` | flag lived in that same slot; dump was compiled out |

Related behaviour that followed the same `#ifdef`, and is now common too:
`load @flash` / `run @flash`, persistent `loglevel` in the slot, `sysinfo`
and `meminfo` lines for auto-start / ram-dump / program flash, and a
BusFault dump of SRAM to `/freya.ram` when the flag is on.
