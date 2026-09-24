# w1

The worked example for the 1-Wire calls. It searches the pin, starts a
conversion on every device it found, and prints a temperature for each
DS18B20 (family `28`) and DS1822 (family `22`). Anything else is named by
its family code. A scratchpad whose CRC does not match is `bad crc`.

The API it uses is described in `docs/w1.md`.

## Wiring

One pin, open drain, with a pull-up to 3.3 V. 4.7 kΩ is the ordinary value.
The sample's default is PB12, which is free on both boards and is not a PWM
pin or an I2C pin. The Black Pill also enables its weak internal pull-up,
which can be enough for one sensor on a short wire and is not a substitute
for the resistor.

```
PB12 ----+---- DQ of the sensor
         |
        4.7 kΩ
         |
        3.3 V
```

Ground the sensor's ground to the board. A sensor with its own supply takes
3.3 V on VDD. A parasite-powered one ties VDD to ground and draws the
conversion current from the data pin; the sample holds that pin high for
750 ms either way.

PB12 can be something else at the time. A pin that is already a PWM output,
or either line of an open I2C bus, stays that until `pwm <pin> off` or
`i2c <bus> off`.

## Run

```
freya: run w1.bin
--- w1 starting (Ctrl-C stops it) ---
w1: PB12
28 aa 14 1e 0b 00 00 9a  21.875 C
w1: 1 device

--- w1 returned, exit status 0, 812 ms ---
```

```
run w1.bin           # PB12
run w1.bin B0        # PB0
run w1.bin PB1       # PB1
```

The pin may be `PB12` or `B12`. Nothing answering is exit status 1 and
`no device`. A pin the console already has open is `that pin is taken`;
`w1 PB12 off` releases it.

## Cleaning up

The sample closes the pin, which returns it to an input. Freya does that
anyway when the run ends. A pin opened at the console with `w1 PB12` is
not the program's and stays open.
