# Freya

A small bare metal operating system for the STM32F411CEU6 "Black Pill" and the
STM32F103C8T6 "Blue Pill", written from scratch in C and ARM assembly. No HAL,
no CMSIS, no third party libraries: Freya brings the chip up itself, talks to
the hardware through its own register definitions, and lives entirely in
internal flash.

Freya gives you a serial console, a real FAT filesystem on an SD card, and the
ability to download a program over the console, load it into RAM and run it —
then stop it again with Ctrl-C.

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
[boot] console    : USART2 115200 8N1 on PA2/PA3
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
| Program region | 56 KiB | 8 KiB |
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
[boot] console    : USART2 115200 8N1 on PA2/PA3
```

## What it does

* Boots from internal flash and configures the whole clock tree itself. On the
  Black Pill that is a 25 MHz crystal → PLL → 96 MHz SYSCLK, 48 MHz APB1,
  96 MHz APB2, 3 flash wait states, voltage scale 1, prefetch and caches on; on
  the Blue Pill an 8 MHz crystal → PLL ×9 → 72 MHz SYSCLK, 36 MHz APB1, 72 MHz
  APB2, 2 flash wait states. Both fall back to the internal oscillator if the
  crystal does not start, and the SysTick reload, the console divisor and the
  microsecond delay all follow whatever the clock tree actually came up at.
* Console shell on USART2 at 115200 8N1, interrupt driven, with line editing and
  command history.
* SD / SDHC cards over SPI, and a FAT16 / FAT32 implementation that reads *and*
  writes: files, directories, long file names, MBR partitions.
* Receives files over the console with XMODEM / XMODEM-1K.
* Loads a program from the card into a RAM region and executes it as machine
  code, with a service table for console, memory, timing and file access.
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
other: the result is `build/<board>/freya.bin` (about 30 KiB either way) plus
`build/<board>/freya.hex`, and the example programs in `build/<board>/apps/`.
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

Then open the console:

```sh
picocom -b 115200 /dev/ttyUSB0      # or minicom, screen, putty ...
```

## Commands

| Command | What it does |
|---|---|
| `help [command]` | list commands, or describe one |
| `sysinfo` | CPU, unique id, clocks, reset cause, uptime, card, filesystem |
| `meminfo` | flash and RAM usage: .data, .bss, heap, program region, stack |
| `mount` | initialise the card and mount the filesystem |
| `ls [-l] [path]` | list a directory |
| `ll [path]` | list with sizes, dates and attributes |
| `cd [path]`, `pwd` | move around |
| `mkdir <dir>...` | create directories |
| `rm [-r] <path>...` | remove files, empty directories, or whole trees |
| `download <file> [--raw]` | receive a file over XMODEM |
| `cat <file>` | print a file |
| `write <file> <text...>` | append a line to a file |
| `hexdump <file> [off] [len]` | dump a file in hex |
| `df` | capacity, free and used space |
| `load <file>` | load a program image into RAM |
| `run [file] [args...]` | run the loaded program |
| `stop` | stop, or unload, the program |
| `date [YYYY-MM-DD HH:MM:SS]` | show or set the clock used for file timestamps |
| `uptime`, `led`, `echo`, `clear`, `reboot` | the usual small change |

Ctrl-C stops a running program, Ctrl-U clears the input line, and the up and
down cursor keys walk the command history.

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
sx -k build/apps/hello.bin < /dev/ttyUSB0 > /dev/ttyUSB0   # lrzsz
python3 tools/send.py /dev/ttyUSB0 build/apps/hello.bin    # no lrzsz needed
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
minimal starting point.

```
freya:/> run hello.bin
--- hello starting (Ctrl-C stops it) ---
hello from a program running in Freya's RAM region
  api version 1, table size 108 bytes
...
--- hello stopped by Ctrl-C, exit code 0, 4193 ms ---
```

`run <file>` loads and runs in one step, `load` then `run` separates the two,
and any extra words on the line arrive as `argv`. Only one program exists at a
time — Freya does not multitask.

The service table (`include/freya_api.h`) gives a program console I/O and
`printf`, `malloc`/`free`, milliseconds and delays, the LED, and the filesystem:
`open`, `read`, `write`, `seek`, `close`, `unlink`, `mkdir`, `opendir`,
`readdir`, `closedir`.

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

--- spin killed by bus fault, exit code 0, 3 ms ---
```

A fault in the kernel itself is a different matter: that prints a register dump
and halts.

### Autorun

If `/autorun.bin` exists it is started automatically at boot, with two seconds
to press a key and cancel.

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

Blue Pill — the same shape, squeezed into a fifth of the RAM. The stack keeps
6 KiB because the kernel's deepest path (an XMODEM download writing through the
filesystem) needs a little over three, and the heap takes whatever `.bss`
leaves behind:

```
0x08000000  +--------------------------------+
            |  Freya kernel (~30 KiB used)   |  64 KiB internal flash
0x08010000  +--------------------------------+

0x20000000  +--------------------------------+
            |  .data + .bss (~4 KiB)         |
            |  system heap (~2 KiB)          |
0x20001800  +--------------------------------+
            |  user program region (8 KiB)   |  image + .bss, loaded from card
0x20003800  +--------------------------------+
            |  main stack (6 KiB)            |  kernel and program share it
0x20005000  +--------------------------------+
```

`meminfo` reports all of it at runtime, including the heap's largest free block
and the stack high-water mark (the reset handler paints the stack, so the peak
is measured rather than guessed).

## Source layout

| Path | Contents |
|---|---|
| `boards/<board>/` | one directory per board: register header, `startup.s`, `board.c` (clock tree, pin mux, LED), linker scripts, compiler flags |
| `src/system.c` | SysTick, reset cause, delays, software clock |
| `src/uart.c` | USART2 console, interrupt driven receive |
| `src/spi.c`, `src/sd.c` | SPI1 and the SD / SDHC card protocol |
| `src/fat.c` | FAT16 / FAT32, including long file names and writing |
| `src/fs.c` | paths, working directory, descriptor table |
| `src/xmodem.c` | the `download` receiver |
| `src/loader.c` | program loading, the service table, start and stop |
| `src/fault.c` | fault containment and the kernel panic dump |
| `src/shell.c` | line editing and the commands |
| `src/heap.c`, `src/print.c`, `src/string.c` | allocator, formatting, freestanding libc |
| `apps/`, `include/freya_api.h` | example programs and the program ABI |
| `samples/` | small standalone samples, `blink` to start from |
| `tests/` | host side tests |
| `tools/send.py` | XMODEM sender for hosts without lrzsz |

## Tests

`make test` compiles the filesystem and XMODEM sources **unchanged** for the
host, points them at disk image files instead of a card, and checks the result
with the system's own FAT tools.

The FAT tests run against freshly formatted FAT16 and FAT32 images: directory
creation, small and multi-cluster files, read-back verification, seeking,
appending, long names and their generated 8.3 aliases, forty files in one
directory, deletion, and a check that every allocated cluster is returned.
`fsck.vfat` then confirms the images are consistent. An interoperability pass
copies a 40 KB file that `mcopy` wrote, using only Freya calls, and verifies the
copy is byte identical when read back with mtools.

The XMODEM tests drive `src/xmodem.c` with an emulated sender that answers the
receiver's own handshake: CRC mode and checksum fallback, 128 and 1024 byte
packets, a packet corrupted in transit and retransmitted, a duplicated packet,
line noise before the first packet, and the padding of the final block.

```
68 checks, 0 failures     FAT16
68 checks, 0 failures     FAT32
11 checks, 0 failures     interoperability
18 checks, 0 failures     XMODEM
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
  flash: a program gets 8 KiB rather than 56, and the heap is a couple of KiB
  instead of sixty. The kernel is the same size on both, and still leaves half
  the F103's flash unused.
* `stop` typed at the prompt unloads the image and reports how the last run
  ended; to interrupt a program that is actually running, press Ctrl-C.
