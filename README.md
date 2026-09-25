# Freya

Freya is a 32-bit, single-user, text OS for STMicroelectronics
STM32 small MCUs, written from scratch in C and ARM assembly. It runs bare
metal on the STM32F411CEU6 "Black Pill" and the STM32F103C8T6 "Blue Pill".
No HAL and no CMSIS: Freya brings the chip up itself. LittleFS, on the SPI flash, is the one vendored library. Freya
talks to the hardware through its own register definitions, and lives
entirely in internal flash. This is release 2.0.1, "Reptiloid". The notes
are in [RELEASE_NOTES.md](RELEASE_NOTES.md).

Freya gives you a serial console, a real FAT filesystem on an SD card, and the
ability to download a program over the console, load it into RAM and run it —
then stop it again with Ctrl-C. The Black Pill can also mount a SPI NOR chip
soldered on its SOP-8 footprint at `/spi1`, formatted as LittleFS, with the same file calls. Both boards also keep one program in a reserved
area of their own flash and run it from there, so the program survives a power
cycle and needs no card at all.

```
  ______
 |  ____|
 | |__ _ __ ___ _   _  __ _
 |  __| '__/ _ \ | | |/ _` |
 | |  | | |  __/ |_| | (_| |
 |_|  |_|  \___|\__, |\__,_|
                 __/ |
                |___/
Freya 2.0.1 "Reptiloid" for STM32F411CEU6
96 MHz, power-on reset. Type 'help()'.

[boot] clocks     : HSE 25 MHz crystal + PLL, sysclk 96 MHz, flash 3 WS
[boot] console    : USART2 921600 8N1 on PA2/PA3
[boot] SD card    : SD v2 (SDHC/SDXC), 14.8 GiB (31116288 blocks)
[boot] filesystem : FAT32 "FREYA", cluster 16.0 KiB, mounted on /

freya:
```

## Boards

|  | Black Pill | Blue Pill |
|---|---|---|
| MCU | STM32F411CEU6 | STM32F103C8T6 |
| Core | Cortex-M4F at 96 MHz | Cortex-M3 at 72 MHz |
| Crystal | 25 MHz | 8 MHz |
| Flash | 512 KiB | 128 KiB |
| SRAM | 128 KiB | 20 KiB |
| Program region | 56 KiB RAM, or 64 KiB flash | 8 KiB RAM, or 33664 B flash |
| Build | `make` | `make BOARD=bluepill` |

Everything a board needs lives in `boards/<board>`: its register header, its
startup code and vector table, its bring-up (`board.c`: clock tree, pin mux,
LED), its two linker scripts and its compiler flags. Everything under `src/` is
the same code on both, and a third board is a third directory rather than a
fork.

The Blue Pill boots the same way, on three quarters of the clock and a fifth of
the RAM:

```
Freya 2.0.1 "Reptiloid" for STM32F103C8T6
72 MHz, power-on reset. Type 'help'.

[boot] clocks     : HSE 8 MHz crystal + PLL, sysclk 72 MHz, flash 2 WS
[boot] console    : USART2 921600 8N1 on PA2/PA3
```

## What it does

* Boots from internal flash and configures the whole clock tree itself. On the
  Black Pill that is a 25 MHz crystal → PLL → 96 MHz SYSCLK, 48 MHz APB1,
  96 MHz APB2, 3 flash wait states, voltage scale 1, prefetch and caches on; on
  the Blue Pill an 8 MHz crystal → PLL ×9 → 72 MHz SYSCLK, 36 MHz APB1, 72 MHz
  APB2, 2 flash wait states. Both fall back to the internal oscillator if the
  crystal does not start, and the SysTick reload, the console divisor and the
  microsecond delay all follow whatever the clock tree actually came up at.
* Console shell on USART2 at 921600 8N1, interrupt driven, with line editing and
  command history.
* SD / SDHC cards over SPI, and a FAT16 / FAT32 implementation that reads *and*
  writes: files, directories, long file names, MBR partitions. The socket's
  supply is a power domain: `power sd off` drops VDD, `power sd on` brings
  it back, and a program does the same with `api->power(FREYA_PWR_SD, on)`.
* Receives files over the console with XMODEM / XMODEM-1K.
* Logs dated messages from programs and the kernel to `/freya.log` on the card,
  keeping one previous file when the log reaches 1 MiB.
* Loads a program from the card into a RAM region and executes it as machine
  code, with a service table for console, memory, timing and file access.
* Gives a program the board's spare pins, an interrupt on any edge of one of
  them, and three hardware timers that interrupt it from ten microseconds to
  forty seconds apart — with a handler that faults, or never returns, killed
  the same way a program is ([docs/interrupts.md](docs/interrupts.md)).
* Drives eight of those pins as PWM from the same timers, 1 Hz to 1 MHz with
  the duty cycle in ten-thousandths or as a pulse width for a servo, from a
  program or straight from the console with `pwm PB6 1000 25`.
* Speaks I2C as a master, 7-bit, from 10 kHz up to 400 kHz, including the
  repeated start a register read needs. The clock is that rate or a little
  slower when a microsecond cannot land on it. Bus 1 is PB6/PB7 on both
  boards; `i2c 1 scan` at the console and `samples/i2c` do the same thing
  ([docs/i2c.md](docs/i2c.md)).
* Speaks 1-Wire at standard speed on any spare pin: presence, byte reads
  and writes, and the ROM search. The data line needs a pull-up to 3.3 V.
  `w1 PB12 search` at the console lists the ROMs, and `samples/w1` reads
  a DS18B20 ([docs/w1.md](docs/w1.md)).
* Speaks SPI as a master, 8-bit, modes 0 to 3. The card keeps SPI1. A
  program gets SPI2, SCK/MISO/MOSI on PB13/PB14/PB15 on both boards, and
  drives chip select itself. The clock is the fastest power-of-two
  division of the bus clock that does not exceed the rate asked for,
  from 187.5 kHz to 24 MHz. `spi 1 1000000` at the console and
  `samples/spi` do the same thing ([docs/spi.md](docs/spi.md)).
* Encrypts and decrypts with XTEA in CTR mode. The key is 16 bytes and the
  nonce is 8; the same call does both, and a message longer than 4096 bytes
  is handed over in pieces. `crypt` at the console and `samples/crypt` do
  the same thing ([docs/crypt.md](docs/crypt.md)).
* Runs a PDP-11 whose eight registers and whose words are 32 bits. The
  opcodes and the condition codes are the PDP-11's. A program steps it
  with `api->vm_step()` or runs a stretch of instructions with
  `api->vm_run()`. `samples/vm` adds two numbers and returns
  ([docs/vm.md](docs/vm.md)).
* Keeps one program in a reserved area of its own internal flash and executes
  it in place from there. On the Blue Pill that raises the ceiling on program
  size from 8 KiB to 33664 bytes; on the Black Pill the flash region is 64 KiB
  (sector 4) and is there so the same console commands work with no card in
  the socket. The program can be copied from the card, or packed into the
  module when Freya itself is flashed.
* Runs threads inside a program. A thread has a name and a numeric priority;
  the highest priority that is ready runs, and equal priorities take turns.
  `threads` lists them. `stop blink` stops that thread; `stop` with no name
  still stops the whole program, threads included.
  [docs/threads.md](docs/threads.md) is the call list.
* Stops a running program at any time — even one stuck in a tight loop — and
  contains a program that crashes instead of taking the system down with it.
* Reports how every run ended: the program's own exit code, 130 for Ctrl-C or
  128 plus the fault that killed it, readable at the prompt as `$?` or with
  `status`, written to the log, and available to the next program that runs.

## Hardware

The console is wired the same way on both boards.

| Function | Pin | Connect to |
|---|---|---|
| Console TX | PA2 | RX of a 3.3 V USB-serial adapter |
| Console RX | PA3 | TX of the adapter |
| Status LED | PC13 | on board, active low |

The card is SPI1 on PA4 to PA7 on both boards. Those pins sit in different
places on the two headers; the slot drawings are in
[docs/sd-slot.txt](docs/sd-slot.txt). VDD is switched: PA8 drives the gate
of a P-channel MOSFET, low to power the socket. A pull-down on that gate
keeps the card on through reset. On the Black Pill the SOP-8 footprint on
the back shares that bus. A SPI NOR chip fitted there, and no card in the
socket, is mounted at `/spi1` as LittleFS. A blank chip is formatted on the first mount.

Freya keeps seven pins: PA2 and PA3 for the console, PA4 to PA7 for the
card, and PA8 for its supply.
Every other pin of ports A, B and C is a program's
to drive or take interrupts on, and eight of them — PA0, PA1, PB0, PB1 and
PB6 to PB9 — have a timer channel behind them and can be driven as PWM.

I2C uses two more of those pins, and one pair that is not. Bus 1 is PB6
(SCL) and PB7 (SDA) on both boards. Bus 2 is PB10/PB11 on the Blue Pill and
PB10/PB9 on the Black Pill, which has no PB11. Both lines are open drain and
need a pull-up to 3.3 V; 4.7 kΩ is the usual value. The Black Pill also turns
on the pin's own weak pull-up; the Blue Pill cannot, so the resistors are
required there. A pin that is already a PWM output is not also an I2C pin
until that channel is turned off.

1-Wire uses one spare pin, open drain, with a pull-up to 3.3 V. 4.7 kΩ is
the usual value. The Black Pill also turns on the pin's own weak pull-up;
the Blue Pill cannot, so the resistor is required there. A pin that is
already a PWM output or an I2C line is not also a 1-Wire pin until that is
turned off. Up to four pins may be open at once.

SPI for a program is SPI2, the same three pins on both boards: SCK on
PB13, MISO on PB14 and MOSI on PB15. The card keeps SPI1, so a program's
bus is not the socket. Chip select is any other spare pin, driven with
the pin calls; `pin PB12 0`, then `spi 1 x 9F FF FF FF`, then `pin PB12 1`
is one transfer.
MISO is pulled up. The clock is the fastest of eight taps that does not
go faster than the rate asked for. A pin that is already PWM, I2C or
1-Wire is not also an SPI pin until that is turned off.

The console runs at 921600 baud, the fastest rate every common adapter agrees
on: a CP2101, a CP2102 and an FT232 all list it, where 1 Mbaud is already the
CP2102's ceiling and past a CP2101 entirely. Neither board divides it exactly —
USARTDIV rounds to 52 against the Black Pill's 48 MHz APB1 and to 39 against
the Blue Pill's 36 MHz, both landing on 923077 baud, 0.16% fast and far inside
what 8N1 tolerates. A Blue Pill whose crystal did not start runs its APB1 at
32 MHz instead and comes out 0.8% slow, which is still comfortable. The USART
would reach 3 Mbaud on the Black Pill and 2.25 on the Blue Pill, so if the
adapter is a faster one, the rate is `uart_init()` in `src/main.c` and the
`BOARD_CONSOLE_NAME` string. Nothing drives RTS or CTS, so leave hardware flow
control off on the host, and ground the adapter with the board.

Card identification runs inside the 100–400 kHz window the spec demands
(375 kHz on the Black Pill, 281 kHz on the Blue Pill) and the bus then
switches to 12 MHz, or 9 MHz on the Blue Pill.

## Building

Needs an `arm-none-eabi` GCC (the one in the STM32CubeCLT works, so does any
distribution package) and `make`.

```sh
make                   # Black Pill kernel image + example programs
make BOARD=bluepill    # the same for the Blue Pill
make size              # section sizes
make test              # run the filesystem and XMODEM code on the host
make clean
```

Each board builds into its own directory, so the two never overwrite each
other: the result is `build/<board>/freya.bin` (around 42.5 KiB on the Black
Pill once the flash programmer is in, 42 on the Blue Pill) plus
`build/<board>/freya.hex`, and the example programs in `build/<board>/apps/` —
each one built both as a `.bin` to load into RAM and as a `.xip.bin` to
install into flash.
`BOARD=` applies to every target below as well.

Flashing, whichever tool you have:

```sh
make flash          # st-flash --reset write build/<board>/freya.bin 0x08000000
make openocd        # ST-Link via OpenOCD, with the board's target script
make bootloader     # the chip's own ROM loader
```

`make bootloader` is USB DFU on the Black Pill (hold BOOT0, tap NRST). The F103
has no USB loader, so on the Blue Pill it drives the serial loader in ROM with
`stm32flash`: pull BOOT0 high, tap NRST, and add `PORT=/dev/ttyUSB1` if the
adapter is not on `ttyUSB0`.

On the Blue Pill the size register often still reads 64 KiB. `make flash` and
`make openocd` tell the programmer 128 KiB, which is the size every one of
these boards has, so an image that uses the top half is written in full.

On either board, `PROGRAM` packs one program into the image that those
targets write, so the module comes up with it already in the program flash
region. It is an app name, a sample name, or the path of a `.xip.bin`:

```sh
make flash PROGRAM=hello
make BOARD=bluepill flash PROGRAM=hello
make BOARD=bluepill flash PROGRAM=blink
make BOARD=bluepill flash PROGRAM=path/to/mine.xip.bin
make flash PROGRAM=hello AUTOSTART=1
make flash SCRIPT=boot.sh
make flash SCRIPT=boot.sh AUTOSTART=1
```

The file written is `build/<board>/freya+hello.bin` (the tag follows the
program or the script name; `AUTOSTART=1` adds `+autostart`). `make image
PROGRAM=hello` builds that file without programming the chip. A kernel-only
`make flash` still leaves whatever is already in the region alone. `runflash`
starts a packed program afterwards. `SCRIPT=` stores a shell script in that
same region, in the form `install` writes, so `source @flash` runs it.
`AUTOSTART=1` writes the auto-start flag into the packed image so the next
reset runs the program or the script; the default is off, so packing does
not autorun on every reset unless you asked. `/autorun.bin` on the card
still overrides either one.

A first script blinks a pin until Ctrl-C. Save it as `blink.sh`:

```
# Blink PB2 until Ctrl-C.

loop bool(1)
    pin("PB2", "toggle")
    sleep(500)
end
```

`loop bool(1)` repeats while that condition is true, and tests it again
before every pass. `pin("PB2", "toggle")` flips the pin; `sleep(500)` waits
half a second. Pack it so the board runs it at boot:

```sh
make BOARD=bluepill flash SCRIPT=blink.sh AUTOSTART=1
```

Then open the console:

```sh
picocom -b 921600 /dev/ttyUSB0      # or minicom, screen, putty ...
```

## Commands

The full command list, and the six that used to exist only on the Blue Pill,
are in [docs/console-commands.md](docs/console-commands.md).

| Command | What it does |
|---|---|
| `help [command]` | list commands, or describe one |
| `sysinfo` | CPU, unique id, clocks, reset cause, uptime, log level, auto-start and ram-dump flags, card, filesystem |
| `meminfo` | flash and RAM usage: .data, .bss, heap, program region, stack |
| `mount` | initialise the card and mount the filesystem |
| `power [sd [on\|off]]` | show the socket supply, or switch it |
| `ls [-l] [path]` | list a directory; `-l` adds sizes, dates and attributes |
| `cd [path]`, `pwd` | move around |
| `mkdir <dir>...` | create directories |
| `rm [-r] <path>...` | remove files, empty directories, or whole trees |
| `rename <old> <new>` | rename or move a file or directory (no data copy) |
| `download <file> [--raw] [--size <n>]` | receive a file over XMODEM; `--size` keeps that many bytes |
| `upload <file>` | send a file over XMODEM |
| `cat <file>` | print a file |
| `write <file> <text...>` | append a line to a file |
| `hexdump <file> [off] [len]` | dump a file in hex |
| `flashdump [file]` | write internal flash to a file on the card (default `/freya.flash`) |
| `df` | capacity, free and used space |
| `load <file>` | load a program image into RAM |
| `run [file] [args...]` | run the loaded program |
| `runflash [args...]` | run the program stored in internal flash |
| `stop` | stop, or unload, the program |
| `status` | exit status of the last command and the last program (also `$?`) |
| `install <file>` | write a program, or a shell script, into internal flash |
| `saveflash [file]` | copy the installed program or script from flash onto the card |
| `uninstall` | erase the program flash region |
| `autostart [on\|off]` | run the flash program or script automatically at boot |
| `ramdump [on\|off]` | write SRAM to `/freya.ram` after a BusFault (default off) |
| `date [YYYY-MM-DD HH:MM:SS]` | show or set the clock used for file timestamps |
| `loglevel [level]` | show or set the file log level (`off`/`error`/`warn`/`info`/`debug`, or `0`..`4`) |
| `crypt [<key> <nonce> <hex>]` | XTEA-CTR: the same call encrypts and decrypts |
| `pin <pin> [mode] [0\|1\|toggle]` | read or drive one pin: `pin PB5 out 1`, `pin PB0 up` |
| `pwm [<pin> <hz> <duty%>]` | list the PWM channels, or start one: `pwm PB6 1000 25`, `pwm PB6 off` |
| `sleep <ms>` | wait that many milliseconds; Ctrl-C returns early |
| `source <file>\|@flash` | run a shell script from a file, or from program flash |
| `set`, `unset`, `$name` | integer, float, string, array and dict variables |
| `fn`, `return` | a function of 0..32 arguments and 1..32 values |
| `if` / `else` / `end`, `loop <count\|condition>` | run commands when a status is 0, or repeat them |
| `uptime`, `led`, `echo`, `clear`, `reboot` | the usual small change |

Ctrl-C stops a running program, Ctrl-U clears the input line, and the up and
down cursor keys walk the command history.

`flashdump` copies the chip's mapped internal flash (from `0x08000000`, using
the size the MCU reports) onto the card as a raw image. It overwrites
`/freya.flash` unless you name another file. Ctrl-C stops the write and
leaves whatever was written. `saveflash` copies only the installed program
image (not the kernel) to `/<name>.xip.bin`, or to a path you give.

`ls` prints names only; `ls -l` adds sizes and timestamps:

```
freya: ls -l
/:
  d---a      <DIR>  2026-09-21 20:14  apps
  -w--a       2048  2026-09-21 20:31  notes.txt
  -w--a      13284  2026-09-21 20:33  hello.bin
  2 files, 1 directory, 14.9 KiB total
```

## Getting files onto the card

The card is ordinary FAT, so a card reader works. To transfer over the console
instead, start the receiver on Freya and then send from the host:

```
freya: download hello.bin
Ready to receive 'hello.bin' over XMODEM.
```

```sh
stty -F /dev/ttyUSB0 921600 raw -echo -crtscts             # sx uses the line as it finds it
sx -k build/apps/hello.bin < /dev/ttyUSB0 > /dev/ttyUSB0   # lrzsz
python3 tools/send.py /dev/ttyUSB0 build/apps/hello.bin    # no lrzsz needed, sets the rate itself
```

`tools/fremote.py` is the other way round: it opens the console itself and
drives the shell, in the same shape as MicroPython's `mpremote`. A path
with a leading `:` is on the card.

```sh
python3 tools/fremote.py                          # shell; Ctrl-X leaves it
python3 tools/fremote.py u0 fs ls :/
python3 tools/fremote.py fs cp build/apps/hello.bin :/hello.bin
python3 tools/fremote.py fs cp :/notes.txt .
python3 tools/fremote.py exec "led blink" + fs df
```

`fs cp` and `fs cat` use `download --size` and `upload`, so the copy is the
same bytes as the file, including a trailing 0x1A. `fs ls`, `fs rm`,
`fs mkdir`, `fs df` and `fs cd` are the shell commands of the
same name. `fs mv` is the shell's `rename`. `u0` is `/dev/ttyUSB0`, `a0` is `/dev/ttyACM0` and `c3` is
`COM3`. With no port, the only USB serial device is used.

minicom, Tera Term and ExtraPuTTY can send XMODEM from their menus. Because
XMODEM has no length field, the sender pads the last packet; Freya strips that
padding, and `--raw` keeps it if you ever need the padded stream verbatim.

## Programs

A program is a raw binary linked to run from Freya's program region, starting
with a small header that the loader checks. `apps/hello` and `apps/spin` are
complete examples, and `make` builds them into `build/apps/`.

```c
#include "freya_api.h"

int app_main(const freya_api_t *api, int argc, char **argv)
{
    api->printf("hello, %d arguments\r\n", argc);
    return 0;
}
```

Build your own by dropping a directory under `apps/` and adding its name to
`APPS` in the Makefile; it is linked with `apps/common/app_start.c`, which
supplies the header, and the board's `app.ld`. A program is built for one board
— the load address is part of the header and the loader refuses an image linked
for somewhere else. `samples/` works the same way through
the `SAMPLES` variable and builds into `build/samples/`; `samples/blink` is a
minimal starting point, `samples/log` writes one line at each log level,
`samples/irq` blinks from a timer interrupt and counts button presses from a
pin one (`samples/irq/README.md`), `samples/pwm` fades an LED and sweeps a
servo (`samples/pwm/README.md`), `samples/i2c` scans a bus, `samples/spi`
loops SPI back to itself (`samples/spi/README.md`), `samples/w1`
reads a 1-Wire thermometer (`samples/w1/README.md`), `samples/crypt`
checks XTEA-CTR and encrypts a file (`samples/crypt/README.md`),
`samples/flashprobe`
finds out how much
internal flash the chip really has (`samples/flashprobe/README.md`),
`samples/tetris` is a console game (keys
in `samples/tetris/README.md`), `samples/edit` is a terminal text editor
(`samples/edit/README.md`), `samples/forth` is an interactive Forth
with a compiler and 122 words (`samples/forth/README.md`), and
`samples/altair` is an Altair 8800b Turnkey that runs Altair BASIC and
other original 8080 software from the card, or from an XMODEM upload
in its menu (`samples/altair/README.md`;
Black Pill only, from flash). `samples/altair16` is that machine with
16 KiB of RAM. On the Black Pill it fits the program region and runs
from RAM as well; on the Blue Pill, whose SRAM is 20 KiB, that RAM is
kept in program flash (`samples/altair16/README.md`). Every
app and sample is also built as `.xip.bin` for `install`, and a sample too
large for a board's program RAM region is built there as the flash image
alone — which on the Blue Pill is what happens to `forth`, whose
interpreter is 8 KiB on its own.

Single-precision `float` compiles for both boards. The Black Pill uses its
FPU. The Blue Pill has none, so the program is linked with `src/softfp.c`,
the add, subtract, multiply, divide, compare and integer-conversion helpers
the compiler emits. They round to nearest, ties to even, and they keep
subnormals. A program that never uses `float` does not carry that code.

```
freya: run hello.bin
--- hello starting (Ctrl-C stops it) ---
hello from a program running in Freya's program RAM region
  api version 3, table size 316 bytes
  code at 0x20001840, data at 0x20001c7c
  initialised data survived the load: .data ok, .bss clear
...
--- hello stopped by Ctrl-C, exit status 130, 4193 ms ---
```

`run <file>` loads and runs in one step, `load` then `run` separates the two,
and any extra words on the line arrive as `argv`. Only one program exists at a
time — Freya does not multitask.

The service table (`include/freya_api.h`) gives a program console I/O and
`printf`, `malloc`/`free`, milliseconds and delays, the LED, the filesystem:
`open`, `read`, `write`, `seek`, `close`, `unlink`, `mkdir`, `rename`,
`opendir`, `readdir`, `closedir`, the exit status of the run before it:
`exit`, `last_exit`, `exit_reason_str`, the pins, the timers, PWM and the
interrupts, XTEA in CTR mode (`crypt`), a raw console (`console_raw`,
which hands Ctrl-C to the program as an ordinary key, as an emulator needs;
Freya takes it back when the run ends), and a file log: `log`, `get_log_level`,
`set_log_level`. Log lines are `YYYY-MM-DD HH:MM:SS LEVEL message` in
`/freya.log` at the root of the card. The file is capped at 1 MiB; when it
fills, it is renamed to `/freya.log.old` (replacing any previous copy) and a
new `/freya.log` is started. With no card mounted the same line goes to the
console instead, and the SD driver is not touched. The default level is
`info` (3). That number is stored in the second word of the auto-start flash
slot, so it survives a reset; `loglevel` writes it, and toggling `autostart`
or `ramdump` leaves it alone.

### Pins, timers, PWM and interrupts

A program can drive any pin the kernel does not keep, take an interrupt on
its edges, and have one of the three general purpose timers interrupt it
every so many microseconds:

```c
api->pin_mode(FREYA_PB(0), FREYA_PIN_IN_PULLUP);
api->pin_irq_attach(FREYA_PB(0), FREYA_EDGE_FALLING | FREYA_EDGE_DEBOUNCE,
                    on_press, &presses);

int t = api->timer_open(250000, 0, on_tick, (void *)api);   /* 250 ms */
api->timer_start(t);
```

The same timers drive eight of those pins as PWM — PA0, PA1, PB0, PB1 and
PB6 to PB9, the same on both boards — where the hardware does the toggling
and the program only says what the duty cycle should be:

```c
int led = api->pwm_open(FREYA_PB(6), 1000, FREYA_PWM_FULL / 4);  /* 25% */
api->pwm_duty(led, FREYA_PWM_FULL / 2);

int servo = api->pwm_open(FREYA_PA(0), 50, 0);
api->pwm_pulse_us(servo, 1500);          /* a servo wants a pulse width */
```

Three timers serve both, so one driving pins is not one `timer_open()` can
hand out, and the channels of a single timer share its frequency. `pin` and
`pwm` at the console are the same calls with a prompt in front of them, which
is the quickest way to find out whether something is wired up right before
writing a program at all.

The handler is the program's own code running in interrupt context, so it may
print, drive pins and read the clock, but not allocate or touch the card —
the kernel refuses those rather than let a program corrupt the heap or the
filesystem from underneath itself. A handler that faults is reported as a
program fault, a handler that never returns still answers Ctrl-C, and every
line and timer is given back when the run ends, however it ended. A program
that would rather not have a handler at all attaches none and sleeps in
`api->irq_wait()` instead, where it may do anything.

`samples/irq` and `samples/pwm` are the worked examples, and
[docs/interrupts.md](docs/interrupts.md) is the reference: the pin numbering,
the sixteen shared interrupt lines, the period and frequency ranges, which
pins have a PWM channel behind them and what a handler may call.

### Stopping a program

Three things end a run, and all three return control to the shell cleanly:
the program returns from `app_main`, it calls `api->exit()`, or it is stopped.

Stopping works even if the program never cooperates. The console interrupt
flags the request and pends PendSV; PendSV runs at the lowest priority, so by
the time it executes the program's own exception frame is on top of the stack,
and rewriting the stacked PC makes the program resume inside an abort
trampoline that unwinds into the shell. A polite program can also poll
`api->should_stop()` or call `api->yield()`.

The kill is held off while an SD transfer is in flight, so a program can never
be stopped half way through a block write and leave the card inconsistent.
Memory the program allocated is reclaimed and its open files are closed
whichever way the run ended.

A program that crashes is contained the same way: the fault is reported with
the faulting address and the decoded fault status, and the shell comes back.

```
freya: run spin.bin fault
spin: about to touch 0xF0000000 ...

[freya] program fault at pc=0x2001004e lr=0x20010027
  cause : BusFault
  CFSR  : 0x00008200   HFSR: 0x00000000
  BFAR  : 0xf0000000 (bus address)
  detail: precise data bus error

[freya] ram dump skipped (disabled)

--- spin killed by bus fault, exit status 133, 3 ms ---
```

On either board a BusFault can write SRAM to `/freya.ram` at the volume root
(20 KiB on the Blue Pill, 128 KiB on the Black Pill). That is off by default: the third word of the auto-start slot is
the enable flag, erased flash means off, and `ramdump on` programs it. With the
flag on, a card present, and the filesystem mountable, the file is a raw image
from `0x20000000`, overwritten on each BusFault, and loads in GDB with
`restore freya.ram binary 0x20000000`. The dump runs in thread mode after the
fault is contained, because the SD driver times out against SysTick, which does
not preempt the BusFault handler. With the flag off, or with no card, the dump
is skipped and the shell still comes back. A kernel BusFault in thread mode
writes the same file (when enabled) and then halts.

A fault in the kernel itself is otherwise a different matter: that prints a
register dump and halts.

### Exit status

Every run ends with a status, and the rule is the one a POSIX shell uses. A
program that finished decides its own: whatever `app_main` returns, or the
argument to `api->exit()`, keeping the low byte — so `exit(-1)` reads back as
255. A run Freya ended itself reports `128 + the reason` instead, which puts
Ctrl-C at 130 because the stop reason for the console and `SIGINT` are both 2,
and the four fault statuses after it:

| Status | Means |
|---|---|
| 0 | the program succeeded (`FREYA_EXIT_OK`) |
| 1 .. 125 | it failed and said how: 1 is `FREYA_EXIT_FAIL`, 2 `FREYA_EXIT_USAGE` |
| 126 | the image was there and the loader refused it (`FREYA_EXIT_NOEXEC`) |
| 127 | there was nothing to run (`FREYA_EXIT_NOTFOUND`) |
| 130 | stopped with Ctrl-C (`FREYA_EXIT_STOPPED`) |
| 131 .. 134 | killed by a hard, memory, bus or usage fault |

Everything above 125 is the shell's convention or the kernel's, but nothing
stops a program returning those numbers itself, so the reason is kept beside
the status rather than deduced from it. `run` prints both on its closing line,
`status` prints them again later — even after the image has been unloaded or
replaced — and `$?` in a command line is the number on its own:

```
freya: run hello.bin 3
--- hello starting (Ctrl-C stops it) ---
...
exiting with status 3

--- hello exited, exit status 3, 12 ms ---
freya: echo $?
3
freya: status
  command    : 0
  program    : hello
  ended by   : exited
  exit status: 3
  run time   : 12 ms
  runs       : 1 since reset
```

`$?` is the shell's own status too: a command that failed is 1, a word that is
not a command is 127, and an empty line leaves it alone. Since `$?` is
expanded before the line is split, `write /runs.txt $?` records the status of
the last run on the card. `$name` expands a shell variable the same way.
Each run also writes its outcome to `/freya.log`,
at `info` when the status is 0 and at `warn` when it is not.

`;` separates commands on one line. `set n 1 + 2` stores an integer, a
byte, a bool, empty, none, a float, a string, an auto array, or a dict under a name, and `$n` expands it. `array(10, 20)` grows when `set a[2] 30` writes past the end, and every element stays one type. `min` and `max` pick an element, and `sort` returns the array in order. `dict("b", 2, "a", 1)` keeps its keys sorted. Four of those may exist at once, and `unset` frees them. `if $n > 1` is true
when the comparison is. `fn add` ... `return $1 + $2` ... `end` defines a
function; `add(2, 3)` in an expression passes up to 32 arguments.
`return` leaves from anywhere in the body with 1 to 32 values, and a
call used as one value yields the first. `int`, `float`, `byte`, `bool`, `str` and `hex` convert a
value between an integer, a byte (0 to 255, written `65b`), a bool
(`true` and `false`), a float, text and hexadecimal.
`empty` is the empty value and `none` is a different value with no number. `rand()` is
the ANSI C 1989 example generator, 0 to 32767, and `srand(seed)` sets
its state. `pi()` is the circle constant, and `sin(angle)` and
`cos(angle)` take an angle in radians. `now()` is the software clock as seconds since
1970, `date()` prints it as `YYYY-MM-DD HH:MM:SS`, and `year`, `month`,
`day`, `hour`, `minute` and `second` read one field. `time` builds the
seconds from those six fields. `get`, `set`,
`adc` and `pwm` are built in:
`get("PB0")` reads a pin, `set("PB5", 1)` drives it, `adc("PA0")` returns
one raw sample, and `pwm("PB6", 1000, 25)` starts a channel. `open`,
`read`, `write`, `close`, `seek` and `flush` are the file calls, in the
shape of Lua's `io` library. `match`, `find` and `gsub` search a string
with a Lua pattern. `ticks()` is
milliseconds since boot. `timer(1000000, 0, "ontick")` starts a hardware
timer and `irq("PB0", 2, "onpress")` arms a pin edge; `wait(0)` calls
the named function when one of them fires. `break` leaves a loop. `if <command>` still runs the following commands up
to `else` or `end` when that command's status is 0, and the `else` commands
otherwise. `loop <count>` repeats up to `end`, and `loop <condition>`
repeats while that condition is true (`loop true` does not stop on its
own). `sleep <ms>` waits that
many milliseconds. `spawn("blink", 1)` runs a function beside the
script until it sleeps or yields; two of those fit, and `join` waits
for one. They share the interpreter, so they are not the threads a
program starts. A block left open is finished
on the next lines (`>` is the prompt); Ctrl-C throws those lines away,
and also cuts a `sleep`, a `loop` or a `wait` short. `source <file>` runs a script
from the card, and `source @flash` runs one kept in the program flash
region. `install` of a text file stores that script there. The language
is written out in [docs/shell.md](docs/shell.md), and the commands in
[docs/console-commands.md](docs/console-commands.md).

A program reads the status of the run before it with `api->last_exit()`,
which fills in the name, the reason, the status and how long that run took;
`api->exit_reason_str()` names the reason. Both were appended to the service
table, so a program built against an older kernel keeps working and one
built against this ABI can check before calling:
`FREYA_API_HAS(api, last_exit)`.

### Running from flash

On the Blue Pill 8 KiB is all a 20 KiB SRAM can spare for a program, while
most of the 128 KiB of flash sits idle. So the board reserves 33664 bytes
at the top of flash — the rest of page 48 after a 128-byte auto-start slot,
then pages 49 to 80 — for one program image. The last 47 KiB holds the
kernel extension (the thread scheduler, the shell's script interpreter,
its variables and functions, XMODEM, the SPI master, the cipher and the
virtual machine), which is flashed as its own image. The size register on
these parts often still reads 64 KiB; the region runs through the 128 KiB
anyway. The Black Pill does not need
the size (it already has 56 KiB of program RAM) but it keeps the same
commands: a 128-byte slot at the end of sector 3, then the whole of sector
4 (64 KiB) for the image. `install` writes an
image there from the card, and `make flash PROGRAM=<app>` writes the same
kind of image into the module together with the kernel:

```
freya: install hello.xip.bin
install: console input is dropped while flash is busy
  erasing 2 pages ... writing ... ok
installed /hello.xip.bin at 0x0800c080: 1.2 KiB in 2 pages

freya: run @flash
--- hello starting (Ctrl-C stops it) ---
hello from a program running in Freya's program flash region
  api version 3, table size 316 bytes
  code at 0x0800c0c0, data at 0x20001800
  initialised data survived the load: .data ok, .bss clear
```

The installed image is named `@flash` rather than by a path, so `load`, `run`
and `stop` need no special case for it and none of them need a mounted card.
`runflash` is that same run with the path filled in: it starts whatever the
region holds, whether it was packed in at program time or installed from the
card.
`uninstall` erases the region. `saveflash` copies the installed image back
onto the card (the header's `image_size` bytes), defaulting to
`/<name>.xip.bin` from the program header; Ctrl-C stops the write the same
way as `flashdump`. `install` writes nothing if the region already holds
the same image — flash endurance is 10k cycles, and there is no reason to
spend one per `run`. `autostart on` writes a flag into the first word of the
128-byte slot immediately before the program region so the next boot runs that
program without waiting for `runflash`. The second word of the same slot is
the default log level; the third is the ram-dump-on-BusFault flag (`ramdump
on`, off in erased flash). `autostart off` erases the flag; the log level and
the ram-dump flag are written back. On the Blue Pill the rest of that 1 KiB
page is restored as well, so the start of the program image is kept. On the
Black Pill the slot is in the previous sector and the image is not touched.

Such a program is linked differently. A RAM image is one contiguous blob whose
`.data` is writable where it lands; a flash image is the ordinary split, with
`.text` and `.rodata` executing in place from flash and `.data` copied out of
flash into the RAM region before `app_main` is called. That is what the second
linker script (`boards/<board>/app_flash.ld`) describes, and `make` builds
every app and sample both ways from the same objects: `hello.bin` to `load`,
`hello.xip.bin` to `install`. A flash program on the Blue Pill therefore
spends the 8 KiB RAM window entirely on its variables, and gets 33664 bytes
for code instead of 8 KiB. On the Black Pill the RAM window is still 56 KiB and
the flash image may be up to 64 KiB.

Executing from flash needs nothing special — an address in `0x0800xxxx` is
fetchable exactly the way one in `0x2000xxxx` is. Writing to flash does: neither
the F103 nor the F411 has read-while-write, so the flash controller stalls
bus reads for as long as an erase or a program is in flight. The routines that
wait on it are linked for the base of the program RAM region and copied there
when an install begins, which costs nothing permanently because a program
cannot be loaded at that moment anyway. What that does not fix is the console:
the USART2 handler is itself in flash, so it stalls too, and console input
during an install is lost. Interrupts are masked per operation rather than
across the whole install so that the handler gets to drain the receive buffer
between operations, and the software clock loses roughly the time the install takes.

Ctrl-C cannot interrupt an install half way through an erase unit. Nothing outside
`boards/<board>/flash.c` can write to flash at all, one function there bounds
every address against the writable regions (the auto-start slot and the program
region), and the service table has no flash call in it: a program cannot
rewrite the kernel that is running it.

### Autorun

If `/autorun.bin` exists it is started automatically at boot, with two seconds
to press a key and cancel. Failing that, on a board that keeps a program in
flash, an installed image is started the same way when the auto-start flag is
on — `autostart on` after `install`, or `AUTOSTART=1` when the program is
packed into the module, so a board with nothing in the card socket still
boots Freya and runs a program. An installed shell script is started the
same way when that is what the region holds. `make flash SCRIPT=boot.sh
AUTOSTART=1` packs that script with the flag already on.

## Memory map

Black Pill:

```
0x08000000  +--------------------------------+
            |  Freya kernel (~47.8 KiB used) |  48 KiB, sectors 0..2
0x0800C000  +--------------------------------+
            |  unused                        |  rest of sector 3
0x0800FF80  +--------------------------------+
            |  auto-start flag + log level  |  128 B, end of sector 3
0x08010000  +--------------------------------+
            |  program flash region          |  64 KiB, sector 4
0x08020000  +--------------------------------+
            |  kernel extension              |  48 KiB, start of sector 5
0x08080000  +--------------------------------+

0x20000000  +--------------------------------+
            |  .data + .bss + system heap    |
0x2000F000  +--------------------------------+
            |  thread stacks, 4 x 1 KiB      |
0x20010000  +--------------------------------+
            |  user program region (56 KiB)  |  image + .bss, loaded from
0x2001E000  +--------------------------------+  card, or just .data + .bss
            |  shell stack (6 KiB)           |  the program's main thread
0x2001F800  +--------------------------------+
            |  interrupt stack (2 KiB)       |
0x20020000  +--------------------------------+
```

Blue Pill — the same shape, squeezed into a fifth of the RAM. The top 8 KiB
holds two thread stacks, the shell stack and the interrupt stack, and the
heap takes whatever `.bss` leaves behind:

```
0x08000000  +--------------------------------+
            |  Freya kernel (~47.5 KiB used) |  48 KiB, pages 0..47
0x0800C000  +--------------------------------+
            |  auto-start flag + log level  |  128 B, page 48
0x0800C080  +--------------------------------+
            |  program flash region          |  33664 B, rest of page 48
            |                                |  and pages 49..80, installed
0x08014400  +--------------------------------+  from the card
            |  kernel extension              |  47 KiB, pages 81..127
0x08020000  +--------------------------------+

0x20000000  +--------------------------------+
            |  .data + .bss + system heap    |
0x20001800  +--------------------------------+
            |  user program region (8 KiB)   |  image + .bss loaded from
0x20003800  +--------------------------------+  card, or just .data + .bss
            |  thread stacks, 2 x 1 KiB      |
0x20004000  +--------------------------------+
            |  shell stack (2560 B)          |  the program's main thread
0x20004A00  +--------------------------------+
            |  interrupt stack (1536 B)      |
0x20005000  +--------------------------------+
```

A flash-resident program uses the RAM window for its `.data` and `.bss` alone.
The kernel flash ceiling is a hard limit: each board's `freya.ld` fails the
link rather than let the kernel grow into the auto-start slot.

`meminfo` reports all of it at runtime, including the heap's largest free block
and the stack high-water mark (the reset handler paints the stack, so the peak
is measured rather than guessed).

## Source layout

| Path | Contents |
|---|---|
| `boards/<board>/` | one directory per board: register header, `startup.s`, `board.c` (clock tree, pin mux, LED), linker scripts, compiler flags |
| `boards/<board>/app_flash.ld` | the second program linker script: code in flash, data in RAM |
| `src/system.c` | SysTick, reset cause, delays, software clock |
| `src/uart.c` | USART2 console, interrupt driven receive |
| `src/spi.c`, `src/sd.c`, `src/spiflash.c` | SPI1 for the card and the Black Pill SPI flash, and SPI master for a program |
| `src/gpio.c` | pins a program may drive, and the sixteen EXTI interrupt lines |
| `src/timer.c` | the general purpose timers and their interrupts |
| `src/pwm.c` | the compare channels of those timers, driving pins |
| `src/i2c.c` | I2C master, on the buses the board header names |
| `src/w1.c` | 1-Wire master, standard speed, on a pin a program names |
| `src/crypt.c` | XTEA in CTR mode, for a program and for `crypt` |
| `src/fat.c` | FAT16 / FAT32, including long file names and writing |
| `src/lfsvol.c`, `third_party/littlefs/` | LittleFS on the Black Pill SPI flash (the default there) |
| `src/fs.c` | paths, working directory, descriptor table |
| `src/xmodem.c` | XMODEM receive (`download`) and send (`upload`) |
| `src/loader.c` | program loading and installing, the service table, start and stop |
| `boards/<board>/flash.c` | internal flash erase and program, bounded to the program region |
| `src/fault.c` | fault containment and the kernel panic dump |
| `src/ramdump.c` | SRAM dump to `/freya.ram` after a BusFault |
| `src/shell.c` | line editing and the commands |
| `src/log.c` | file log (`/freya.log`) and rotation |
| `src/heap.c`, `src/print.c`, `src/string.c` | allocator, formatting, freestanding libc |
| `apps/`, `include/freya_api.h` | example programs and the program ABI |
| `samples/` | small standalone samples: `blink`, `log`, `irq`, `pwm`, `i2c`, `spi`, `w1`, `crypt`, `flashprobe`, `tetris`, `edit`, `forth`, `altair`, `altair16` |
| `tests/` | host side tests |
| `docs/shell.md` | the shell language: values, expressions, control, variables, functions |
| `docs/console-commands.md` | full command list, and the six that were Blue Pill only |
| `docs/interrupts.md` | the pin, timer, PWM and interrupt API, and what a handler may do |
| `docs/i2c.md` | the I2C master API, the pins, and the `i2c` command |
| `docs/spi.md` | the SPI master API, the pins, and the `spi` command |
| `docs/w1.md` | the 1-Wire master API, the pin, and the `w1` command |
| `docs/crypt.md` | the XTEA-CTR API and the `crypt` command |
| `docs/sd-slot.txt` | SD slot wiring for the Blue Pill and the Black Pill |
| `tools/send.py` | XMODEM sender for hosts without lrzsz |
| `tools/fremote.py` | remote shell and SD card utility (`fs ls`, `fs cp`, …) |
| `tools/pack_image.py` | packs the kernel and one `.xip.bin` or shell script into the image `make flash PROGRAM=` / `SCRIPT=` writes |

## Tests

`make test` compiles the filesystem and XMODEM sources **unchanged** for the
host, points them at disk image files instead of a card, and checks the result
with the system's own FAT tools.

The FAT tests run against freshly formatted FAT16 and FAT32 images: directory
creation, small and multi-cluster files, read-back verification, seeking,
appending, long names and their generated 8.3 aliases, forty files in one
directory, deletion, a check that every allocated cluster is returned, and
rotation of `/freya.log` at 1 MiB via an atomic rename to `/freya.log.old`.
`fsck.vfat` then confirms the images are consistent. An interoperability pass
copies a 40 KB file that `mcopy` wrote, using only Freya calls, and verifies the
copy is byte identical when read back with mtools.

The XMODEM tests drive `src/xmodem.c` with an emulated sender that answers the
receiver's own handshake: CRC mode and checksum fallback, 128 and 1024 byte
packets, a packet corrupted in transit and retransmitted, a duplicated packet,
line noise before the first packet, and the padding of the final block.

The `forth` sample is a program rather than kernel code, but it is the one
sample with enough behaviour to be worth testing, so it is built for the
host too — unchanged, against a service table that captures what it prints
— and driven a line at a time: arithmetic and the number bases, every
control structure, defining words, string literals, recursion, a source
file read through `include`, and each way the interpreter can fail. The
same binary talks to a terminal with `-i`, which is the quickest way to
try the language without a board.

The `altair` sample is tested the same way. The 8080's flags, `DAA`,
timing, memory map, ports, loaders and tapes are always checked. The
CPU exercisers and Altair BASIC itself cannot be committed, so they run
only when `ALTAIR_TESTS`, `ALTAIR_BASIC` and `ALTAIR_MBL` point at them
(`samples/altair/README.md`). Ctrl-C with the console raw and not raw
is checked on `src/uart.c` itself, against a fake USART.

A run's exit status is decided in one place — `freya_exit_status()` in the ABI
header — so that the closing line of `run`, `$?`, the log line and a program
asking `last_exit()` can never disagree. That rule is a pure function of how
the run ended and what the program asked for, so it is checked on the host:
the truncation to a byte, the `128 + reason` statuses Freya synthesises for
Ctrl-C and the four faults, and the `FREYA_API_HAS` test a program uses on a
service table older than itself.

Neither a pin nor a timer exists on the host either, but the arithmetic behind
them does not need one, and it is the part that would be quietly wrong: a
period off by a factor of two looks like working code on the bench. So
`src/timer.c` and `src/pwm.c` are compiled unchanged and their dividers driven
over the whole range they accept — every period and every frequency at every
clock either board can run at, checked against what the prescaler and the
reload each chose will actually do. Periods come out inside 0.04% everywhere
and exact on the round numbers; frequencies inside 0.6%, which is the counts
running out at the top of the range rather than the arithmetic. The pin
encoding is checked beside them, and so is the board's table of PWM channels:
a hand written table whose two temptations are naming a pin the kernel keeps
and giving one timer channel to two pins. The I2C half-period gets the same
treatment: each half of the clock is a whole number of microseconds, rounded
up, so the bus is the rate that was asked for or a little slower and never
faster, and bus 1 of the pin table is PB6/PB7 on either board. The SPI
baud tap gets the same treatment: of the eight power-of-two divisions of
the bus clock, the one chosen is the fastest that does not exceed the
rate asked for, at 48, 36 and 32 MHz, and bus 1 is SCK/MISO/MOSI on
PB13/PB14/PB15 on either board. The 1-Wire
ROM search and its CRC-8 get the same treatment, against device ids planted
on the host: that walk is the part that would be quietly wrong.

XTEA is checked the same way. `src/crypt.c` is compiled unchanged and the
published block vector pins the 32 rounds and the big-endian words. CTR is
then checked against that block: a split message, a counter that carries,
and a piece that starts in the middle of a block.

The virtual machine is an instruction set, so what would be quietly wrong
about it is the addressing and the flags rather than the arithmetic.
`src/vm.c` is compiled unchanged and driven an instruction at a time: each
of the eight modes, against both the value it produces and the register it
stepped; the word a byte operand does not shorten on R6 and R7; the address
JMP and JSR take, which is neither; MOVB into a register, which is the one
byte instruction that reaches the whole of it; V and C on the shifts and
the subtractions; a dividend whose high word is negative and the two ways a
quotient can fail to exist; and every opcode the machine does not have,
each of which has to leave R7 where a caller can read it.

The flash programming itself cannot be reached from the host, which is the main
argument for keeping that driver small and its bounds check absolute. What can
be checked off the board is the part most likely to be quietly wrong: a last
pass builds a flash image and compares every field of the header
`apps/common/app_start.c` emits against the section addresses the linker
actually produced, and compares the regions declared in `include/freya_api.h`
against the ones the linker scripts describe. A linker script cannot include a
C header, so those two descriptions of the memory map are written down twice;
the kernel compares them at boot, and this compares them at build time.

```
132 checks, 0 failures     FAT16
132 checks, 0 failures     FAT32
11 checks, 0 failures     interoperability
18 checks, 0 failures     XMODEM
79 checks, 0 failures     forth
26 checks, 0 failures     exit status
168 checks, 0 failures    pins, timers, PWM, I2C, 1-Wire and SPI
42 checks, 0 failures     XTEA
136 checks, 0 failures    PDP-11 virtual machine
39 checks, 0 failures     program image layout
ALL TESTS PASSED
```

## Notes and limits

* FAT12 is not supported (cards that small are rare); FAT16 and FAT32 are.
* Long file names are read and written as ASCII; UTF-16 beyond ASCII becomes
  `?` on display.
* There is no battery backed clock on the board, so file timestamps come from a
  software clock that starts at 2026-01-01 and is set with `date`.
* One program at a time. Its main thread is the shell's stack; any thread
  it creates has a 1 KiB stack of its own. There is no MPU isolation.
* A pin interrupt is one of sixteen hardware lines, and line *n* serves pin
  *n* of one port at a time, so PA0 and PB0 cannot both have one. Three
  timers serve both the periodic interrupts and PWM, so a program wanting
  both has three between them, and the channels of one timer share its
  frequency. PWM reaches the eight pins that mean the same thing on both
  boards, not every pin either chip could route. ADC1 provides synchronous
  12-bit reads; input capture is not implemented. Handlers all run at one priority and never nest, and
  only the console sits above them — which is what makes Ctrl-C work against
  a handler that loops.
* Every supported board has at least 128 KiB of flash. The Blue Pill size
  register often still reads 64; `sysinfo`, `meminfo` and `flashdump` use
  128 KiB anyway, and `make flash` / `make openocd` tell the programmer the
  same. `samples/flashprobe` programs and reads back a block in each erase
  unit above that floor when the question is whether one chip has still more,
  and erases each unit again afterwards. The kernel's region is a build-time
  constant, not whatever the probe found.
* On the Blue Pill the 20 KiB of SRAM is the real limit, not the 128 KiB of
  flash: a RAM program gets 8 KiB rather than 56, and the heap is a couple of
  KiB instead of sixty. Installing a program into flash is the answer to the
  first half of that, not the second — such a program gets 33664 bytes of code, but
  the heap is still small and the main thread still uses the shell stack.
* The Black Pill keeps a program in flash for the same console commands, not
  because 56 KiB of program RAM is too small. Its erase unit at the program
  region is a 64 KiB sector; the auto-start slot sits in the previous 16 KiB
  sector so toggling the flag does not erase the image. The kernel image is
  limited to the first 48 KiB so it never shares a sector with that slot.
  The thread scheduler is a second image at the start of the next free
  sector, past the program, so flashing the kernel does not erase either.
* One program in flash at a time, as with RAM. `install` erases and rewrites
  the region; `uninstall` erases it. `make flash` writes only the kernel and
  leaves an installed program alone, which is convenient but does mean a stale
  image can outlive the kernel that installed it — the loader checks the
  header rather than trusting it. `make flash PROGRAM=<app>` is the other
  choice: the image it writes covers the whole program region, so the program
  packed in replaces whatever was there.
* Console input is lost while flash is being erased or programmed, and the
  software clock loses about the duration of the install. Both follow from
  there being no read-while-write, and neither is worth putting the console
  interrupt handler in RAM to avoid.
* `stop` with no name interrupts a program that is running, and at the
  prompt unloads the image and reports how the last run ended. `stop` with
  a thread name stops that thread. Ctrl-C stops the whole run.
