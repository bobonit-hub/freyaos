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
| Program flash | 64 KiB (sector 4) | 81792 bytes |
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
| Samples | `blink`, `log`, `irq`, `pwm`, `i2c`, `flashprobe`, `tetris`, `forth` |

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
