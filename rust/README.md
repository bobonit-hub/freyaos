# Rust programs for Freya

`rust/freya` is a `no_std` crate with bindings to the program ABI in
`include/freya_api.h`. A Freya program written in Rust is a Cargo package
that depends on it and builds as a static library. The top level Makefile
links that library with `apps/common/app_start.c`, which supplies the
program header, and with the board's `app.ld` or `app_flash.ld`, the same
way it links a C program. The result is the usual `.bin` for `load` and
`.xip.bin` for `install`. `samples/rustdemo` is the worked example.

## Toolchain

Rust comes from [rustup](https://rustup.rs). Add the targets for the boards:

```sh
rustup target add thumbv7em-none-eabihf    # Black Pill, STM32F405 (Cortex-M4F)
rustup target add thumbv7m-none-eabi       # Blue Pill (Cortex-M3)
```

The Makefile finds `cargo` on `PATH`, or in `~/.cargo/bin` when rustup was
installed without changing `PATH`. You can also set `CARGO=`. Each board
names its target and CPU in `boards/<board>/board.mk` as `RUST_TARGET` and
`RUST_CPU`. `arm-none-eabi-gcc` is still needed: it links the program, and
the bindings use it to check their own layout (see below).

Without cargo, `make` leaves the Rust samples out and builds everything else,
and `make test` skips the Rust case.

## Building

```sh
make rust                        # the Rust samples for the default board
make BOARD=bluepill rust
make                             # everything, the Rust samples included
make flash PROGRAM=rustdemo      # a Rust sample works as PROGRAM= too
```

The output goes where the C samples' does: `build/<board>/samples/rustdemo.bin`
and `rustdemo.xip.bin`. Cargo's own output is under `build/<board>/rust/`.

## A program

`Cargo.toml`:

```toml
[package]
name = "myprog"            # the directory name under samples/
version = "0.1.0"
edition = "2024"

[lib]
crate-type = ["staticlib"]

[dependencies]
freya = { path = "../../rust/freya" }

[profile.release]
opt-level = "z"
lto = true
codegen-units = 1
panic = "abort"
```

`src/lib.rs`:

```rust
#![no_std]

use freya::{println, Api, Args, Pin, Mode};

freya::entry!(main);

fn main(api: &'static Api, args: Args) -> freya::Result<()> {
    println!("hello, {} arguments", args.len());
    let led = Pin::pb(2);
    led.mode(Mode::Out)?;
    while !api.should_stop() {
        led.toggle()?;
        api.delay_ms(250);
    }
    Ok(())
}
```

Add the name to `RUST_SAMPLES` in the Makefile. If the program is too big for
a board's program RAM window, add it to that board's `XIP_ONLY_<board>` too.

`main` returns an `i32` status, `()` for 0, or a `Result`. An `Err` is
printed and gives `FREYA_EXIT_FAIL`. `entry!` exports the `app_main` the
header points at.

## What the crate covers

* **The console.** `print!` and `println!`. `println!` ends a line with
  `"\r\n"`. `Console` implements `core::fmt::Write`. `getc`, `getc_timeout`,
  `kbhit` and `console_raw` are methods on `Api`.
* **Time and flow.** `ticks_ms`, `delay_ms`, `cpu_hz`, `led`,
  `should_stop`, `yield_now` and `exit`. `exit` works from anywhere,
  including a pin or timer handler.
* **The heap.** A global allocator over the kernel heap, so `alloc::vec::Vec`,
  `String` and `Box` work. Blocks are 8-byte aligned. A program can hold at
  most 32 at a time, and the kernel reclaims whatever is left when the run
  ends. A handler can't allocate.
* **Files.** `File` (`read`, `write`, `write_all`, `seek`, `len`, and `write!`
  through `core::fmt::Write`) and `Dir`, an iterator of `DirEntry`, both
  closed when dropped. `fs::unlink`, `fs::mkdir` and `fs::rename` take paths
  as `&str`, at most 127 bytes. Errors are `FsError`, the kernel's
  `FAT_ERR_*` codes.
* **The log, the previous run and the clock.** `log`, `set_log_level`,
  `last_exit`, `rtc_get` and `rtc_set`. A `DateTime` prints as
  `YYYY-MM-DD HH:MM:SS`.
* **Pins and interrupts.** `Pin::pa(n)`, `pb(n)` and `pc(n)`, with `mode`,
  `read`, `write`, `toggle`, `irq_attach`, `irq_detach` and `irq_count`.
  `Api::irq_count` and `Api::irq_wait` cover every pin and timer.
* **Timers, PWM and buses.** `Timer`, `Pwm`, `I2c`, `Spi` and `W1`, each
  closed when dropped. Also `adc_read`, `adc_temp` and `adc_vref`.
* **Threads.** `spawn(c"name", priority, f)`, `thread_sleep`,
  `thread_yield`, `thread_self` and `thread_exit`.
* **Ciphers and compression.** `crypt` (XTEA-CTR, in place),
  `aead_encrypt` and `aead_decrypt` (Ascon-AEAD128), `compress` and
  `decompress` (heatshrink).
* **Everything else.** Wi-Fi, sockets, TLS, the web service, the script
  runner, settings and the PDP-11 go through `api.raw()`. That's the C table
  exactly as `sys::freya_api_t` declares it, plus every constant from the
  header in `freya::sys`.

Calls other than the error-reporting ones return `freya::Result<T>`, whose
error is `freya::Error`, the `FREYA_ERR_*` code. Any call that was appended to
the table after ABI 1 first checks the running kernel's table size, and
returns `Error::UNSUPPORTED` if the kernel is too old to have it, the way
`FREYA_API_HAS` does in C. `freya::api_has!(api.raw(), field)` is that test.

A handler is a plain `fn`. `irq_attach` and `Timer::open` pass it to the
kernel as the argument of a trampoline, so attaching one needs neither the
heap nor `unsafe`. It runs in interrupt context, so keep its state in
atomics. [docs/interrupts.md](../docs/interrupts.md) lists what it may call.
A handler that panics ends the run, the same as one that faults.

A thread is also a plain `fn`. It has `FREYA_THREAD_STACK`, 1 KiB, of stack.
That's enough for light formatting, but a deep `core::fmt` call doesn't
belong there.

## A panic

The crate's panic handler prints the message and its location and ends the
run with status 101, the status a Rust program on a PC exits with:

```
[rust] panicked at src/lib.rs:104:31:
index out of bounds: the len is 0 but the index is 2
```

Turn off the `panic-handler` feature to supply your own, and `global-alloc`
to supply your own allocator.

## The layout check

`rust/freya/build.rs` compiles `freya_api.h` with `arm-none-eabi-gcc` into
assembly that spells out every struct size, every field offset and every
constant `src/sys.rs` names. It turns those numbers into const assertions, so
a Rust table that doesn't match the C one fails to compile and names the
field or constant. When a call is appended to the C table, add the same field
at the end of `freya_api_t` in `src/sys.rs`. `FREYA_CC` and `FREYA_BOARD`
choose the compiler and the board, and the Makefile sets both. A build for
the host has nothing to compare against and skips the check.

## Sizes

`core::fmt` and the allocator take about 10 KiB, so `rustdemo` is 12 KiB.
That fits the 56 KiB program RAM window on the F4 boards. The Blue Pill's
window is 7 KiB, so there `rustdemo` is built only as a flash image, as
`forth` is. A program that leaves out formatting and `alloc` is much smaller.

## Installed images

LLVM sometimes builds an address into a `movw`/`movt` instruction pair where
gcc would use a word in a literal pool. Even `core`, which comes precompiled,
does this. An installed image's relocation table only moves words, so when
the loader copies an installed program into RAM, those pairs still point at
the flash copy. That copy is the same bytes and stays in place for the whole
run, so the code and constants they reach are still correct. They're just
reached in flash.

## Tests

`make test` builds `samples/rustdemo` unchanged for the host and runs it
against a service table in `tests/host_rust_test.c` that captures what it
prints. The cases cover a run until Ctrl-C (the heap, a file, the directory
listing, timer interrupts and a thread), a kernel whose table ends before
`rtc_get`, `exit(7)`, a panic, and `exit()` from the timer handler.
