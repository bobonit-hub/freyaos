# Boards

Freya runs on eight boards. This page is everything that differs between
them: the parts, the clock trees, the console and card clocks, and the flash
and RAM maps. It starts with a comparison table and the parts every board
shares, then gives each board a section of its own. The wiring that is the
same on every board is in [hardware.md](hardware.md).

## Comparison

|  | Black Pill | Blue Pill | STM32F405xx | Black Pill 2 | STM32U585 | STM32H523 | STM32H562 | STM32H723 |
|---|---|---|---|---|---|---|---|---|
| MCU | STM32F411CEU6 | STM32F103C8T6 | STM32F405xx | AT32F403ACGU7 | STM32U585CIU6 | STM32H523CET6 | STM32H562RGT6 | STM32H723VGT6 |
| Core | Cortex-M4F at 96 MHz | Cortex-M3 at 72 MHz | Cortex-M4F at 168 MHz | Cortex-M4F at 240 MHz | Cortex-M33F at 160 MHz | Cortex-M33F at 250 MHz | Cortex-M33F at 250 MHz | Cortex-M7F at 520 MHz |
| Crystal | 25 MHz | 8 MHz | 8 MHz | 8 MHz | 25 MHz | 8 MHz | 8 MHz | 25 MHz |
| Flash | 512 KiB | 128 KiB | 1 MiB | 1 MiB | 2 MiB | 512 KiB | 1 MiB | 1 MiB |
| SRAM | 128 KiB | 20 KiB | 128 KiB | 96 KiB | 768 KiB | 272 KiB | 640 KiB | 564 KiB (320 KiB used) |
| Program region | 56 KiB RAM, or 320 KiB flash | 7 KiB RAM (9 KiB without threads), or 24 KiB flash | 56 KiB RAM, or 832 KiB flash | 40 KiB RAM, or 832 KiB flash | 504 KiB RAM, or 1856 KiB flash | 168 KiB RAM, or 320 KiB flash | 504 KiB RAM, or 832 KiB flash | 216 KiB RAM, or 640 KiB flash |
| Build | `make` | `make BOARD=bluepill` | `make BOARD=stm32f405` | `make BOARD=blackpill2` | `make BOARD=stm32u585` | `make BOARD=stm32h523` | `make BOARD=stm32h562` | `make BOARD=stm32h723` |
| Console divisor (USARTDIV) | 52 at 48 MHz APB1 | 39 at 36 MHz APB1 | 46 at 42 MHz APB1 | 130 at 120 MHz APB1 | 87 at 80 MHz APB1 | 271 at 250 MHz APB1 | 271 at 250 MHz APB1 | 141 at 130 MHz APB1 |
| Console rate | 923077 baud | 923077 baud | 913043 baud | 923077 baud | 919540 baud | 922509 baud | 922509 baud | 921986 baud |
| SD identification clock | 375 kHz | 281 kHz | 328 kHz | 234 kHz | 312.5 kHz | 390.6 kHz | about 250 kHz, bit-banged | about 250 kHz, bit-banged |
| SD data clock | 12 MHz | 9 MHz | 10.5 MHz | 15 MHz | 10 MHz | 12.5 MHz | bit-banged, unmeasured | bit-banged, unmeasured |
| SPI flash volume (`/spi1`) | yes | no | no | no | yes | yes | no | yes, beside the card |
| Calendar RTC (`RTC=internal`, [rtc.md](rtc.md)) | yes | no | yes | no | yes | yes | yes | yes |

## What a board is

Everything a board needs lives in `boards/<board>`: its register header, its
startup code and vector table, its bring-up (`board.c`: clock tree, pin mux,
LED), its two linker scripts and its compiler flags. Everything under `src/` is
the same code on every board, and another board is another directory rather
than a fork. Another board is also another column in the table above and
another section below, in the shape the existing ones have.

## Shared

### Clocks

Freya boots from internal flash and configures the whole clock tree itself;
each board's tree is in its section below. All of them fall back to the
internal oscillator if the crystal does not start, and the SysTick reload,
the console divisor and the microsecond delay all follow whatever the clock
tree actually came up at.

### Console rate

The console runs at 921600 baud on every board (why that rate is in
[hardware.md](hardware.md#console)). No board divides it exactly: USARTDIV
rounds to the value in the table above against that board's APB1. The
Black Pill, the Blue Pill and the Black Pill 2 land on 923077 baud, 0.16%
fast; the STM32H723 on 921986, 0.04% fast; the STM32H523 and the STM32H562
on 922509, 0.10% fast; the STM32U585 on 919540,
0.22% slow; the STM32F405 on 913043, 0.93% slow. All of them are far inside what 8N1 tolerates. If the
adapter is a faster one, the rate is `uart_init()` in `src/main.c` and the
`BOARD_CONSOLE_NAME` string.

### SD card clock

Card identification runs inside the 100–400 kHz window the spec demands, at
the identification clock in the table above, and the bus then switches to the
data clock.

### Program regions and the memory maps

Each board has a program region in RAM and a program flash region; the sizes
are in the table above and the maps in each board's section. How programs use
them is in [programs.md](programs.md).

A flash-resident program uses the RAM window for its `.data` and `.bss` alone.
The kernel flash ceiling is a hard limit: each board's `freya.ld` fails the
link rather than let the kernel grow into the auto-start slot.

`meminfo` reports all of it at runtime, including the heap's largest free block
and the stack high-water mark (the reset handler paints the stack, so the peak
is measured rather than guessed).

### Flash size

Every supported board has at least 128 KiB of flash. The Blue Pill size
register often still reads 64; `sysinfo`, `meminfo` and `flashdump` use
128 KiB anyway, and `make flash` / `make openocd` tell the programmer the
same, which is the size every one of these boards has, so an image that uses
the top half is written in full. `samples/flashprobe` programs and reads back
a block in each erase unit above that floor when the question is whether one
chip has still more, and erases each unit again afterwards. The kernel's
region is a build-time constant, not whatever the probe found.

### The F4 sector layout

The F4 boards give the program flash region everything between the kernel
and the kernel extension: 320 KiB on the Black Pill (sectors 4..6), 832 KiB
on the 1 MiB STM32F405 (sectors 4..10). The system settings are the first
1 KiB of sector 3, a 16 KiB sector nothing else uses, so toggling the
auto-start flag never erases the image, and the kernel image is limited to
the first 48 KiB so it never shares a sector with them. The thread scheduler
is a second image in the last sector, past the program, so flashing the
kernel or installing a program does not erase it. The region's erase units
are one 64 KiB sector and then 128 KiB sectors; a write erases only the
sectors it touches, which is why the shell's `flash_write()` 64 KiB blocks
come in pairs there.

## Black Pill (STM32F411CEU6)

Build with `make`; this is the default board.

Clock tree: a 25 MHz crystal → PLL → 96 MHz SYSCLK, 48 MHz APB1, 96 MHz APB2,
3 flash wait states, voltage scale 1, prefetch and caches on.

Console: USARTDIV rounds to 52 against the 48 MHz APB1. The USART would reach
3 Mbaud. Card: identification at 375 kHz, then 12 MHz.

The SOP-8 footprint on the back shares the card's SPI1 bus. A SPI NOR chip
fitted there, and no card in the socket, is mounted at `/spi1` as LittleFS;
see [hardware.md](hardware.md#sd-card-and-spi-flash). I2C bus 2 is PB10/PB9,
because the chip has no PB11. The Black Pill also turns on a pin's own weak
pull-up for I2C and 1-Wire.

A BusFault ram dump (see [programs.md](programs.md#stopping-a-program)) is
128 KiB.

Flash:

```
0x08000000  +--------------------------------+
            |  Freya kernel (~47.8 KiB used) |  48 KiB, sectors 0..2
0x0800C000  +--------------------------------+
            |  system settings               |  first 1 KiB of sector 3
0x0800C400  +--------------------------------+
            |  unused                        |  rest of sector 3
0x08010000  +--------------------------------+
            |  program flash region          |  320 KiB, sectors 4..6
0x08060000  +--------------------------------+
            |  kernel extension              |  128 KiB, sector 7
0x08080000  +--------------------------------+
```

RAM (the STM32F405 has the same):

```
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

The thread stacks are below the program window, so a program that starts no
threads gets no more RAM here; the `NOTHREADS` flag only refuses threads.

## STM32F405xx

Build with `make BOARD=stm32f405`; `make BOARD=stm32f405 dfu` packs
`build/stm32f405/freya.dfu` for the ROM DFU loader (see
[building.md](building.md#flashing)).

A Cortex-M4F at 168 MHz from an 8 MHz crystal, with 1 MiB of flash and
128 KiB of SRAM.

Clock tree: an 8 MHz crystal → PLL (M=8, N=336, P=2) → 168 MHz SYSCLK,
42 MHz APB1, 84 MHz APB2; PLLQ=7 gives the 48 MHz the USB domain wants.
Without the crystal, HSI 16 MHz with M=16 gives the same 168 MHz.

Console: USARTDIV rounds to 46 against the 42 MHz APB1, 0.93% slow. Card:
identification at 328 kHz, then 10.5 MHz.

The STM32F405 has 1 MiB in twelve sectors and the Black Pill's map,
stretched: the program flash region is sectors 4..10 (`0x08010000`..`0x080DFFFF`,
832 KiB) and the kernel extension is sector 11 (`0x080E0000`). Each
sector from 5 on is 128 KiB, so a write into the region erases only the
sectors it touches and a program never shares a sector with the settings
or the extension. Its RAM map is the Black Pill's, above. I2C bus 2 is
PB10/PB11.

## Black Pill 2 (AT32F403ACGU7)

Build with `make BOARD=blackpill2`.

The Black Pill 2 is WeAct's board of that name
([WeActStudio.BlackPill](https://github.com/WeActStudio/WeActStudio.BlackPill)),
the Black Pill's outline with an Artery part on it: the AT32F403ACGU7, an
STM32F103 at heart with a Cortex-M4F core. Its peripherals are the
F103's, so it is wired and driven like the Blue Pill; its core has the FPU,
so it builds everything the F4 boards do except the 48 KiB Altair, which
does not fit its 40 KiB program window. The AT32F403A can trade fast flash
for 128 KiB more SRAM through an option byte; Freya leaves the option byte
alone and uses the 96 KiB the chip ships with. It has no SPI flash volume:
the board's SOP-8 footprint is wired to the chip's SPIM pins, not SPI1.

Clock tree: an 8 MHz crystal → PLL ×30 → 240 MHz SYSCLK, 120 MHz APB1,
60 MHz APB2, switched in steps by the chip's auto step mode.

Console: USARTDIV rounds to 130 against the 120 MHz APB1. Card:
identification at 234 kHz, then 15 MHz.

I2C bus 2 is PB10/PB11. The pins have no weak pull-up of their own, so the
I2C resistors are required.

Programming: st-flash does not know Artery parts, so `make flash` refuses
this board; `make openocd` programs it over SWD with OpenOCD's
`target/artery/at32f4x.cfg`, and `make bootloader` is USB DFU (hold BOOT0,
tap NRST). `make BOARD=blackpill2 dfu` packs a DfuSe file for Artery's loader
(`2e3c:df11`).

Flash — 2 KiB pages, two banks, and the first 256 KiB read without wait
states, so both kernel images sit there:

```
0x08000000  +--------------------------------+
            |  Freya kernel                  |  48 KiB
0x0800C000  +--------------------------------+
            |  system settings               |  1 KiB, a page of their own
0x0800C400  +--------------------------------+
            |  unused                        |
0x08010000  +--------------------------------+
            |  kernel extension              |  128 KiB
0x08030000  +--------------------------------+
            |  program flash region          |  832 KiB, bank 2 from
0x08100000  +--------------------------------+  0x08080000
```

Its RAM is the Black Pill's shape in 96 KiB: kernel data and heap up to
`0x2000AFFF`, the thread stacks, the 40 KiB program region at `0x2000C000`,
then the shell and interrupt stacks to `0x20018000`.

## STM32U585 (WeAct STM32U585CIU6 core board)

Build with `make BOARD=stm32u585`.

The board is WeAct's
[STM32U585Cx core board](https://github.com/WeActStudio/WeActStudio.STM32U585Cx_CoreBoard):
the Black Pill's outline and pinout with an STM32U585CIU6 on it, a
Cortex-M33 with an FPU, 2 MiB of flash and 768 KiB of SRAM. It is wired
like the Black Pill: the console on PA2/PA3, the card on SPI1 (PA4 to PA7),
the socket supply on PA8, the LED on PC13 (active low) and KEY on PA0. Its
SOP-8 footprint is a SPI NOR chip on SPI1 with chip select PA4, as on the
Black Pill, so it has the same `/spi1` LittleFS volume. Freya runs it with
TrustZone off, which is how the chip ships.

It builds everything the Black Pill does, the Altair included. The U5's
USART, SPI, EXTI, DMA, ADC and flash controller are newer designs than the
F4's; the board header turns on the paths in `src/` that drive them
(`BOARD_USART_ISR`, `BOARD_SPI_FIFO`, `BOARD_EXTI_SPLIT`,
`BOARD_ESP_GPDMA`). GPIO and the timers are the F4's, and every pin, PWM
channel and alternate function is the Black Pill's. Each EXTI line has an
interrupt of its own.

Clock tree: a 25 MHz crystal → PLL (M=5, N=64, R=2) → 160 MHz SYSCLK,
80 MHz APB1 and APB2, voltage range 1 with the EPOD booster (12.5 MHz,
MBOOST /2), 4 flash wait states, prefetch and the instruction cache on.
Without the crystal, HSI 16 MHz with M=4 and N=80 gives the same 160 MHz.
APB1 and APB2 run at half speed so that SPI1 still has a divider inside the
card identification window.

Console: USARTDIV rounds to 87 against the 80 MHz APB1, 0.22% slow. Card:
identification at 312.5 kHz, then 10 MHz. The ESP32-C6 link runs SPI2 at
20 MHz through GPDMA1 channels 0 and 1.

Limits that follow from the clocks: the timers count at 160 MHz, so the
longest timer period is 26.8 s rather than the 40 s the API allows on the
slower boards; a longer one is refused. A program's SPI2 runs from 80 MHz,
so its slowest rate is 312.5 kHz. I2C bus 2 is PB10/PB9, as on the Black
Pill, because the package has no PB11. The ADC is the U5's 14-bit converter
run at 12 bits, so `adc` returns the same range as on the other boards; its
channel numbers are the U5's (the temperature sensor is 19, Vref is 0).

Programming: `make flash` uses st-flash, and `make openocd` OpenOCD's
`target/stm32u5x.cfg`; `make bootloader` is USB DFU through the board's
USB-C socket (hold BOOT0, tap NRST). `make BOARD=stm32u585 dfu` packs a
DfuSe file for that loader (`0483:df11`). Rust programs need the
`thumbv8m.main-none-eabihf` target; without it, `make` skips them.

Flash — 8 KiB pages, two banks of 1 MiB, programmed 16 bytes at a time:

```
0x08000000  +--------------------------------+
            |  Freya kernel                  |  48 KiB, pages 0..5
0x0800C000  +--------------------------------+
            |  system settings               |  1 KiB, a page of their own
0x0800C400  +--------------------------------+
            |  unused                        |
0x08010000  +--------------------------------+
            |  kernel extension              |  128 KiB
0x08030000  +--------------------------------+
            |  program flash region          |  1856 KiB, bank 2 from
0x08200000  +--------------------------------+  0x08100000
```

RAM — SRAM1, SRAM2 and SRAM3 are one block, in the Black Pill's shape:

```
0x20000000  +--------------------------------+
            |  .data + .bss + system heap    |  252 KiB
0x2003F000  +--------------------------------+
            |  thread stacks, 4 x 1 KiB      |
0x20040000  +--------------------------------+
            |  user program region (504 KiB) |  image + .bss, loaded from
0x200BE000  +--------------------------------+  card, or just .data + .bss
            |  shell stack (6 KiB)           |  the program's main thread
0x200BF800  +--------------------------------+
            |  interrupt stack (2 KiB)       |
0x200C0000  +--------------------------------+
```

A BusFault ram dump is 768 KiB.

## STM32H523 (WeAct STM32H523CET6 core board)

Build with `make BOARD=stm32h523`.

The board is WeAct's
[STM32H523 core board](https://github.com/WeActStudio/WeActStudio.STM32H523CoreBoard)
in its LQFP48 form (CxTx): the Black Pill's pinout with an STM32H523CET6
on it, a Cortex-M33 with an FPU at 250 MHz, 512 KiB of flash and 272 KiB
of SRAM. It is wired like the Black Pill and the STM32U585 board: the
console on PA2/PA3, the card on SPI1 (PA4 to PA7), the socket supply on
PA8, the LED on PC13 (active low), KEY on PA0, and a SOP-8 footprint for a
SPI NOR chip on SPI1 with chip select PA4, mounted at `/spi1`. Freya runs it
with TrustZone off, which is how the chip ships.

It builds everything the Black Pill does, the Altair included. The H5's
USART, FIFO SPI, EXTI and GPDMA are the U5's blocks, so it uses the same
paths in `src/`; its clock tree, power controller, ADC and flash controller
are its own and live in `boards/stm32h523`. Every pin, PWM channel and
alternate function is the Black Pill's, and each EXTI line has an
interrupt of its own.

Clock tree: an 8 MHz crystal → PLL1 (M=4, N=250) → 500 MHz VCO, P=2 →
250 MHz SYSCLK with every APB bus at 250 MHz, Q=5 → 100 MHz for SPI1 and
SPI2; voltage scale 0, 5 flash wait states, the longer programming delay,
prefetch and the instruction cache on. Without the crystal, HSI comes out
of reset at 32 MHz and M=16 gives the same 250 MHz. The SPIs take PLL1Q
rather than an APB clock, which lets the buses run at full speed while the
card's identification clock stays inside its window; `src/spi.c` works a
program's SPI rates out from that 100 MHz (`BOARD_SPI_KERNEL_HZ`).

Console: USARTDIV rounds to 271 against the 250 MHz APB1, 0.10% fast. Card:
identification at 390.6 kHz, then 12.5 MHz. The ESP32-C6 link runs SPI2 at
12.5 MHz through GPDMA1 channels 0 and 1; the next tap up, 25 MHz, is
faster than the F4 boards run it.

Limits that follow from the clocks: the timers count at 250 MHz, so the
longest timer period is 17.1 s; a longer one is refused. A program's SPI2
runs from 100 MHz, so its slowest rate is 390.6 kHz. I2C bus 2 is
PB10/PB9, because the LQFP48 has no PB11. The ADC is 12 bits; its channel
numbers are the H5's (the temperature sensor is 16, Vref is 17).

Programming: `make flash` uses st-flash, and `make bootloader` is USB DFU
through the board's USB-C socket (hold BOOT0, tap NRST), and `make BOARD=stm32h523 dfu` packs a DfuSe file for it
(`0483:df11`). `make openocd` uses `target/stm32h5x.cfg`, which OpenOCD 0.12
does not ship. Rust programs need the `thumbv8m.main-none-eabihf` target.

Flash — 8 KiB sectors, two banks of 256 KiB, programmed 16 bytes at a time;
the U585's map in 512 KiB:

```
0x08000000  +--------------------------------+
            |  Freya kernel                  |  48 KiB, sectors 0..5
0x0800C000  +--------------------------------+
            |  system settings               |  1 KiB, a sector of their own
0x0800C400  +--------------------------------+
            |  unused                        |
0x08010000  +--------------------------------+
            |  kernel extension              |  128 KiB
0x08030000  +--------------------------------+
            |  program flash region          |  320 KiB, bank 2 from
0x08080000  +--------------------------------+  0x08040000
```

RAM — SRAM1, SRAM2 and SRAM3 are one block, in the Black Pill's shape:

```
0x20000000  +--------------------------------+
            |  .data + .bss + system heap    |  92 KiB
0x20017000  +--------------------------------+
            |  thread stacks, 4 x 1 KiB      |
0x20018000  +--------------------------------+
            |  user program region (168 KiB) |  image + .bss, loaded from
0x20042000  +--------------------------------+  card, or just .data + .bss
            |  shell stack (6 KiB)           |  the program's main thread
0x20043800  +--------------------------------+
            |  interrupt stack (2 KiB)       |
0x20044000  +--------------------------------+
```

A BusFault ram dump is 272 KiB.

## STM32H562 (WeAct STM32H5 64-pin core board, STM32H562RGT6)

Build with `make BOARD=stm32h562`.

The board is WeAct's
[STM32H5 64-pin core board](https://github.com/WeActStudio/WeActStudio.STM32H5_64Pin_CoreBoard)
with the STM32H562RGT6 fitted: a Cortex-M33 with an FPU at 250 MHz, 1 MiB
of flash and 640 KiB of SRAM in an LQFP64, an 8 MHz crystal, a 32.768 kHz
crystal, the LED on PB2 (active high), KEY on PC13 (high when pressed) and
a microSD slot. There is no SPI NOR footprint, so there is no `/spi1`.
The same board is sold with an STM32H503 and with F4 parts; the H562 spends
the pads the F4s call PB9 and PB11 on its VCAP capacitors, so neither pin
exists here. Freya runs it with TrustZone off, which is how the chip ships.

The chip is the STM32H523's sibling under the same reference manual
(RM0481), and `boards/stm32h562` is `boards/stm32h523` with the H562's
memory, the card and port D: the same clock tree, power controller, ADC,
flash controller and vector table, and the same USART, FIFO SPI, EXTI and
GPDMA paths in `src/`. It builds everything the STM32H523 does.

The card: the slot is the MiniSTM32H723's, wired to SDMMC1 on the same
pins, and driven the same way, in SPI mode by hand: CS on DAT3 (PC11), SCK
on CLK (PC12), MOSI on CMD (PD2), MISO on DAT0 (PC8) (`BOARD_SD_BITBANG`;
`sdspi_*()` are in `boards/stm32h562/board.c`). Identification runs at
about 250 kHz, and data as fast as the pins toggle. The slot has no supply
switch: `power sd off` unmounts and lets go of the pins, and the card stays
powered. Freya keeps PA2, PA3, PC8 to PC12 and PD2; PA4 to PA8, the card's
pins on the Black Pill, are a program's here. SPI1 is unused.

Pins for programs: ports A to D (port D is PD2 alone). PWM is the Black
Pill's eight pins with PB5 (TIM3 CH2) in place of PB9, and I2C bus 2 is
PB10/PB3, because PB9 and PB11 are gone. SPI2 on PB13 to PB15, the
ESP32-C6 link on PB10/PB12, the ADC channels and every alternate function
are the STM32H523's.

Clock tree: as on the STM32H523, an 8 MHz crystal → PLL1 (M=4, N=250) →
500 MHz VCO, P=2 → 250 MHz SYSCLK with every APB bus at 250 MHz, Q=5 →
100 MHz for SPI2; voltage scale 0, 5 flash wait states, the longer
programming delay, prefetch and the instruction cache on. Without the
crystal, HSI 32 MHz with M=16 gives the same 250 MHz.

Console: USARTDIV rounds to 271 against the 250 MHz APB1, 0.10% fast. The
ESP32-C6 link runs SPI2 at 12.5 MHz through GPDMA1 channels 0 and 1.

Limits that follow from the clocks: the timers count at 250 MHz, so the
longest timer period is 17.1 s. A program's SPI2 runs from 100 MHz, so its
slowest rate is 390.6 kHz.

Programming: `make flash` uses st-flash and updates the settings in their
8 KiB sector; `make bootloader` is USB DFU through the board's USB-C socket
(hold BOOT0, tap NRST), and `make BOARD=stm32h562 dfu` packs a DfuSe file
for it (`0483:df11`). `make openocd` uses `target/stm32h5x.cfg`, which
OpenOCD 0.12 does not ship. Rust programs need the
`thumbv8m.main-none-eabihf` target.

Flash — 8 KiB sectors, two banks of 512 KiB, programmed 16 bytes at a time;
the STM32H523's map in 1 MiB:

```
0x08000000  +--------------------------------+
            |  Freya kernel                  |  48 KiB, sectors 0..5
0x0800C000  +--------------------------------+
            |  system settings               |  1 KiB, a sector of their own
0x0800C400  +--------------------------------+
            |  unused                        |
0x08010000  +--------------------------------+
            |  kernel extension              |  128 KiB
0x08030000  +--------------------------------+
            |  program flash region          |  832 KiB, bank 2 from
0x08100000  +--------------------------------+  0x08080000
```

RAM — SRAM1, SRAM2 and SRAM3 are one block, in the STM32U585's shape:

```
0x20000000  +--------------------------------+
            |  .data + .bss + system heap    |  124 KiB
0x2001F000  +--------------------------------+
            |  thread stacks, 4 x 1 KiB      |
0x20020000  +--------------------------------+
            |  user program region (504 KiB) |  image + .bss, loaded from
0x2009E000  +--------------------------------+  card, or just .data + .bss
            |  shell stack (6 KiB)           |  the program's main thread
0x2009F800  +--------------------------------+
            |  interrupt stack (2 KiB)       |
0x200A0000  +--------------------------------+
```

A BusFault ram dump is 640 KiB.

## STM32H723 (WeAct MiniSTM32H723, STM32H723VGT6)

Build with `make BOARD=stm32h723`.

The board is WeAct's
[MiniSTM32H723](https://github.com/WeActStudio/WeActStudio.MiniSTM32H723):
an STM32H723VGT6 (Cortex-M7 with a double-precision FPU, 1 MiB of flash,
564 KiB of SRAM) in an LQFP100 with a 25 MHz crystal, the LED on PE3
(active high), KEY on PC13, a microSD slot, an 8 MiB SPI NOR on SPI1, an
8 MiB OSPI NOR, an ST7735 LCD on SPI4 and a camera connector. It is not
the Black Pill's layout, but the pins Freya gives programs on the other
boards are free here too: the console on PA2/PA3, PWM on PA0, PA1, PB0,
PB1 and PB6 to PB9, I2C on PB6/PB7 and PB10/PB11 (the LQFP100 has PB11),
SPI2 on PB13 to PB15 and the ESP32-C6 link on PB10/PB12. Programs may name
pins on ports A to E.

The card: the slot is wired to SDMMC1, which no SPI peripheral reaches,
so the board drives the card in SPI mode by hand on the same pins: CS on
DAT3 (PC11), SCK on CLK (PC12), MOSI on CMD (PD2), MISO on DAT0 (PC8)
(`BOARD_SD_BITBANG`; `sdspi_*()` are in `boards/stm32h723/board.c`).
Identification runs at about 250 kHz, and data as fast as the pins
toggle, which is under the card's 25 MHz. The slot has no supply switch:
`power sd off` unmounts and lets go of the pins, and the card stays
powered. Freya keeps PC8 to PC12 and PD2.

The SPI flash: the 8 MiB NOR has SPI1 to itself (SCK PB3, MISO PB4, MOSI
PD7, chip select PD6), so it is mounted at `/spi1` beside the card rather
than in its absence (`BOARD_SPIFLASH_OWN_BUS`). The shell starts in `/` when
there is a card and in `/spi1` when there is not. Freya keeps PB3, PB4,
PD6 and PD7. PB6, a PWM and I2C pin, is also the OSPI NOR's chip select;
that chip ignores it while its clock (PB2) is still. PB7 to PB9 also go to
the camera connector.

Peripherals: the USART and the FIFO SPI are the U5's blocks and use the
same paths in `src/`. EXTI is the F4's model, with the port chosen in
SYSCFG and lines 5 to 9 and 10 to 15 sharing an interrupt; the H7's
registers are given the F4's names. The ESP32-C6 link uses DMA1 streams 3
and 4, the F4's, with SPI2's requests routed to them through DMAMUX1
(`BOARD_ESP_DMAMUX`). The ADC is split: ADC1 (16 bits, run at 12) reads
the pins, and ADC3 the temperature sensor (channel 17) and Vref
(channel 18); PC2 and PC3 are left out of `adc`.

Clock tree: a 25 MHz crystal → PLL1 (M=5, N=104) → 520 MHz VCO, P=1 →
520 MHz core, HCLK (AXI and AHB) 260 MHz, every APB bus 130 MHz, Q=6 →
86.7 MHz for SPI1 and SPI2. LDO supply, voltage scale 0, 3 flash wait
states. 520 MHz is the most scale 0 allows without the CPU_FREQ_BOOST
option byte, which Freya leaves alone. Without the crystal, HSI 64 MHz with
M=16 and N=130 gives the same 520 MHz. The instruction cache is on, and
the loader and the flash driver invalidate it after writing code
(`BOARD_ICACHE`); the data cache is off, so DMA needs no cache upkeep.
`sysinfo` reports the core clock as AHB, because SysTick and the
microsecond delay count core cycles. That delay reads the DWT cycle
counter here (`BOARD_DELAY_CYCCNT`) rather than counting a loop: the M7
dual-issues, so a nop loop's cycles a pass are not a constant, and 1-Wire
and I2C need the microseconds right.

Console: USARTDIV rounds to 141 against the 130 MHz APB1, 0.04% fast. The
ESP32-C6 link runs SPI2 at 21.7 MHz.

Limits that follow from the clocks: the timers count at 260 MHz, so the
longest timer period is 16.5 s. A program's SPI2 runs from 86.7 MHz, so its
slowest rate is 338 kHz.

Programming: `make flash` uses st-flash and updates the settings in sector
1; `make openocd` uses `target/stm32h7x.cfg`; `make bootloader` is USB DFU
through the board's USB-C socket (hold BOOT0, tap NRST), and
`make BOARD=stm32h723 dfu` packs a DfuSe file for it (`0483:df11`). Rust
programs use the `thumbv7em-none-eabihf` target.

Flash — eight 128 KiB sectors in one bank, programmed 32 bytes at a time,
in the F4's order:

```
0x08000000  +--------------------------------+
            |  Freya kernel                  |  128 KiB, sector 0
0x08020000  +--------------------------------+
            |  system settings               |  first 1 KiB of sector 1
0x08020400  +--------------------------------+
            |  unused                        |  rest of sector 1
0x08040000  +--------------------------------+
            |  program flash region          |  640 KiB, sectors 2..6
0x080E0000  +--------------------------------+
            |  kernel extension              |  128 KiB, sector 7
0x08100000  +--------------------------------+
```

RAM — everything is in the 320 KiB of AXI SRAM, which DMA1 reaches; the
128 KiB of DTCM is left unused. The reset handler writes all of it once,
eight bytes at a time, so the ECC is valid before anything reads it:

```
0x24000000  +--------------------------------+
            |  .data + .bss + system heap    |  92 KiB
0x24017000  +--------------------------------+
            |  thread stacks, 4 x 1 KiB      |
0x24018000  +--------------------------------+
            |  user program region (216 KiB) |  image + .bss, loaded from
0x2404E000  +--------------------------------+  card, or just .data + .bss
            |  shell stack (6 KiB)           |  the program's main thread
0x2404F800  +--------------------------------+
            |  interrupt stack (2 KiB)       |
0x24050000  +--------------------------------+
```

A BusFault ram dump is 320 KiB, from `0x24000000`.

## Blue Pill (STM32F103C8T6)

Build with `make BOARD=bluepill`.

The Blue Pill boots the same way as the Black Pill, on three quarters of the
clock and a fifth of the RAM:

```
Freya 3.3.0 "Poltergeist" for STM32F103C8T6
72 MHz, power-on reset. Type 'help'.

[boot] clocks     : HSE 8 MHz crystal + PLL, sysclk 72 MHz, flash 2 WS
[boot] console    : USART2 921600 8N1 on PA2/PA3
```

Clock tree: an 8 MHz crystal → PLL ×9 → 72 MHz SYSCLK, 36 MHz APB1, 72 MHz
APB2, 2 flash wait states.

Console: USARTDIV rounds to 39 against the 36 MHz APB1. A Blue Pill whose
crystal did not start runs its APB1 at 32 MHz instead and comes out 0.8% slow,
which is still comfortable. The USART would reach 2.25 Mbaud. Card:
identification at 281 kHz, then 9 MHz.

I2C bus 2 is PB10/PB11. The pins have no weak pull-up of their own, so the
I2C and 1-Wire resistors are required.

Programming: the F103 has no USB loader, so `make bootloader` drives the
serial loader in ROM with `stm32flash`: pull BOOT0 high, tap NRST, and add
`PORT=/dev/ttyUSB1` if the adapter is not on `ttyUSB0`. The size register
often still reads 64 KiB; see [Flash size](#flash-size).

What the Blue Pill leaves out for lack of room: the heatshrink coder, the
Ascon cipher, the PDP-11 virtual machine, floats in the shell, `syslog`, and
the `rtc_get` / `rtc_set` calls (its kernel extension had no room, though its
clock still keeps file timestamps). Programs still have `float`, through
`src/softfp.c`. A BusFault ram dump is 20 KiB.

On the Blue Pill the 20 KiB of SRAM is the real limit, not the 128 KiB of
flash: a RAM program gets 7 KiB rather than 56 (9 KiB if it starts no
threads), and the heap is a few hundred bytes instead of sixty KiB.
Installing a program into flash is the answer to the first half of that, not
the second — such a program gets 24 KiB of code, but the heap is still small
and the main thread still uses the shell stack. Ways to make the regions
larger are in [bluepill-memory.md](bluepill-memory.md).

The memory map is the same shape as the Black Pill's, squeezed into a fifth of
the RAM. The top 8 KiB holds two thread stacks, the shell stack and the
interrupt stack, and the heap takes whatever `.bss` leaves behind:

```
0x08000000  +--------------------------------+
            |  Freya kernel (~47.9 KiB used) |  48 KiB, pages 0..47
0x0800C000  +--------------------------------+
            |  program flash region          |  24 KiB, pages 48..71
            |                                |  installed from the card
0x08012000  +--------------------------------+
            |  kernel extension              |  55 KiB, pages 72..126
0x0801FC00  +--------------------------------+
            |  system settings               |  1 KiB, page 127
0x08020000  +--------------------------------+

0x20000000  +--------------------------------+
            |  .data + .bss + system heap    |  7 KiB; the buffers alone
0x20001C00  +--------------------------------+  are 6.4 KiB of it
            |  user program region (7 KiB)   |  image + .bss loaded from
0x20003800  +--------------------------------+  card, or just .data + .bss
            |  thread stacks, 2 x 1 KiB      |  or the RAM of a program
0x20004000  +--------------------------------+  that starts no threads
            |  shell stack (2560 B)          |  the program's main thread
0x20004A00  +--------------------------------+
            |  interrupt stack (1536 B)      |
0x20005000  +--------------------------------+
```

The kernel extension (the thread scheduler, the shell's script interpreter,
its variables and functions, XMODEM and the SPI master) is flashed as its own
image. Why the flash is split this way is in
[flash-programs.md](flash-programs.md).
