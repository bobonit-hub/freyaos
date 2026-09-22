# irq

The worked example for the pin and timer half of the service table. A
hardware timer raises an interrupt every so many milliseconds and its
handler toggles the LED; a pin raises one on every falling edge of a button
wired between that pin and ground. The program itself never polls: it sleeps
in `api->irq_wait()` and prints what the two handlers counted.

The API it uses is described in `docs/interrupts.md`.

## Wiring

Nothing has to be connected. The timer blinks the on-board LED on its own,
and the pin interrupt can be tested by touching a wire from the pin to any
ground pad — each contact counts as a press.

For a real button, put it between the pin and ground. The sample sets the pin
to `FREYA_PIN_IN_PULLUP` and asks for the falling edge, so the line reads
high until the button is pressed and no external resistor is needed. Bounce
is handled by `FREYA_EDGE_DEBOUNCE`, which takes the first edge and ignores
the rest for 20 ms.

## Run

```
freya:/> run irq.bin
--- irq starting (Ctrl-C stops it) ---
irq: timer every 500 ms, falling edges on PA0 (button to ground)
press the button, or Ctrl-C to stop
  24 ticks, 3 presses, pin high
irq: 24 timer interrupts, 3 presses, 27 in all

--- irq stopped by Ctrl-C, exit status 130, 12431 ms ---
```

The running line is redrawn in place whenever interrupts have arrived since
the last one. The second that `irq_wait()` gives up after is what lets the
loop notice `Ctrl-C` when nothing is arriving at all. Both arguments are
optional:

```
run irq.bin           # PA0, 500 ms
run irq.bin B1        # a button on PB1 instead
run irq.bin B1 100    # and a faster timer
```

| Argument | |
|---|---|
| pin | a port letter and a number, `A0` to `C15`, either case |
| period_ms | 1 to 40000; anything that is not a number falls back to 500 |

The main loop only ends when a stop is requested, so a normal run ends at
`Ctrl-C` with status 130. A pin the kernel keeps, or a period the timer
cannot reach, ends it with 1 instead, and an argument that is not a pin with
2.

Which pins are yours is the same rule as everywhere else: Freya keeps PA2 and
PA3 for the console and PA4 to PA7 for the card, and refuses them with
`FREYA_ERR_PIN` (printed here as `Freya keeps that one`). The interrupt lines
go by pin *number*, not by port, so only one of PA0, PB0 and PC0 can have one
at a time. The sample attaches a single pin and never meets that, but a
program of your own that wanted a button on PA0 and another on PB0 would have
its second `pin_irq_attach()` refused with `FREYA_ERR_BUSY`; PA0 and PB1 are
fine together.

A period is a whole millisecond here because the argument is; `timer_open()`
itself takes microseconds, from 10 µs to 40 seconds. The LED toggles once per
expiry, so the visible blink is half the period rate: `100` gives 5 Hz. A
period the hardware cannot divide down to is refused, and the sample prints
the error it got back — `irq: no timer (-3)` is `FREYA_ERR_ARG`.

## Handlers

Both handlers are two or three instructions, which is the habit that makes
interrupt context easy to live with. `on_press` takes the counter to bump as
its `arg`, so the same function could serve several pins; `on_tick` takes the
service table, because the LED is reached through it.

A handler may print, drive pins, read the clock and touch the timers, but not
allocate memory or use the card — the kernel refuses those rather than let a
handler corrupt the heap or the filesystem it interrupted. It cannot hang the
board either: `Ctrl-C` unwinds a handler that never returns, and the run ends
with the usual status.

The counters are read before the timer is closed and the line detached. Freya
drops both when a run ends however it ended, so nothing the sample leaves
behind can fire into the shell, but a program that carries on afterwards
should give them back itself.

## Build

```sh
make                   # build/blackpill/samples/irq.bin and .xip.bin
make BOARD=bluepill    # build/bluepill/samples/irq.bin and .xip.bin
```

The image is about 1 KiB, so it fits the program RAM region on either board.
Copy the `.bin` onto the card (or `download` it over XMODEM) and `run` it, or
install the flash image so it needs no card:

```
freya:/> install irq.xip.bin
freya:/> runflash
```
