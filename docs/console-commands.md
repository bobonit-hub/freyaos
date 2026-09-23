# Console commands

The shell on USART2 is the same program on both boards.  `help` lists
whatever the running image was compiled with; after the Black Pill gained
a program flash region that is the full set on both.

## Blue Pill commands

Every command Freya implements.  The Black Pill now has the same list.

| Command | What it does |
|---|---|
| `help [command]` | list commands, or describe one |
| `sysinfo` | CPU, unique id, clocks, reset cause, uptime, log level, auto-start and ram-dump flags, card, filesystem |
| `meminfo` | flash and RAM usage: .data, .bss, heap, program region, stack |
| `mount` | initialise the card and mount the filesystem |
| `ls [-l] [path]` | list a directory |
| `ll [path]` | list with sizes, dates and attributes |
| `cd [path]`, `pwd` | move around |
| `mkdir <dir>...` | create directories |
| `rm [-r] <path>...` | remove files, empty directories, or whole trees |
| `rename <old> <new>`, `mv` | rename or move a file or directory (no data copy) |
| `download <file> [--raw]` | receive a file over XMODEM |
| `cat <file>` | print a file |
| `write <file> <text...>` | append a line to a file |
| `hexdump <file> [off] [len]` | dump a file in hex |
| `flashdump [file]` | write internal flash to a file on the card (default `/freya.flash`) |
| `df` | capacity, free and used space |
| `load <file>\|@flash` | load a program image into RAM, or bind the flash image |
| `run [file\|@flash] [args...]` | run the loaded program |
| `runflash [args...]` | run the program stored in internal flash |
| `stop [thread]` | stop the program, or one thread by name |
| `threads` | list threads: id, priority, state, name |
| `status` | exit status of the last command and the last program |
| `install <file>` | write a program, or a shell script, into internal flash |
| `saveflash [file]` | copy the installed program or script from flash onto the card |
| `uninstall` | erase the program flash region |
| `autostart [on\|off]` | run the flash program or script automatically at boot |
| `ramdump [on\|off]` | write SRAM to `/freya.ram` after a BusFault (default off) |
| `date [YYYY-MM-DD HH:MM:SS]` | show or set the clock used for file timestamps |
| `loglevel [level]` | show or set the file log level (`off`/`error`/`warn`/`info`/`debug`, or `0`..`4`) |
| `pin <pin> [mode] [0\|1\|toggle]` | read or drive one pin, by the name a program uses for it |
| `pwm [<pin> <hz> <duty%>\|<pin> off]` | list the PWM channels, or start and stop one |
| `i2c [<bus> <hz>\|<bus> off\|<bus> scan\|<bus> <addr> …]` | list the I2C buses, or open, scan and talk to one |
| `spi [<bus> <hz> [mode]\|<bus> off\|<bus> x <byte>…]` | list the SPI buses, or open one and shift bytes |
| `w1 [<pin>\|<pin> off\|<pin> search\|<pin> reset\|…]` | list open 1-Wire pins, or open one and talk to it |
| `sleep <ms>` | wait that many milliseconds; Ctrl-C returns early |
| `source <file>\|@flash` | run a shell script from a file, or from program flash |
| `if <command>` ... `else` ... `end` | run the following commands when that command's status is 0 |
| `loop <count>` ... `end` | repeat the commands up to `end` |
| `uptime`, `led`, `echo`, `clear`, `reboot` | the usual small change |

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
freya:/> pin PB5 out
PB5 = 0
freya:/> pin PB5 1
PB5 = 1
freya:/> pin PB0 up
PB0 = 1
```

`pwm` with no arguments lists the board's eight channels, which timer and
channel each pin is, and what each is doing:

```
freya:/> pwm PB6 1000 25
PB6  TIM4 CH1  1000 Hz 25.00%
freya:/> pwm
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
freya:/> i2c
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
freya:/> spi
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
freya:/> w1 PB12
PB12  1-Wire
freya:/> w1
  PB12
pull the data pin up to 3.3 V
usage: w1 [<pin> | <pin> off | <pin> reset | <pin> search]
```

The pin is named the way `pin` names one. `search` prints every ROM and
`off` puts the pin back to an input. A pin a *program* opened is closed
when its run ends instead. Reading a thermometer is `samples/w1`.
[docs/w1.md](w1.md) has the worked transcript.

Ctrl-C stops a running program and every thread it created. It also
stops a `sleep` or a `loop`, and throws away a script that is still
being typed. `stop` with no name does the same when a program is
running, and unloads it otherwise;
`stop <name>` stops that thread and leaves the run going. `threads` lists
them. Ctrl-U clears the input line, and the up and down cursor keys walk
the command history. While a program runs, only `threads`, `stop` and
`help` are read from the console; anything else waits until the run ends.

`$?` anywhere in a line becomes the exit status of the previous command: 0 when
it worked, 1 when it failed, 127 for a word that is not a command, and for
`run` the status of the program — its own code, 130 after Ctrl-C, or 131..134
after a fault. It is expanded before the line is split, so `echo $?` and
`write /runs.txt $?` both work, and `status` prints the same numbers with the
reason and the run time beside them.

## Scripts

`;` separates commands on one line. A new line separates them the same
way, and quotes hide a semicolon, so `echo "a;b"` is one command.

`if` runs the command written after it. When that command's status is
0, the commands up to `else` or `end` run. Otherwise they are skipped,
and the commands between `else` and `end` run if an `else` was written.
`loop` repeats the commands up to `end` the number of times given.
`sleep` waits that many milliseconds. A count is a decimal number, at
most 1000000. `$?` may be the count: it is read when the loop starts.

```
freya:/> if echo hi
> echo yes
> else
> echo no
> end
hi
yes
freya:/> loop 3; echo tick; sleep 200; end
tick
tick
tick
```

The prompt changes to `>` while a block is still open, and Ctrl-C on
that prompt throws the lines away. Those lines together have to stay
within 160 characters. Nothing runs until the block is closed, so a
missing `end` or an `else` in the wrong place does not half-run the
commands. A branch that is skipped does not change `$?`. `$?` is
expanded when each command runs, so a loop sees a new value every pass.
Blocks nest, eight deep.

A `#` at the start of a statement, or after a space, comments out the
rest of that statement. Quotes hide it, so `echo "a # b"` prints the
hash. A line that is only a comment is skipped.

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
in place of a program image. `saveflash` copies it back (default
`/script.sh`). `uninstall` erases it. `runflash` on a script says to
use `source @flash`. With `autostart on`, the next boot runs the script
when `/autorun.bin` is absent.

## Commands that were Blue Pill only

These lived behind `FREYA_APP_FLASH_ADDR`, which only the Blue Pill defined.
They are implemented on both boards now (Black Pill: 128-byte slot at the
end of sector 3, 64 KiB program in sector 4).  Acceptance: type any of
them at `freya:/>` on either module.

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
