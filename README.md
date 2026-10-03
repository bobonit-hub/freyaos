# Freya

Freya is a 32-bit, single-user, text OS for STMicroelectronics
STM32 small MCUs, written from scratch in C and ARM assembly. It runs bare
metal on the STM32F411CEU6 "Black Pill", the STM32F103C8T6 "Blue Pill",
the STM32F405xx, the WeAct "Black Pill 2" with Artery's AT32F403ACGU7,
an STM32F103 at heart with a Cortex-M4F core, and WeAct's STM32U585CIU6
and STM32H523CET6 core boards, both Cortex-M33.
No HAL and no CMSIS: Freya brings the chip up itself. LittleFS, on the SPI flash, is the one vendored library. Freya
talks to the hardware through its own register definitions, and lives
entirely in internal flash. This is release 3.3.0, "Poltergeist". The notes
are in [RELEASE_NOTES.md](RELEASE_NOTES.md).

Freya gives you a serial console, a real FAT filesystem on an SD card, and the
ability to download a program over the console, load it into RAM and run it —
then stop it again with Ctrl-C. The Black Pill and the STM32U585 and STM32H523
boards can also mount a SPI NOR chip soldered on their SOP-8 footprint at `/spi1`,
formatted as LittleFS, with the same file calls. Every board also keeps one
program in a reserved area of its own flash and run it from there, so the program survives a power
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
Freya 3.3.0 "Poltergeist" for STM32F411CEU6
96 MHz, power-on reset. Type 'help()'.

[boot] clocks     : HSE 25 MHz crystal + PLL, sysclk 96 MHz, flash 3 WS
[boot] console    : USART2 921600 8N1 on PA2/PA3
[boot] SD card    : SD v2 (SDHC/SDXC), 14.8 GiB (31116288 blocks)
[boot] filesystem : FAT32 "FREYA", cluster 16.0 KiB, mounted on /

freya:
```


## Boards

| Board | MCU | Core and clock | Build |
|---|---|---|---|
| Black Pill | STM32F411CEU6 | Cortex-M4F at 96 MHz | `make` |
| Blue Pill | STM32F103C8T6 | Cortex-M3 at 72 MHz | `make BOARD=bluepill` |
| STM32F405xx | STM32F405xx | Cortex-M4F at 168 MHz | `make BOARD=stm32f405` |
| Black Pill 2 | AT32F403ACGU7 | Cortex-M4F at 240 MHz | `make BOARD=blackpill2` |
| STM32U585 | STM32U585CIU6 | Cortex-M33F at 160 MHz | `make BOARD=stm32u585` |
| STM32H523 | STM32H523CET6 | Cortex-M33F at 250 MHz | `make BOARD=stm32h523` |

Flash, SRAM, program regions, clock trees and memory maps are in
[docs/boards.md](docs/boards.md), and the wiring in
[docs/hardware.md](docs/hardware.md).

## What it does

* Boots from internal flash and brings up the whole clock tree itself ([docs/boards.md](docs/boards.md)).
* A console shell on USART2 at 921600 8N1, with line editing and history ([docs/console-commands.md](docs/console-commands.md)).
* A shell language with variables, functions, loops and scripts, also built for Linux as `fsh` ([docs/shell.md](docs/shell.md), [docs/linux.md](docs/linux.md)).
* SD / SDHC cards over SPI with FAT16 / FAT32 that reads and writes, a switchable socket supply, and LittleFS on the SPI flash of the Black Pill, the STM32U585 and the STM32H523 ([docs/files.md](docs/files.md)).
* XMODEM / XMODEM-1K over the console, and `tools/fremote.py` on the host ([docs/files.md](docs/files.md)).
* Wi-Fi, DNS, ping, TCP/UDP sockets and TLS 1.3 through an optional ESP32-C6, on every board but the Blue Pill ([docs/network.md](docs/network.md)).
* A dated log in `/freya.log` from programs, the shell and the kernel, rotated at 1 MiB ([docs/programs.md](docs/programs.md#the-service-table)).
* Programs loaded from the card into RAM and run as machine code, in C or Rust ([docs/programs.md](docs/programs.md)).
* Spare pins, pin interrupts and three timers for a program ([docs/interrupts.md](docs/interrupts.md)).
* PWM on eight pins, 1 Hz to 1 MHz, from a program or the console ([docs/interrupts.md](docs/interrupts.md)).
* An ADC: one raw 12-bit sample of a pin, the temperature sensor or the reference ([docs/adc.md](docs/adc.md)).
* I2C master, 10 kHz to 400 kHz, and an optional DS3231 clock ([docs/i2c.md](docs/i2c.md)).
* 1-Wire master on any spare pin ([docs/w1.md](docs/w1.md)).
* SPI master on SPI2 for a program ([docs/spi.md](docs/spi.md)).
* heatshrink LZSS compression, not on the Blue Pill ([docs/compress.md](docs/compress.md)).
* Ascon-AEAD128 sealing and opening, not on the Blue Pill ([docs/aead.md](docs/aead.md)).
* A 32-bit PDP-11 virtual machine and a C compiler for it, not on the Blue Pill ([docs/vm.md](docs/vm.md)).
* A BASIC-11 style interpreter for the boards with an FPU ([basic/README.md](basic/README.md)).
* One program kept in internal flash and run in place, packed in at flash time or installed from the card ([docs/programs.md](docs/programs.md#running-from-flash)).
* Threads inside a program, by name and priority ([docs/threads.md](docs/threads.md)).
* Stops any program with Ctrl-C, and contains one that crashes ([docs/programs.md](docs/programs.md#stopping-a-program)).
* Reports how every run ended as a POSIX-style exit status ([docs/programs.md](docs/programs.md#exit-status)).

## Quick start

```sh
make BOARD=bluepill                 # or make, BOARD=stm32f405, blackpill2, stm32u585, stm32h523
make BOARD=bluepill flash           # st-flash; see docs/building.md for the others
picocom -b 921600 /dev/ttyUSB0      # the console: PA2 to the adapter's RX, PA3 to its TX
```

Then `help()` at the `freya:` prompt. [docs/building.md](docs/building.md)
has the toolchain, every make target, the other flashing tools and packing a
program or a script into the image; [docs/files.md](docs/files.md) has
getting a program onto the card, and [docs/programs.md](docs/programs.md)
writing one.

## Documentation

| Document | What it covers |
|---|---|
| [RELEASE_NOTES.md](RELEASE_NOTES.md) | what changed in each release |
| [docs/boards.md](docs/boards.md) | every board: parts, clock trees, console and card clocks, memory maps |
| [docs/hardware.md](docs/hardware.md) | wiring: console, LED, SD slot and its supply, SPI flash, reserved pins, PWM, I2C, DS3231, 1-Wire, SPI |
| [docs/sd-slot.txt](docs/sd-slot.txt) | SD slot wiring drawings for the Blue Pill and the Black Pill |
| [docs/building.md](docs/building.md) | toolchain, make targets and variables, flashing, packing a program or a script |
| [docs/files.md](docs/files.md) | the filesystem, XMODEM, `tools/send.py` and `tools/fremote.py` |
| [docs/console-commands.md](docs/console-commands.md) | every console command |
| [docs/shell.md](docs/shell.md) | the shell language: values, expressions, control, variables, functions |
| [docs/linux.md](docs/linux.md) | the shell language as a Linux program, `fsh`, and `run()` |
| [docs/programs.md](docs/programs.md) | writing, running, stopping and installing programs, exit status, autorun |
| [docs/interrupts.md](docs/interrupts.md) | the pin, timer, PWM and interrupt API, and what a handler may do |
| [docs/threads.md](docs/threads.md) | the thread calls |
| [docs/adc.md](docs/adc.md) | the ADC call and the `adc` command |
| [docs/i2c.md](docs/i2c.md) | the I2C master API, the pins, the optional DS3231, and the `i2c` command |
| [docs/spi.md](docs/spi.md) | the SPI master API, the pins, and the `spi` command |
| [docs/w1.md](docs/w1.md) | the 1-Wire master API, the pin, and the `w1` command |
| [docs/network.md](docs/network.md) | ESP32-C6 wiring, Wi-Fi commands and the asynchronous network API |
| [docs/aead.md](docs/aead.md) | the Ascon-AEAD128 API, the `aead` command and `tools/aead` |
| [docs/compress.md](docs/compress.md) | the heatshrink API and the `compress` / `decompress` commands |
| [docs/vm.md](docs/vm.md) | the PDP-11 virtual machine calls |
| [docs/flash-programs.md](docs/flash-programs.md) | the design note behind flash-resident programs |
| [docs/bluepill-memory.md](docs/bluepill-memory.md) | proposals for more program RAM and flash on the Blue Pill |
| [docs/source-layout.md](docs/source-layout.md) | where each part of the source lives |
| [docs/tests.md](docs/tests.md) | what `make test` checks on the host |
| [docs/limits.md](docs/limits.md) | notes and limits |
| [rust/README.md](rust/README.md) | the Rust bindings |
| [basic/README.md](basic/README.md) | the BASIC-11 style interpreter |
