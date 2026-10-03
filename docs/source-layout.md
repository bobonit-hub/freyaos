# Source layout

Where each part of Freya lives in the tree: the board directories, the kernel
sources, the program ABI, the samples, the host tests, the documents and the
host tools.

| Path | Contents |
|---|---|
| `boards/<board>/` | one directory per board: register header, `startup.s`, `board.c` (clock tree, pin mux, LED), linker scripts, compiler flags |
| `boards/<board>/app_flash.ld` | the second program linker script: code in flash, data in RAM |
| `src/system.c` | SysTick, reset cause, delays, software clock |
| `src/uart.c` | USART2 console, interrupt driven receive; the older SR/DR USART and the U5's and H5's ISR/RDR/TDR one (`BOARD_USART_ISR`) |
| `src/spi.c`, `src/sd.c`, `src/spiflash.c` | SPI1 for the card and the SPI flash (`BOARD_SPIFLASH`), and SPI master for a program; the F4's SPI and the U5's and H5's FIFO SPI (`BOARD_SPI_FIFO`), which may run from a kernel clock of its own (`BOARD_SPI_KERNEL_HZ`); on a board that drives its card itself (`BOARD_SD_BITBANG`), SPI1 is the SPI flash's alone (`flspi_*()`) |
| `src/gpio.c` | pins a program may drive, and the sixteen EXTI interrupt lines, shared or one interrupt each (`BOARD_EXTI_SPLIT`) |
| `src/timer.c` | the general purpose timers and their interrupts |
| `src/pwm.c` | the compare channels of those timers, driving pins |
| `src/i2c.c` | I2C master, on the buses the board header names |
| `src/ds3231.c` | optional DS3231 clock, built with `RTC=ds3231`; SCL is PB6, SDA is PB7 |
| `src/w1.c` | 1-Wire master, standard speed, on a pin a program names |
| `src/aead.c`, `third_party/ascon/` | Ascon-AEAD128, for a program and for `aead`; not built into the STM32F103. Keys come from `tools/aead` |
| `src/lz.c`, `third_party/heatshrink/` | heatshrink LZSS, for a program and for `compress` / `decompress`; not built into the Blue Pill |
| `src/fat.c` | FAT16 / FAT32, including long file names and writing |
| `src/lfsvol.c`, `third_party/littlefs/` | LittleFS on the SPI flash of the Black Pill, the STM32U585, the STM32H523 and the STM32H723 (the default there) |
| `src/fs.c` | paths, working directory, descriptor table |
| `src/xmodem.c` | XMODEM receive (`download`) and send (`upload`) |
| `src/loader.c` | program loading and installing, the service table, start and stop |
| `boards/<board>/flash.c` | internal flash erase and program, bounded to the program region |
| `src/fault.c` | fault containment and the kernel panic dump |
| `src/ramdump.c` | SRAM dump to `/freya.ram` after a BusFault |
| `src/shell.c` | line editing and the commands |
| `linux/` | `fsh`, the shell built for Linux: its board header, its platform layer, its tests |
| `src/log.c` | file log (`/freya.log`) and rotation |
| `src/heap.c`, `src/print.c`, `src/string.c` | allocator, formatting, freestanding libc |
| `apps/`, `include/freya_api.h` | example programs and the program ABI |
| `rust/freya/` | Rust bindings to the program ABI, checked against `freya_api.h` when they build ([rust/README.md](../rust/README.md)) |
| `samples/` | small standalone samples: `blink`, `log`, `irq`, `pwm`, `i2c`, `spi`, `w1`, `aead`, `compress`, `flashprobe`, `tetris`, `edit`, `forth`, `altair`, `altair16`, `vm`, `basic11`, and `rustdemo` in Rust |
| `qbe/` | QBE target and cproc patch for the virtual machine, and `as.py`, the assembler that makes an image |
| `basic/` | BASIC-11 style interpreter for the FPU boards: the interpreter, its `float` arithmetic, the PC build it is tested on |
| `tests/` | host side tests |
| `docs/boards.md` | every board: parts, clock trees, console and card clocks, memory maps |
| `docs/hardware.md` | wiring: console, LED, SD slot and its supply, SPI flash, reserved pins, PWM, I2C, DS3231, 1-Wire, SPI |
| `docs/building.md` | toolchain, make targets and variables, flashing, packing a program or a script |
| `docs/files.md` | the filesystem, XMODEM, `tools/send.py` and `tools/fremote.py` |
| `docs/programs.md` | writing, running, stopping and installing programs, exit status, autorun |
| `docs/source-layout.md` | this table |
| `docs/tests.md` | what `make test` checks |
| `docs/limits.md` | notes and limits |
| `docs/shell.md` | the shell language: values, expressions, control, variables, functions |
| `docs/linux.md` | the shell language as a Linux program, `fsh`, and `run()` |
| `docs/console-commands.md` | full command list, and the six that were Blue Pill only |
| `docs/interrupts.md` | the pin, timer, PWM and interrupt API, and what a handler may do |
| `docs/i2c.md` | the I2C master API, the pins, the optional DS3231, and the `i2c` command |
| `docs/spi.md` | the SPI master API, the pins, and the `spi` command |
| `docs/network.md` | ESP32-C6 wiring, Wi-Fi commands and the asynchronous network API |
| `docs/w1.md` | the 1-Wire master API, the pin, and the `w1` command |
| `docs/aead.md` | the Ascon-AEAD128 API, the `aead` command and `tools/aead` |
| `docs/compress.md` | the heatshrink API and the `compress` / `decompress` commands |
| `docs/sd-slot.txt` | SD slot wiring for the Blue Pill and the Black Pill |
| `docs/adc.md` | the ADC call and the `adc` command |
| `docs/threads.md` | the thread calls |
| `docs/vm.md` | the PDP-11 virtual machine calls |
| `docs/flash-programs.md` | the design note behind flash-resident programs |
| `docs/bluepill-memory.md` | proposals for more program RAM and flash on the Blue Pill |
| `tools/send.py` | XMODEM sender for hosts without lrzsz |
| `tools/fremote.py` | remote shell and SD card utility (`fs ls`, `fs cp`, …) |
| `tools/pack_image.py` | packs the kernel and one `.xip.bin` or shell script into the image `make flash PROGRAM=` / `SCRIPT=` writes |
