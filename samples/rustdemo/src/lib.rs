//! rustdemo - a Freya program written in Rust.
//!
//! Shows the console, arguments, the clock, the heap through `alloc`, a
//! file written with `write!` and read back, a directory listing, a timer
//! interrupt, a thread, and the ways a run can end.
//!
//!     run rustdemo.bin           count until Ctrl-C
//!     run rustdemo.bin 7         exit with status 7
//!     run rustdemo.bin panic     panic: status 101
//!     run rustdemo.bin irqexit   exit with status 9 from the timer handler

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use freya::{println, sys, Api, Args, Dir, File, OpenFlags, Timer, TimerId};

freya::entry!(main);

static TICKS: AtomicU32 = AtomicU32::new(0);
static EXIT_FROM_HANDLER: AtomicBool = AtomicBool::new(false);
static BEATS: AtomicU32 = AtomicU32::new(0);

/// Runs in interrupt context every 250 ms.
fn on_tick(_timer: TimerId) {
    let n = TICKS.fetch_add(1, Ordering::Relaxed) + 1;
    freya::api().led(n & 1 != 0);
    if n == 4 && EXIT_FROM_HANDLER.load(Ordering::Relaxed) {
        println!("  exiting from the timer handler");
        freya::api().exit(9);
    }
}

/// A thread of the run, beside main, until the run ends.
fn heartbeat() {
    while freya::thread_sleep(1000).is_ok() {
        BEATS.fetch_add(1, Ordering::Relaxed);
    }
}

fn primes(limit: u32) -> Vec<u32> {
    let mut v: Vec<u32> = Vec::new();
    for n in 2..limit {
        if v.iter().take_while(|&&p| p * p <= n).all(|&p| n % p != 0) {
            v.push(n);
        }
    }
    v
}

fn files(api: &Api) -> Result<(), freya::FsError> {
    let path = "/rustdemo.txt";
    {
        let mut f = File::open(path, OpenFlags::WRITE | OpenFlags::CREATE | OpenFlags::APPEND)?;
        writeln!(f, "rustdemo ran at {} ms\r", api.ticks_ms()).map_err(|_| freya::FsError::IO)?;
    }

    let mut f = File::open(path, OpenFlags::READ)?;
    let size = f.len()?;
    let mut last = [0u8; 64];
    let from = size.saturating_sub(last.len() as u32);
    f.seek(freya::SeekFrom::Start(from))?;
    let n = f.read(&mut last)?;
    let tail = core::str::from_utf8(&last[..n]).unwrap_or("?");
    let line = tail.trim_end().rsplit('\n').next().unwrap_or("").trim_end();
    println!("  {} is {} bytes, last line: \"{}\"", path, size, line);

    println!("\r\nroot directory:");
    for e in Dir::open("/")? {
        if e.is_dir() {
            println!("  {:<24} <dir>", e.name());
        } else {
            println!("  {:<24} {}", e.name(), e.size());
        }
    }
    Ok(())
}

fn main(api: &'static Api, args: Args) -> i32 {
    let start = api.ticks_ms();

    println!("\r\nhello from Rust on Freya");
    println!("  api version {}, table {} bytes, cpu {} Hz", api.version(), api.size(), api.cpu_hz());
    match api.rtc_get() {
        Ok(t) => println!("  the clock says {}", t),
        Err(e) => println!("  no clock: {}", e),
    }
    if let Some(prev) = api.last_exit() {
        println!("  previous run: {} {}, status {}, {} ms", prev.name(), prev.reason_str(), prev.status, prev.run_ms);
    }
    for (i, a) in args.iter().enumerate() {
        println!("  argv[{}] = \"{}\"", i, a);
    }

    match args.get(1) {
        Some("panic") => {
            let v: Vec<u32> = Vec::new();
            println!("  about to index an empty vector");
            println!("  {}", v[args.len()]);
        }
        Some("irqexit") => EXIT_FROM_HANDLER.store(true, Ordering::Relaxed),
        Some(a) => {
            if let Ok(code) = a.parse::<i32>() {
                println!("  exiting with status {}", code);
                api.exit(code);
            }
        }
        None => {}
    }

    let p = primes(200);
    let mut s = String::new();
    for x in p.iter().rev().take(5) {
        let _ = write!(s, "{} ", x);
    }
    println!("\r\n  {} primes below 200, sum {}, largest: {}", p.len(), p.iter().sum::<u32>(), s.trim_end());

    println!("\r\nwriting a file ...");
    if let Err(e) = files(api) {
        println!("  filesystem: {:?}", e);
    }

    let timer = match Timer::open(250_000, false, Some(on_tick)) {
        Ok(t) => t,
        Err(e) => {
            println!("no timer: {}", e);
            return sys::FREYA_EXIT_FAIL;
        }
    };
    if let Err(e) = freya::spawn(c"heartbeat", sys::FREYA_PRIO_NORMAL, heartbeat) {
        println!("no thread: {}", e);
    }
    let _ = timer.start();

    println!("\r\nblinking from a timer interrupt - press Ctrl-C to stop");
    let mut seconds = 0;
    while !api.should_stop() {
        api.delay_ms(1000);
        seconds += 1;
        println!(
            "  {:>3} s: {} timer interrupts, {} heartbeats",
            seconds,
            timer.count(),
            BEATS.load(Ordering::Relaxed)
        );
        api.yield_now();
    }

    drop(timer);
    api.led(false);
    println!("\r\nfinished after {} ms", api.ticks_ms() - start);
    sys::FREYA_EXIT_OK
}
