# The calendar RTC

What `make RTC=internal` builds in: a driver for the chip's own real time
clock, which keeps civil time across a reset, and across a power cut when
VBAT has a battery. It does for these boards what `make RTC=ds3231` does
with an external DS3231 ([i2c.md](i2c.md)), without any wiring.

## Boards

The driver, `src/rtc.c`, is for the calendar RTC that the F4, U5, H5 and
H7 have in common, run from the 32.768 kHz crystal (LSE):

| Board | `make` |
|---|---|
| Black Pill (STM32F411CEU6) | `make RTC=internal` |
| STM32F405xx | `make BOARD=stm32f405 RTC=internal` |
| STM32U585 | `make BOARD=stm32u585 RTC=internal` |
| STM32H523 | `make BOARD=stm32h523 RTC=internal` |
| STM32H562 | `make BOARD=stm32h562 RTC=internal` |
| STM32H723 | `make BOARD=stm32h723 RTC=internal` |

The Blue Pill and the Black Pill 2 have the F103's RTC, a plain seconds
counter, which this driver does not drive; `RTC=internal` is refused for
them, and `RTC=ds3231` is their clock. A board allows the option with
`RTC_INTERNAL := 1` in its `board.mk`, and its `board.h` names the RTC's
base address, where its CR is, and `board_rtc_access()`, which turns on
the clocks the RTC registers need and opens the backup domain for
writing. `RTC=internal` and `RTC=ds3231` are one variable, so an image has
one or the other. Leave `RTC` unset and neither is compiled.

## What it does

Freya still counts civil time from SysTick, as without the option. The
RTC is copied into that count at boot, and written when the clock is set:

- At boot, one line says what the RTC holds:

  ```
  [boot] RTC        : 2026-10-03 21:05:09
  ```

  `not set; set it with date()` means the RTC runs but nobody has set
  it since its backup domain last lost power. `no 32.768 kHz crystal`
  means the crystal did not start within two seconds; the RTC is then
  left off, and the software clock starts at 2026-01-01 as usual. A
  board whose RTC is off has its crystal started here, so the RTC
  counts from the first date that is set.
- `date("YYYY-MM-DD HH:MM:SS")` and a program's `rtc_set()` write the
  RTC first and then the software clock. A date the RTC cannot hold is
  refused and nothing changes. If the RTC does not respond, the software
  clock is set all the same and `date` says the RTC was not written.
- `date()` with no arguments copies the RTC into the software clock
  again before it prints.

The RTC keeps the time in BCD, 24 hour format, with a two digit year, so
it holds 2000-01-01 to 2099-12-31. The prescalers are set for 1 Hz from
32768 Hz (127 and 255), and the weekday it also keeps is worked out from
the date. The crystal's source can only be changed by resetting the
backup domain; that is done when another source had been chosen, and
otherwise the backup domain is left alone.

## Hardware

The crystal goes on PC14/PC15. The WeAct boards here (the Black Pill and
the U585, H523, H562 and H723 boards) have it fitted; an STM32F405 board may
not, and then boot says so and the image runs on as without the option.

The RTC keeps time as long as its backup domain has power. While the
board is powered that comes from 3.3 V; through a power cut it has to
come from a battery on VBAT. Without one the RTC keeps the time through a
reset and `reboot()`, and loses it with the power.

## Connecting a battery

On every WeAct board here VBAT is fed through a BAT54C, a pair of
Schottky diodes with a common cathode: one from the board's 3.3 V, one
from a header pin. Whichever is higher feeds the RTC, so the battery is
not drawn on while the board is powered, and the board never pushes
current into the battery.

```
   board 3.3 V  ──►|──┐
                      ├──  VBAT (the MCU pin, with 100 nF to GND)
   header pin   ──►|──┘
                 BAT54C
```

| Board | Battery + | Battery − |
|---|---|---|
| Black Pill (STM32F411CEU6) | `VB`, at the end of the header row, next to PC13 | any GND pin |
| STM32U585 | `VB`, at the end of the header row, next to PC13 | any GND pin |
| STM32H523 | `VB`, at the end of the header row, next to PC13 | any GND pin |
| STM32H562 | `VB`, pin 1 of header P1, next to PC13 | any GND pin |
| STM32H723 | `VBAT`, pin 8 of the 2x22 header P2, between PE5 and NRST (net `VBAT_Pin` in the schematic) | any GND pin, such as P2 pin 15 |

The pins are named as in WeAct's schematics (Black Pill V2.0 to V3.1,
STM32U585Cx, STM32H523CxTx, STM32H5 64-pin V1.1, MiniSTM32H723 V1.2); check the board's own
print before soldering.

1. Take a 3 V lithium coin cell, CR2032 or CR1220, in a holder with
   wires. The VBAT pin accepts 1.65 V to 3.6 V.
2. Connect its + to the pin in the table and its − to GND. Nothing
   else is needed: the diode and the capacitor are on the board.
3. Power the board, and set the clock once: `date("YYYY-MM-DD HH:MM:SS")`.
   From then on boot prints the time it kept.

Things not to do:

- Do not use a rechargeable LIR2032 or LIR1220: a charged one is 4.2 V,
  over VBAT's 3.6 V. The board cannot charge a cell anyway; the diode
  blocks it.
- Do not use a supercapacitor for the same reason: nothing charges it.
- Do not connect the cell to the 3.3 V pin, or to VBAT past the diode on
  a board you have changed: the cell would then try to power the board.

The backup domain with the crystal draws about a microampere, so a
CR2032 (about 220 mAh) outlasts the board. If the cell is removed or flat
while the board is unpowered, the RTC stops: boot then says `not set` or,
if the backup domain was reset, starts the crystal again, and the clock
has to be set again.

On an STM32F405 board, look up how VBAT is wired: if it is tied straight
to 3.3 V, a battery needs the same pair of diodes, or the board's 3.3 V
would charge it and the battery would feed the board. The STM32
datasheets ask for VBAT to be tied to VDD when there is no battery.

## Tests

`make test` compiles `src/rtc.c` for each of these boards against a
stand-in register block and checks the BCD it writes and reads, the
weekday, the prescalers, the 24 hour format, the 2000..2099 range, an RTC
that was never set, one that never syncs, a crystal that never starts and
an RTC that never enters init mode, and what boot does in each case.
