# I2C

A program can drive the board's I2C pins as a master: open a bus at a
speed, write, read, or write and then read with the repeated start kept
inside one call. It is polled, like SPI. There is no slave mode and no
interrupt. The console drives the same calls with `i2c`.

`samples/i2c` scans a bus and reads a register. Build it with `make` and run
`run i2c.bin`.

## Calls

```c
int (*i2c_open)(int bus, uint32_t hz);          /* 0, or a FREYA_ERR_* */
int (*i2c_close)(int bus);
int (*i2c_write)(int bus, int addr, const void *buf, int len);
int (*i2c_read)(int bus, int addr, void *buf, int len);
int (*i2c_transfer)(int bus, int addr,
                    const void *tx, int txlen, void *rx, int rxlen);
```

`bus` is 1 for the first controller and 2 for the second. `addr` is the
7-bit address, so a device whose datasheet says `0xD0` write / `0xD1` read
is opened as `0x68`. A length of zero writes the address and stops, which is
how a scan asks "is anyone there?" without handing the device a byte it
would treat as a register.

`i2c_transfer()` with both buffers is the ordinary register read. The stop
that would otherwise fall between the write and the read is not sent, so the
device does not forget which register it was just shown:

```c
uint8_t reg = 0x75, who = 0;
api->i2c_open(1, 100000);
api->i2c_transfer(1, 0x68, &reg, 1, &who, 1);   /* WHO_AM_I */
api->i2c_close(1);
```

`i2c_write()` and `i2c_read()` are that call with one side empty. Speeds are
10 kHz to 400 kHz (`FREYA_I2C_MIN_HZ`, `FREYA_I2C_MAX_HZ`). A transfer is at
most 255 bytes (`FREYA_I2C_MAX_LEN`). Opening a bus that is already open, by
the same side, programs a new speed.

These calls were appended to the service table. A program built against this
header and handed an older kernel checks before it calls:

```c
if (!FREYA_API_HAS(api, i2c_transfer)) {
    api->puts("this kernel has no I2C\r\n");
    return FREYA_EXIT_FAIL;
}
```

## Pins

Bus 1 is the same pair on both boards. Bus 2 is the second controller, and
the Black Pill's package does not bond the pin the Blue Pill uses for its
data line, so the two boards do not share it. `i2c` at the console prints
the pins of the image that is running.

| | Black Pill | Blue Pill |
|---|---|---|
| bus 1, SCL / SDA | PB6 / PB7 | PB6 / PB7 |
| bus 2, SCL / SDA | PB10 / PB9 | PB10 / PB11 |

PB6 and PB7 are also PWM pins, and on the Black Pill so is PB9. A pin can be
one of those at a time: opening I2C while PWM still drives it, or while the
pin is a 1-Wire bus, or the other way round, returns `FREYA_ERR_BUSY`.

Both lines are open drain and need a pull-up to 3.3 V. 4.7 kΩ is the ordinary
choice, 2.2 kΩ once the bus is long or at 400 kHz. The Black Pill turns on
the pin's own pull-up as well, about 40 kΩ, which will sometimes carry one
device on a short run of wire and will not carry a bus. The Blue Pill cannot
turn a pull-up on for an output, so the resistors are not optional there.

## What a transfer does

The call does not return until the bytes have moved or the bus has had long
enough. A missing pull-up, a stuck line or a slave that stretches the clock
past the budget comes back as `FREYA_ERR_TIMEOUT`, and the pins are clocked
free so the next call starts clean. An address or a written byte that is
not acknowledged is `FREYA_ERR_NACK`, which is what a scan sees for every
empty address and is not a fault.

The lines are ordinary open-drain GPIO, the same code on both boards. Each
half of the clock is a whole number of microseconds, rounded up, so the bus
is the speed that was asked for or a little slower and never faster. At
100 kHz that lands exactly. At 400 kHz a microsecond is too coarse and the
clock runs at 250 kHz. A slave that holds SCL down is waited on, so clock
stretching still works.

A bus a program opened is closed when the run ends, including by Ctrl-C or a
fault, and the pins go back to inputs. A bus opened at the console stays
open, and a program that tries to open it is told the bus is taken.

The calls spin and they are not reentrant, so a handler may not make them.
The kernel returns `FREYA_ERR_HANDLER`.

## Errors

| | |
|---|---|
| `FREYA_ERR_BUSY` | the bus is open from the other side, or a pin is already PWM or 1-Wire |
| `FREYA_ERR_ARG` | no such bus, not open, speed, address or length out of range |
| `FREYA_ERR_NACK` | the address or a written byte was not acknowledged |
| `FREYA_ERR_TIMEOUT` | the bus did not finish |
| `FREYA_ERR_HANDLER` | called from a handler |
| `FREYA_ERR_IO` | a bus error, or the run was asked to stop mid-transfer |

## At the console

`i2c` with no arguments lists the buses, the pins and whether each is open.
The same commands a program can make:

```
freya:/> i2c 1 100000
I2C1  100000 Hz
freya:/> i2c 1 scan
68
freya:/> i2c 1 0x68 w 0x75 r 1
68
freya:/> i2c 1 off
I2C1 off
```

`scan` prints each address that answered, from `0x08` to `0x77`. `w` takes
up to 32 bytes, each decimal or hex. `r` takes a count. Ctrl-C abandons a
scan. A bus left open here is still open after a program runs, until
`i2c <bus> off`.
