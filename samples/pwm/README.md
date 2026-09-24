# pwm

The worked example for the PWM half of the service table. A timer channel
drives a pin, and the program does nothing but decide what the duty cycle
should be next: the toggling itself is the hardware's, which is the whole
reason to use PWM rather than a pin and a delay.

It runs in two shapes. By default it fades an LED on PB6 up and down at
1 kHz; with `servo` it sweeps a servo on the pin instead, at 50 Hz, by pulse
width.

The API it uses is described in `docs/interrupts.md`.

## Wiring

An LED and a resistor between the pin and ground, or a servo's signal wire on
it. Neither has to be connected to watch the sample run — the percentage it
prints is the duty cycle it is asking for — but PB6 with an LED on it is the
version worth looking at.

A servo needs more current than the board's regulator wants to give: power it
from its own 5 V supply and tie that supply's ground to the board's, with only
the signal wire going to the pin.

## Run

```
freya: run pwm.bin
--- pwm starting (Ctrl-C stops it) ---
pwm: PB6 at 1000 Hz, fading an LED. Ctrl-C stops it
  64.00%
pwm: channel closed, the pin is an input again

--- pwm stopped by Ctrl-C, exit status 130, 8122 ms ---
```

```
run pwm.bin            # fade an LED on PB6 at 1 kHz
run pwm.bin B7         # the same on PB7
run pwm.bin B7 200     # and at 200 Hz, slow enough to see it flicker
run pwm.bin A0 servo   # sweep a servo on PA0 instead
```

| Argument | |
|---|---|
| pin | a port letter and a number, `A0` to `C15`, either case |
| hz | 1 to 1000000; anything that is not a number falls back to 1000 |
| `servo` | 50 Hz, and the duty cycle set as a 1000 to 2000 µs pulse |

## The pins

Eight pins have a timer channel behind them, the same eight on both boards:

| Pin | | Pin | | Pin | | Pin | |
|---|---|---|---|---|---|---|---|
| PA0 | TIM2 CH1 | PB0 | TIM3 CH3 | PB6 | TIM4 CH1 | PB8 | TIM4 CH3 |
| PA1 | TIM2 CH2 | PB1 | TIM3 CH4 | PB7 | TIM4 CH2 | PB9 | TIM4 CH4 |

Any other pin ends the run with 1 and `no timer channel on that pin`
(`FREYA_ERR_PIN`), and `pwm` at the console lists the table. On a Black Pill
PA0 is also the KEY button, which holds the pin low while it is pressed.

The three timers are the ones `timer_open()` hands out as well, so a timer
driving a pin cannot also be a program's periodic interrupt, and all the
channels of one timer run at one frequency: PB6 and PB9 always share, PB6 and
PB0 never do. Asking a second pin on the same timer for a different frequency
is refused with `FREYA_ERR_BUSY` — here, `its timer is taken`.

## Two ways to say the same thing

A duty cycle is a fraction of the period in ten-thousandths, so
`FREYA_PWM_FULL` is fully on and half of it is half:

```c
api->pwm_duty(pwm, level * level);       /* level 0..100 -> 0..10000 */
```

The square is the fade curve. The eye is roughly logarithmic, so stepping the
duty cycle evenly makes a fade that spends most of its time looking bright;
squaring a 0..100 level both fixes that and lands exactly on the range the
call wants.

A servo is specified as a pulse width instead, and says so:

```c
api->pwm_pulse_us(pwm, 1500);            /* 1.5 ms of every 20 */
```

which is worked out from the timer clock rather than from the duty fraction,
so it lands on 312 ns steps rather than on ten-thousandths of a 20 ms frame.

## Cleaning up

The sample closes its channel, which stops the pin and makes it an input
again. Freya does that anyway when a run ends — Ctrl-C and faults included —
so nothing a program leaves behind keeps driving a motor once the shell is
back. A channel started at the console with `pwm PB6 1000 25` is not a
program's and is left alone; `pwm PB6 off` stops that one.

## Build

```sh
make                   # build/blackpill/samples/pwm.bin and .xip.bin
make BOARD=bluepill    # build/bluepill/samples/pwm.bin and .xip.bin
```

The image is under a kilobyte, so it fits the program RAM region on either
board. Copy the `.bin` onto the card (or `download` it over XMODEM) and `run`
it, or install the flash image so it needs no card:

```
freya: install pwm.xip.bin
freya: runflash
```
