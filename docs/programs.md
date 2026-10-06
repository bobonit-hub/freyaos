# Programs

How to write a program for Freya in C or Rust, what the service table gives
it, how it drives pins, timers, PWM and interrupts, how it stops, faults and
reports an exit status, how it runs from internal flash, and how it starts
at boot. The memory each board gives a program is in [boards.md](boards.md).

## Writing a program

Freya loads a program from the card into a RAM region and executes it as
machine code, with a service table for console, memory, timing and file
access.

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
reads a 1-Wire thermometer (`samples/w1/README.md`),
`samples/compress` packs and unpacks a file with heatshrink
(`samples/compress/README.md`),
`samples/cpubench` times the CPU with Dhrystone 2.1 and a
single-precision Whetstone, the tests the Linux shell runs as
`cpubench()` (`samples/cpubench/README.md`; every board but the Blue
Pill),
`samples/wget` fetches a URL with the HTTP client library
(`samples/wget/README.md`; every board but the Blue Pill),
`samples/jsonget` fetches JSON and prints it, or one value of it, with
cJSON (`samples/jsonget/README.md`; the STM32U585, STM32H523, STM32H562
and STM32H723),
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
kept in program flash (`samples/altair16/README.md`). `samples/basic11`
is the BASIC-11 style interpreter of `basic/` compiled natively, with
its numbers on the FPU, or on the Blue Pill in software and built
smaller, as a flash image only (`run basic11 [-m KiB] [program.bas |
-e text]`; [basic/README.md](../basic/README.md)). `samples/basic11vm`
runs the same interpreter compiled for the virtual machine, the card's
`/basic11.vm`, on the boards with a program window of 168 KiB or more
(`run basic11vm [-m KiB] [-i image] [program.bas | -e text]`). Every
app and sample is also built as `.xip.bin` for `install`, and a sample too
large for a board's program RAM region is built there as the flash image
alone — which on the Blue Pill is what happens to `forth`, whose
interpreter is 8 KiB on its own.

### Rust

A program can also be written in Rust. `rust/freya` holds the bindings to
the service table, and `samples/rustdemo` is a complete program built with
them. Cargo builds the program as a static library, and it is linked with
the same `app_start.c` and linker scripts as a C program, so it comes out as
the same `.bin` and `.xip.bin`. `make rust` builds the Rust samples, and
`make` includes them whenever `cargo` is installed. The bindings check their
own layout against `freya_api.h` when they build. See
[rust/README.md](../rust/README.md).

### BASIC and the virtual machine

`samples/basic11` is a BASIC-11 style interpreter with a flat, compacting
string pool, written in C and compiled with GCC into a 21 KiB native program
for the Cortex-M4F boards, whose numbers are the FPU's `float`, and into a
smaller one for the Blue Pill, with the same `float` in software, with `OLD`,
`SAVE` and the file statements on the card, `DATE$`, `TIME$`, `TIME` and
`SLEEP` on the clock, `PIN`, `PWM` and `ADC` on the pins, and `ON TIMER` and
`ON KEY` subroutines called every so many milliseconds or when a debounced
button is pressed ([basic/README.md](../basic/README.md)).

Freya also runs a PDP-11 whose eight registers and whose words are 32 bits
([vm.md](vm.md)). It has a C compiler for that machine, cproc and QBE with a
target in `qbe/`, which turns a C program into an image the VM runs from
address 0. The same BASIC is compiled that way too, into the 128 KiB
`basic11.vm`, with its floats computed in integers to the FPU's bits, and
`samples/basic11vm` runs it on the STM32U585, STM32H523, STM32H562 and
STM32H723: a program prints the same under either, only slower on the
machine.

### Floats

Single-precision `float` compiles for every board. The Cortex-M4F boards use
their FPU. The Blue Pill has none, so the program is linked with `src/softfp.c`,
the add, subtract, multiply, divide, compare and integer-conversion helpers
the compiler emits. They round to nearest, ties to even, and they keep
subnormals. A program that never uses `float` does not carry that code.

### Running it

```
freya: run("hello.bin")
--- hello starting (Ctrl-C stops it) ---
hello from a program running in Freya's program RAM region
  api version 3, table size 316 bytes
  code at 0x20001840, data at 0x20001c7c
  initialised data survived the load: .data ok, .bss clear
...
--- hello stopped by Ctrl-C, exit status 130, 4193 ms ---
```

`run("file")` loads and runs in one step, `load` then `run` separates the two,
and any extra words on the line arrive as `argv`. Only one program exists at a
time — Freya does not multitask.

### The service table

The service table (`include/freya_api.h`) gives a program console I/O and
`printf`, `malloc`/`free`, milliseconds and delays, the LED, the filesystem:
`open`, `read`, `write`, `seek`, `close`, `unlink`, `mkdir`, `rename`,
`opendir`, `readdir`, `closedir`, the exit status of the run before it:
`exit`, `last_exit`, `exit_reason_str`, the pins, the timers, PWM and the
interrupts, Ascon-AEAD128 (`aead_encrypt`,
`aead_decrypt`), heatshrink LZSS (`compress`,
`decompress`), the civil clock (`rtc_get`, `rtc_set`, which also
writes the DS3231 or the chip's RTC when the image has one; not on the Blue Pill, whose
kernel extension had no room, though its clock still keeps file
timestamps), flash written when the run ends (`flash_text_save`, a
text kept after the program's image in flash that the boot hands it as
`-e TEXT`, and `autostart_set`), audio on a USB headset (`audio_open`,
`audio_read`, `audio_write`, `audio_status`, `audio_gain`, `audio_close`;
[audio.md](audio.md)), a raw console (`console_raw`,
which hands Ctrl-C to the program as an ordinary key, as an emulator needs;
Freya takes it back when the run ends), and a file log: `log`, `get_log_level`,
`set_log_level`. A script writes the same line with `log(level, message)`.
Freya logs dated messages from programs, the shell and the kernel.
Log lines are `YYYY-MM-DD HH:MM:SS LEVEL message` in
`/freya.log` at the root of the card. The file is capped at 1 MiB; when it
fills, it is renamed to `/freya.log.old` (replacing any previous copy) and a
new `/freya.log` is started. With no card mounted the same line goes to the
console instead, and the SD driver is not touched. The default level is
`info` (3). That number is stored in the second word of the auto-start flash
slot, so it survives a reset; `loglevel` writes it, and toggling `autostart`
or `ramdump` leaves it alone.

## Pins, timers, PWM and interrupts

A program can drive any pin the kernel does not keep, take an interrupt on
its edges, and have one of the three general purpose timers interrupt it
every so many microseconds, from ten microseconds to forty seconds apart:

```c
api->pin_mode(FREYA_PB(0), FREYA_PIN_IN_PULLUP);
api->pin_irq_attach(FREYA_PB(0), FREYA_EDGE_FALLING | FREYA_EDGE_DEBOUNCE,
                    on_press, &presses);

int t = api->timer_open(250000, 0, on_tick, (void *)api);   /* 250 ms */
api->timer_start(t);
```

The same timers drive eight of those pins as PWM — PA0, PA1, PB0, PB1 and
PB6 to PB9, the same on every board — where the hardware does the toggling
and the program only says what the duty cycle should be:

```c
int led = api->pwm_open(FREYA_PB(6), 1000, FREYA_PWM_FULL / 4);  /* 25% */
api->pwm_duty(led, FREYA_PWM_FULL / 2);

int servo = api->pwm_open(FREYA_PA(0), 50, 0);
api->pwm_pulse_us(servo, 1500);          /* a servo wants a pulse width */
```

Frequencies run from 1 Hz to 1 MHz, with the duty cycle in ten-thousandths
or as a pulse width for a servo, from a program or straight from the console
with `pwm PB6 1000 25`.

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
[interrupts.md](interrupts.md) is the reference: the pin numbering,
the sixteen shared interrupt lines, the period and frequency ranges, which
pins have a PWM channel behind them and what a handler may call.

## Stopping a program

Three things end a run, and all three return control to the shell cleanly:
the program returns from `app_main`, it calls `api->exit()`, or it is stopped.

Stopping works even if the program never cooperates. The console interrupt
flags the request and pends PendSV; PendSV runs at the lowest priority, so by
the time it executes the program's own exception frame is on top of the stack,
and rewriting the stacked PC makes the program resume inside an abort
trampoline that unwinds into the shell. A polite program can also poll
`api->should_stop()` or call `api->yield()`.

A program can run threads. A thread has a name and a numeric priority;
the highest priority that is ready runs, and equal priorities take turns.
`threads` lists them. `stop blink` stops that thread; `stop` with no name
still stops the whole program, threads included, and at the prompt unloads
the image and reports how the last run ended. Ctrl-C stops the whole run.
[threads.md](threads.md) is the call list.

The kill is held off while an SD transfer is in flight, so a program can never
be stopped half way through a block write and leave the card inconsistent.
Memory the program allocated is reclaimed and its open files are closed
whichever way the run ended.

A program that crashes is contained the same way: the fault is reported with
the faulting address and the decoded fault status, and the shell comes back.

```
freya: run("spin.bin", "fault")
spin: about to touch 0xF0000000 ...

[freya] program fault at pc=0x2001004e lr=0x20010027
  cause : BusFault
  CFSR  : 0x00008200   HFSR: 0x00000000
  BFAR  : 0xf0000000 (bus address)
  detail: precise data bus error

[freya] ram dump skipped (disabled)

--- spin killed by bus fault, exit status 133, 3 ms ---
```

On every board a BusFault can write SRAM to `/freya.ram` at the volume root
(20 KiB on the Blue Pill, 128 KiB on the Black Pill; see [boards.md](boards.md)). That is off by default: the third word of the auto-start slot is
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

## Exit status

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
freya: run("hello.bin", 3)
--- hello starting (Ctrl-C stops it) ---
...
exiting with status 3

--- hello exited, exit status 3, 12 ms ---
freya: echo($?)
3
freya: status()
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

The shell language that uses `$?` — variables, expressions, functions,
blocks and loops — is written out in [shell.md](shell.md), and the commands
in [console-commands.md](console-commands.md).

A program reads the status of the run before it with `api->last_exit()`,
which fills in the name, the reason, the status and how long that run took;
`api->exit_reason_str()` names the reason. Both were appended to the service
table, so a program built against an older kernel keeps working and one
built against this ABI can check before calling:
`FREYA_API_HAS(api, last_exit)`.

## Running from flash

Freya keeps one program in a reserved area of its own internal flash and
executes it in place from there, so the program survives a power cycle and
the same console commands work with no card in the socket. The program can be
copied from the card, or packed into the module when Freya itself is flashed
([building.md](building.md#packing-a-program-or-a-script)). The design note
the feature was built from is [flash-programs.md](flash-programs.md).

On the Blue Pill 7 KiB is all a 20 KiB SRAM can spare for a program, while
most of the 128 KiB of flash sits idle. So the board reserves 24 KiB
from page 48 through page 71 for one program image. The next 55 KiB holds the
kernel extension (the thread scheduler, the shell's script interpreter,
its variables and functions, XMODEM and the SPI master), which is flashed
as its own image. The size register on
these parts often still reads 64 KiB; the region runs through the 128 KiB
anyway. The F4 boards put the same commands to a different use: their
region is every sector between the kernel and the kernel extension, 320 KiB
(sectors 4..6) on the Black Pill and 832 KiB (sectors 4..10) on the 1 MiB
STM32F405, so a program several times the size of the 56 KiB program RAM
runs from flash there; the system settings are in sector 3 in front of it.
`install` writes an
image there from the card, and `make flash PROGRAM=<app>` writes the same
kind of image into the module together with the kernel:

```
freya: install("hello.xip.bin")
install: console input is dropped while flash is busy
  erasing 2 pages ... writing ... ok
installed /hello.xip.bin at 0x0800c080: 1.2 KiB in 2 pages

freya: run("@flash")
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
on`, off in erased flash). Eight bytes at offset 16 are the terminal
password (`password`, off when those bytes are still erased). `autostart off`
erases the flag; the log level, the ram-dump flag and the password are
written back. On the Blue Pill the rest of that 1 KiB
page is restored as well, so the start of the program image is kept. On the
Black Pill the slot is in the previous sector and the image is not touched.

Such a program is linked differently. A RAM image is one contiguous blob whose
`.data` is writable where it lands; a flash image is the ordinary split, with
`.text` and `.rodata` executing in place from flash and `.data` copied out of
flash into the RAM region before `app_main` is called. That is what the second
linker script (`boards/<board>/app_flash.ld`) describes, and `make` builds
every app and sample both ways from the same objects: `hello.bin` to `load`,
`hello.xip.bin` to `install`. A flash program on the Blue Pill therefore
spends the 7 KiB RAM window entirely on its variables, and gets 24 KiB
for code instead of 7 KiB. On the Black Pill the RAM window is still 56 KiB and
the flash image may be up to 320 KiB; on the STM32F405, 832 KiB.

A program that starts no threads may have their stacks too. On the Blue
Pill the two 1 KiB thread stacks follow the window, so such a program gets
9 KiB of RAM instead of 7. It is listed in `NOTHREADS` in the Makefile,
which sets `FREYA_APP_F_NOTHREADS` in its header and lets the linker
script allow the larger window; forth is built that way. `thread_create()`
returns `FREYA_ERR_UNSUPPORTED` to it. The shell keeps its scratch in the
same stacks, so while such a program's RAM reaches into them a script it
asks for is refused, and if the shell needs the scratch while the program
is only loaded it unloads the program. `run("forth.bin")` works as one
command. On the F4 boards the thread stacks are below the window, so the
flag only refuses threads.

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

## Autorun

If `/autorun.bin` exists it is started automatically at boot, with two seconds
to press a key and cancel. Failing that, on a board that keeps a program in
flash, an installed image is started the same way when the auto-start flag is
on — `autostart on` after `install`, or `AUTOSTART=1` when the program is
packed into the module, so a board with nothing in the card socket still
boots Freya and runs a program. An installed shell script is started the
same way when that is what the region holds. `make flash SCRIPT=boot.sh
AUTOSTART=1` packs that script with the flag already on.
