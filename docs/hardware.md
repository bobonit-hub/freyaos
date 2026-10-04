# Hardware

How to wire a board for Freya: the console, the LED, the SD card slot and its
power switch, the SPI flash footprint, the pins Freya keeps, and the pins a
program can use for PWM, I2C, a DS3231, 1-Wire and SPI. What differs from one
board to another (clocks, maps, divisors) is in [boards.md](boards.md).

## Console

The console is wired the same way on every board.

| Function | Pin | Connect to |
|---|---|---|
| Console TX | PA2 | RX of a 3.3 V USB-serial adapter |
| Console RX | PA3 | TX of the adapter |
| Status LED | PC13 | on board, active low |

The console shell is on USART2 at 921600 8N1, interrupt driven, with line
editing and command history. 921600 is the fastest rate every common adapter
agrees on: a CP2101, a CP2102 and an FT232 all list it, where 1 Mbaud is
already the CP2102's ceiling and past a CP2101 entirely. No board divides it
exactly, but every one lands within 0.16%, far inside what 8N1 tolerates; the
divisor each board uses, and how fast its USART would go, are in
[boards.md](boards.md#console-rate). If the adapter is a faster one, the rate
is `uart_init()` in `src/main.c` and the `BOARD_CONSOLE_NAME` string. Nothing
drives RTS or CTS, so leave hardware flow control off on the host, and ground
the adapter with the board.

## SD card and SPI flash

The card is used only by a kernel built with `make SD=1`
([building.md](building.md#the-sd-card)); the wiring below is the same
either way. The card is SPI1 on PA4 to PA7 on every board. Those pins sit in different
places on the headers; the slot drawings are in [sd-slot.txt](sd-slot.txt).
VDD is switched: PA8 drives the gate of a P-channel MOSFET, low to power the
socket. A pull-down on that gate keeps the card on through reset. The
socket's supply is a power domain: `power sd off` drops VDD, `power sd on`
brings it back, and a program does the same with
`api->power(FREYA_PWR_SD, on)` (see
[console-commands.md](console-commands.md#socket-power)).

The identification and data clocks of the card bus are per board, in
[boards.md](boards.md#sd-card-clock).

On the Black Pill and the STM32U585 and STM32H523 boards the SOP-8 footprint on the back
shares that bus, with chip select on PA4 like the card. A SPI NOR
chip fitted there, and no card in the socket, is mounted at `/spi1` as
LittleFS, with the same file calls. A blank chip is formatted on the first
mount. The Black Pill 2 has no SPI flash volume: its footprint is wired to
the chip's SPIM pins, not SPI1.

The STM32H723 board is wired differently: its own microSD slot, driven in
SPI mode on the SDMMC1 pins, and its own 8 MiB SPI NOR on SPI1 with a
chip select of its own, mounted at `/spi1` beside the card. Its pins are
in [boards.md](boards.md#stm32h723-weact-ministm32h723-stm32h723vgt6).
The STM32H562 board drives its microSD slot the same way, on the same
pins, and has no SPI flash
([boards.md](boards.md#stm32h562-weact-stm32h5-64-pin-core-board-stm32h562rgt6)).

## Reserved pins

Freya keeps seven pins: PA2 and PA3 for the console, PA4 to PA7 for the
card, and PA8 for its supply. Every other pin of ports A, B and C is a
program's to drive or take interrupts on. The STM32H723 board keeps PA2
and PA3, PC8 to PC12 and PD2 for the card, and PB3, PB4, PD6 and PD7 for
the SPI flash, and gives programs ports A to E. The STM32H562 board keeps
PA2 and PA3, and PC8 to PC12 and PD2 for the card, and gives programs
ports A to D. A build with `make USB=1` also keeps PA11 and PA12, the USB
socket's data lines ([usb.md](usb.md)).

## PWM

Eight of the spare pins — PA0, PA1, PB0, PB1 and PB6 to PB9, the same on
every board but the STM32H562, which has PB5 in place of PB9 — have a
timer channel behind them and can be driven as PWM.
[interrupts.md](interrupts.md) has which timer and channel each one is.

## I2C

Freya speaks I2C as a master, 7-bit, from 10 kHz up to 400 kHz, including the
repeated start a register read needs. The clock is that rate or a little
slower when a microsecond cannot land on it.

I2C uses two more of the spare pins, and one pair that is not. Bus 1 is PB6
(SCL) and PB7 (SDA) on every board. Bus 2 is PB10/PB11 on the Blue Pill, the
Black Pill 2, the STM32F405xx and the STM32H723, PB10/PB9 on the Black Pill, the
STM32U585 and the STM32H523, which have no PB11, and PB10/PB3 on the
STM32H562, which has neither. Both lines are open drain and need a pull-up to 3.3 V; 4.7 kΩ is the
usual value. The Black Pill, the STM32F405xx, the STM32U585, the STM32H523,
the STM32H562 and the STM32H723 also turn on the pin's own weak pull-up; the Blue Pill and the Black Pill 2 cannot, so the resistors are required there. A
pin that is already a PWM output is not also an I2C pin until that channel is
turned off. `i2c 1 scan` at the console and `samples/i2c` do the same thing;
[i2c.md](i2c.md) is the reference.

## DS3231

Freya can keep civil time on a DS3231 when the image is built with
`make RTC=ds3231`. The chip uses the I2C bus 1 pair and no other MCU pin.
SCL is PB6, SDA is PB7, and the chip address is 0x68. Leave 32 kHz, INT/SQW
and RST unconnected. The driver is compiled only with `make RTC=ds3231`. At
boot a valid chip is copied into the software clock, and `date` writes both;
the wiring and what `date` does with the chip are in [i2c.md](i2c.md).

The boards with a calendar RTC can keep the time in the chip instead,
with `make RTC=internal`: the 32.768 kHz crystal on PC14/PC15 is all it
needs, and a coin cell on the board's `VB` pin keeps it through a power
cut; where that pin is on each board, and which cells to use, is in
[rtc.md](rtc.md#connecting-a-battery).

## 1-Wire

1-Wire runs at standard speed on any spare pin: presence, byte reads and
writes, and the ROM search. It uses one spare pin, open drain, with a pull-up
to 3.3 V. 4.7 kΩ is the usual value. The Black Pill, the STM32F405xx, the
STM32U585, the STM32H523, the STM32H562 and the STM32H723 also turn on the pin's own
weak pull-up; the Blue Pill and the
Black Pill 2 cannot, so the resistor is required there. A
pin that is already a PWM output or an I2C line is not also a 1-Wire pin
until that is turned off. Up to four pins may be open at once. `w1 PB12
search` at the console lists the ROMs, and `samples/w1` reads a DS18B20;
[w1.md](w1.md) is the reference.

## SPI for programs

Freya speaks SPI as a master, 8-bit, modes 0 to 3. SPI for a program is SPI2,
the same three pins on every board: SCK on PB13, MISO on PB14 and MOSI on
PB15. The card keeps SPI1, so a program's bus is not the socket. Chip select
is any other spare pin, driven with the pin calls; `pin PB12 0`, then
`spi 1 x 9F FF FF FF`, then `pin PB12 1` is one transfer. MISO is pulled up.
The clock is the fastest of eight taps — power-of-two divisions of the bus
clock — that does not go faster than the rate asked for, from 187.5 kHz to
24 MHz. A pin that is already PWM, I2C or 1-Wire is not also an SPI pin until
that is turned off. `spi 1 1000000` at the console and `samples/spi` do the
same thing; [spi.md](spi.md) is the reference.

An optional ESP32-C6 network coprocessor sits on the same SPI2. While Wi-Fi is
on, the C6 owns SPI2; turning it off returns the bus to the SPI API. The C6
wiring is in [network.md](network.md).
