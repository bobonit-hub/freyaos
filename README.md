# Freya

A small bare metal operating system for the STM32F411CEU6 "Black Pill" and the
STM32F103C8T6 "Blue Pill", written from scratch in C and ARM assembly. No HAL,
no CMSIS, no third party libraries: Freya brings the chip up itself, talks to
the hardware through its own register definitions, and lives entirely in
internal flash.

Freya gives you a serial console, a real FAT filesystem on an SD card, and the
ability to download a program over the console, load it into RAM and run it —
then stop it again with Ctrl-C. On the Blue Pill it will also write a program
into a reserved area of its own flash and run it from there, so the program
survives a power cycle and needs no card at all.

```
  ______
 |  ____|
 | |__ _ __ ___ _   _  __ _
 |  __| '__/ _ \ | | |/ _` |
 | |  | | |  __/ |_| | (_| |
 |_|  |_|  \___|\__, |\__,_|
                 __/ |
                |___/
Freya 1.0 for STM32F411CEU6
96 MHz, power-on reset. Type 'help'.

[boot] clocks     : HSE 25 MHz crystal + PLL, sysclk 96 MHz, flash 3 WS
[boot] console    : USART2 921600 8N1 on PA2/PA3
[boot] SD card    : SD v2 (SDHC/SDXC), 14.8 GiB (31116288 blocks)
[boot] filesystem : FAT32 "FREYA", cluster 16.0 KiB, mounted on /

freya:/>
```

## Boards

|  | Black Pill | Blue Pill |
|---|---|---|
| MCU | STM32F411CEU6 | STM32F103C8T6 |
| Core | Cortex-M4F at 96 MHz | Cortex-M3 at 72 MHz |
| Crystal | 25 MHz | 8 MHz |
| Flash | 512 KiB | 64 KiB |
| SRAM | 128 KiB | 20 KiB |
| Program region | 56 KiB RAM | 8 KiB RAM, or 25472 B flash |
| Build | `make` | `make BOARD=bluepill` |

Everything a board needs lives in `boards/<board>`: its register header, its
startup code and vector table, its bring-up (`board.c`: clock tree, pin mux,
LED), its two linker scripts and its compiler flags. Everything under `src/` is
the same code on both, and a third board is a third directory rather than a
fork.

The Blue Pill boots the same way, on three quarters of the clock and a fifth of
the RAM:

```
Freya 1.0 for STM32F103C8T6
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
  writes: files, directories, long file names, MBR partitions.
* Receives files over the console with XMODEM / XMODEM-1K.
* Logs dated messages from programs and the kernel to `/freya.log` on the card,
  keeping one previous file when the log reaches 1 MiB.
* Loads a program from the card into a RAM region and executes it as machine
  code, with a service table for console, memory, timing and file access.
* On the Blue Pill, also keeps one program in a reserved area of its own
  internal flash and executes it in place from there, which raises the
  ceiling on program size from 8 KiB to 25472 bytes and leaves a program that
  starts at boot with no card in the socket. The program can be copied from
  the card, or packed into the module when Freya itself is flashed.
* Stops a running program at any time — even one stuck in a tight loop — and
  contains a program that crashes instead of taking the system down with it.

## Hardware

The wiring is the same on both boards — the console and the card sit on pins
that exist, and mean the same thing, on the F103 and the F411 alike.

| Function | Pin | Connect to |
|---|---|---|
| Console TX | PA2 | RX of a 3.3 V USB-serial adapter |
| Console RX | PA3 | TX of the adapter |
| SD clock | PA5 | CLK / SCK |
| SD data out | PA6 | DO / MISO |
| SD data in | PA7 | DI / MOSI |
| SD chip select | PA4 | CS |
| Power | 3V3, GND | the card's 3.3 V and ground |
| Status LED | PC13 | on board, active low |

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
control off on the host.

SD cards are 3.3 V devices, so no level shifting is needed. Card identification
runs inside the 100–400 kHz window the spec demands (375 kHz on the Black Pill,
281 kHz on the Blue Pill) and the bus then switches to 12 MHz, or 9 MHz on the
Blue Pill. Ground the adapter and the board together.

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
other: the result is `build/<board>/freya.bin` (30 KiB on the Black Pill, 34 on
the Blue Pill, which also carries the flash programming code) plus
`build/<board>/freya.hex`, and the example programs in `build/<board>/apps/` —
each one built both as a `.bin` to load into RAM and, on the Blue Pill, as a
`.xip.bin` to install into flash.
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

On the Blue Pill, `PROGRAM` packs one program into the image that those
targets write, so the module comes up with it already in the program flash
region. It is an app name, a sample name, or the path of a `.xip.bin`:

```sh
make BOARD=bluepill flash PROGRAM=hello
make BOARD=bluepill flash PROGRAM=blink
make BOARD=bluepill flash PROGRAM=path/to/mine.xip.bin
make BOARD=bluepill flash PROGRAM=hello AUTOSTART=1
```

The file written is `build/bluepill/freya+hello.bin` (the tag follows the
program; `AUTOSTART=1` adds `+autostart`). `make image PROGRAM=hello` builds
that file without programming the chip. A kernel-only `make flash` still
leaves whatever is already in the region alone. `runflash` starts the
program afterwards. `AUTOSTART=1` writes the auto-start flag into the packed
image so the next reset runs it; the default is off, so packing a program
does not autorun on every reset unless you asked.

Then open the console:

```sh
picocom -b 921600 /dev/ttyUSB0      # or minicom, screen, putty ...
```

## Commands

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
| `load <file>` | load a program image into RAM |
| `run [file] [args...]` | run the loaded program |
| `runflash [args...]` | run the program stored in internal flash (Blue Pill) |
| `stop` | stop, or unload, the program |
| `install <file>` | write a program into internal flash (Blue Pill) |
| `saveflash [file]` | copy the installed program from flash onto the card (Blue Pill; default `/<name>.xip.bin`) |
| `uninstall` | erase the program flash region (Blue Pill) |
| `autostart [on\|off]` | run the flash program automatically at boot (Blue Pill) |
| `ramdump [on\|off]` | write SRAM to `/freya.ram` after a BusFault (Blue Pill; default off) |
| `date [YYYY-MM-DD HH:MM:SS]` | show or set the clock used for file timestamps |
| `loglevel [level]` | show or set the file log level (`off`/`error`/`warn`/`info`/`debug`, or `0`..`4`) |
| `uptime`, `led`, `echo`, `clear`, `reboot` | the usual small change |

Ctrl-C stops a running program, Ctrl-U clears the input line, and the up and
down cursor keys walk the command history.

`flashdump` copies the chip's mapped internal flash (from `0x08000000`, using
the size the MCU reports) onto the card as a raw image. It overwrites
`/freya.flash` unless you name another file. Ctrl-C stops the write and
leaves whatever was written. On the Blue Pill, `saveflash` copies only the
installed program image (not the kernel) to `/<name>.xip.bin`, or to a
path you give.

`ls` prints names only; `ll` (or `ls -l`) adds sizes and timestamps:

```
freya:/> ll
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
freya:/> download hello.bin
Ready to receive 'hello.bin' over XMODEM.
```

```sh
stty -F /dev/ttyUSB0 921600 raw -echo -crtscts             # sx uses the line as it finds it
sx -k build/apps/hello.bin < /dev/ttyUSB0 > /dev/ttyUSB0   # lrzsz
python3 tools/send.py /dev/ttyUSB0 build/apps/hello.bin    # no lrzsz needed, sets the rate itself
```

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
minimal starting point, `samples/log` writes one line at each log level, and
`samples/tetris` is a console game (keys in `samples/tetris/README.md`).

```
freya:/> run hello.bin
--- hello starting (Ctrl-C stops it) ---
hello from a program running in Freya's program RAM region
  api version 2, table size 132 bytes
  code at 0x20001840, data at 0x20001c7c
  initialised data survived the load: .data ok, .bss clear
...
--- hello stopped by Ctrl-C, exit code 0, 4193 ms ---
```

`run <file>` loads and runs in one step, `load` then `run` separates the two,
and any extra words on the line arrive as `argv`. Only one program exists at a
time — Freya does not multitask.

The service table (`include/freya_api.h`) gives a program console I/O and
`printf`, `malloc`/`free`, milliseconds and delays, the LED, the filesystem:
`open`, `read`, `write`, `seek`, `close`, `unlink`, `mkdir`, `rename`,
`opendir`, `readdir`, `closedir`, and a file log: `log`, `get_log_level`,
`set_log_level`. Log lines are `YYYY-MM-DD HH:MM:SS LEVEL message` in
`/freya.log` at the root of the card. The file is capped at 1 MiB; when it
fills, it is renamed to `/freya.log.old` (replacing any previous copy) and a
new `/freya.log` is started. With no card mounted the same line goes to the
console instead, and the SD driver is not touched. The default level is
`info` (3). On the Blue Pill
that number is stored in the second word of the auto-start flash slot, so it
survives a reset; `loglevel` writes it, and toggling `autostart` or `ramdump`
leaves it alone. On the Black Pill the level is RAM only.

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
freya:/> run spin.bin fault
spin: about to touch 0xF0000000 ...

[freya] program fault at pc=0x2001004e lr=0x20010027
  cause : BusFault
  CFSR  : 0x00008200   HFSR: 0x00000000
  BFAR  : 0xf0000000 (bus address)
  detail: precise data bus error

[freya] ram dump skipped (disabled)

--- spin killed by bus fault, exit code 0, 3 ms ---
```

On the Blue Pill a BusFault can write the 20 KiB of SRAM to `/freya.ram` at the
volume root. That is off by default: the third word of the auto-start slot is
the enable flag, erased flash means off, and `ramdump on` programs it. With the
flag on, a card present, and the filesystem mountable, the file is a raw image
from `0x20000000`, overwritten on each BusFault, and loads in GDB with
`restore freya.ram binary 0x20000000`. The dump runs in thread mode after the
fault is contained, because the SD driver times out against SysTick, which does
not preempt the BusFault handler. With the flag off, or with no card, the dump
is skipped and the shell still comes back. A kernel BusFault in thread mode
writes the same file (when enabled) and then halts.

A fault in the kernel itself is otherwise a different matter: that prints a
register dump and halts. The Black Pill does not write a ram dump.

### Running from flash

On the Blue Pill 8 KiB is all a 20 KiB SRAM can spare for a program, while
34 KiB of the 64 KiB of flash sits idle. So the board reserves 25472 bytes
at the top of flash — the rest of page 39 after a 128-byte auto-start slot,
then pages 40 to 63 — for one program image. `install` writes an
image there from the card, and `make flash PROGRAM=<app>` writes the same
kind of image into the module together with the kernel:

```
freya:/> install hello.xip.bin
install: console input is dropped while flash is busy
  erasing 2 pages ... writing ... ok
installed /hello.xip.bin at 0x08009c80: 1.2 KiB in 2 pages

freya:/> run @flash
--- hello starting (Ctrl-C stops it) ---
hello from a program running in Freya's program flash region
  api version 2, table size 132 bytes
  code at 0x08009cc0, data at 0x20001800
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
on`, off in erased flash). `autostart off` erases the flag (the log level, the
ram-dump flag and the rest of that 1 KiB page are restored, so the program
image is kept).

Such a program is linked differently. A RAM image is one contiguous blob whose
`.data` is writable where it lands; a flash image is the ordinary split, with
`.text` and `.rodata` executing in place from flash and `.data` copied out of
flash into the RAM region before `app_main` is called. That is what the second
linker script, `boards/bluepill/app_flash.ld`, describes, and `make` builds
every app and sample both ways from the same objects: `hello.bin` to `load`,
`hello.xip.bin` to `install`. A flash program therefore spends the 8 KiB RAM
window entirely on its variables, and gets 25472 bytes for code instead of 8.

Executing from flash needs nothing special — the Cortex-M3 is Harvard only in
its bus topology, over a single unified address map, so an address in
`0x0800xxxx` is fetchable exactly the way one in `0x2000xxxx` is. Writing to
flash does: the F103 has no read-while-write, so the flash controller stalls
bus reads for as long as an erase or a program is in flight. The routines that
wait on it are linked for the base of the program RAM region and copied there
when an install begins, which costs nothing permanently because a program
cannot be loaded at that moment anyway. What that does not fix is the console:
the USART2 handler is itself in flash, so it stalls too, and console input
during an install is lost. Interrupts are masked per operation rather than
across the whole install so that the handler gets to drain the receive buffer
between pages, and the software clock loses roughly the time the install takes.

Ctrl-C cannot interrupt an install half way through a page. Nothing outside
`boards/bluepill/flash.c` can write to flash at all, one function there bounds
every address against the writable pages (the auto-start slot and the program
region), and the service table has no flash call in it: a program cannot
rewrite the kernel that is running it.

### Autorun

If `/autorun.bin` exists it is started automatically at boot, with two seconds
to press a key and cancel. Failing that, on a board that keeps a program in
flash, an installed image is started the same way when the auto-start flag is
on — `autostart on` after `install`, or `AUTOSTART=1` when the program is
packed into the module, so a Blue Pill with nothing in the card socket still
boots Freya and runs a program.

## Memory map

Black Pill:

```
0x08000000  +--------------------------------+
            |  Freya kernel (~30 KiB used)   |  512 KiB internal flash
0x08080000  +--------------------------------+

0x20000000  +--------------------------------+
            |  .data + .bss (~4 KiB)         |
            |  system heap (~60 KiB)         |
0x20010000  +--------------------------------+
            |  user program region (56 KiB)  |  image + .bss, loaded from card
0x2001E000  +--------------------------------+
            |  main stack (8 KiB)            |  kernel and program share it
0x20020000  +--------------------------------+
```

Blue Pill — the same shape, squeezed into a fifth of the RAM, and with its
flash split in two so that a program can live there. The stack keeps 6 KiB
because the kernel's deepest path (an XMODEM download writing through the
filesystem) needs a little over three, and the heap takes whatever `.bss`
leaves behind:

```
0x08000000  +--------------------------------+
            |  Freya kernel (~33 KiB used)   |  39 KiB, pages 0..38
0x08009C00  +--------------------------------+
            |  auto-start flag + log level  |  128 B, page 39
0x08009C80  +--------------------------------+
            |  program flash region          |  25472 B, rest of page 39
            |                                |  and pages 40..63, installed
0x08010000  +--------------------------------+  from the card

0x20000000  +--------------------------------+
            |  .data + .bss (~4 KiB)         |
            |  system heap (~2 KiB)          |
0x20001800  +--------------------------------+
            |  user program region (8 KiB)   |  image + .bss loaded from
0x20003800  +--------------------------------+  card, or just .data + .bss
            |  main stack (6 KiB)            |  kernel and program share it
0x20005000  +--------------------------------+
```

A flash-resident program uses the RAM window for its `.data` and `.bss` alone,
so it gets 25472 bytes of code where a RAM image gets 8 KiB for everything. The
kernel's 39 KiB is a hard limit: `boards/bluepill/freya.ld` fails the link
rather than let the kernel grow into the auto-start slot.

`meminfo` reports all of it at runtime, including the heap's largest free block
and the stack high-water mark (the reset handler paints the stack, so the peak
is measured rather than guessed).

## Source layout

| Path | Contents |
|---|---|
| `boards/<board>/` | one directory per board: register header, `startup.s`, `board.c` (clock tree, pin mux, LED), linker scripts, compiler flags |
| `boards/bluepill/app_flash.ld` | the second program linker script: code in flash, data in RAM |
| `src/system.c` | SysTick, reset cause, delays, software clock |
| `src/uart.c` | USART2 console, interrupt driven receive |
| `src/spi.c`, `src/sd.c` | SPI1 and the SD / SDHC card protocol |
| `src/fat.c` | FAT16 / FAT32, including long file names and writing |
| `src/fs.c` | paths, working directory, descriptor table |
| `src/xmodem.c` | the `download` receiver |
| `src/loader.c` | program loading and installing, the service table, start and stop |
| `boards/bluepill/flash.c` | internal flash erase and program, bounded to the program region |
| `src/fault.c` | fault containment and the kernel panic dump |
| `src/ramdump.c` | Blue Pill SRAM dump to `/freya.ram` after a BusFault |
| `src/shell.c` | line editing and the commands |
| `src/log.c` | file log (`/freya.log`) and rotation |
| `src/heap.c`, `src/print.c`, `src/string.c` | allocator, formatting, freestanding libc |
| `apps/`, `include/freya_api.h` | example programs and the program ABI |
| `samples/` | small standalone samples: `blink`, `log`, `tetris` |
| `tests/` | host side tests |
| `tools/send.py` | XMODEM sender for hosts without lrzsz |
| `tools/pack_image.py` | packs the kernel and one `.xip.bin` into the image `make flash PROGRAM=` writes |

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
35 checks, 0 failures     program image layout
ALL TESTS PASSED
```

## Notes and limits

* FAT12 is not supported (cards that small are rare); FAT16 and FAT32 are.
* Long file names are read and written as ASCII; UTF-16 beyond ASCII becomes
  `?` on display.
* There is no battery backed clock on the board, so file timestamps come from a
  software clock that starts at 2026-01-01 and is set with `date`.
* One program at a time, sharing the main stack with the kernel — no
  multitasking and no MPU isolation, which is what a system this size should be.
* On the Blue Pill the 20 KiB of SRAM is the real limit, not the 64 KiB of
  flash: a RAM program gets 8 KiB rather than 56, and the heap is a couple of
  KiB instead of sixty. Installing a program into flash is the answer to the
  first half of that, not the second — such a program gets 25472 bytes of code, but
  the heap is still small and the stack is still shared.
* Only the Blue Pill keeps a program in flash. The Black Pill could, but its
  erase granularity past the kernel is a 64 KiB sector where the F103's is a
  1 KiB page, and nothing about a board with 56 KiB of program RAM needs it.
* One program in flash at a time, as with RAM. `install` erases and rewrites
  the region; `uninstall` erases it. `make flash` writes only the kernel and
  leaves an installed program alone, which is convenient but does mean a stale
  image can outlive the kernel that installed it — the loader checks the
  header rather than trusting it. `make flash PROGRAM=<app>` is the other
  choice: the image it writes covers the whole program region, so the program
  packed in replaces whatever was there.
* Console input is lost while flash is being erased or programmed, and the
  software clock loses about the duration of the install. Both follow from the
  F103 having no read-while-write, and neither is worth putting the console
  interrupt handler in RAM to avoid.
* `stop` typed at the prompt unloads the image and reports how the last run
  ended; to interrupt a program that is actually running, press Ctrl-C.
