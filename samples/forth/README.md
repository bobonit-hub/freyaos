# forth

A Forth machine: an interactive interpreter on the serial console, a
compiler that turns definitions into threaded code, and the virtual
machine that runs them. 122 words are built in, new ones are defined at
the prompt or read from a file on the card, and a program can reach any
address in the memory map with `@` and `!`.

```
freya:/> run forth.bin
--- forth starting (Ctrl-C stops it) ---
forth: 122 words, 43008 byte dictionary, 32-bit cells
  'words' lists them, 'bye' or Ctrl-C leaves
forth> : square dup * ;
  ok
forth> 12 square .
144   ok
forth> : stars 0 do [char] * emit loop cr ;
  ok
forth> : triangle 1+ 1 do i stars loop ;
  ok
forth> 5 triangle
*
**
***
****
*****
  ok
forth> bye
  ok
forth: 80 of 43008 dictionary bytes used
--- forth returned, exit status 0, 41283 ms ---
```

## Build and run

```sh
make                   # Black Pill: build/blackpill/samples/forth.bin and .xip.bin
make BOARD=bluepill    # Blue Pill:  build/bluepill/samples/forth.xip.bin only
```

The interpreter is 8 KiB of code, which is the whole of the Blue Pill's
8 KiB program RAM region before a single dictionary byte is counted, so
on that board Forth is a flash resident program and the Makefile builds
the `.xip.bin` alone. The RAM window then holds nothing but the
dictionary and the stacks, which is the arrangement the flash region
exists for.

```
freya:/> run forth.bin              Black Pill, from the card
freya:/> install forth.xip.bin      either board, into internal flash
freya:/> runflash                   and run it from there
```

```sh
make BOARD=bluepill flash PROGRAM=forth    # packed into the module
```

Any arguments are source files, interpreted before the prompt appears:

```
freya:/> run forth.bin demo.fs
```

## Memory

Everything is sized from the board's program region, so the same source
builds for both:

| | Black Pill | Blue Pill (flash image) |
|---|---|---|
| dictionary | 43008 B | 7168 B |
| data stack | 128 cells | 48 cells |
| return stack | 128 cells | 48 cells |
| input line | 200 B | 128 B |

A definition costs its name plus two bytes per compiled word, so
`demo.fs` — sixteen definitions — takes 524 bytes. There is no
garbage collection: `forget <name>` drops a word and everything defined
after it, which is how space comes back.

## Words

Case does not matter: `DUP` and `dup` are the same word. Flags are 0 and
-1. `words` prints the whole list at any time, newest first.

| Group | Words |
|---|---|
| Stack | `dup drop swap over rot -rot nip tuck ?dup depth` |
| Double | `2dup 2drop 2swap 2over` |
| Return stack | `>r r> r@` |
| Arithmetic | `+ - * / mod /mod negate abs min max 1+ 1- 2* 2/` |
| Bitwise | `and or xor invert lshift rshift` |
| Compare | `= <> < > u< 0= 0< 0>` |
| Constants | `true false bl cell+ cells` |
| Memory | `@ ! c@ c! +! , c, here allot align unused move fill` |
| Defining | `: ; variable constant create ' ['] execute immediate forget recurse literal [ ]` |
| Control | `if else then begin until while repeat again do loop +loop i j leave unloop exit` |
| Console | `emit key key? . u. .s cr space spaces type dump page words` |
| Strings | `." s" char [char]` |
| Base | `base hex decimal` |
| Comments | `( ... )` and `\ to end of line` |
| Freya | `ms ticks led cpuhz include bye abort` |

Numbers are decimal unless `hex` or `base !` says otherwise; `$ff`,
`#42`, `%1010` and `'A'` override the base for one number.

The last row is the service table: `ms` delays, `ticks` reads the
millisecond counter, `led` drives PC13, `cpuhz` is the clock the board
actually came up at, and `include <file>` interprets a file from the SD
card — a file may include another, and no deeper. `abort` clears the
stack and returns to the prompt; `bye` returns to the shell.

## demo.fs

`samples/forth/demo.fs` defines stars and triangles, FizzBuzz, factorial
and gcd, a blink, and reads the SysTick counter through `@` to show that
addresses are real. Copy it onto the card next to the program:

```
freya:/> run forth.bin demo.fs
forth> 15 fizzbuzz
1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz
  ok
forth> 6 fact .
720   ok
forth> mhz
96 MHz
  ok
```

## Errors

Everything the interpreter can catch is caught, and the prompt comes
back: an unknown word, a full dictionary, a stack that ran out, a
division by zero. A definition that fails half way is rolled back rather
than left in the dictionary.

```
forth> : broken 1 + oops ;
 ? oops
   definition abandoned
forth> 1 0 /
 ? division by zero
```

What the interpreter cannot catch, Freya does. `@` and `!` take real
addresses, so `$F0000000 @` is a BusFault — and the fault handler
reports it, ends the program and gives the shell back, exactly as it
does for any other program.

`Ctrl-C` stops the program from anywhere, including from inside
`begin ... again`: the machine yields to the kernel every thousand
tokens.

## How it works

A word is a 16-bit token. Below 132 it is a primitive, and `do_prim()`
in `main.c` is the switch that implements it. At or above 132 it is a
definition, and the token is the byte offset of its body in the
dictionary divided by two. Compiling a call is therefore two bytes
rather than the four cell threading would need, which is what makes a
useful dictionary possible in the Blue Pill's RAM window.

A dictionary entry is a two-byte link to the previous one, a byte of
flags and name length, the name, and then the body: a list of tokens
ending in the one the compiler emits for `;`. The machine keeps the
caller's position on the return stack, so a call is a push and a return
is a pop, and that same stack carries `do` loop parameters.

Ten primitives have no name and cannot be typed. They are what the
compiler emits for a literal, the two kinds of branch, a return, the
body of a `variable`, the two kinds of inline string, and the three
halves of a `do` loop.

## Testing it without a board

`make test` builds this file for the host with a service table that
captures its output and runs 79 cases through it — arithmetic, every
control structure, defining words, string literals, source files and
each error path. The same binary will also talk to a terminal:

```sh
make test
build/tests/hostforth -i samples/forth/demo.fs
```

## Limits

* One cell is 32 bits. There are no double-cell or floating point words.
* No `does>`, no vocabularies, no `postpone`, and no block files.
* `create` gives a word that pushes its data address; the data is
  whatever `allot` and `,` put there.
* `include` nests two deep, and a line is at most 200 bytes (128 on the
  Blue Pill).
* Names are at most 31 characters.
* The dictionary lives in RAM and is lost on reset. Keep source on the
  card and `include` it, or pack it into the module next to the program.
