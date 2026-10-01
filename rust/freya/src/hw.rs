//! Pins, interrupts, timers, PWM, the buses, the ADC, the ciphers and
//! compression.
//!
//! A handler is a plain `fn`, passed to the kernel as the `arg` of a
//! trampoline, so no heap and no `unsafe` is needed to attach one.  It
//! runs in interrupt context: keep its state in atomics, and see
//! docs/interrupts.md for what it may call.  A handler that panics ends
//! the run like one that faults.

use core::ffi::{c_int, c_void};
use core::fmt;

use crate::{check, raw, require, sys, Result};

/* --------------------------------------------------------------- pins */

/// A pin: port and number, as `FREYA_PIN()` packs them.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Pin(pub c_int);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    In,
    InPullUp,
    InPullDown,
    Out,
    OutOpenDrain,
    Analog,
}

impl Mode {
    fn raw(self) -> c_int {
        match self {
            Mode::In => sys::FREYA_PIN_IN,
            Mode::InPullUp => sys::FREYA_PIN_IN_PULLUP,
            Mode::InPullDown => sys::FREYA_PIN_IN_PULLDOWN,
            Mode::Out => sys::FREYA_PIN_OUT,
            Mode::OutOpenDrain => sys::FREYA_PIN_OUT_OD,
            Mode::Analog => sys::FREYA_PIN_ANALOG,
        }
    }
}

/// Which edges interrupt; combine with `|`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Edge(pub c_int);

impl Edge {
    pub const RISING: Edge = Edge(sys::FREYA_EDGE_RISING);
    pub const FALLING: Edge = Edge(sys::FREYA_EDGE_FALLING);
    pub const BOTH: Edge = Edge(sys::FREYA_EDGE_BOTH);
    /// Take the first edge and ignore the rest for `FREYA_DEBOUNCE_MS`.
    pub const DEBOUNCE: Edge = Edge(sys::FREYA_EDGE_DEBOUNCE);
}

impl core::ops::BitOr for Edge {
    type Output = Edge;
    fn bitor(self, rhs: Edge) -> Edge {
        Edge(self.0 | rhs.0)
    }
}

unsafe extern "C" fn pin_trampoline(source: c_int, arg: *mut c_void) {
    // SAFETY: arg is the fn(Pin) that pin_irq_attach() was given.
    let f = unsafe { core::mem::transmute::<*mut c_void, fn(Pin)>(arg) };
    f(Pin(source))
}

unsafe extern "C" fn timer_trampoline(source: c_int, arg: *mut c_void) {
    // SAFETY: arg is the fn(TimerId) that Timer::open() was given.
    let f = unsafe { core::mem::transmute::<*mut c_void, fn(TimerId)>(arg) };
    f(TimerId(source))
}

impl Pin {
    pub const fn new(port: c_int, n: c_int) -> Pin {
        Pin(sys::freya_pin(port, n))
    }
    pub const fn pa(n: c_int) -> Pin {
        Pin::new(0, n)
    }
    pub const fn pb(n: c_int) -> Pin {
        Pin::new(1, n)
    }
    pub const fn pc(n: c_int) -> Pin {
        Pin::new(2, n)
    }
    pub const fn port(self) -> c_int {
        sys::freya_pin_port(self.0)
    }
    pub const fn num(self) -> c_int {
        sys::freya_pin_num(self.0)
    }

    pub fn mode(self, mode: Mode) -> Result<()> {
        require!(pin_mode);
        check(unsafe { (raw().pin_mode)(self.0, mode.raw()) }).map(drop)
    }
    pub fn read(self) -> Result<bool> {
        require!(pin_read);
        check(unsafe { (raw().pin_read)(self.0) }).map(|v| v != 0)
    }
    pub fn write(self, high: bool) -> Result<()> {
        require!(pin_write);
        check(unsafe { (raw().pin_write)(self.0, high as c_int) }).map(drop)
    }
    pub fn toggle(self) -> Result<()> {
        require!(pin_toggle);
        check(unsafe { (raw().pin_toggle)(self.0) }).map(drop)
    }

    /// Call `handler` on `edge`, or with `None` only count the edges.
    pub fn irq_attach(self, edge: Edge, handler: Option<fn(Pin)>) -> Result<()> {
        require!(pin_irq_attach);
        let (f, arg): (sys::freya_irq_fn, *mut c_void) = match handler {
            Some(h) => (Some(pin_trampoline), h as *mut c_void),
            None => (None, core::ptr::null_mut()),
        };
        check(unsafe { (raw().pin_irq_attach)(self.0, edge.0, f, arg) }).map(drop)
    }
    pub fn irq_detach(self) -> Result<()> {
        require!(pin_irq_detach);
        check(unsafe { (raw().pin_irq_detach)(self.0) }).map(drop)
    }
    /// Edges since the handler was attached.
    pub fn irq_count(self) -> u32 {
        if crate::api_has!(raw(), pin_irq_count) { unsafe { (raw().pin_irq_count)(self.0) } } else { 0 }
    }
}

impl fmt::Debug for Pin {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, f)
    }
}

impl fmt::Display for Pin {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "P{}{}", (b'A' + self.port() as u8) as char, self.num())
    }
}

/* ------------------------------------------------------------- timers */

/// A hardware timer.  Closed when dropped; the kernel also closes it when
/// the run ends.
#[derive(PartialEq, Eq, Debug)]
pub struct Timer(c_int);

/// Which timer called a handler: the handle [`Timer::id`] returns.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TimerId(pub c_int);

impl Timer {
    /// A timer that interrupts every `period_us` microseconds, or once
    /// with `oneshot`.  It does not run until [`Timer::start`].  With no
    /// handler the expiries are only counted.
    pub fn open(period_us: u32, oneshot: bool, handler: Option<fn(TimerId)>) -> Result<Timer> {
        require!(timer_open);
        let (f, arg): (sys::freya_irq_fn, *mut c_void) = match handler {
            Some(h) => (Some(timer_trampoline), h as *mut c_void),
            None => (None, core::ptr::null_mut()),
        };
        let flags = if oneshot { sys::FREYA_TIMER_ONESHOT } else { 0 };
        check(unsafe { (raw().timer_open)(period_us, flags, f, arg) }).map(Timer)
    }
    pub fn start(&self) -> Result<()> {
        check(unsafe { (raw().timer_start)(self.0) }).map(drop)
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { (raw().timer_stop)(self.0) }).map(drop)
    }
    pub fn set_period(&self, period_us: u32) -> Result<()> {
        check(unsafe { (raw().timer_period)(self.0, period_us) }).map(drop)
    }
    /// Expiries so far.
    pub fn count(&self) -> u32 {
        unsafe { (raw().timer_count)(self.0) }
    }
    pub fn id(&self) -> TimerId {
        TimerId(self.0)
    }
}

impl Drop for Timer {
    fn drop(&mut self) {
        unsafe { (raw().timer_close)(self.0) };
    }
}

/* ---------------------------------------------------------------- PWM */

/// A PWM channel on a pin, running from the moment it opens.  Closed when
/// dropped.
pub struct Pwm(c_int);

impl Pwm {
    /// `duty` is in ten-thousandths of the period, up to `FREYA_PWM_FULL`.
    pub fn open(pin: Pin, freq_hz: u32, duty: u32) -> Result<Pwm> {
        require!(pwm_open);
        check(unsafe { (raw().pwm_open)(pin.0, freq_hz, duty) }).map(Pwm)
    }
    pub fn set_duty(&self, duty: u32) -> Result<()> {
        check(unsafe { (raw().pwm_duty)(self.0, duty) }).map(drop)
    }
    /// Set the high time in microseconds, as a servo wants it.
    pub fn set_pulse_us(&self, us: u32) -> Result<()> {
        check(unsafe { (raw().pwm_pulse_us)(self.0, us) }).map(drop)
    }
    /// Set the frequency of the whole timer, every channel on it.
    pub fn set_freq(&self, freq_hz: u32) -> Result<()> {
        check(unsafe { (raw().pwm_freq)(self.0, freq_hz) }).map(drop)
    }
}

impl Drop for Pwm {
    fn drop(&mut self) {
        unsafe { (raw().pwm_close)(self.0) };
    }
}

fn len(b: &[u8]) -> Result<c_int> {
    c_int::try_from(b.len()).map_err(|_| crate::Error::ARG)
}

/* ---------------------------------------------------------------- I2C */

/// An I2C master bus, 7-bit addresses.  Closed when dropped.
pub struct I2c(c_int);

impl I2c {
    /// `bus` is 1 for the first bus the board lists.
    pub fn open(bus: c_int, hz: u32) -> Result<I2c> {
        require!(i2c_open);
        check(unsafe { (raw().i2c_open)(bus, hz) })?;
        Ok(I2c(bus))
    }
    pub fn write(&self, addr: u8, buf: &[u8]) -> Result<()> {
        check(unsafe { (raw().i2c_write)(self.0, addr as c_int, buf.as_ptr() as *const c_void, len(buf)?) })
            .map(drop)
    }
    pub fn read(&self, addr: u8, buf: &mut [u8]) -> Result<()> {
        let n = len(buf)?;
        check(unsafe { (raw().i2c_read)(self.0, addr as c_int, buf.as_mut_ptr() as *mut c_void, n) }).map(drop)
    }
    /// Write `tx`, then read `rx` after a repeated start: a register read.
    pub fn transfer(&self, addr: u8, tx: &[u8], rx: &mut [u8]) -> Result<()> {
        let (tl, rl) = (len(tx)?, len(rx)?);
        check(unsafe {
            (raw().i2c_transfer)(
                self.0,
                addr as c_int,
                tx.as_ptr() as *const c_void,
                tl,
                rx.as_mut_ptr() as *mut c_void,
                rl,
            )
        })
        .map(drop)
    }
    /// Whether a device answers at `addr`.
    pub fn probe(&self, addr: u8) -> bool {
        self.write(addr, &[]).is_ok()
    }
}

impl Drop for I2c {
    fn drop(&mut self) {
        unsafe { (raw().i2c_close)(self.0) };
    }
}

/* ---------------------------------------------------------------- SPI */

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SpiMode {
    Mode0,
    Mode1,
    Mode2,
    Mode3,
}

/// An SPI master bus, 8-bit, MSB first.  Chip select is a pin the
/// program drives.  Closed when dropped.
pub struct Spi(c_int);

impl Spi {
    pub fn open(bus: c_int, hz: u32, mode: SpiMode) -> Result<Spi> {
        require!(spi_open);
        let m = match mode {
            SpiMode::Mode0 => sys::FREYA_SPI_MODE0,
            SpiMode::Mode1 => sys::FREYA_SPI_MODE1,
            SpiMode::Mode2 => sys::FREYA_SPI_MODE2,
            SpiMode::Mode3 => sys::FREYA_SPI_MODE3,
        };
        check(unsafe { (raw().spi_open)(bus, hz, m) })?;
        Ok(Spi(bus))
    }
    /// Shift `tx` out and `rx` in; the two must be the same length.
    pub fn transfer(&self, tx: &[u8], rx: &mut [u8]) -> Result<()> {
        if tx.len() != rx.len() {
            return Err(crate::Error::ARG);
        }
        let n = len(tx)?;
        check(unsafe {
            (raw().spi_transfer)(self.0, tx.as_ptr() as *const c_void, rx.as_mut_ptr() as *mut c_void, n)
        })
        .map(drop)
    }
    pub fn write(&self, buf: &[u8]) -> Result<()> {
        check(unsafe { (raw().spi_write)(self.0, buf.as_ptr() as *const c_void, len(buf)?) }).map(drop)
    }
    pub fn read(&self, buf: &mut [u8]) -> Result<()> {
        let n = len(buf)?;
        check(unsafe { (raw().spi_read)(self.0, buf.as_mut_ptr() as *mut c_void, n) }).map(drop)
    }
}

impl Drop for Spi {
    fn drop(&mut self) {
        unsafe { (raw().spi_close)(self.0) };
    }
}

/* ------------------------------------------------------------- 1-Wire */

/// A 1-Wire bus on one open-drain pin.  Closed when dropped.
pub struct W1(c_int);

impl W1 {
    pub fn open(pin: Pin) -> Result<W1> {
        require!(w1_open);
        check(unsafe { (raw().w1_open)(pin.0) })?;
        Ok(W1(pin.0))
    }
    /// The reset and presence pulse: true when a device answered.
    pub fn reset(&self) -> Result<bool> {
        match unsafe { (raw().w1_reset)(self.0) } {
            sys::FREYA_ERR_NACK => Ok(false),
            r => check(r).map(|_| true),
        }
    }
    pub fn write(&self, buf: &[u8]) -> Result<()> {
        check(unsafe { (raw().w1_write)(self.0, buf.as_ptr() as *const c_void, len(buf)?) }).map(drop)
    }
    pub fn read(&self, buf: &mut [u8]) -> Result<()> {
        let n = len(buf)?;
        check(unsafe { (raw().w1_read)(self.0, buf.as_mut_ptr() as *mut c_void, n) }).map(drop)
    }
    /// The next ROM of the search, or `None` once the walk is finished;
    /// the call after that starts over.
    pub fn search(&self) -> Result<Option<[u8; 8]>> {
        let mut rom = [0u8; 8];
        match unsafe { (raw().w1_search)(self.0, rom.as_mut_ptr() as *mut c_void) } {
            sys::FREYA_ERR_NACK => Ok(None),
            r => check(r).map(|_| Some(rom)),
        }
    }
    /// Drive the line high for a parasite-powered device.
    pub fn pullup(&self, on: bool) -> Result<()> {
        check(unsafe { (raw().w1_pullup)(self.0, on as c_int) }).map(drop)
    }
}

impl Drop for W1 {
    fn drop(&mut self) {
        unsafe { (raw().w1_close)(self.0) };
    }
}

/// The 1-Wire CRC-8; 0 over a ROM or scratchpad that carries its own CRC.
pub fn w1_crc(buf: &[u8]) -> Result<u8> {
    require!(w1_crc);
    check(unsafe { (raw().w1_crc)(buf.as_ptr() as *const c_void, len(buf)?) }).map(|c| c as u8)
}

/* ---------------------------------------------------------------- ADC */

fn adc(source: c_int) -> Result<u16> {
    require!(adc_read);
    check(unsafe { (raw().adc_read)(source) }).map(|v| v as u16)
}

/// One 12-bit conversion of a pin, which is left in analog mode.
pub fn adc_read(pin: Pin) -> Result<u16> {
    adc(pin.0)
}
/// The temperature sensor, in raw counts.
pub fn adc_temp() -> Result<u16> {
    adc(sys::FREYA_ADC_TEMP)
}
/// The internal reference, in raw counts.
pub fn adc_vref() -> Result<u16> {
    adc(sys::FREYA_ADC_VREF)
}

/* ------------------------------------------------------------ ciphers */

/// Ascon-AEAD128: the ciphertext of `input` followed by the 16-byte tag
/// into `out`, which needs `input.len() + 16` bytes.  Returns that length.
pub fn aead_encrypt(key: &[u8; 16], nonce: &[u8; 16], ad: &[u8], input: &[u8], out: &mut [u8]) -> Result<usize> {
    require!(aead_encrypt);
    let (al, il, ol) = (len(ad)?, len(input)?, len(out)?);
    check(unsafe {
        (raw().aead_encrypt)(
            key.as_ptr() as *const c_void,
            nonce.as_ptr() as *const c_void,
            if al == 0 { core::ptr::null() } else { ad.as_ptr() as *const c_void },
            al,
            input.as_ptr() as *const c_void,
            il,
            out.as_mut_ptr() as *mut c_void,
            ol,
        )
    })
    .map(|n| n as usize)
}

/// Check the tag on `input` (ciphertext and tag) and write the plaintext.
/// A tag that does not match is [`Error::IO`](crate::Error::IO).
pub fn aead_decrypt(key: &[u8; 16], nonce: &[u8; 16], ad: &[u8], input: &[u8], out: &mut [u8]) -> Result<usize> {
    require!(aead_decrypt);
    let (al, il, ol) = (len(ad)?, len(input)?, len(out)?);
    check(unsafe {
        (raw().aead_decrypt)(
            key.as_ptr() as *const c_void,
            nonce.as_ptr() as *const c_void,
            if al == 0 { core::ptr::null() } else { ad.as_ptr() as *const c_void },
            al,
            input.as_ptr() as *const c_void,
            il,
            out.as_mut_ptr() as *mut c_void,
            ol,
        )
    })
    .map(|n| n as usize)
}

/* -------------------------------------------------------- compression */

/// heatshrink LZSS.  `out` needs at most `sys::freya_compress_bound(len)`.
pub fn compress(input: &[u8], out: &mut [u8]) -> Result<usize> {
    require!(compress);
    let (il, ol) = (len(input)?, len(out)?);
    check(unsafe { (raw().compress)(input.as_ptr() as *const c_void, il, out.as_mut_ptr() as *mut c_void, ol) })
        .map(|n| n as usize)
}

pub fn decompress(input: &[u8], out: &mut [u8]) -> Result<usize> {
    require!(decompress);
    let (il, ol) = (len(input)?, len(out)?);
    check(unsafe { (raw().decompress)(input.as_ptr() as *const c_void, il, out.as_mut_ptr() as *mut c_void, ol) })
        .map(|n| n as usize)
}
