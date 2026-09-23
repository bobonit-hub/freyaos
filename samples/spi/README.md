# spi

The worked example for the SPI master calls. With no arguments it clocks
eight bytes out of MOSI and checks that MISO brought the same bytes back,
which is a wire from PB15 to PB14. With `id` it reads the JEDEC id of a
SPI flash on chip select PB12, or on a pin you name.

The API it uses is described in `docs/spi.md`.

## Wiring

Bus 1 is the same on both boards. It is SPI2. The card's socket is SPI1
and is not these pins.

| | Pin |
|---|---|
| SCK | PB13 |
| MISO | PB14 |
| MOSI | PB15 |

MISO has a pull-up, so a loopback with the wire missing reads `FF`. A
flash chip needs its chip select on a spare pin, held high until the
transfer. PB12 is the one the sample uses when you do not name another.
Mode 0 at 1 MHz is what the sample asks for. The divider steps down from
there; `spi 1 1000000` at the console prints the rate the hardware runs.

## Run

```
freya:/> run spi.bin
--- spi starting (Ctrl-C stops it) ---
  00 -> 00
  ff -> ff
  a5 -> a5
  5a -> 5a
  01 -> 01
  80 -> 80
  0f -> 0f
  f0 -> f0
spi: loopback ok

--- spi returned, exit status 0, 2 ms ---
```

```
run spi.bin           # MOSI tied to MISO
run spi.bin id        # JEDEC id, chip select PB12
run spi.bin id PB10   # the same, chip select PB10
```

A bus the console already has open is `bus or its pins are taken`;
`spi 1 off` releases it.

## Cleaning up

The sample closes the bus, which returns the three pins to inputs. Freya
does that anyway when the run ends. A bus opened at the console with
`spi 1 1000000` is not the program's and stays open. A chip select the
sample drove as an output is left as an output, idle high.
