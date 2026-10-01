//! Rust bindings to the Freya program ABI.
//!
//! A Freya program written in Rust is a `staticlib` crate that depends on
//! this one and names its entry point with [`entry!`]:
//!
//! ```ignore
//! #![no_std]
//! use freya::{println, Api, Args};
//!
//! freya::entry!(main);
//!
//! fn main(api: &'static Api, args: Args) -> i32 {
//!     println!("hello, {} arguments", args.len());
//!     api.delay_ms(100);
//!     0
//! }
//! ```
//!
//! The top level Makefile builds the crate for the board and links it with
//! `apps/common/app_start.c`, which supplies the program header, and the
//! board's linker scripts, exactly as it links a C program; the result is
//! the same `.bin` for `load` and `.xip.bin` for `install`.
//!
//! [`sys`] is the whole service table as C sees it, checked against
//! `include/freya_api.h` when the crate builds.  The rest of the crate
//! wraps the parts a program usually wants: the console with `print!`,
//! time, the filesystem, the log, pins and interrupts, timers, PWM, the
//! buses, the ADC, threads, the ciphers, compression and the clock.
//! Calls appended to the table after ABI 1 are checked against the
//! running kernel's table size and report [`Error::UNSUPPORTED`] when it
//! is too old to have them.  The network, the web service and the PDP-11
//! are reached through [`Api::raw`].
//!
//! The crate also provides a panic handler, which prints the panic and
//! ends the run with status 101 as a Rust program on a PC would, and a
//! global allocator over the kernel heap; turn off the `panic-handler`
//! or `global-alloc` features to supply your own.

#![no_std]

use core::ffi::{c_char, c_int, c_void, CStr};
use core::fmt;
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

pub mod sys;

pub mod fs;
mod hw;
mod thread;

pub use fs::{Dir, DirEntry, File, FsError, OpenFlags, SeekFrom};
pub use hw::{
    adc_read, adc_temp, adc_vref, aead_decrypt, aead_encrypt, compress, decompress,
    w1_crc, Edge, I2c, Mode, Pin, Pwm, Spi, SpiMode, Timer, TimerId, W1,
};
pub use thread::{spawn, thread_exit, thread_self, thread_sleep, thread_yield, ThreadId};

#[allow(dead_code, unused_imports)]
mod layout {
    use crate::sys;
    include!(concat!(env!("OUT_DIR"), "/layout_check.rs"));
}

/* ------------------------------------------------------------- errors */

/// A `FREYA_ERR_*` code from a pin, timer, bus, cipher or thread call.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Error(pub c_int);

impl Error {
    pub const PIN: Error = Error(sys::FREYA_ERR_PIN);
    pub const BUSY: Error = Error(sys::FREYA_ERR_BUSY);
    pub const ARG: Error = Error(sys::FREYA_ERR_ARG);
    pub const HANDLER: Error = Error(sys::FREYA_ERR_HANDLER);
    pub const NACK: Error = Error(sys::FREYA_ERR_NACK);
    pub const TIMEOUT: Error = Error(sys::FREYA_ERR_TIMEOUT);
    pub const IO: Error = Error(sys::FREYA_ERR_IO);
    pub const AGAIN: Error = Error(sys::FREYA_ERR_AGAIN);
    pub const UNSUPPORTED: Error = Error(sys::FREYA_ERR_UNSUPPORTED);

    pub fn name(self) -> &'static str {
        match self.0 {
            sys::FREYA_ERR_PIN => "no such pin",
            sys::FREYA_ERR_BUSY => "busy",
            sys::FREYA_ERR_ARG => "argument out of range",
            sys::FREYA_ERR_HANDLER => "not allowed from a handler",
            sys::FREYA_ERR_NACK => "not acknowledged",
            sys::FREYA_ERR_TIMEOUT => "timed out",
            sys::FREYA_ERR_IO => "i/o error",
            sys::FREYA_ERR_AGAIN => "not ready",
            sys::FREYA_ERR_UNSUPPORTED => "not supported on this board",
            _ => "unknown error",
        }
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} ({})", self.name(), self.0)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

impl core::error::Error for Error {}

pub type Result<T, E = Error> = core::result::Result<T, E>;

/// A negative return is an error; anything else is the value asked for.
pub(crate) fn check(r: c_int) -> Result<c_int> {
    if r < 0 { Err(Error(r)) } else { Ok(r) }
}

/* -------------------------------------------------- the service table */

static API: AtomicPtr<sys::freya_api_t> = AtomicPtr::new(ptr::null_mut());

/// The service table, as the kernel handed it to `app_main`.
#[repr(transparent)]
pub struct Api(sys::freya_api_t);

/// The table of the run in progress.  Set before `main` is called, so it
/// is there for every part of the program, handlers and threads included.
pub fn api() -> &'static Api {
    match try_api() {
        Some(a) => a,
        None => loop {
            core::hint::spin_loop();
        },
    }
}

/// The table, or `None` before the entry point has run.
pub fn try_api() -> Option<&'static Api> {
    let p = API.load(Ordering::Relaxed);
    // SAFETY: the kernel's table outlives the run, and Api is transparent.
    unsafe { (p as *const Api).as_ref() }
}

pub(crate) fn raw() -> &'static sys::freya_api_t {
    &api().0
}

/// Return early with [`Error::UNSUPPORTED`] when the running kernel's
/// table stops before `field`.
macro_rules! require {
    ($field:ident) => {
        if !$crate::api_has!($crate::raw(), $field) {
            return Err($crate::Error::UNSUPPORTED);
        }
    };
}
pub(crate) use require;

/// What became of the run before this one.
#[derive(Clone, Copy)]
pub struct LastExit {
    pub status: i32,
    pub reason: i32,
    pub run_ms: u32,
    name: [c_char; 20],
}

impl LastExit {
    pub fn name(&self) -> &str {
        str_from_c(&self.name)
    }
    /// "returned", "exited", "stopped by Ctrl-C", "killed by ...".
    pub fn reason_str(&self) -> &'static str {
        api().exit_reason_str(self.reason)
    }
}

impl Api {
    /// The C table, for the calls this crate does not wrap.
    pub fn raw(&self) -> &sys::freya_api_t {
        &self.0
    }
    pub fn version(&self) -> u32 {
        self.0.version
    }
    /// The size of the running kernel's table in bytes.
    pub fn size(&self) -> u32 {
        self.0.size
    }

    pub fn ticks_ms(&self) -> u32 {
        unsafe { (self.0.ticks_ms)() }
    }
    pub fn delay_ms(&self, ms: u32) {
        unsafe { (self.0.delay_ms)(ms) }
    }
    pub fn cpu_hz(&self) -> u32 {
        unsafe { (self.0.cpu_hz)() }
    }
    pub fn led(&self, on: bool) {
        unsafe { (self.0.led)(on as c_int) }
    }

    /// True once Ctrl-C was pressed: time to tidy up and return.
    pub fn should_stop(&self) -> bool {
        unsafe { (self.0.should_stop)() != 0 }
    }
    /// Let other threads run.  When the run has been stopped this does not
    /// return: the program unwinds into the shell from here.
    pub fn yield_now(&self) {
        unsafe { (self.0.yield_)() }
    }
    /// End the run with `code`, from anywhere, a handler included.
    pub fn exit(&self, code: i32) -> ! {
        unsafe { (self.0.exit)(code) }
    }

    /// Wait for a key.  `None` when the run is stopped while waiting.
    pub fn getc(&self) -> Option<u8> {
        let c = unsafe { (self.0.getc)() };
        if c < 0 { None } else { Some(c as u8) }
    }
    /// A key within `ms` milliseconds, or `None`.
    pub fn getc_timeout(&self, ms: u32) -> Option<u8> {
        let c = unsafe { (self.0.getc_timeout)(ms) };
        if c < 0 { None } else { Some(c as u8) }
    }
    pub fn kbhit(&self) -> bool {
        unsafe { (self.0.kbhit)() != 0 }
    }
    /// Take Ctrl-C away from the kernel: it then arrives through `getc`.
    /// Returns the previous setting.
    pub fn console_raw(&self, on: bool) -> Result<bool> {
        require!(console_raw);
        Ok(unsafe { (self.0.console_raw)(on as c_int) } != 0)
    }

    /// Write `msg` to `/freya.log` at `level` (`sys::FREYA_LOG_*`).
    pub fn log(&self, level: c_int, msg: &str) {
        if api_has!(&self.0, log) {
            unsafe { (self.0.log)(level, c"%.*s".as_ptr(), msg.len() as c_int, msg.as_ptr()) }
        }
    }
    pub fn log_level(&self) -> c_int {
        if api_has!(&self.0, get_log_level) { unsafe { (self.0.get_log_level)() } } else { 0 }
    }
    pub fn set_log_level(&self, level: c_int) -> Result<()> {
        require!(set_log_level);
        check(unsafe { (self.0.set_log_level)(level) }).map(drop)
    }

    /// How the previous run ended, if anything ran since reset.
    pub fn last_exit(&self) -> Option<LastExit> {
        if !api_has!(&self.0, last_exit) {
            return None;
        }
        let mut st = sys::freya_exit_t { status: 0, reason: 0, run_ms: 0, name: [0; 20] };
        if unsafe { (self.0.last_exit)(&mut st) } != 0 {
            return None;
        }
        Some(LastExit { status: st.status, reason: st.reason, run_ms: st.run_ms, name: st.name })
    }
    pub fn exit_reason_str(&self, reason: i32) -> &'static str {
        if !api_has!(&self.0, exit_reason_str) {
            return "";
        }
        let p = unsafe { (self.0.exit_reason_str)(reason) };
        if p.is_null() { "" } else { unsafe { CStr::from_ptr(p) }.to_str().unwrap_or("") }
    }

    /// Pin and timer events so far this run.
    pub fn irq_count(&self) -> u32 {
        if api_has!(&self.0, irq_count) { unsafe { (self.0.irq_count)() } } else { 0 }
    }
    /// Sleep until a pin or timer event arrives; false on timeout or stop.
    /// Zero waits indefinitely.
    pub fn irq_wait(&self, ms: u32) -> bool {
        api_has!(&self.0, irq_wait) && unsafe { (self.0.irq_wait)(ms) } == 0
    }

    /// Switch a supply (`sys::FREYA_PWR_SD`).  Returns the state found.
    pub fn power(&self, domain: c_int, on: bool) -> Result<bool> {
        require!(power);
        check(unsafe { (self.0.power)(domain, on as c_int) }).map(|r| r != 0)
    }

    /// The civil clock.
    pub fn rtc_get(&self) -> Result<DateTime> {
        require!(rtc_get);
        let mut t = DateTime::default();
        check(unsafe { (self.0.rtc_get)(&mut t) })?;
        Ok(t)
    }
    pub fn rtc_set(&self, t: &DateTime) -> Result<()> {
        require!(rtc_set);
        check(unsafe { (self.0.rtc_set)(t) }).map(drop)
    }
}

/// Civil time, `freya_rtc_t`.  Prints as `YYYY-MM-DD HH:MM:SS`.
pub type DateTime = sys::freya_rtc_t;

impl fmt::Display for sys::freya_rtc_t {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
            self.year, self.mon, self.day, self.hour, self.min, self.sec
        )
    }
}

/// The text of a NUL-padded C array, up to its first NUL.
pub(crate) fn str_from_c(b: &[c_char]) -> &str {
    // SAFETY: c_char and u8 have the same size and alignment.
    let b = unsafe { core::slice::from_raw_parts(b.as_ptr() as *const u8, b.len()) };
    let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    core::str::from_utf8(&b[..n]).unwrap_or("?")
}

/* ------------------------------------------------------------ console */

/// The console as a `core::fmt::Write`; `print!` and `println!` use it.
pub struct Console;

impl Console {
    pub fn write_bytes(&mut self, mut b: &[u8]) {
        let api = raw();
        // printf's %s stops at a NUL, so a NUL goes out on its own.
        while !b.is_empty() {
            let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
            if n > 0 {
                unsafe { (api.printf)(c"%.*s".as_ptr(), n as c_int, b.as_ptr()) };
            }
            if n < b.len() {
                unsafe { (api.putc)(0) };
                b = &b[n + 1..];
            } else {
                break;
            }
        }
    }
}

impl fmt::Write for Console {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.write_bytes(s.as_bytes());
        Ok(())
    }
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments<'_>) {
    let _ = fmt::Write::write_fmt(&mut Console, args);
}

/// Print to the console.
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => { $crate::_print(::core::format_args!($($arg)*)) };
}

/// Print to the console, ending the line with "\r\n" as a terminal wants.
#[macro_export]
macro_rules! println {
    () => { $crate::_print(::core::format_args!("\r\n")) };
    ($($arg:tt)*) => {{
        $crate::_print(::core::format_args!($($arg)*));
        $crate::_print(::core::format_args!("\r\n"));
    }};
}

/* -------------------------------------------------------- entry point */

/// The program's arguments; the first is the program itself.
#[derive(Clone, Copy)]
pub struct Args {
    argc: usize,
    argv: *const *const c_char,
}

impl Args {
    pub fn len(&self) -> usize {
        self.argc
    }
    pub fn is_empty(&self) -> bool {
        self.argc == 0
    }
    /// Argument `i`, or `None` past the end or when it is not UTF-8.
    pub fn get(&self, i: usize) -> Option<&'static str> {
        self.get_cstr(i).and_then(|s| s.to_str().ok())
    }
    pub fn get_cstr(&self, i: usize) -> Option<&'static CStr> {
        if i >= self.argc || self.argv.is_null() {
            return None;
        }
        // SAFETY: the kernel hands over argc NUL-terminated strings that
        // stay put for the whole run.
        unsafe {
            let p = *self.argv.add(i);
            if p.is_null() { None } else { Some(CStr::from_ptr(p)) }
        }
    }
    pub fn iter(&self) -> impl Iterator<Item = &'static str> + '_ {
        (0..self.argc).map(|i| self.get(i).unwrap_or("?"))
    }
}

/// What `main` may return: an exit status.
pub trait Status {
    fn code(self) -> i32;
}

impl Status for i32 {
    fn code(self) -> i32 {
        self
    }
}

impl Status for () {
    fn code(self) -> i32 {
        sys::FREYA_EXIT_OK
    }
}

impl<T: Status, E: fmt::Debug> Status for core::result::Result<T, E> {
    fn code(self) -> i32 {
        match self {
            Ok(v) => v.code(),
            Err(e) => {
                println!("error: {:?}", e);
                sys::FREYA_EXIT_FAIL
            }
        }
    }
}

/// Make `main` the program's entry point.  `main` is
/// `fn(&'static Api, Args) -> S` where `S` is `i32`, `()` or a `Result`.
#[macro_export]
macro_rules! entry {
    ($main:path) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn app_main(
            api: *const $crate::sys::freya_api_t,
            argc: ::core::ffi::c_int,
            argv: *const *const ::core::ffi::c_char,
        ) -> ::core::ffi::c_int {
            unsafe { $crate::__start(api, argc, argv, $main) }
        }
    };
}

#[doc(hidden)]
pub unsafe fn __start<S: Status>(
    api: *const sys::freya_api_t,
    argc: c_int,
    argv: *const *const c_char,
    main: fn(&'static Api, Args) -> S,
) -> c_int {
    API.store(api as *mut _, Ordering::Relaxed);
    let args = Args { argc: argc.max(0) as usize, argv };
    main(self::api(), args).code()
}

/* -------------------------------------------------------------- panic */

#[cfg(feature = "panic-handler")]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    match try_api() {
        Some(api) => {
            println!("\r\n[rust] {}", info);
            api.exit(101)
        }
        None => loop {
            core::hint::spin_loop();
        },
    }
}

/* ---------------------------------------------------------- allocator */

/// The kernel heap as a `GlobalAlloc`.  Blocks are 8-byte aligned, a
/// program may hold up to 32 at once, and whatever is still allocated
/// when the run ends is reclaimed by the kernel.  A pin or timer handler
/// is refused, so `alloc` there fails.
pub struct FreyaAlloc;

unsafe impl core::alloc::GlobalAlloc for FreyaAlloc {
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        if layout.align() > 8 || layout.size() > u32::MAX as usize {
            return ptr::null_mut();
        }
        unsafe { (raw().malloc)(layout.size().max(1) as u32) as *mut u8 }
    }
    unsafe fn dealloc(&self, p: *mut u8, _layout: core::alloc::Layout) {
        unsafe { (raw().free)(p as *mut c_void) }
    }
}

#[cfg(feature = "global-alloc")]
#[global_allocator]
static ALLOC: FreyaAlloc = FreyaAlloc;
