# Freya 2.0.1 "Reptiloid"

24 September 2026

Reptiloid follows UFOnaut. The console banner and `sysinfo` print the
version and this name:

```
Freya 2.0.1 "Reptiloid" for STM32F411CEU6
```

The program ABI is still version 3. SPI, XTEA, the raw console, board
power and the ADC are appended to the service table, so a program built
against 1.1 still loads. One built against this kernel can check
`FREYA_API_HAS` before calling the new entries.

The shell is a small language on the scripts from 1.1: values, variables,
functions, and the calls below. See [docs/shell.md](docs/shell.md).

## What changed

* `upload` sends a file on the card to the host as XMODEM or XMODEM-1K.
  The line before the transfer states the size in bytes. `download`
  takes `--size <bytes>` and stores that many, so the padding byte is
  not part of the file. `tools/fremote.py` uses both: a remote shell,
  and `fs ls`, `fs cp`, `fs cat`, `fs rm`, `fs mkdir`, `fs mv` and
  `fs df` against the card. A path with a leading `:` is on the board.
* The shell searches a string with a Lua pattern. `match("abc-12", "%d+")`
  returns the matched text, or the captures when the pattern has them,
  or `none` when nothing matches. `find` returns the start and the end,
  counting from 1. `gsub` returns the new string and how many
  replacements it made. A class is `%d` or `%a`, an uppercase class is
  the complement, and `*` `+` `-` `?` `^` `$` and `()` work as they do
  in Lua. There is no alternation.
* The shell can run two functions at once. `spawn("blink", 1)` starts
  one and returns its id. It runs until `sleep`, `yield` or `return`,
  then another ready one runs, and a larger priority goes first.
  `join(id)` waits until it has finished. `stop blink` stops it.
  The two share the interpreter and the variables. They are not the
  threads a program starts, and `run` is refused while one is alive.
* The shell has a small file API in the shape of Lua's `io` library.
  `open(path, mode)` returns a handle. `read` takes a line, the rest of
  the file, a count of characters, or a number, and returns `empty` at
  the end. `write` writes strings, the text of a number, or one raw
  byte. `close`, `seek` and `flush` finish the set. Four files may be
  open, and a string read back is at most 31 characters.
* The shell orders an array. `min` is the least element, `max` is the
  greatest, and `sort` returns a new array in that order. Numbers
  compare by value and strings by text. An empty array has no least
  or greatest element.
* The shell has auto arrays and dicts. `array(10, 20)` stores elements
  of one type and grows when an index past the end is written, up to 8.
  `dict("b", 2, "a", 1)` stores pairs whose keys are one type and whose
  values are one type. The keys are kept sorted, and a lookup is a
  binary search. `$a[i]` and `$d["a"]` read, `set a[i]` and `set d[k]`
  write, and `len` is the count. Assigning a name copies. Four may
  exist at once. The cells are taken from the heap and freed when the
  name is unset or replaced.
* A function can return 1 to 32 values from anywhere in its body.
  `return 1, 2.5, "ok"` keeps each type. A call used as one value
  yields the first. `set a, b, c pair(1)` stores one returned value
  per name.
* The shell can arm a hardware timer or a pin interrupt and run a
  script function when it fires. `timer(1000000, 0, "ontick")` starts
  a one-second tick, `irq("PB0", 2, "onpress")` arms a falling edge,
  and `wait(0)` calls the named function from the shell, not from the
  interrupt. `$1` is how many times that source has fired. `ticks()`
  is milliseconds since boot. A timer or a pin armed this way keeps
  running across a program, until `tclose` or `irq(pin, 0)`.
* The shell reads the software clock. `now()` is seconds since
  1970-01-01 00:00:00, through 2038-01-19 03:14:07. `date()` is
  `YYYY-MM-DD HH:MM:SS`, and `year`, `month`, `day`, `hour`, `minute`
  and `second` each return one field. `time` builds the seconds from
  those six fields. The `date` command is still what sets the clock.
* The shell has `sin(angle)`, `cos(angle)` and `pi()`. The angle is in
  radians. `sin(pi() / 2)` is 1 and `cos(pi())` is -1.
* The shell has the ANSI C 1989 example generator. `rand()` returns
  an integer from 0 to 32767, and `srand(seed)` sets the 32-bit state.
  The state starts at 1, and the same seed repeats the same sequence.
* The shell has bool and none values. `true` and `false` are the bools,
  `bool(0)` is `false`, and `bool("true")` is `true`. `none` is a value
  with no number, distinct from `empty`. `if true` takes the first branch.
* The shell converts between an integer, a byte, a bool, a float, text
  and hex. A byte is 0 to 255, written `65b`, and `byte(n)` makes one.
  `byte(0x41)` is the hex form. `empty` is a value with no number;
  `empty()` is the same value. `int("0x10")` is 16, `float(16)` is 16,
  `str(16)` is `"16"`, and `hex(16)` is `"10"`. `hex("ffffffff")` and
  the literal `0xFFFFFFFF` are both -1. A float is truncated toward zero.
* The shell has built-in functions for pins. `get("PB0")` reads a pin,
  `set("PB5", 1)` drives it and returns the level read back,
  `adc("PA0")` (also `"temp"` and `"vref"`) returns one raw sample,
  and `pwm("PB6", 1000, 25)` starts a channel. `pwm("PB6")` stops it.
* The shell has functions. `fn add` ... `return $1 + $2` ... `end`
  defines one. A call is an expression, `add(2, 3)`, with 0 to 32
  arguments. `$0` is the count and `$1` .. `$32` are the arguments.
  Four functions, each body at most 127 characters.
* The shell has variables. `set n 1 + 2 * 3` stores an integer, a byte,
  empty, a float or a string; `$n` expands it. Numbers have `+ - * /`,
  integers and bytes also have `%` and `~ & | ^ << >>`, and a string is
  concatenated with `+` or formatted (`set s "%d" $n`). `==` and `/=`
  compare, and `if $n == 7` takes that as the condition. Eight names,
  each at most seven characters.
* The kernel extension grew to hold the shell language, the SPI master,
  XMODEM and the cipher. On the Blue Pill it is the last 43 KiB of the
  128 KiB, and the program flash region is 37760 bytes, through
  `0x080153FF`. On the Black Pill it is 42 KiB at the start of sector 5,
  and the program region stays 64 KiB.
* Programs can take synchronous 12-bit ADC1 samples from the common analog
  pins, the internal temperature sensor, and Vref. `adc PA0`, `adc temp`,
  and `samples/adc` use the same appended service-table call.
* A program can encrypt and decrypt with XTEA in CTR mode. The key is
  16 bytes and the nonce is 8. The same call does both, and a message
  longer than 4096 bytes is handed over in pieces. `crypt` at the
  console takes hex, and `samples/crypt` checks the published block
  vector or encrypts a file. See [docs/crypt.md](docs/crypt.md).
* A program can speak SPI as a master. The card keeps SPI1. Bus 1 is
  SPI2 on both boards: SCK PB13, MISO PB14, MOSI PB15. Chip select is a
  pin the program drives. The clock is the fastest power-of-two division
  of the bus clock that does not exceed the rate asked for, from 187.5 kHz
  to 24 MHz. `spi 1 1000000` opens it, and `samples/spi` checks the wires.
  See [docs/spi.md](docs/spi.md).
* A program can take the console raw. Ctrl-C then arrives through `getc`
  as `0x03`, and the kernel turns raw mode off when the run ends.
* A program can switch the SD socket supply. `power sd off` drops VDD
  and `power sd on` brings it back. `api->power(FREYA_PWR_SD, on)` is
  the same call.
* `samples/edit` is a terminal text editor for a file on the card.
  See [samples/edit/README.md](samples/edit/README.md).
* `samples/altair` is an Altair 8800b Turnkey emulator. It runs Altair
  BASIC and other original software from the card: memory images, Intel
  HEX and MITS paper tapes. The menu can also take a memory image or
  Intel HEX over XMODEM. Ctrl-] opens the front panel. The 48 KiB
  machine is Black Pill only, from flash. `samples/altair16` keeps the
  8080's RAM at 16 KiB so it fits the program RAM region, and on the
  Blue Pill that RAM lives in program flash. See
  [samples/altair/README.md](samples/altair/README.md).

# Freya 1.1 "UFOnaut"

23 September 2026

UFOnaut follows Chupacabra. The console banner and `sysinfo` print the
version and this name:

```
Freya 1.1 "UFOnaut" for STM32F411CEU6
```

The program ABI is still version 3. The 1-Wire and thread calls are
appended to the service table, so a program built against 1.0.1 still
loads. One built against this kernel can check `FREYA_API_HAS` before
calling the new entries.

## What changed

* The shell runs scripts. `source <file>` reads a text file from the
  card, at most 1024 bytes, and runs it with the same rules as a typed
  line: `;`, newlines, `if`/`else`/`end`, `loop`, `sleep` and `$?`. A
  `#` at the start of a statement, or after a space, is a comment.
  Each command is still one line of at most 159 characters. Ctrl-C
  stops the script. Scripts may nest, three deep including the line
  that started them.
* `source @flash` runs a script stored in the program flash region, and
  needs no card. `install` of a text file stores that script there, in
  place of a program image. `saveflash` copies it back (default
  `/script.sh`). `uninstall` erases it. `meminfo` names it. `runflash`
  of a script says to use `source @flash`. With `autostart on`, the next
  boot runs the script when `/autorun.bin` is absent. A short flash
  script is copied into RAM before it runs, so the script may `install`
  or `uninstall` without erasing the text it is still reading.
* A program can start named threads. Each has a name and a priority from
  0 to 7; the highest priority that is ready runs, and equal priorities
  take turns. `threads` lists them. `stop blink` stops that thread;
  `stop` with no name still stops the whole program. The Black Pill has
  room for four of these threads, the Blue Pill for two, each with 1 KiB
  of stack. See [docs/threads.md](docs/threads.md).
* The scheduler and the script interpreter live in a kernel extension, a
  second flash image, so the 48 KiB kernel still does not share an erase
  unit with the auto-start slot. On the Blue Pill that extension is the
  last 8 KiB of the 128 KiB, and the program flash region is 73600 bytes,
  through `0x0801DFFF`. On the Black Pill the extension is 16 KiB at the
  start of sector 5, and the program region stays 64 KiB.
* A program can speak 1-Wire at standard speed on a spare pin: presence,
  byte reads and writes, a ROM search, and a strong pull-up. Up to four
  pins may be open at once. `w1 PB12 search` lists the devices, and
  `samples/w1` reads a DS18B20. See [docs/w1.md](docs/w1.md).
* The Blue Pill has no FPU. A program that uses single-precision float is
  linked with `src/softfp.c` (add, subtract, multiply, divide, compare,
  and conversion to or from an integer). Helpers the program does not
  call stay out of the image.
* Host tests delete the FAT disk images when a run finishes (`make test`).

# Freya 1.0.1 "Chupacabra"

22 September 2026

1.0.1 is a small follow-up to Chupacabra. The kernel, the program ABI and
both boards are unchanged. The console banner and `sysinfo` print the new
version:

```
Freya 1.0.1 "Chupacabra" for STM32F411CEU6
```

## What changed

* The tree is under the MIT license. See [LICENSE](LICENSE).
* `help` lists each command by the word you type. `help <command>` prints
  that name, then a usage line when the usage is more than the name.
  Commands that take no arguments no longer show a description in place of
  the name.
* The README describes Freya as a 32-bit, single-task, single-user, text OS
  for STM32 small MCUs.
* SD slot wiring for each board is drawn in
  [docs/sd-slot.txt](docs/sd-slot.txt). The README points there, and says
  to ground the USB-serial adapter with the board.
* Host tests cover the shell help text (`make test`).

# Freya 1.0 "Chupacabra"

22 September 2026

Chupacabra is the first release of Freya, a bare-metal operating system for
the STM32F411CEU6 "Black Pill" and the STM32F103C8T6 "Blue Pill". It is
written in C and ARM assembly, with no HAL, no CMSIS and no third-party
libraries. The kernel lives in internal flash, brings the chip up itself,
and talks to the hardware through its own register definitions.

The console banner and `sysinfo` print the version and this name:

```
Freya 1.0 "Chupacabra" for STM32F411CEU6
```

## Boards

|  | Black Pill | Blue Pill |
|---|---|---|
| MCU | STM32F411CEU6 | STM32F103C8T6 |
| Core | Cortex-M4F at 96 MHz | Cortex-M3 at 72 MHz |
| Crystal | 25 MHz | 8 MHz |
| Flash | 512 KiB | 128 KiB |
| SRAM | 128 KiB | 20 KiB |
| Program RAM | 56 KiB at `0x20010000` | 8 KiB at `0x20001800` |
| Program flash | 64 KiB (sector 4) | 77696 bytes |
| Build | `make` | `make BOARD=bluepill` |

Both boards fall back to the internal oscillator if the crystal does not
start. SysTick, the console divisor and the microsecond delay follow the
clock the chip actually came up at. Every supported board is treated as
having at least 128 KiB of internal flash. On the Blue Pill the size
register often still reads 64 KiB; flashing and the program region use the
full 128 KiB anyway.

The console is USART2 at 921600 8N1 on PA2/PA3, interrupt driven, with line
editing and command history. Neither board divides 921600 exactly; both
land about 0.16% fast, inside what 8N1 tolerates. There is no hardware flow
control.

## What this release includes

* A serial shell. The command list is the same on both boards, including
  `install`, `runflash`, `saveflash`, `uninstall`, `autostart`, `ramdump`,
  `flashdump`, `pin`, `pwm` and `i2c`. See
  [docs/console-commands.md](docs/console-commands.md).
* SD and SDHC cards over SPI, and a FAT16/FAT32 filesystem that reads and
  writes files, directories, long names and MBR partitions. Directory
  entries can be renamed without copying the data.
* XMODEM and XMODEM-1K receive (`download`), with padding stripped unless
  `--raw` is given.
* A dated file log at `/freya.log`, capped at 1 MiB and rotated to
  `/freya.log.old`. The level (`off` through `debug`) is stored in the
  auto-start flash slot and survives reset. With no card mounted, the same
  line goes to the console.
* One program at a time, loaded from the card into RAM or installed in a
  reserved flash region and copied into RAM before it runs. Pointers in an
  installed image are rebased from the relocation table. A program that
  does not fit in RAM with its writable state still executes in place.
  `make flash PROGRAM=<name>` packs that image into the file written to
  the module. `AUTOSTART=1` sets the boot flag in the packed image.
* A program service table: console I/O, `printf`, `malloc`/`free`, time,
  the LED, the filesystem, exit status, pins, pin interrupts, three
  hardware timers, PWM, I2C and the file log. The header is
  `include/freya_api.h`.
* Pins the kernel does not keep, an interrupt on any edge of one of them,
  and timers from ten microseconds to forty seconds. Eight pins (PA0, PA1,
  PB0, PB1, PB6–PB9) can be PWM, 1 Hz to 1 MHz, with duty in
  ten-thousandths or as a servo pulse width. Handlers that fault, or never
  return, are stopped the same way a program is. See
  [docs/interrupts.md](docs/interrupts.md).
* I2C master, 7-bit, 10 kHz to 400 kHz, including the repeated start a
  register read needs. Bus 1 is PB6/PB7 on both boards. See
  [docs/i2c.md](docs/i2c.md).
* Ctrl-C stops a running program, including one stuck in a loop. A fault
  in a program is reported and the shell returns. A BusFault can write
  SRAM to `/freya.ram` when `ramdump` is on (off by default).
* Exit status for every run, in the POSIX shape: the program's own code,
  130 for Ctrl-C, or 128 plus the fault. It is printed, stored as `$?`,
  written to the log, and readable by the next program.
* `/autorun.bin` on the card starts at boot after a two-second cancel
  window. If that file is absent and auto-start is on, the installed flash
  program runs instead.
* Host tests for the filesystem, XMODEM, regions, exit status, Forth and
  interrupts (`make test`).

Programs shipped with the tree:

| Kind | Names |
|---|---|
| Apps | `hello`, `spin` |
| Samples | `blink`, `log`, `irq`, `pwm`, `i2c`, `spi`, `w1`, `threads`, `flashprobe`, `tetris`, `edit`, `forth` |

`forth` is larger than the Blue Pill program RAM region, so on that board
it is built as a flash image only.

## Program ABI

The program ABI is version 3. The kernel still loads ABI 1 RAM images and
reads an ABI 2 header. A flash image must be ABI 3: that version appends
the relocation table used when the image is copied into RAM. ABI 1 is a
prefix of ABI 2, and ABI 2 is a prefix of ABI 3, so an older RAM image
already on a card still loads.

Images are linked for one board. The loader refuses a file whose load
address is not this board's program region.

## Building

```sh
make                      # Black Pill
make BOARD=bluepill       # Blue Pill
make test
make flash PROGRAM=hello
make BOARD=bluepill flash PROGRAM=blink AUTOSTART=1
```

`make flash` uses `st-flash`. `make openocd` uses ST-Link. `make bootloader`
is USB DFU on the Black Pill and the ROM serial loader (`stm32flash`) on
the Blue Pill.

Wiring, the command reference and the program model are in [README.md](README.md).
