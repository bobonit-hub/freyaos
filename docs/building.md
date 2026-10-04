# Building and flashing

How to build Freya, what the build produces, how to write it to a board, how
to pack a program or a shell script into the image, and how to open the
console afterwards. Board-specific programming details are also in
[boards.md](boards.md).

## Toolchain

Needs an `arm-none-eabi` GCC (the one in the STM32CubeCLT works, so does any
distribution package) and `make`. Rust programs also need `cargo`; see
[programs.md](programs.md#rust).

## Make targets and variables

```sh
make                   # Black Pill kernel image + example programs
make BOARD=bluepill    # the same for the Blue Pill
make BOARD=stm32f405   # the same for the STM32F405xx
make BOARD=weact_f405  # the same for the WeAct STM32F4 64-pin board (F405RGT6)
make BOARD=apm32f407   # the same for an APM32F407ZGT6 board (LED on PF9)
make BOARD=blackpill2  # the same for the Black Pill 2
make BOARD=stm32u585   # the same for the WeAct STM32U585CIU6 board
make BOARD=stm32h523   # the same for the WeAct STM32H523CET6 board
make BOARD=stm32h562   # the same for the WeAct STM32H562RGT6 board
make BOARD=stm32h723   # the same for the WeAct MiniSTM32H723
make SD=1              # also build the SD card and FAT16 / FAT32 code
make USB=1             # also build the USB host: a FAT stick at /usb
make USB=1 AUDIO=1     # and USB headsets behind the audio calls
make CODECS=1          # the codec pack: G.711 and Opus, a library programs link
make RTC=ds3231        # also build the DS3231 driver (PB6 SCL, PB7 SDA)
make RTC=internal      # or the driver for the chip's own calendar RTC
make NOSHELL=1         # no shell: always run the autorun program
make BASIC=prog.bas    # build prog.bas into the kernel; basic11 runs it at boot
make FIRMWARE_VERSION=3.1.1
                       # override the hardcoded firmware version
make size              # section sizes
make test              # run the filesystem and XMODEM code on the host
make clean
```

`SD=1`, `USB=1`, `RTC=ds3231`, `RTC=internal` and `FIRMWARE_VERSION=`
combine with `BOARD=`. `USB=1` is for the boards with a USB OTG core that
can be the host, and is refused for the others ([usb.md](usb.md)). `AUDIO=1`
needs `USB=1` and is for the boards with more than 512 KiB of flash: the
two STM32F405 boards, the STM32U585 and the STM32H723 ([audio.md](audio.md)).
`CODECS=1` builds `build/<board>/codecs/libfreya_codecs.a` and, where there
is audio, `samples/opusrec`; `OPUS_SCRATCH=` sizes libopus's scratch, 24 KiB
by default ([codecs.md](codecs.md)). On every board but the Blue Pill, plain
`make` also builds the HTTP client library,
`build/<board>/http/libfreya_http.a`, and `samples/wget` ([http.md](http.md)),
and, on the boards with 192 KiB of SRAM or more (`CJSON := 1` in their
`board.mk`), cJSON, `build/<board>/json/libfreya_cjson.a`, and
`samples/jsonget` ([json.md](json.md)). Leave `SD` unset and the card is not in the image (see below).
Leave `RTC` unset and neither clock driver is in the image.
`RTC=internal` is for the boards with a calendar RTC
([rtc.md](rtc.md)) and is refused for the others. The banner and the first
line of `sysinfo()` print the OS version from this documentation, 4.0.0
"Bigfoot"; `FIRMWARE_VERSION` does not change that. The firmware
version defaults to the value hardcoded in `src/freya.h`; an override must
have `major.minor.patch` numeric form. `sysinfo()` prints that firmware
version on its own line.

## The SD card

The SD card driver and the FAT16 / FAT32 code are built only with
`make SD=1`. Without it `src/nosd.c` takes their place, and the kernel is
about 10 KiB of flash and 1 KiB of RAM smaller:

- Nothing is mounted on `/`. The boot does not look for a card, and
  `/autorun.bin` is never run.
- On a board with SPI flash (the Black Pill, the STM32U585, the STM32H523
  and the STM32H723), `/spi1` is still LittleFS with the same file calls,
  and the shell starts there. `mount()` mounts that volume again.
- With no SPI flash either, every file call answers that there is no
  filesystem. `mount()` is not in the shell.
- `power()` is not in the shell, and a program's
  `api->power(FREYA_PWR_SD, on)` returns `FREYA_ERR_UNSUPPORTED`. The
  board still powers the socket at reset as it always has.
- The file log goes to the console, and a BusFault dump to `/freya.ram`
  is skipped.

```sh
make BOARD=bluepill SD=1 flash
```

Switching `SD` on or off rebuilds the objects it changes.

## Building without the shell

`make NOSHELL=1` leaves the shell out, for a board that only ever runs one
program. On the Blue Pill that takes the kernel from about 96 KiB of flash
to about 37 KiB, because the linker also drops the code that only the shell
used. With no shell to go back to, autorun is always on:

- The boot runs `/autorun.bin` from the card if there is one (`SD=1`), otherwise the
  program installed in flash, whether or not its auto-start flag is set.
  There is no two-second window to cancel it.
- When the program exits, or Ctrl-C stops it, it is started again a second
  later.
- With neither, or with an image that does not load, the boot says so and
  the board halts.
- A shell script needs the shell. `SCRIPT=` is refused, and a script
  already installed in flash is not run. A program's
  `shell_source_capture()` call returns `FREYA_ERR_UNSUPPORTED`.

The program goes in with `PROGRAM=`, which packs it with the auto-start
flag set, so the image also autostarts under a kernel that has the shell:

```sh
make BOARD=bluepill NOSHELL=1 flash PROGRAM=hello
```

`NOSHELL=1` combines with `BOARD=`, `SD=` and `RTC=`. Switching it on or off
rebuilds the objects it changes.

### A BASIC program at boot

`BASIC=prog.bas` builds the program's text into the kernel and flashes
`basic11` with it, which then runs it at every boot. It sets
`PROGRAM=basic11` (unless `PROGRAM=` is given) and `AUTOSTART=1`, and the
boot starts the program as `basic11 -e TEXT`. On the Blue Pill:

```sh
make BOARD=bluepill NOSHELL=1 flash BASIC=prog.bas
```

The Blue Pill build of `basic11`, with software floating point and about
5 KiB for the BASIC program and its data, is described in
[basic/README.md](../basic/README.md#the-blue-pill). `BASIC=` works with
the shell too, and on the other boards. A program saved from `basic11`
with `FSAVE` is run instead of the built-in one
([basic/README.md](../basic/README.md#a-program-saved-in-flash)).

`make test` is described in [tests.md](tests.md). `make linux` builds the
shell as a Linux program; see [linux.md](linux.md).

## Build outputs

Each board builds into its own directory, so the boards never overwrite each
other: the result is `build/<board>/freya.bin` (around 42.5 KiB on the Black
Pill once the flash programmer is in, 42 on the Blue Pill) plus
`build/<board>/freya.hex`, and the example programs in `build/<board>/apps/` —
each one built both as a `.bin` to load into RAM and as a `.xip.bin` to
install into flash.
`BOARD=` applies to every target below as well.

## Flashing

Flashing, whichever tool you have:

```sh
make flash          # st-flash --reset write build/<board>/freya.bin 0x08000000
make openocd        # ST-Link via OpenOCD, with the board's target script
make bootloader     # the chip's own ROM loader
make BOARD=stm32f405 dfu
                    # build/stm32f405/freya.dfu for the ROM DFU loader
```

`make bootloader` is USB DFU on the Black Pill, the STM32F405xx and the Black
Pill 2 (hold BOOT0, tap NRST), and `make BOARD=blackpill2 dfu` packs a DfuSe
file for Artery's loader (`2e3c:df11`). st-flash does not know Artery parts,
so `make flash` refuses the Black Pill 2; `make openocd` programs it over SWD
with OpenOCD's `target/artery/at32f4x.cfg`. The STM32U585, the STM32H523,
the STM32H562 and the STM32H723 take `make flash` (st-flash 1.8 knows all
four), and
`make bootloader` is USB DFU through the board's USB-C socket (hold BOOT0,
tap NRST); `make BOARD=<board> dfu` packs a DfuSe file for ST's loader
(`0483:df11`). `make openocd` uses `target/stm32u5x.cfg` and
`target/stm32h7x.cfg`; the H523's and the H562's need an OpenOCD that ships
`target/stm32h5x.cfg`, which 0.12 does not. A board whose system settings
are not at `0x0800C000` in a 16 KiB unit names its settings sector in its
`board.mk` (`CKSUM_PAGE_BASE`, `CKSUM_PAGE_SIZE`), which is where
`make flash` updates the firmware sum. `make BOARD=stm32f405 dfu` packs
the kernel and the extension into one DfuSe file, at the addresses they are
linked for, and leaves the gap between them untouched. The F103 has no USB
loader, so on the Blue Pill it drives the serial loader in ROM with
`stm32flash`: pull BOOT0 high, tap NRST, and add `PORT=/dev/ttyUSB1` if the
adapter is not on `ttyUSB0`.

On the Blue Pill the size register often still reads 64 KiB. `make flash` and
`make openocd` tell the programmer 128 KiB, which is the size every one of
these boards has, so an image that uses the top half is written in full (see
[boards.md](boards.md#flash-size)).

## Packing a program or a script

On every board, `PROGRAM` packs one program into the image that those
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
still overrides either one. `tools/pack_image.py` does the packing.

## A first script

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

The language is in [shell.md](shell.md).

## Opening the console

```sh
picocom -b 921600 /dev/ttyUSB0      # or minicom, screen, putty ...
```

Leave hardware flow control off; the wiring is in [hardware.md](hardware.md).
The commands are in [console-commands.md](console-commands.md), and
[files.md](files.md) has `tools/fremote.py`, which also opens the console.
