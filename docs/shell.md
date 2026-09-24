# Shell language

The console shell is a small language as well as a list of commands. A
line can store a value, test it, repeat a command, call a function, and
read or drive a pin. The same rules apply to a line typed at `freya:`
and to a script run with `source`. The commands themselves are listed in
[console-commands.md](console-commands.md).

A value is an integer, a float, a byte, a bool, empty, none, a string
of at most 31 characters, an auto array, or a dict.
An expression produces one value. Typed on its own, it is printed:
`3 + 2` prints `5`, `"Sun"` prints `"Sun"`, and `(2 + 2) % 10` prints
`4`. Console commands use function syntax: `help()`, `sysinfo()`,
`ls("-l", "/")`. Their arguments are expression values, so text is quoted.
A command produces a status, which `$?` reads back. The statement forms
`set`, `fn`, `return`, `if`, `else`, `loop`, `break`, and `end` are shell
language syntax rather than console commands and do not take parentheses.

## Lines

`;` separates commands on one line. A new line separates them the same
way. Quotes hide a semicolon, so `echo("a;b")` is one command. A command
is at most 159 characters.

A `#` at the start of a statement, or after a space, comments out the
rest of that statement. Quotes hide it, so `echo("a # b")` prints the
hash. A line that is only a comment is skipped.

`$?` anywhere in a command becomes the status of the previous command,
as decimal text. `$name` becomes that variable, also as text. `$0` is
how many arguments the current function call received, and `$1` .. `$32`
are those arguments. All of these are expanded when the command runs, so
each pass of a loop sees the values the previous command left. A name
that is not set, or an argument that was not passed, is an error. The
expansion happens before the line is split, so `echo($n)` and
`write("/runs.txt", $?)` both work.

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
prompt changes to `>`. Those lines together have to stay within 159
characters. Nothing runs until the block is closed, so a missing `end`
or an `else` in the wrong place does not half-run the commands. Ctrl-C
on the `>` prompt throws the lines away.

Blocks nest, eight deep. `if`, `loop` and `fn` each count.

`if` has two forms. When the text after `if` contains `==`, `/=`, `<`,
`>` or `><` outside quotes, that text is a condition. True runs the
commands up to `else` or `end`. False runs the `else`, when one was
written. The condition's value is an integer or a bool. An integer is false
at 0 and true otherwise. A bool is `true` or `false`. A condition
that is neither, such as a bare float, is an error.

`true`, `false`, and `bool(...)` are conditions even without a
comparison. Anything else after `if` is a command. Status 0 takes the first branch.
Any other status takes the `else`. A branch that is skipped does not
change `$?`. A condition does: true leaves 0 and false leaves 1.

```
freya: if echo hi
> echo yes
> else
> echo no
> end
hi
yes
freya: if $n == 7
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
freya: loop 3; echo tick; sleep 200; end
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
`0xFFFFFFFF` is `-1`. A byte is a decimal literal with a trailing `b`
(`0b`, `255b`) and is only 0 to 255. `b` is a hex digit, so a hex byte
is `byte(0xFF)`, not a `b` after the digits. A float has a decimal point
(`7.5`, `.5`, `1.`). There is no exponent. A string is written in double quotes. There are no
backslash escapes; a quote ends the string. A string in an expression is
at most 31 characters. `true` and `false` are the bool values.
`empty` is the only value of the empty type. `none` is the only
value of the none type. `empty` and `none` are not the same value.

`pi()` is the float constant 3.14159265. It takes no argument.

`$name` inside an expression is the stored value, with its type. `$?` is
an integer. `$0` .. `$32` are the arguments of the call in progress.
Parentheses group. A call is `name(arg, ...)`.

Unary `+` and `-` apply to an integer, a byte, or a float. Unary `+`
on a byte leaves the byte. Unary `-` on a byte leaves the integer of
the opposite sign. Unary `~` applies to an integer or a byte and leaves
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

`+ - * /` work on numbers. A byte is promoted to an integer first, so
`1b + 2b` is the integer 3. If either side is a
float the result is a float, and integer `/` truncates toward zero.
`%`, `~`, `&`, `|`, `^`, `<<` and `>>` require integers or bytes. The
shifts fill with zero bits, so
`-1 >> 1` is 2147483647, not -1. Division by zero and an integer result
that does not fit are errors.

`+` concatenates when either side is a string. The other side is
rendered as text and the result is a string. A string written next to
further values, with no operator, is a format. Each value fills the
next conversion, and a later conversion is left for the value after
that.

| Conversion | Accepts | Result |
|---|---|---|
| `%d`, `%i` | integer, byte, or a float truncated toward zero | decimal |
| `%u` | integer or byte | unsigned decimal |
| `%x` | integer or byte | lowercase hex |
| `%X` | integer or byte | uppercase hex |
| `%f` | integer, byte, or float | the float's text |
| `%s` | any value | its text |
| `%%` | nothing | one `%` |

```
freya: set n 1 + 2 * 3
freya: set x 7.5 / 2
freya: set s "%d %s" $n "items"
freya: echo $s
7 items
```

`==` and `/=` compare two values and leave the integer 1 or 0. Numbers
match by value, so `1 == 1.0` and `1 == 1b` are true. A string matches
only the same text, and a string does not match a number. A bool
matches only a bool, so `true == 1` is false. `empty` matches only
`empty`, and `none` matches only `none`. `<`, `>` and `><` compare
numbers only: less, greater, and not equal. A string, a bool, `empty`,
or `none` on either side is an error. Several comparisons associate left to right, and each result
is an integer, so `1 < 2 < 3` is `(1 < 2) < 3`.

## Variables

`set` with no name lists the variables. `set <name> <expr>` stores the
value of the expression. `set <name>, <name>, ... <call>` stores the
values that call returned, one per name. The type is whatever each
value was produced as. There are eight variables. A name is a letter or `_`, then letters,
digits or `_`, at most seven characters. `unset <name>` removes one. A
name that is not set is an error. Defining a variable does not change
`$?` when it succeeds.

`set` prints nothing on success. The list prints `name = 7` for an
integer, `name = 65b` for a byte, `name = true` or `name = false`
for a bool, `name = empty` for empty, `name = none` for none,
`name = 1.5` for a float, `name = "text"` for a string,
`name = [1, 2]` for an array, and `name = {"a": 1}` for a dict.
With no variables it prints `no variables`.

```
freya: set n 1 + 2 * 3
freya: echo $n
7
freya: set
n = 7
```

## Functions

`fn` with no name lists the functions. `fn <name>` ... `end` defines
one. The body is not run at the definition. A name has the same shape as
a variable, and there are four functions. Defining the same name again
replaces the body. The body is at most 127 characters. A longer one is
`function too long`. The body is kept on the heap, so it can also be
refused with `out of memory`. The list
prints one name per line, or `no functions`.

A call is an expression, `name(arg, ...)`, with 0 to 32 arguments. Each
argument is an expression. Inside the body, `$0` is how many arguments
were passed and `$1` .. `$32` are those values, each with the type it
was given. An argument that was not passed is an error. `$?` and
`$name` still mean what they mean outside.

`return` leaves the function from wherever it is written: the top of
the body, a branch, or a loop. It takes 1 to 32 expressions, separated
by commas. Each keeps the type the expression produced, so one `return`
can hand back an integer, a float, a byte, a bool, empty, none, and a string together. When the last
expression is only a call, every value that call returned is handed on.
No `return` leaves the integer 0.

Where a call is used as one value, that value is the first one returned.
`set` with several names takes a call and stores the first values, one
per name, and leaves any further values. Fewer values than names is
`too few values`. More than 32 values is `too many values`.

```
freya: fn add
> return $1 + $2
> end
freya: set n add(2, 3)
freya: echo $n
5
freya: fn pair
> if $1 /= 0
> return 1, 2.5, "ok"
> end
> return 0, 0, "no"
> end
freya: set a, b, c pair(1)
freya: echo $a
1
freya: echo $b
2.5
freya: echo $c
ok
freya: fn
add
pair
```

`return` outside a function is refused before anything runs, the way
`break` is refused outside a loop. A call may call another function.
Calls nest four deep. A call that is too deep or too wide is refused
rather than grown without limit. `break` does not cross a function out
into a loop that called it.

## Reserved words

These names are built in. `fn` refuses each of them with `bad name`.

| Group | Names |
|---|---|
| Blocks | `if`, `else`, `end`, `loop`, `break`, `fn`, `return` |
| Pins | `get`, `set`, `adc`, `pwm` |
| Values | `int`, `float`, `byte`, `bool`, `str`, `hex`, `true`, `false`, `empty`, `none` |
| Arrays and dicts | `array`, `dict`, `len`, `min`, `max`, `sort` |
| Numbers | `rand`, `srand`, `sin`, `cos`, `pi` |
| Clock | `now`, `date`, `time`, `year`, `month`, `day`, `hour`, `minute`, `second` |
| Timers | `ticks`, `timer`, `tstart`, `tstop`, `tcount`, `tclose`, `tperiod`, `irq`, `wait` |
| Threads | `spawn`, `yield`, `join` |
| Files | `open`, `read`, `write`, `close`, `seek`, `flush` |
| Patterns | `match`, `find`, `gsub` |

## Arrays and dicts

An array grows. `array()` is empty. `array(10, 20, 30)` holds those
values. Every element is the same type, fixed by the first one: all
integers, all bytes, all bools, all floats, all strings, all
`empty`, or all `none`. A later
element of another type is `type mismatch`. `1` and `1b` are not the
same type.

`$a[i]` reads the element at that index. The index is an integer or a
byte. `set a[i] <expr>` writes it. An index past the end grows the
array and fills the gap with the zero of the element type: `0`, `0b`,
`0.0`, `false`, `""`, `empty`, or `none`. The first write on a new name, `set a[0] 4`,
creates the array. There are at most 8 elements. An index past that is
`out of range`, and `array` with more than 8 values is `array too long`.

A dict maps keys to values. `dict()` is empty. `dict("b", 2, "a", 1)`
holds those pairs. Every key is one type and every value is one type,
each fixed by the first pair. The keys are kept in sorted order, and a
lookup is a binary search. `$d["a"]` reads a value. A missing key is
`no such key`. `set d["c"] 3` inserts or replaces, still in order. A
string, float, bool, `empty`, or `none` index on a new name creates a dict;
`set d["x"] 7` is that. An integer index on a new name creates an
array instead, so an integer-keyed dict starts with `dict()`. There
are at most 8 pairs. One past that is `dict too long`.

`len` is the number of elements, or of pairs. `min` is the least
element of an array and `max` is the greatest. Numbers compare by
value and strings by text, the same order a dict keeps its keys in.
An empty array is `empty array`. `sort` returns a new array in that
order and leaves the one it was given alone. `set b $a` copies the
array or the dict. The copy does not follow later writes to the
original. `==` and `/=` compare the elements, or the pairs in order.
`<` and `>` do not order an array or a dict.

Four arrays and dicts may exist at once, counting a copy and a value
that a call is still holding. The cells live on the heap. `unset`, or
`set` of a different value over the same name, frees them. A fifth is
`out of memory`.

```
freya: set a array(30, 10, 20)
freya: echo min($a)
10
freya: echo max($a)
30
freya: set b sort($a)
freya: echo $b
[10, 20, 30]
freya: unset b
freya: set a array(10, 20)
freya: set a[2] 30
freya: echo $a[1]
20
freya: set d dict("b", 2, "a", 1)
freya: echo $d["a"]
1
freya: set
a = [10, 20, 30]
d = {"a": 1, "b": 2}
```

## Conversion

`int`, `float`, `byte`, `bool`, `str` and `hex` each take one value.
`empty()` takes none and returns `empty`, the same value as the literal.
`none()` returns `none`. `true()` and `false()` return those bools.

`int` makes an integer. A byte is widened. A float is truncated toward zero. A string is a
decimal integer, a `0x` hex integer, or a decimal float that is then
truncated. A leading `+` or `-` is allowed. Hex keeps all 32 bits, so
`int("0xFFFFFFFF")` is `-1`, and the decimal `-2147483648` is accepted.

`float` makes a float from an integer, a byte, or from decimal text. A `0x`
string is read as an integer first and then widened. A leading sign is
allowed.

`byte` makes a byte, 0 to 255. An integer or a byte in that range is
kept. A float is truncated toward zero and then has to fit. A string is
read the way `int` reads it, and the result has to fit. `byte(256)` and
`byte(-1)` are `integer overflow`.

`bool` makes a bool. An integer, a byte, or a float is `false` only
at zero, and any other number is `true`. A float that is not a number
is `not a number`. A string is `true` or `false`. `bool(true)` is
`true`.

`str` renders any value as text, the same way `$name` does. A string is
left as it is. `true` is the text `true`, `false` is `false`,
`empty` is `empty`, and `none` is `none`.

`hex` of a number is lowercase hex text with no `0x`. A byte is the
same as that integer. A float is
truncated first. `hex(-1)` is `"ffffffff"`. `hex` of a string parses hex
digits, with an optional sign and an optional `0x`, back to an integer.
`hex(hex(255))` is 255, and `hex(hex("ff"))` is `"ff"`.

`int(true)` is 1 and `int(false)` is 0. `float` and `byte` do the
same, and `hex(true)` is `"1"`. A bool is not a number in arithmetic,
so `true + 1` is an error. `%d` of a bool is `bad format`; `%s` prints
`true` or `false`.

`empty` and `none` have no number. `int(empty)`, `float(none)`,
`byte(empty)`, `bool(none)` and `hex(empty)` are `not a number`.
Adding to either, or ordering it, is an error. `"x" + empty` is the
string `xempty`, and `"x" + none` is `xnone`.

A value that is not of that form is `not a number`. One that does not
fit in an integer is `integer overflow`.

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

## Patterns

`match`, `find` and `gsub` search a string with a Lua pattern. The
subject and the pattern are strings. There is no alternation and no
count such as `{2,4}`. A pattern that is not well formed is
`bad pattern`. One that would have to try too many ways is
`pattern too complex`.

A pattern is matched anywhere in the text, unless it starts with `^`,
which matches only at the first character searched. `$` at the end of
the pattern matches only at the end of the text. `.` is any character.
`%` names a class, or quotes the character after it:

| Class | Matches |
|---|---|
| `%a` `%A` | letters, or everything else |
| `%c` `%C` | control characters, or everything else |
| `%d` `%D` | digits, or everything else |
| `%g` `%G` | printable characters except space, or everything else |
| `%l` `%L` | lowercase letters, or everything else |
| `%p` `%P` | punctuation, or everything else |
| `%s` `%S` | spaces, or everything else |
| `%u` `%U` | uppercase letters, or everything else |
| `%w` `%W` | letters and digits, or everything else |
| `%x` `%X` | hex digits, or everything else |

`[abc]` is any of those characters. `[^abc]` is any other. A range is
written `a-z`, and a `%` class may sit inside the brackets. `*` is the
longest run of zero or more, `+` the longest run of one or more, `-`
the shortest run of zero or more, and `?` is one or none.

`()` captures the text it covers. At most nine captures. An empty
`()` captures the position, an integer counting from 1. `%1` through
`%9` in the pattern repeat the text of that capture. `%b()` matches
from a `(` to the `)` that balances it, and the two characters after
`%b` are that pair.

`match(text, pattern)` returns the whole match when the pattern has no
captures, and the captures when it has some. No match is `none`.
`match(text, pattern, index)` starts at that character, counting from
1. A negative index counts back from the end.

`find(text, pattern)` returns where the match starts and where it
ends, counting from 1, and then the captures. The end of an empty
match is the character before it started. No match is `none`.
`find(text, pattern, index, true)` looks for those exact characters
and does not read the pattern.

`gsub(text, pattern, repl)` replaces every match. In `repl`, `%0` is
the whole match, `%1` through `%9` are the captures, and `%%` is one
`%`. The result is the new string, and the second value is how many
replacements were made. `gsub(text, pattern, repl, n)` stops after `n`.
A result longer than 31 characters is `string too long`.

```
freya: set s match("abc-12", "%a+")
freya: echo $s
abc
freya: set a, b match("abc-12", "(%a+)%-(%d+)")
freya: echo $a
abc
freya: echo $b
12
freya: set a, b find("abc-12", "%d+")
freya: echo $a
5
freya: echo $b
6
freya: set s, n gsub("a1b2", "%d", "x")
freya: echo $s
axbx
freya: echo $n
2
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
as its bit pattern and `srand(-1)` is the seed `0xFFFFFFFF`. A float, a
byte, empty, or a string is refused. The same seed repeats the same sequence.

```
freya: set n srand(1)
freya: set n rand()
freya: echo $n
16838
freya: set n rand()
freya: echo $n
5758
```

## Sine and cosine

`sin(angle)` and `cos(angle)` take one integer, byte, or float, in radians, and
return a float. `sin(pi() / 2)` is 1 and `cos(pi())` is -1. An angle past
about a million radians, or a value that is not a number, is
`not a number`.

```
freya: set x sin(pi() / 2)
freya: echo $x
1
freya: set x cos(pi())
freya: echo $x
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
`integer overflow`. A float, a byte, empty, or a string in any of these is refused.

```
freya: set n time(2026, 1, 1, 0, 0, 0)
freya: set s date($n)
freya: echo $s
2026-01-01 00:00:00
freya: set n year($n)
freya: echo $n
2026
freya: set n now()
freya: echo $n
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
freya: set n set("PB5", 1)
freya: echo get("PB5")
1
freya: set n adc("PA0")
freya: set n pwm("PB6", 1000, 25)
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
freya: fn ontick
> echo tick
> return $1
> end
freya: set t timer(1000000, 0, "ontick")
freya: set n wait(0)
tick
freya: echo $n
0
freya: set n tclose($t)
```

The same calls from a program are described in
[interrupts.md](interrupts.md).

## Threads

A script can run two functions at once. They are not the threads a
program starts. Those have a stack of their own and a priority the
scheduler enforces. A script thread shares the one interpreter, so it
runs until `sleep`, `yield` or `return`, and then another ready one
runs. A larger priority runs first. The same priority takes turns at
`yield`.

`spawn(name, priority)` starts the function of that name and returns
its id, 2 or 3. The name is a string. The priority is an integer from
0 to 7. The function must already be defined. `shell` and `idle` are
not names a script thread can take, and a name that is already running
is refused. A third thread is `too many threads`.

The body is the function. It is copied when the thread starts, so
defining that name again does not change the thread that is already
running. `$1` is not an argument of the thread. `return` ends it. The
value is dropped.

`yield` gives the other ready threads a turn and returns to this one
after they have slept, yielded or finished. `yield()` is the same call.
`sleep` does too, and a thread that is sleeping runs again when the
time is up. `join(id)` waits until that thread has finished and
returns 0. An id that is not running has already finished. A thread
cannot join itself.

Variables are shared, and so is `$?`. One statement finishes before
another thread starts, so a `set` is not torn, but two threads can
store into the same name. A loop that never sleeps or yields keeps
the CPU until it ends. Ctrl-C stops every script thread. `stop add`
stops the one named `add`. `threads` lists them with the others.
`run` is refused while one is still alive.

```
freya: fn blink
> echo tick
> sleep 200
> echo tock
> end
freya: set n spawn("blink", 1)
tick
freya: set n join($n)
tock
freya: echo $n
0
```

## Files

`open`, `read`, `write`, `close`, `seek` and `flush` are the file calls.
They are the same shape as Lua's `io` library, written as functions
because a call here has no method. A handle is the integer `open`
returned. Four files may be open at once, the same table a program
uses. `mount` closes them.

`open(path)` reads. `open(path, mode)` takes a mode string: `"r"`,
`"w"`, `"a"`, `"r+"`, `"w+"` or `"a+"`. A trailing `b` is accepted and
changes nothing, because a file is bytes either way. `"w"` creates or
truncates. `"a"` creates and writes at the end. The path is a string,
absolute or from the working directory.

`read(file)` reads the next line and drops the newline. A `\r` just
before that newline is dropped too. `read(file, "*l")` is the same.
`read(file, "*L")` keeps the newline. `read(file, "*a")` reads what
remains. `read(file, n)` reads up to `n` characters, `n` from 0 to 31.
`read(file, "*n")` skips spaces and reads a number, an integer when it
has no decimal point and a float when it does. The end of the file is
`empty`. An empty line is `""`. A result longer than 31 characters is
`string too long`, and the position is left where the read started.

`write(file, value, ...)` writes each value and returns how many bytes
were written. A string is those characters. An integer or a float is
its text. A byte is one raw byte, which is how a newline is written:
`10b`. There is no escape in a string.

`close(file)` closes it and returns 0. `seek(file)` is the position.
`seek(file, "set", offset)`, `seek(file, "cur", offset)` and
`seek(file, "end", offset)` move it and return the new position. The
offset may be left out and is then 0. `flush(file)` writes the card's
pending data and returns 0.

```
freya: set f open("/n.txt", "w")
freya: set n write($f, "hi", 10b)
freya: set n close($f)
freya: set f open("/n.txt")
freya: set s read($f)
freya: echo $s
hi
freya: set s read($f)
freya: echo $s
empty
freya: set n close($f)
```

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
in place of a program image. `saveflash` copies it back, default
`/script.sh`. `uninstall` erases it. `runflash` on a script says to use
`source @flash`. With `autostart on`, the next boot runs the script when
`/autorun.bin` is absent.

## Limits

| Limit | Value |
|---|---|
| Command length | 159 characters |
| Open block being typed | 159 characters |
| Nested `if` / `loop` / `fn` | 8 |
| Variables | 8, names of at most 7 characters |
| String value | 31 characters |
| Array | 8 elements, one type, grows to the index |
| Dict | 8 pairs, one key type, one value type, keys sorted |
| Arrays and dicts alive at once | 4, freed by `unset` or by replacing the name |
| Functions | 4, each body at most 127 characters |
| Arguments | 32 |
| Values from `return` | 1 .. 32 |
| Nested calls | 4 |
| Loop count and `sleep` | 0 .. 1000000 |
| `wait` | 0 waits for an event; otherwise 1 .. 1000000 ms |
| Script threads | 2, priority 0 .. 7, named for the function |
| Hardware timers | 3, period 10 .. 40000000 µs |
| `irq` edges | 1 rising, 2 falling, 3 both, plus 4 to debounce |
| Interrupt functions | 8 armed at once |
| Open files | 4, shared with programs; `mount` closes them |
| `read` result | 31 characters |
| Pattern captures | 9 |
| `gsub` result | 31 characters |
| Script file | 1024 bytes |
| Nested `source` | 3, including the outermost |
| `rand()` | 0 .. 32767 |
| `sin` / `cos` argument | about -1000000 .. 1000000 radians |
| `now` / `time` | 1970-01-01 00:00:00 .. 2038-01-19 03:14:07 |
