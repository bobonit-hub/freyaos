# SPI

A program can drive the board's SPI pins as a master: open a bus at a
speed and a mode, then shift bytes. Every byte out is a byte in. It is
polled. There is no slave mode and no interrupt. The console drives the
same calls with `spi`.

`samples/spi` loops the bus back to itself, or reads a flash chip's
JEDEC id. Build it with `make` and run `run spi.bin`.

## Calls

```c
int (*spi_open)(int bus, uint32_t hz, int mode);   /* 0, or a FREYA_ERR_* */
int (*spi_close)(int bus);
int (*spi_transfer)(int bus, const void *tx, void *rx, int len);
int (*spi_write)(int bus, const void *buf, int len);
int (*spi_read)(int bus, void *buf, int len);
```

`bus` is 1 for the first controller. `mode` is `FREYA_SPI_MODE0` through
`FREYA_SPI_MODE3`: mode 0 is clock idle low, sampled on the rising edge,
which is what most devices want. A transfer shifts `len` bytes. What
went out is `tx` and what came back is `rx`; either buffer may be left
out, and a read with no `tx` clocks out `0xFF`. `spi_write()` and
`spi_read()` are that call with one side empty.

Chip select is not a pin of the bus. The program drives it, so two
devices can share the wires:

```c
uint8_t tx[4] = { 0x9F, 0xFF, 0xFF, 0xFF };
uint8_t rx[4];

api->spi_open(1, 1000000, FREYA_SPI_MODE0);
api->pin_write(FREYA_PB(12), 1);
api->pin_mode(FREYA_PB(12), FREYA_PIN_OUT);
api->pin_write(FREYA_PB(12), 0);
api->spi_transfer(1, tx, rx, 4);          /* JEDEC id in rx[1..3] */
api->pin_write(FREYA_PB(12), 1);
api->spi_close(1);
```

Speeds are 187.5 kHz to 24 MHz (`FREYA_SPI_MIN_HZ`, `FREYA_SPI_MAX_HZ`).
A transfer is at most 4096 bytes (`FREYA_SPI_MAX_LEN`). Opening a bus
that is already open, by the same side, programs a new speed and mode.

These calls were appended to the service table. A program built against
this header and handed an older kernel checks before it calls:

```c
if (!FREYA_API_HAS(api, spi_transfer)) {
    api->puts("this kernel has no SPI\r\n");
    return FREYA_EXIT_FAIL;
}
```

## Pins

The card keeps SPI1, on PA4 to PA7. A program's bus is SPI2, and it is
the same three pins on both boards. `spi` at the console prints them.

| | Black Pill | Blue Pill |
|---|---|---|
| bus 1, SCK / MISO / MOSI | PB13 / PB14 / PB15 | PB13 / PB14 / PB15 |

MISO is pulled up, so an idle line reads high. SCK and MOSI idle however
the mode left the clock and the last bit. None of the three is a PWM
pin or an I2C pin on either board. A pin can still be only one thing at
a time: opening SPI while a 1-Wire bus, an I2C bus or a PWM channel
still owns one of them, or the other way round, returns `FREYA_ERR_BUSY`.

## What a transfer does

The call does not return until the bytes have moved, including the last
bit leaving the shifter, so the program can raise chip select as soon as
it gets back. The clock is one of eight taps, PCLK/2 down to PCLK/256.
SPI2 is on APB1, which is 48 MHz on the Black Pill and 36 MHz on the
Blue Pill (32 MHz if that board's crystal did not start). The tap used
is the fastest one that does not exceed the rate asked for:

| asked | Black Pill | Blue Pill |
|---|---|---|
| 24 MHz | 24 MHz | 18 MHz |
| 1 MHz | 750 kHz | 562.5 kHz |
| 187.5 kHz | 187.5 kHz | 140.625 kHz |

The console prints the rate it programmed, not only the one it was
asked for. A missing clock or a shifter that never finishes comes back
as `FREYA_ERR_TIMEOUT`, and the controller is set up again so the next
call starts clean.

A bus a program opened is closed when the run ends, including by Ctrl-C
or a fault, and the pins go back to inputs. A bus opened at the console
stays open, and a program that tries to open it is told the bus is
taken.

The calls spin and they are not reentrant, so a handler may not make
them. The kernel returns `FREYA_ERR_HANDLER`.

## Errors

| | |
|---|---|
| `FREYA_ERR_BUSY` | the bus is open from the other side, or a pin is already PWM, I2C or 1-Wire |
| `FREYA_ERR_ARG` | no such bus, not open, speed, mode or length out of range |
| `FREYA_ERR_TIMEOUT` | the transfer did not finish |
| `FREYA_ERR_HANDLER` | called from a handler |
| `FREYA_ERR_IO` | the run was asked to stop mid-transfer |

## At the console

`spi` with no arguments lists the buses, the pins and whether each is
open. The same commands a program can make:

```
freya:/> spi 1 1000000
SPI2  750000 Hz  mode 0
freya:/> pin PB12 out 1
PB12 = 1
freya:/> pin PB12 0
PB12 = 0
freya:/> spi 1 x 0x9F 0xFF 0xFF 0xFF
ff ef 40 18
freya:/> pin PB12 1
PB12 = 1
freya:/> spi 1 off
SPI2 off
```

`x` shifts up to 8 bytes and prints what came back. Chip select is
`pin`: drive it high, then low for the transfer, then high again.
750 kHz is what the Black Pill's divider makes of 1 MHz; the Blue Pill
lands on 562.5 kHz. A bus left open here is still open after a program
runs, until `spi <bus> off`.
