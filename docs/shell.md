# Shell language

The console shell is a small language as well as a list of commands. A
line can store a value, test it, repeat a command, call a function, and
read or drive a pin. The same rules apply to a line typed at `freya:/>`
and to a script run with `source`. The commands themselves are listed in
[console-commands.md](console-commands.md).

A value is an integer, a float, or a string of at most 31 characters.
An expression produces one value. A command produces a status, which
`$?` reads back.

## Lines

`;` separates commands on one line. A new line separates them the same
way. Quotes hide a semicolon, so `echo "a;b"` is one command. A command
is at most 159 characters.

A `#` at the start of a statement, or after a space, comments out the
rest of that statement. Quotes hide it, so `echo "a # b"` prints the
hash. A line that is only a comment is skipped.

`$?` anywhere in a command becomes the status of the previous command,
as decimal text. `$name` becomes that variable, also as text. `$0` is
how many arguments the current function call received, and `$1` .. `$32`
are those arguments. All of these are expanded when the command runs, so
each pass of a loop sees the values the previous command left. A name
that is not set, or an argument that was not passed, is an error. The
expansion happens before the line is split, so `echo $n` and
`write /runs.txt $?` both work.

`set` and a comparison used as an `if` condition do not go through that
expansion. They parse `$name`, `$?` and `$1` as values inside the
expression, so the type is kept: `$n + 1` adds, it does not paste digits
onto text.

| Status | Means |
|---|---|
| 0 | the command worked |
| 1 | it failed |
| 127 | the word is not a command |
| 130 | a program was stopped with Ctrl-C |
| 131 .. 134 | a program was killed by a fault |

`run` leaves the program's own status. `status` prints the same number
with the reason and the run time beside it. An empty line leaves `$?`
alone.

## Blocks

`if`, `else`, `end` and `loop` group commands. A block that is still
open at the end of a typed line is finished on later lines, and the
prompt changes to `>`. Those lines together have to stay within 160
characters. Nothing runs until the block is closed, so a missing `end`
or an `else` in the wrong place does not half-run the commands. Ctrl-C
on the `>` prompt throws the lines away.

Blocks nest, eight deep. `if`, `loop` and `fn` each count.

`if` has two forms. When the text after `if` contains `==`, `/=`, `<`,
`>` or `><` outside quotes, that text is a condition. True runs the
commands up to `else` or `end`. False runs the `else`, when one was
written. The condition's value is an integer: 0 is false and anything
else is true. A condition that is not an integer, such as a bare float,
is an error.

Anything else after `if` is a command. Status 0 takes the first branch.
Any other status takes the `else`. A branch that is skipped does not
change `$?`. A condition does: true leaves 0 and false leaves 1.

```
freya:/> if echo hi
> echo yes
> else
> echo no
> end
hi
yes
freya:/> if $n == 7
> echo yes
> else
> echo no
> end
yes
```

`loop <count>` repeats the commands up to `end` that many times. The
count is a decimal integer from 0 to 1000000, or `$?`, or `$name` after
expansion. It is read once, when the loop starts. `loop 0` runs nothing.
`break` leaves the innermost loop and continues after its `end`. A
`break` outside a loop is refused before anything runs, and `break`
takes no argument. `break` inside a function does not leave a loop
outside that function.

```
freya:/> loop 3; echo tick; sleep 200; end
tick
tick
tick
```

`sleep <ms>` waits that many milliseconds. The count has the same shape
and the same limit as a loop count. Ctrl-C cuts a `sleep` or a `loop`
short, and stops a `source` the same way.

## Values

An integer is a decimal literal, from -2147483648 to 2147483647, or a
`0x` hex literal. Hex keeps all 32 bits, so the high bit is the sign and
`0xFFFFFFFF` is `-1`. A float has a decimal point (`7.5`, `.5`, `1.`).
There is no exponent. A string is written in double quotes. There are no
backslash escapes; a quote ends the string. A string in an expression is
at most 31 characters.

`pi()` is the float constant 3.14159265. It takes no argument.

`$name` inside an expression is the stored value, with its type. `$?` is
an integer. `$0` .. `$32` are the arguments of the call in progress.
Parentheses group. A call is `name(arg, ...)`.

Unary `+` and `-` apply to an integer or a float. Unary `~` applies to
an integer. The operators, from tightest to loosest:

| Operators | What they do |
|---|---|
| `* / %` | multiply, divide, remainder |
| `+ -` | add, subtract; `+` also joins strings |
| `<< >>` | logical shift, count 0..31 |
| `&` | bitwise and |
| `^` | bitwise exclusive or |
| `\|` | bitwise or |
| `== /= < > ><` | compare |

`+ - * /` work on numbers. If either side is a float the result is a
float, and integer `/` truncates toward zero. `%`, `~`, `&`, `|`, `^`,
`<<` and `>>` require integers. The shifts fill with zero bits, so
`-1 >> 1` is 2147483647, not -1. Division by zero and an integer result
that does not fit are errors.

`+` concatenates when either side is a string. The other side is
rendered as text and the result is a string. A string written next to
further values, with no operator, is a format. Each value fills the
next conversion, and a later conversion is left for the value after
that.

| Conversion | Accepts | Result |
|---|---|---|
| `%d`, `%i` | integer, or a float truncated toward zero | decimal |
| `%u` | integer | unsigned decimal |
| `%x` | integer | lowercase hex |
| `%X` | integer | uppercase hex |
| `%f` | integer or float | the float's text |
| `%s` | any value | its text |
| `%%` | nothing | one `%` |

```
freya:/> set n 1 + 2 * 3
freya:/> set x 7.5 / 2
freya:/> set s "%d %s" $n "items"
freya:/> echo $s
7 items
```

`==` and `/=` compare two values and leave the integer 1 or 0. Numbers
match by value, so `1 == 1.0` is true. A string matches only the same
text, and a string does not match a number. `<`, `>` and `><` compare
numbers only: less, greater, and not equal. A string on either side is
an error. Several comparisons associate left to right, and each result
is an integer, so `1 < 2 < 3` is `(1 < 2) < 3`.

## Variables

`set` with no name lists the variables. `set <name> <expr>` stores the
value of the expression. The type is whatever the expression produced.
There are eight variables. A name is a letter or `_`, then letters,
digits or `_`, at most seven characters. `unset <name>` removes one. A
name that is not set is an error. Defining a variable does not change
`$?` when it succeeds.

`set` prints nothing on success. The list prints `name = 7` for an
integer, `name = 1.5` for a float, and `name = "text"` for a string.
With no variables it prints `no variables`.

```
freya:/> set n 1 + 2 * 3
freya:/> echo $n
7
freya:/> set
n = 7
```

## Functions

`fn` with no name lists the functions. `fn <name>` ... `end` defines
one. The body is not run at the definition. A name has the same shape as
a variable, and there are four functions. Defining the same name again
replaces the body. The body is at most 127 characters and is kept on
the heap, so a long one can be refused with `out of memory`. The list
prints one name per line, or `no functions`.

A call is an expression, `name(arg, ...)`, with 0 to 32 arguments. Each
argument is an expression. The call's value is what `return <expr>`
produced, or the integer 0 when the body has no `return`. Inside the
body, `$0` is how many arguments were passed and `$1` .. `$32` are those
values, each with the type it was given. An argument that was not passed
is an error. `$?` and `$name` still mean what they mean outside.

```
freya:/> fn add
> return $1 + $2
> end
freya:/> set n add(2, 3)
freya:/> echo $n
5
freya:/> fn
add
```

`return` outside a function is refused before anything runs, the way
`break` is refused outside a loop. `return` takes one expression. A call
may call another function. Calls nest four deep. A call that is too
deep or too wide is refused rather than grown without limit. `break`
does not cross a function out into a loop that called it.

These names are built in and cannot be defined with `fn`: `if`, `else`,
`end`, `loop`, `break`, `fn`, `return`, `get`, `set`, `adc`, `pwm`,
`int`, `float`, `str`, `hex`, `rand`, `srand`, `sin`, `cos`, `pi`,
`now`, `date`, `time`, `year`, `month`, `day`, `hour`, `minute`,
`second`, `ticks`, `timer`, `tstart`, `tstop`, `tcount`, `tclose`,
`tperiod`, `irq`, `wait`.

## Conversion

`int`, `float`, `str` and `hex` each take one value.

`int` makes an integer. A float is truncated toward zero. A string is a
decimal integer, a `0x` hex integer, or a decimal float that is then
truncated. A leading `+` or `-` is allowed. Hex keeps all 32 bits, so
`int("0xFFFFFFFF")` is `-1`, and the decimal `-2147483648` is accepted.

`float` makes a float from an integer or from decimal text. A `0x`
string is read as an integer first and then widened. A leading sign is
allowed.

`str` renders any value as text, the same way `$name` does. A string is
left as it is.

`hex` of a number is lowercase hex text with no `0x`. A float is
truncated first. `hex(-1)` is `"ffffffff"`. `hex` of a string parses hex
digits, with an optional sign and an optional `0x`, back to an integer.
`hex(hex(255))` is 255, and `hex(hex("ff"))` is `"ff"`.

A value that is not of that form is `not a number`. One that does not
fit in an integer is `integer overflow`.

```
freya:/> set n int("0x10")
freya:/> set x float($n)
freya:/> set s hex($n)
freya:/> echo $s
10
freya:/> set n hex($s)
freya:/> echo $n
16
```

## Random numbers

`rand()` returns the next value from the ANSI C 1989 example generator.
The state is 32 bits. Each step is

```
state = state * 1103515245 + 12345
result = (state / 65536) % 32768
```

so the result is an integer from 0 to 32767. The state starts at 1.
`rand` takes no argument.

`srand(seed)` replaces the state with that integer and returns 0. The
seed is one integer. All 32 bits are kept, so a negative seed is stored
as its bit pattern and `srand(-1)` is the seed `0xFFFFFFFF`. A float or
a string is refused. The same seed repeats the same sequence.

```
freya:/> set n srand(1)
freya:/> set n rand()
freya:/> echo $n
16838
freya:/> set n rand()
freya:/> echo $n
5758
```

## Sine and cosine

`sin(angle)` and `cos(angle)` take one integer or float, in radians, and
return a float. `sin(pi() / 2)` is 1 and `cos(pi())` is -1. An angle past
about a million radians, or a value that is not a number, is
`not a number`.

```
freya:/> set x sin(pi() / 2)
freya:/> echo $x
1
freya:/> set x cos(pi())
freya:/> echo $x
-1
```

## Date and time

The clock is the software clock the `date` command shows and sets. It
has no battery. Until `date` is used, it reads 2026-01-01 00:00:00.
These functions only read it, or convert a count of seconds. Setting
the clock is still the `date` command.

`now()` is that clock as seconds since 1970-01-01 00:00:00. The value
is an integer, so the last instant it can name is 2038-01-19 03:14:07.
A later clock is `integer overflow`. `now` takes no argument.

`date()` is the same clock as text, `YYYY-MM-DD HH:MM:SS`.
`date(seconds)` formats one integer the same way. A negative count is
`integer overflow`.

`year`, `month`, `day`, `hour`, `minute` and `second` each return that
one field. With no argument they read the clock. With one integer they
read that count of seconds. The month is 1 to 12 and the day starts at 1.

`time(year, month, day, hour, minute, second)` builds the count of
seconds from six integers. A field that is not a real date, including a
year before 1970 or a day past the end of the month, is
`bad expression`. A later instant that does not fit in an integer is
`integer overflow`. A float or a string in any of these is refused.

```
freya:/> set n time(2026, 1, 1, 0, 0, 0)
freya:/> set s date($n)
freya:/> echo $s
2026-01-01 00:00:00
freya:/> set n year($n)
freya:/> echo $n
2026
freya:/> set n now()
freya:/> echo $n
1767225600
```

## Pins

`get`, `set`, `adc` and `pwm` are expressions. A pin is a string in the
same form as the `pin` command (`"PB0"`, `"pb0"`, `"B0"`).

`get(pin)` reads it and returns 0 or 1. `set(pin, level)` makes it a
push-pull output, writes 0 or 1, and returns the level read back.
`adc(pin)` takes one raw sample and returns that count, 0 to 4095.
`"temp"` and `"vref"` are the internal sources. `pwm(pin, hz, duty)`
starts a channel and returns the rate. The duty is a percent and may be
a float (`7.5`). `pwm(pin)` stops the channel and returns 0. Stopping a
channel that is not running is an error.

```
freya:/> set n set("PB5", 1)
freya:/> echo get("PB5")
1
freya:/> set n adc("PA0")
freya:/> set n pwm("PB6", 1000, 25)
```

The console commands `pin`, `adc` and `pwm` do the same work from a
command line. They are described in
[console-commands.md](console-commands.md).

## Timers and interrupts

`ticks()` is milliseconds since boot, the same clock `uptime` prints.
The value is an integer, so past about 24 days it is `integer overflow`.
`ticks` takes no argument.

`timer`, `tstart`, `tstop`, `tcount`, `tclose` and `tperiod` are the
hardware timers. A period is microseconds, from 10 to 40000000, and it
is an integer. There are three timers. A handle is the integer
`timer` returned, 0, 1 or 2.

`timer(us)` opens one, starts it, and returns the handle. It fires
until `tstop` or `tclose`. `timer(us, 1)` fires once and then stops
itself. `timer(us, 0)` is the same as `timer(us)`.
`timer(us, flags, "name")` does that and names a script function to
run on each expiry. The function must already be defined.

`tstart(handle)` and `tstop(handle)` start and stop it, and return 0.
`tcount(handle)` is how many times it has expired since it was opened.
`tperiod(handle, us)` changes the period and returns 0. The new period
takes effect at the next expiry. `tclose(handle)` stops it, frees it,
and returns 0. A handle that is not open is `out of range`. When every
timer is already open, `timer` says `every timer is taken`.

`irq` is a pin edge. The pin is a string, as in `get`. The edge is an
integer: 1 rising, 2 falling, 3 both. Add 4 to ignore further edges
for 20 ms after the first, which is what a switch wants.
`irq("PB0", 2)` arms the pin and returns 0. `irq("PB0", 2, "name")`
also names a script function, which must already be defined.
`irq("PB0")` is how many edges have been taken since it was armed.
`irq("PB0", 0)` disarms it and returns 0. A pin Freya keeps is
`not a pin`. A second port on the same pin number is
`that interrupt line is taken`.

The named function does not run inside the interrupt. `wait(ms)` sleeps
until a timer or a pin armed with a function has fired, calls that
function, and returns 0. Inside the function, `$1` is how many times
that source has fired. Several sources that fired are each called
once, in the order they were armed. `wait(0)` waits until one fires
or Ctrl-C. Any other count, up to 1000000, gives up after that many
milliseconds and returns -1. Ctrl-C cuts a `wait` short the way it
cuts a `sleep`.

A timer or a pin armed from the shell is not a program's. It keeps
running when a program starts and when that program ends. `tclose` and
`irq(pin, 0)` are what release it.

```
freya:/> fn ontick
> echo tick
> return $1
> end
freya:/> set t timer(1000000, 0, "ontick")
freya:/> set n wait(0)
tick
freya:/> echo $n
0
freya:/> set n tclose($t)
```

The same calls from a program are described in
[interrupts.md](interrupts.md).

## Scripts on the card and in flash

`source <file>` reads a script from the card and runs it. The file is
plain text, at most 1024 bytes, and each command is still one line of
at most 159 characters. A file that is not text is refused. `source`
inside a script is allowed, three deep including the line that started
it.

`source @flash` runs the script stored in the program flash region, and
needs no card. A short flash script is copied into RAM first, so the
script may `install` or `uninstall` without erasing the text it is still
reading. A longer one is read from the flash as it runs.

```
freya:/> source /blink.sh
PB5 = 1
freya:/> install /blink.sh
install: console input is dropped while flash is busy
  erasing 1 page ... writing ... ok
installed script /blink.sh at 0x0800c080: 24 B in 1 page
freya:/> source @flash
PB5 = 1
```

`install` of a text file stores that script in the program flash region,
in place of a program image. `saveflash` copies it back, default
`/script.sh`. `uninstall` erases it. `runflash` on a script says to use
`source @flash`. With `autostart on`, the next boot runs the script when
`/autorun.bin` is absent.

## Limits

| Limit | Value |
|---|---|
| Command length | 159 characters |
| Open block being typed | 160 characters |
| Nested `if` / `loop` / `fn` | 8 |
| Variables | 8, names of at most 7 characters |
| String value | 31 characters |
| Functions | 4, each body at most 127 characters |
| Arguments | 32 |
| Nested calls | 4 |
| Loop count and `sleep` | 0 .. 1000000 |
| `wait` | 0 waits for an event; otherwise 1 .. 1000000 ms |
| Hardware timers | 3, period 10 .. 40000000 µs |
| `irq` edges | 1 rising, 2 falling, 3 both, plus 4 to debounce |
| Interrupt functions | 8 armed at once |
| Script file | 1024 bytes |
| Nested `source` | 3, including the outermost |
| `rand()` | 0 .. 32767 |
| `sin` / `cos` argument | about -1000000 .. 1000000 radians |
| `now` / `time` | 1970-01-01 00:00:00 .. 2038-01-19 03:14:07 |
