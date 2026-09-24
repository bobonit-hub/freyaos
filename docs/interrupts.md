# Pins, timers, PWM and interrupts

A Freya program can drive the board's spare pins, take an interrupt when one
of them changes, have a hardware timer interrupt it every so many
microseconds, and leave a timer square-waving a pin on its own. All of it is
in the service table (`include/freya_api.h`), so a program uses it the way it
uses `printf` or `open`, and none of it needs the card.

```c
static void on_tick(int timer, void *arg)   /* runs in interrupt context */
{
    ((const freya_api_t *)arg)->led(1);
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int t = api->timer_open(250000, 0, on_tick, (void *)api);   /* 250 ms */

    api->timer_start(t);
    while (!api->should_stop()) api->irq_wait(0);
    return 0;
}
```

`samples/irq` is the worked example: a timer blinking the LED and a button on
a pin, counted and reported. `samples/pwm` is the other one, fading an LED and
sweeping a servo. Build them with `make` and run `run irq.bin`.

## Pins

A pin is its port and its number in one integer, so it can be passed around,
stored, and handed back to a handler as the source of an interrupt:

| | |
|---|---|
| `FREYA_PA(n)`, `FREYA_PB(n)`, `FREYA_PC(n)` | the pin, e.g. `FREYA_PB(0)` |
| `FREYA_PIN_PORT(pin)`, `FREYA_PIN_NUM(pin)` | take it apart again |

```c
int (*pin_mode)(int pin, int mode);     /* FREYA_PIN_IN, _IN_PULLUP, ...  */
int (*pin_read)(int pin);               /* 0 or 1                         */
int (*pin_write)(int pin, int value);
int (*pin_toggle)(int pin);
```

The modes are `FREYA_PIN_IN`, `FREYA_PIN_IN_PULLUP`, `FREYA_PIN_IN_PULLDOWN`,
`FREYA_PIN_OUT`, `FREYA_PIN_OUT_OD` (open drain; on the F4 the pin's pull-up
is on as well) and `FREYA_PIN_ANALOG`. Ports
A, B and C exist on both boards; the register layout behind them does not
(the F1 configures a pin in one four-bit field, the F4 in four two-bit ones),
which is why the chip half lives in `boards/<board>/board.c`.

Freya keeps **PA2 and PA3** for the console, **PA4 to PA7** for the card
and **PA8** for the socket's power switch, and refuses them with
`FREYA_ERR_PIN`. That is the whole reservation list: PC13 is the LED and
a program may drive it either as a pin or through `api->led()`, and every
other pin is the program's. A write to a pin is a
single store to `BSRR`, so it cannot be caught halfway by an interrupt, and
neither can a `pin_toggle()`.

## Pin interrupts

```c
int      (*pin_irq_attach)(int pin, int edge, freya_irq_fn fn, void *arg);
int      (*pin_irq_detach)(int pin);
uint32_t (*pin_irq_count)(int pin);
```

`edge` is `FREYA_EDGE_RISING`, `FREYA_EDGE_FALLING` or `FREYA_EDGE_BOTH`,
optionally with `FREYA_EDGE_DEBOUNCE` added: a switch bounces for a few
milliseconds, and the flag takes the first edge and ignores the rest for
`FREYA_DEBOUNCE_MS` (20). A program that wants another interval leaves the
flag off and times the edges itself.

The hardware gives sixteen interrupt lines, not one per pin: **line *n* is
pin *n* of one port at a time**, so PA0, PB0 and PC0 compete for the same
line and the second attach is refused with `FREYA_ERR_BUSY`. Pin numbers
across different ports are free of each other — PA0 and PB1 are fine
together.

A button between a pin and ground is the usual case, and wants a pull-up and
a falling edge:

```c
api->pin_mode(FREYA_PB(0), FREYA_PIN_IN_PULLUP);
api->pin_irq_attach(FREYA_PB(0), FREYA_EDGE_FALLING | FREYA_EDGE_DEBOUNCE,
                    on_press, &presses);
```

## Timers

```c
int      (*timer_open)(uint32_t period_us, int flags,
                       freya_irq_fn fn, void *arg);
int      (*timer_close)(int timer);
int      (*timer_start)(int timer);
int      (*timer_stop)(int timer);
int      (*timer_period)(int timer, uint32_t period_us);
uint32_t (*timer_count)(int timer);
```

`timer_open()` claims one of the three general purpose timers (TIM2, TIM3 and
TIM4 on both boards) and returns a handle, or `FREYA_ERR_BUSY` when all three
are taken. It comes back stopped; `timer_start()` runs it. With
`FREYA_TIMER_ONESHOT` in `flags` it fires once and stops itself, which is a
timeout rather than a tick.

A period is microseconds, from `FREYA_TIMER_MIN_US` (10) to
`FREYA_TIMER_MAX_US` (40 seconds). The prescaler and the reload are worked
out from the timer clock — the full 96 MHz on the Black Pill and 72 on the
Blue Pill — and the smallest prescaler that fits is the one used, so the
resolution stays as fine as the period allows. Round periods land exactly; the
worst case anywhere in the range is 0.04% off, which `make test` measures at
every clock either board can run at.

`timer_period()` changes the period of a timer that is already running; it
takes effect at its next expiry, because the reload register is buffered.

## PWM

The other thing those timers do is drive a pin directly. `pwm_open()` starts a
square wave and returns a handle; from then on the hardware toggles the pin and
the program can do anything else, or nothing.

```c
int (*pwm_open)(int pin, uint32_t freq_hz, uint32_t duty);  /* -> a handle */
int (*pwm_close)(int pwm);
int (*pwm_duty)(int pwm, uint32_t duty);      /* 0 .. FREYA_PWM_FULL */
int (*pwm_pulse_us)(int pwm, uint32_t us);    /* the same, as a high time */
int (*pwm_freq)(int pwm, uint32_t freq_hz);
```

A duty cycle is a fraction of the period in ten-thousandths, so
`FREYA_PWM_FULL` (10000) is a pin held high the whole period, 5000 is half and
750 is the 7.5% a servo sits in the middle at. It is a fraction rather than a
count of ticks so that it survives a change of frequency and means the same on
both boards. A channel comes up already running at the duty cycle
`pwm_open()` was given, and `pwm_close()` stops it **and puts the pin back to
an input** — a stopped PWM pin would otherwise freeze at whichever level the
period happened to be at, which for whatever it drives is an arbitrary one of
the two.

### Which pins

Eight, the same on both boards, with no remapping and no JTAG pin among them:

| Pin | | Pin | | Pin | | Pin | |
|---|---|---|---|---|---|---|---|
| PA0 | TIM2 CH1 | PB0 | TIM3 CH3 | PB6 | TIM4 CH1 | PB8 | TIM4 CH3 |
| PA1 | TIM2 CH2 | PB1 | TIM3 CH4 | PB7 | TIM4 CH2 | PB9 | TIM4 CH4 |

Any other pin is refused with `FREYA_ERR_PIN`, and `pwm` at the console prints
the table with the state of each channel beside it. On a Black Pill, note that
PA0 is also the KEY button, which holds it low when pressed.

These are the same TIM2, TIM3 and TIM4 that `timer_open()` hands out, so the
two share three timers between them: a timer driving pins is not one a program
can also take a periodic interrupt from, and whichever asks first gets it. The
second one is told `FREYA_ERR_BUSY`.

Within one timer the counter is shared too, which is why **all the channels of
a timer run at one frequency**. The first `pwm_open()` on a timer sets it; a
second pin on the same timer has to ask for the frequency that is already
running, or it is refused with `FREYA_ERR_BUSY`. `pwm_freq()` changes it for
every channel of that timer at once, each keeping the duty cycle it was given.
PB6 and PB9 therefore always share a frequency; PB6 and PB0 never do.

### Frequency, and what it costs in resolution

From `FREYA_PWM_MIN_HZ` (1) to `FREYA_PWM_MAX_HZ` (1 MHz). The prescaler and
the reload are worked out the way a timer period is, smallest prescaler first,
and what the reload comes to is the resolution the duty cycle actually has:
1 kHz at 96 MHz counts 96000 ticks, so a ten-thousandth of the period is real,
while 1 MHz counts 96 and the duty cycle moves in steps of about a percent.
Round frequencies land exactly, and the error only becomes visible up where
the counts run out: the worst the sweep in `make test` finds is 0.3% at
956 kHz, or 0.6% on a Blue Pill running without its crystal.

### Servos

A servo is specified as a pulse width, not a fraction, and `pwm_pulse_us()`
says it that way:

```c
int servo = api->pwm_open(FREYA_PA(0), 50, 0);   /* a 20 ms frame */

api->pwm_pulse_us(servo, 1500);                  /* centre */
```

The compare value is worked out from the timer clock rather than from the duty
fraction, so the pulse lands on the finest step the prescaler left — 312 ns at
50 Hz on either board — instead of on a ten-thousandth of the period. A pulse
longer than the period is `FREYA_ERR_ARG`.

### From a handler, and at the end of a run

`pwm_duty()`, `pwm_pulse_us()` and `pwm_freq()` are safe from an interrupt
handler: they write compare registers and nothing else. `pwm_open()` and
`pwm_close()` are refused with `FREYA_ERR_HANDLER`, like `timer_open()` and
`pin_irq_attach()`, because they rearrange the tables.

Every channel a program opened is closed when its run ends, however it ended,
and its pins go back to being inputs — nothing a program leaves behind keeps
driving a motor after the shell comes back. A channel started at the console
with `pwm` is not a program's, and keeps running until `pwm <pin> off`.

## Handlers, and what they may do

A handler is the program's own code running in interrupt context:

```c
typedef void (*freya_irq_fn)(int source, void *arg);
```

`source` is the pin or the timer handle it came from, so one function can
serve several, and `arg` is whatever was registered beside it.

What a handler may do follows from what it can preempt. Console output, the
LED, `ticks_ms()`, the pin calls and `timer_start` / `timer_stop` /
`timer_period` / the counters are all safe. `malloc()`, `free()` and the
filesystem and the I2C, SPI, 1-Wire and ADC calls are not — a handler can land in
the middle of the heap's or FAT's own bookkeeping, or spin on a bus — so
**the kernel refuses them from a handler** rather than let a program corrupt
the card or the heap: `malloc()` returns `NULL`, the file calls return an
error, `api->log()` writes to the console instead of the card, and an I2C,
SPI, 1-Wire or ADC call returns `FREYA_ERR_HANDLER`. `w1_crc()` and `crypt()` are arithmetic and may be called. `pin_irq_attach`, `pin_irq_detach`, `timer_open` and `timer_close`
are refused too, with `FREYA_ERR_HANDLER`: they rearrange the tables the
interrupt itself is walking.

Handlers never nest. They all run at one interrupt priority, below the
console and above nothing else the program can see, so one handler cannot
interrupt another and a handler cannot interrupt itself.

### A handler is contained the way the program is

Interrupt context is the one place a program could take Freya down with it: a
fault there is not a thread fault, and a loop there is not something the shell
can interrupt. Both are handled.

A handler runs inside a jump buffer. If it faults, the fault is reported as
the program's — `program fault at pc=... (interrupt handler)` — and the
handler is unwound out of the interrupt that called it; that interrupt
returns normally, and the run ends in thread mode with the usual status
(`131`..`134`). If it never returns, Ctrl-C still works: the console
interrupt runs above every handler, and it redirects the looping handler into
the same unwind before ending the run with `130`.

A runaway timer is caught from the other side as well. Once a stop has been
asked for, the next expiry switches the timer off instead of firing again, so
even a 10 µs period that leaves the thread no time to run is recoverable.

When a run ends — returned, exited, Ctrl-C or fault — every line is detached
and every timer is stopped and closed before the program's memory is handed
back. Nothing a program left behind can fire into the shell.

## Waiting instead of handling

The handler is optional. Attached as `NULL`, an interrupt is still counted,
and the program reads the counters from thread mode, where it may do
anything at all:

```c
uint32_t (*irq_count)(void);        /* pin and timer events this run */
int      (*irq_wait)(uint32_t ms);  /* 0 when one arrived, -1 if not  */
```

`irq_wait()` sleeps in `WFI` until the next pin or timer event, giving up
after `ms` milliseconds (zero waits indefinitely) and returning `-1` when a
stop has been requested, the way every other blocking call does.

This is the whole of the interrupt API for a program that wants the timing
but not the hazards: a timer with no handler, `irq_wait()` in the main loop,
and everything else in thread mode.

The shell has the same shape. `timer(us, flags, "name")` and
`irq("PB0", edge, "name")` arm a source, and `wait(ms)` calls that
script function afterwards, in the shell, where it may do anything a
script may do. The function does not run in the interrupt. A source
armed from the shell is not dropped when a program ends. The expressions
are written out in [shell.md](shell.md).

## Errors

| | |
|---|---|
| `FREYA_ERR_PIN` (-1) | no such pin, one the kernel owns, or one with no PWM channel |
| `FREYA_ERR_BUSY` (-2) | that interrupt line is taken, every timer is taken, or the timer is already running at another frequency |
| `FREYA_ERR_ARG` (-3) | mode, edge, period, frequency, duty cycle or handle out of range |
| `FREYA_ERR_HANDLER` (-4) | not callable from a handler |
| `FREYA_ERR_NACK` (-5) | an I2C address or byte was not acknowledged, or no 1-Wire device answered |
| `FREYA_ERR_TIMEOUT` (-6) | an I2C or SPI transfer did not finish, or a 1-Wire line stayed low |
| `FREYA_ERR_IO` (-7) | a bus error, or a stop requested mid-transfer |

I2C, SPI and 1-Wire are the other buses a program can drive. None of them is an
interrupt source; [docs/i2c.md](i2c.md), [docs/spi.md](spi.md) and [docs/w1.md](w1.md) are their APIs.

## Older kernels

These calls were appended to the service table, which is what `size` in it is
for. A program built against this header and handed an older kernel checks
before it calls, and says so rather than jumping into nothing:

```c
if (!FREYA_API_HAS(api, irq_wait)) {
    api->puts("this kernel has no pin or timer interrupts\r\n");
    return FREYA_EXIT_FAIL;
}
```

PWM was appended after those, so it is asked about separately:
`FREYA_API_HAS(api, pwm_open)`.
