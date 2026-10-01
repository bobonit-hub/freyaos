# rustdemo

A Freya program written in Rust against the bindings in `rust/freya`.
[rust/README.md](../../rust/README.md) covers the toolchain and the crate.

What it shows:

* the console with `println!`, the arguments, the table version and the clock
* how the previous run ended
* the heap through `alloc`: a `Vec` of primes and a `String`
* a file appended to with `writeln!`, then read back with a seek, and a
  listing of `/`
* a timer interrupt every 250 ms that blinks the LED, and a thread
  counting seconds beside `main`

```
run rustdemo.bin           count until Ctrl-C
run rustdemo.bin 7         exit with status 7
run rustdemo.bin panic     panic: the run ends with status 101
run rustdemo.bin irqexit   exit with status 9 from inside the timer handler
```

`make rust` builds `build/<board>/samples/rustdemo.bin` and `rustdemo.xip.bin`.
On the Blue Pill only `rustdemo.xip.bin` is built, because the program is
12 KiB and the RAM window there is 7 KiB. Install it with `install` or
`make BOARD=bluepill flash PROGRAM=rustdemo`.
