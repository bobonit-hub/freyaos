# i2c

The worked example for the I2C master calls. With no address it walks the
bus and prints every device that acknowledges; with an address it reads one
byte, and with a register byte it writes that first and then reads, which is
how a sensor's register is read.

The API it uses is described in `docs/i2c.md`.

## Wiring

SCL and SDA each need a pull-up to 3.3 V. 4.7 kΩ is the ordinary value; 2.2 kΩ
is happier at 400 kHz, and the sample itself runs at 100 kHz. The Black Pill
also enables its weak internal pull-ups, which can be enough for one device on
a short pair of wires and are not a substitute for the resistors.

Bus 1, the default, is the same on both boards:

| | SCL | SDA |
|---|---|---|
| bus 1 | PB6 | PB7 |

Bus 2 is the second controller. On the Blue Pill that is PB10 and PB11. On the
Black Pill the package has no PB11, so SDA is PB9. `i2c` with no arguments
prints the pair the board you are looking at actually uses.

PB6 and PB7 are also PWM pins, and on the Black Pill so is PB9. A pin that is
already a PWM output cannot be opened as I2C until `pwm <pin> off`.

## Run

```
freya:/> run i2c.bin
--- i2c starting (Ctrl-C stops it) ---
i2c: scanning bus 1 at 100000 Hz
  0x68
i2c: 1 device

--- i2c returned, exit status 0, 18 ms ---
```

```
run i2c.bin              # scan bus 1
run i2c.bin 2            # scan bus 2
run i2c.bin 1 0x68       # read one byte from 0x68
run i2c.bin 1 0x68 0x75  # write 0x75, then read one byte
```

An address may be decimal or `0x`. Nothing answering is exit status 1 and
`no answer`. A bus the console already has open is `bus or its pins are taken`;
`i2c 1 off` releases it.

## Cleaning up

The sample closes the bus, which returns the pins to inputs. Freya does that
anyway when the run ends. A bus opened at the console with `i2c 1 100000` is
not the program's and stays open.
