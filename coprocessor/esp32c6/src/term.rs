//! The TLS console on TCP port 8022: one session, a login with the user
//! `admin` and Freya's eight-byte terminal password, then the bytes of
//! the STM32's USART2 console both ways.  OP_TERM moves them over SPI.

use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Mutex;
use std::time::Instant;

use esp_idf_svc::sys;
use freya_c6::ring::Ring;
use freya_c6::status::ARG;
use freya_c6::{get16, password_matches, put16};
use log::{info, warn};

use crate::os::{self, lock, Listener, Session};

const PORT: u16 = 8022;
const CHUNK: usize = 240;

const DOWN: u8 = 0;
const LISTEN: u8 = 1;
const LOGIN: u8 = 2;
const OPEN: u8 = 3;

struct Shared {
    /// client to STM32
    rx: Ring<1024>,
    /// STM32 to client
    tx: Ring<1024>,
    pass: [u8; 8],
    pass_on: bool,
}

static SHARED: Mutex<Shared> =
    Mutex::new(Shared { rx: Ring::new(), tx: Ring::new(), pass: [0; 8], pass_on: false });
static STATE: AtomicU8 = AtomicU8::new(DOWN);

/// OP_TERM: [password on, 8 password bytes, tx length (2), tx bytes].  The
/// reply is [state, rx length (2), rx bytes].
pub fn handle(data: &[u8], reply: &mut [u8]) -> (i32, usize) {
    if data.len() < 11 {
        return (ARG, 0);
    }
    let ntx = usize::from(get16(&data[9..]));
    if ntx > CHUNK || 11 + ntx != data.len() {
        return (ARG, 0);
    }
    let nrx = {
        let mut s = lock(&SHARED);
        s.pass_on = data[0] != 0;
        s.pass = if s.pass_on { data[1..9].try_into().unwrap() } else { [0; 8] };
        s.tx.push(&data[11..]);
        s.rx.pop(&mut reply[3..3 + CHUNK])
    };
    reply[0] = if os::net_up() { STATE.load(Ordering::Relaxed) } else { DOWN };
    put16(&mut reply[1..], nrx as u16);
    (0, 3 + nrx)
}

fn send_all(tls: &Session, text: &[u8]) -> Result<(), ()> {
    let start = Instant::now();
    let mut rest = text;
    while !rest.is_empty() {
        let n = tls.write(rest);
        if os::tls_wants(n) {
            if os::expired(start, 10000) {
                return Err(());
            }
            os::yield_tick();
            continue;
        }
        if n <= 0 {
            return Err(());
        }
        rest = &rest[n as usize..];
    }
    Ok(())
}

fn read_line(tls: &Session, line: &mut Vec<u8>, max: usize) -> Result<(), ()> {
    let start = Instant::now();
    line.clear();
    while !os::expired(start, 20000) {
        if !os::net_up() {
            return Err(());
        }
        let mut c = [0u8];
        let r = tls.read(&mut c);
        if os::tls_wants(r) {
            os::yield_tick();
            continue;
        }
        if r <= 0 {
            return Err(());
        }
        match c[0] {
            b'\n' => return Ok(()),
            b'\r' => {}
            _ if line.len() + 1 >= max => return Err(()),
            c => line.push(c),
        }
    }
    Err(())
}

fn login(tls: &Session) -> Result<(), ()> {
    STATE.store(LOGIN, Ordering::Relaxed);
    let mut line = Vec::with_capacity(32);
    send_all(tls, b"username: ")?;
    if read_line(tls, &mut line, 32).is_err() || line != b"admin" {
        let _ = send_all(tls, b"denied\r\n");
        return Err(());
    }
    send_all(tls, b"password: ")?;
    let got = read_line(tls, &mut line, 32).ok().map(|_| line.as_slice());
    let (pass, on) = {
        let s = lock(&SHARED);
        (s.pass, s.pass_on)
    };
    let ok = password_matches(&pass, on, got);
    line.fill(0);
    if !ok {
        let _ = send_all(tls, if on { &b"denied\r\n"[..] } else { b"password not set\r\n" });
        return Err(());
    }
    send_all(tls, b"\r\n")?;
    lock(&SHARED).rx.push(b"\r");
    STATE.store(OPEN, Ordering::Relaxed);
    info!("terminal session open");
    Ok(())
}

/// Moves what is waiting in each direction.  False when the client left.
fn bridge(tls: &Session) -> bool {
    let mut buf = [0u8; CHUNK];
    let r = tls.read(&mut buf);
    if r > 0 {
        lock(&SHARED).rx.push(&buf[..r as usize]);
    } else if !os::tls_wants(r) {
        return false;
    }
    let n = lock(&SHARED).tx.pop(&mut buf);
    let mut off = 0;
    while off < n {
        let w = tls.write(&buf[off..n]);
        if os::tls_wants(w) {
            os::yield_tick();
            continue;
        }
        if w <= 0 {
            return false;
        }
        off += w as usize;
    }
    true
}

fn drop_session(session: &mut Option<Session>) {
    *session = None;
    {
        let mut s = lock(&SHARED);
        s.rx.clear();
        s.tx.clear();
    }
    STATE.store(if os::net_up() { LISTEN } else { DOWN }, Ordering::Relaxed);
}

pub fn run() {
    let mut cfg = os::server_config();
    let mut listener: Option<Listener> = None;
    let mut session: Option<Session> = None;

    loop {
        if !os::net_up() {
            drop_session(&mut session);
            listener = None;
            STATE.store(DOWN, Ordering::Relaxed);
            os::sleep_ms(200);
            continue;
        }
        if listener.is_none() {
            listener = Listener::open(PORT);
            if listener.is_none() {
                os::sleep_ms(1000);
                continue;
            }
            info!("TLS terminal listening on {PORT}");
            STATE.store(LISTEN, Ordering::Relaxed);
        }
        if session.is_none() {
            let Some((fd, _)) = os::accept(listener.as_ref().unwrap().0) else {
                os::sleep_ms(50);
                continue;
            };
            os::set_nonblocking(fd, false);
            os::set_option(fd, sys::IPPROTO_TCP, sys::TCP_NODELAY, 1);
            let Some(tls) = Session::handshake(&mut cfg, fd) else {
                warn!("TLS handshake failed");
                continue;
            };
            os::set_nonblocking(tls.fd(), true);
            if login(&tls).is_err() {
                session = Some(tls);
                drop_session(&mut session);
                continue;
            }
            session = Some(tls);
        }
        if !bridge(session.as_ref().unwrap()) {
            info!("terminal session closed");
            drop_session(&mut session);
            continue;
        }
        os::sleep_ms(10);
    }
}
