# altair

An Altair 8800b Turnkey: an Intel 8080, 48 KiB of RAM (up to 62), and
the Turnkey Module with its 1 KiB of SRAM, its PROM sockets and its
serial port. Original 8080 software runs on it unchanged, Altair BASIC
first of all. The Freya console is its teletype.

The emulator contains no MITS software. BASIC, the loader PROMs and any
programs are files on the SD card, in whatever form they were archived:
a memory image, an Intel HEX file or a paper tape.

```
freya:/> install altair.xip.bin
freya:/> runflash ext41.tap
--- altair starting (Ctrl-C stops it) ---
altair: 8800b Turnkey, 48 KiB RAM at 0000, Turnkey SRAM at F800, PROM at FC00
altair: ext41.tap: loader at 3F00, reading the rest of the tape
altair: full speed, starting at 3F00.  Ctrl-] for the menu

MEMORY SIZE?
LINEPRINTER? C
WANT SIN-COS-TAN-ATN? Y

34318 BYTES FREE
ALTAIR BASIC REV. 4.1
[EXTENDED VERSION]
COPYRIGHT 1977 BY MITS INC.
OK
PRINT 2+2
 4
OK
```

## Build and run

```sh
make                                      # build/blackpill/samples/altair.xip.bin
make flash PROGRAM=altair                 # packed into the module
make flash PROGRAM=altair AUTOSTART=1     # and started on every reset
```

The emulator needs 48 KiB of RAM for the 8080 alone, so it runs from
flash: the Makefile builds only the `.xip.bin`, and the program's RAM
window holds nothing but the Altair's memory. It is for the Black Pill
only. The Blue Pill's 8 KiB window could not hold even 4K BASIC and its
workspace, so `make BOARD=bluepill` skips this sample.

`samples/altair16` is the same machine with 16 KiB of RAM and no more.
That fits the Black Pill's program region beside the interpreter, so it
is built as `altair16.bin` (loaded with `run`) as well as a flash image.
See `samples/altair16/README.md`.

```
freya:/> install altair.xip.bin     once, into program flash
freya:/> runflash                   the files in /altair/
freya:/> runflash ext41.tap         a BASIC tape, read and run
freya:/> runflash xbasic.bin        a memory image at 0000h, run there
freya:/> runflash prog.hex --go 100 Intel HEX, started at 0100h
```

With no image named, the emulator looks in `/altair/`:

| File | What it is | Where it goes |
|---|---|---|
| `xbasic.bin` | a memory image, normally BASIC after a first boot | 0000h |
| `xbasic.tap` | a BASIC tape, read when there is no `xbasic.bin` | through its loader |
| `turmon.bin` | the Turnkey monitor PROM | FD00h |
| `mbl.bin` | the multi-boot loader PROM | FE00h |

The PROMs are fitted whenever they are there, whatever else is loaded.
The machine starts at 0000h, or at FD00h when TURMON is fitted, which is
what the Turnkey's auto-start does. From TURMON, `J 0000` starts BASIC.

### Arguments

| Argument | Meaning |
|---|---|
| `IMAGE` | `.bin` is loaded at `--at`, `.hex` where its records say, and `.tap` is read as a BASIC tape |
| `--at ADDR` | where a `.bin` goes (default 0000) |
| `--go ADDR` | where the CPU starts |
| `--ram KB` | RAM from 0000h, 1 to 62 KiB (default 48) |
| `--sw HEX` | the sense switches, A15-A8 |
| `--mhz N` | throttle the 8080 to N MHz; 2 is a real Altair, 0 (the default) is as fast as it goes |
| `--prom FILE[@ADDR]` | fill a PROM socket (default FD00h), up to four times |
| `--tape FILE` | put a tape on the reader, for a loader PROM to read |
| `--boot TYPE` | which BASIC a `.tap` holds: `4k32`, `4k40`, `8k`, `ext` (the default) or `disk` |
| `--pio WHAT` | what the 88-PIO drives: `tape` (the default), `term` or `off` |

Addresses and switches are hexadecimal; a trailing `h` is allowed.

## Memory and ports

| Address | What |
|---|---|
| 0000-BFFF | RAM, 48 KiB, in the program's RAM window |
| C000-F7FF | more RAM with `--ram` up to 62, taken from Freya's heap |
| F800-FBFF | Turnkey Module SRAM, 1 KiB |
| FC00-FFFF | Turnkey PROM sockets: FD00 TURMON, FE00 MBL, FF00 DBL |

Unfitted memory reads FFh, and writes to it and to PROM are lost, as on
the real bus. RAM is in 1 KiB pages, so `--ram` does not have to be a
power of two.

| Port | What |
|---|---|
| 00h / 01h | 88-SIO: status (active low) and data, the terminal |
| 04h / 05h | 88-PIO: control/status and data, the tape reader or the terminal (`pio`) |
| 06h / 07h | 88-ACR cassette: status and data, the tape reader |
| 10h / 11h | the Turnkey's serial port, a 6850 wired like 88-2SIO port A: the terminal |
| 12h / 13h | 88-2SIO port B: the tape reader |
| FFh | the sense switches |

Any other port reads FFh and ignores writes. Nothing is programmed on
the serial side: a 6850 reset or a baud rate setting is accepted and
changes nothing.

### The 88-PIO

The 88-PIO is MITS's parallel board: an input latch and an output
latch, each with a handshake flip-flop. Its status, read from port 04h,
is active high. Bit 0 is set when the output device can take a byte, and
bit 1 when the input device has strobed one in. Reading port 05h takes
the byte and clears bit 1. A write to 04h sets the board's two interrupt
enables, which have nothing to raise here.

The board was usually a parallel paper-tape reader, such as the OP-80,
or a parallel terminal. `--pio` and the menu's `pio` choose what it is
wired to:

| Setting | Input | Output |
|---|---|---|
| `tape` (the default) | the tape on the reader | dropped |
| `term` | the keyboard | the console |
| `off` | no board: both ports read FFh | ignored |

The keyboard is shared with the serial ports, so with `term` a key goes
to whichever port the program polls. Extended BASIC 4.1 runs its
terminal on the PIO with the high sense switches at 5 (`--sw 50 --pio term`).
MBL and MBLe read a tape through it with the low switches at 5.

## Getting BASIC in

### From a tape

MITS paper tapes are archived as the bytes that went through the reader.
A tape starts with a leader, then the checksum loader, which the
front-panel bootstrap read into the top of memory. After that come the
load records BASIC is made of, and finally a go record. The Turnkey has
no front panel to key the bootstrap in with, so the emulator does its
job itself. It stores the checksum loader where that version of BASIC
wants it, then starts the loader, which reads the rest of the tape from
the terminal port just as it read the teletype's paper-tape reader.

The loader's address depends on the version, which is what `--boot`
and the menu's `boot FILE TYPE` choose:

| Type | BASIC | Loader address |
|---|---|---|
| `4k32` | 4K BASIC 3.2 | 0FAEh |
| `4k40` | 4K BASIC 4.0 | 0FC2h |
| `8k` | 8K BASIC 3.2 to 4.1 | 1FC2h |
| `ext` | Extended BASIC 4.0 and 4.1 | 3FC2h |
| `disk` | Disk BASIC | 7EC2h |

Keys typed while the tape is still going through are held back and
reach BASIC once the tape is finished.

### Through a loader PROM

The Turnkey's own way works too. With MBL or MBLe in the socket at FE00h
(`--prom mble.hex` places a HEX file at the addresses in its records),
and the tape on the reader, start the machine at the loader:

```
freya:/> runflash --prom mble.hex --tape ext41.tap --sw 06 --go FE00
```

The low three switches choose the port the loader reads: 6 or 7 for
2SIO port B, 5 for the 88-PIO, 3 for the ACR, 2 for the SIO, 0 or 1 for
2SIO port A. BASIC reads the high switches for its terminal, where 0 is
the 2SIO and 5 the 88-PIO.

### Answering BASIC

Extended BASIC asks three questions before it is ready:

* `MEMORY SIZE?` — Enter uses all of it.
* `LINEPRINTER?` — `C`, for a Centronics printer. No printer is
  emulated, and nothing is printed to it unless a program asks, but
  BASIC does not start without an answer it knows.
* `WANT SIN-COS-TAN-ATN?` — `Y` keeps the trigonometry.

8K and 4K BASIC ask for the memory size, the terminal width and which
functions to keep.

### Keeping the result

Reading a tape takes a few seconds at full speed. BASIC can be saved
once it is in memory, and from then on the emulator loads the image
instead. Save it before answering the questions: at `MEMORY SIZE?`,
press Ctrl-] and

```
altair> save /altair/xbasic.bin 0 4000
```

The saved image is started at 0000h and asks the questions again. For
4K and 8K BASIC the lengths are 1000h and 2000h.

## The menu

Ctrl-] stops the 8080 and opens the menu, which stands in for the front
panel the 8800b Turnkey does not have. Ctrl-C belongs to the 8080: it is
BASIC's break key, and the emulator asks Freya for a raw console so that
Ctrl-C reaches it rather than ending the program. The shell's `(Ctrl-C
stops it)` is printed before the emulator asks. On a kernel without
`console_raw`, Ctrl-C ends the emulator as it ends any program, and
the startup line says so.

| Command | What it does |
|---|---|
| `c` | continue (also Enter, or Ctrl-] again) |
| `regs` | show the 8080 registers |
| `x ADDR [LEN]` | examine memory |
| `d ADDR BYTE...` | deposit bytes |
| `go ADDR` | jump there and continue |
| `reset` | reset to the start address and continue |
| `load FILE [ADDR]` | load an image (default 0000) or an Intel HEX file |
| `save FILE ADDR LEN` | save memory as an image |
| `prom FILE [ADDR]` | fill a PROM socket (default FD00) |
| `boot FILE [TYPE]` | read a BASIC tape and run it |
| `tape [FILE]` | attach a tape to the reader, or detach it |
| `pio [off\|tape\|term]` | show or set what the 88-PIO drives |
| `sw [HEX]` | show or set the sense switches |
| `speed [MHZ]` | show or set the clock, 0 for as fast as it goes |
| `upper on\|off` | fold typed lowercase to uppercase (default on) |
| `bs on\|off` | send Backspace as DEL, BASIC's rubout (default on) |
| `quit` | leave the emulator |

A program that halts also opens the menu, with the address of its `HLT`.

## How it works

`i8080.c` is the processor: an interpreter over the 256 opcodes,
including the undocumented duplicates. It keeps all five flags, the
auxiliary carry included, so `DAA` is right, and counts clock states the
way the real chip takes them, so `--mhz` gives real timing. It yields to
Freya every 20000 states.

`mem.c` maps the 64 KiB in 1 KiB pages. Each page has a read pointer and
a write pointer, so RAM, PROM, the Turnkey SRAM and empty space cost the
same single lookup. `io.c` is the ports, `term.c` the console side and
`load.c` the card side. The Makefile builds a sample from `main.c`
alone, so `main.c` includes the rest.

## Testing it without a board

`make test` builds the emulator for the host against a service table
that captures what it prints. It runs the 8080's flags, `DAA`, rotates,
stack and timing, the memory map, every port, the loaders and the tape
through it. Three environment variables add tests that use files which
cannot be committed:

```sh
ALTAIR_TESTS=dir   make test   # TST8080, 8080PRE, CPUTEST and 8080EXM .COM under a CP/M stub
ALTAIR_BASIC=file  make test   # boot BASIC (.bin or .tap) and run PRINT, FOR and Ctrl-C;
                               # a tape is also run with BASIC's terminal on the 88-PIO
ALTAIR_MBL=file    make test   # with a tape in ALTAIR_BASIC: load it through MBL at FE00,
                               # from 2SIO port B and from the 88-PIO
```

8080EXM checks every instruction against a real 8080's CRCs and runs
for 23.8 billion states, which takes minutes even on a PC. The same test binary is
also the emulator on a terminal:

```sh
build/tests/hostaltair -i ext41.tap
```

## Limits

* No interrupts: `EI` and `DI` are kept but nothing raises one, so
  software that needs the real-time clock or interrupt-driven I/O
  hangs.
* No disk, no printer and no tape punch. BASIC's `CSAVE` and `CLOAD`
  have nothing to talk to.
* The serial port runs at the console's speed, whatever baud rate the
  software sets.
* Black Pill only.
