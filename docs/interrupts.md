# Pins, timers and interrupts

A Freya program can drive the board's spare pins, take an interrupt when one
of them changes, and have a hardware timer interrupt it every so many
microseconds. All of it is in the service table (`include/freya_api.h`), so a
program uses it the way it uses `printf` or `open`, and none of it needs the
card.

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
a pin, counted and reported. Build it with `make` and run `run irq.bin`.

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
`FREYA_PIN_OUT`, `FREYA_PIN_OUT_OD` (open drain) and `FREYA_PIN_ANALOG`. Ports
A, B and C exist on both boards; the register layout behind them does not
(the F1 configures a pin in one four-bit field, the F4 in four two-bit ones),
which is why the chip half lives in `boards/<board>/board.c`.

Freya keeps **PA2 and PA3** for the console and **PA4 to PA7** for the card,
and refuses them with `FREYA_ERR_PIN`. That is the whole reservation list:
PC13 is the LED and a program may drive it either as a pin or through
`api->led()`, and every other pin is the program's. A write to a pin is a
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
filesystem are not — a handler can land in the middle of the heap's or FAT's
own bookkeeping — so **the kernel refuses them from a handler** rather than
let a program corrupt the card or the heap: `malloc()` returns `NULL`, the
file calls return an error, and `api->log()` writes to the console instead of
the card. `pin_irq_attach`, `pin_irq_detach`, `timer_open` and `timer_close`
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

## Errors

| | |
|---|---|
| `FREYA_ERR_PIN` (-1) | no such pin, or one the kernel owns |
| `FREYA_ERR_BUSY` (-2) | that interrupt line, or every timer, is taken |
| `FREYA_ERR_ARG` (-3) | mode, edge, period or handle out of range |
| `FREYA_ERR_HANDLER` (-4) | not callable from a handler |

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
