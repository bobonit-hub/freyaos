//! The HTTPS server on port 443, TLS 1.3 only.  It parses one request at a
//! time and hands it to the STM32 file service (`samples/httpd`), which
//! drives it with OP_WEB commands:
//!
//! - 0: take the request (method, path, query, POST length)
//! - 4: take a piece of a POST body
//! - 1: start the response (status, length, content type)
//! - 2: a piece of the response body
//! - 3: the response is complete
//!
//! An 8 KiB ring carries the POST body towards the STM32 and then the
//! response body towards the client.

use std::sync::Mutex;
use std::time::Instant;

use esp_idf_svc::sys;
use freya_c6::request::{self, Method, Request};
use freya_c6::ring::Ring;
use freya_c6::status::{AGAIN, ARG, IO};
use freya_c6::{get16, get32, put32};
use log::{info, warn};

use crate::os::{self, lock, Listener, Session};

const PORT: u16 = 443;
const REQUEST_MAX: usize = 2048;
const READ_MAX: usize = 480;
const TYPE_MAX: usize = 40;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Idle,
    /// Parsed, waiting for the STM32 to take it (cmd 0).
    Request,
    /// The STM32 has it; for a POST the ring carries the body to cmd 4
    /// until `req_fed` reaches the Content-Length.
    Taken,
    /// Cmd 1 started the response; the ring carries cmd 2's bytes to TLS.
    Body,
    /// Cmd 3.
    Done,
}

struct Job {
    phase: Phase,
    method: Method,
    status: u16,
    length: u32,
    sent: u32,
    /// The Content-Length of a POST.
    req_len: u32,
    /// Body bytes pushed into the ring.
    req_fed: u32,
    /// Body bytes the STM32 took.
    req_taken: u32,
    path: Vec<u8>,
    query: Vec<u8>,
    content_type: Vec<u8>,
    ring: Ring<8192>,
}

impl Job {
    fn reset(&mut self) {
        self.phase = Phase::Idle;
        self.sent = 0;
        self.length = 0;
        self.req_len = 0;
        self.req_fed = 0;
        self.req_taken = 0;
        self.ring.clear();
    }
}

static JOB: Mutex<Job> = Mutex::new(Job {
    phase: Phase::Idle,
    method: Method::Get,
    status: 0,
    length: 0,
    sent: 0,
    req_len: 0,
    req_fed: 0,
    req_taken: 0,
    path: Vec::new(),
    query: Vec::new(),
    content_type: Vec::new(),
    ring: Ring::new(),
});

/// OP_WEB from the STM32.
pub fn handle(data: &[u8], reply: &mut [u8]) -> (i32, usize) {
    let Some(&cmd) = data.first() else { return (ARG, 0) };
    let mut job = lock(&JOB);
    // The status for a command that came in the wrong phase.
    let misplaced = |job: &Job, wanted: Phase| if job.phase == wanted { ARG } else { IO };

    match cmd {
        0 => {
            if job.phase != Phase::Request {
                return (AGAIN, 0);
            }
            let (plen, qlen) = (job.path.len(), job.query.len());
            reply[0] = job.method as u8;
            reply[1] = plen as u8;
            reply[2] = qlen as u8;
            reply[3..3 + plen].copy_from_slice(&job.path);
            reply[3 + plen..3 + plen + qlen].copy_from_slice(&job.query);
            let mut n = 3 + plen + qlen;
            if job.method == Method::Post {
                put32(&mut reply[n..], job.req_len);
                n += 4;
            }
            job.phase = Phase::Taken;
            (0, n)
        }
        1 => {
            if data.len() < 8 || job.phase != Phase::Taken {
                return (misplaced(&job, Phase::Taken), 0);
            }
            let status = get16(&data[1..]);
            let tlen = usize::from(data[7]);
            if !(100..=599).contains(&status) || !(1..=TYPE_MAX).contains(&tlen) || 8 + tlen != data.len() {
                return (ARG, 0);
            }
            job.status = status;
            job.length = get32(&data[3..]);
            job.sent = 0;
            job.content_type = data[8..].to_vec();
            // Whatever of a request body was not read is dropped here.
            job.ring.clear();
            job.phase = Phase::Body;
            (0, 0)
        }
        2 => {
            if data.len() < 3 || job.phase != Phase::Body {
                return (misplaced(&job, Phase::Body), 0);
            }
            let n = usize::from(get16(&data[1..]));
            if n < 1 || 3 + n != data.len() || job.sent + n as u32 > job.length {
                return (ARG, 0);
            }
            if !job.ring.push_all(&data[3..]) {
                return (AGAIN, 0);
            }
            job.sent += n as u32;
            (0, 0)
        }
        3 => {
            // HEAD carries the real length and no body.
            let head = job.method == Method::Head;
            if job.phase != Phase::Body || (head && job.sent != 0) || (!head && job.sent != job.length) {
                return (IO, 0);
            }
            job.phase = Phase::Done;
            (0, 0)
        }
        4 => {
            // [4, max (2)].  The reply is body bytes; none once all of the
            // body has been handed over.
            if data.len() < 3 || job.phase != Phase::Taken {
                return (misplaced(&job, Phase::Taken), 0);
            }
            let want = usize::from(get16(&data[1..]));
            if !(1..=READ_MAX).contains(&want) {
                return (ARG, 0);
            }
            if job.method != Method::Post {
                return (0, 0);
            }
            let n = job.ring.pop(&mut reply[..want]);
            if n == 0 && job.req_fed < job.req_len {
                return (AGAIN, 0);
            }
            job.req_taken += n as u32;
            (0, n)
        }
        _ => (ARG, 0),
    }
}

fn write_all(tls: &Session, data: &[u8]) -> Result<(), ()> {
    let mut start = Instant::now();
    let mut rest = data;
    while !rest.is_empty() {
        if !os::net_up() {
            return Err(());
        }
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
        start = Instant::now();
    }
    Ok(())
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        411 => "Length Required",
        413 => "Payload Too Large",
        503 => "Unavailable",
        _ => "Error",
    }
}

fn header(status: u16, content_type: &[u8], length: u32) -> String {
    format!(
        "HTTP/1.1 {status} {}\r\nContent-Type: {}\r\nContent-Length: {length}\r\n\
         Connection: close\r\nServer: Freya\r\n\r\n",
        reason(status),
        String::from_utf8_lossy(content_type),
    )
}

fn reply_fixed(tls: &Session, status: u16, body: &str) -> Result<(), ()> {
    write_all(tls, header(status, b"text/plain", body.len() as u32).as_bytes())?;
    write_all(tls, body.as_bytes())
}

/// Reads up to the blank line: the bytes read and where the body starts.
fn read_headers(tls: &Session, buf: &mut [u8]) -> Option<(usize, usize)> {
    let start = Instant::now();
    let mut n = 0;
    while !os::expired(start, 10000) {
        if !os::net_up() {
            return None;
        }
        let end = buf.len() - 1;
        let r = tls.read(&mut buf[n..end]);
        if os::tls_wants(r) {
            os::yield_tick();
            continue;
        }
        if r <= 0 {
            return None;
        }
        // Look again from just before the new bytes: the blank line may
        // straddle two reads.
        let from = n.saturating_sub(3);
        n += r as usize;
        if let Some(end) = buf[from..n].windows(4).position(|w| w == b"\r\n\r\n") {
            return Some((n, from + end + 4));
        }
        if n >= buf.len() - 1 {
            return None;
        }
    }
    None
}

/// Moves POST body bytes from TLS into the ring while the STM32 is still
/// reading them.  The bytes moved, or `Err` when the client went away.
fn feed_body(tls: &Session, chunk: &mut [u8]) -> Result<usize, ()> {
    let want = {
        let job = lock(&JOB);
        if !matches!(job.phase, Phase::Request | Phase::Taken) || job.req_fed >= job.req_len {
            return Ok(0);
        }
        chunk.len().min(job.ring.free()).min((job.req_len - job.req_fed) as usize)
    };
    if want == 0 {
        return Ok(0);
    }
    let r = tls.read(&mut chunk[..want]);
    if os::tls_wants(r) {
        return Ok(0);
    }
    if r <= 0 {
        return Err(());
    }
    let r = r as usize;
    let mut job = lock(&JOB);
    if matches!(job.phase, Phase::Request | Phase::Taken) && job.ring.push_all(&chunk[..r]) {
        job.req_fed += r as u32;
    }
    Ok(r)
}

/// Waits for the STM32's response and sends it.
fn serve(tls: &Session, method: Method) -> Result<(), ()> {
    let mut chunk = [0u8; 480];
    let mut start = Instant::now();
    let mut taken_seen = 0;

    let (status, length, content_type) = loop {
        if !os::net_up() {
            return Err(());
        }
        let moved = if method == Method::Post { feed_body(tls, &mut chunk)? } else { 0 };
        let (phase, taken) = {
            let job = lock(&JOB);
            if job.phase == Phase::Body {
                break (job.status, job.length, job.content_type.clone());
            }
            (job.phase, job.req_taken)
        };
        if phase == Phase::Idle {
            return Err(());
        }
        // Progress on the body restarts the clock: a long upload is bounded
        // by the STM32's pace, not by one timeout.
        if moved > 0 || taken != taken_seen {
            taken_seen = taken;
            start = Instant::now();
        }
        if os::expired(start, 15000) {
            {
                let mut job = lock(&JOB);
                if job.phase == Phase::Body {
                    break (job.status, job.length, job.content_type.clone());
                }
                job.reset();
            }
            let _ = reply_fixed(tls, 503, "file service down\n");
            return Err(());
        }
        if moved == 0 {
            if method == Method::Post {
                os::yield_tick();
            } else {
                os::sleep_ms(20);
            }
        }
    };

    write_all(tls, header(status, &content_type, length).as_bytes())?;

    let mut got = 0;
    start = Instant::now();
    while method != Method::Head && got < length {
        let (n, phase) = {
            let mut job = lock(&JOB);
            (job.ring.pop(&mut chunk), job.phase)
        };
        if n > 0 {
            write_all(tls, &chunk[..n])?;
            got += n as u32;
            start = Instant::now();
            continue;
        }
        if phase == Phase::Idle || !os::net_up() || os::expired(start, 15000) {
            return Err(());
        }
        os::sleep_ms(10);
    }

    start = Instant::now();
    loop {
        let phase = lock(&JOB).phase;
        if phase == Phase::Done {
            return Ok(());
        }
        if phase == Phase::Idle || !os::net_up() || os::expired(start, 15000) {
            return Err(());
        }
        os::sleep_ms(10);
    }
}

/// Handles one request on a fresh session.
fn exchange(tls: &Session, buf: &mut [u8]) {
    let Some((n, header_len)) = read_headers(tls, buf) else { return };
    let request = match request::parse(&buf[..header_len]) {
        Ok(request) => request,
        Err(e) => {
            let (status, text) = match e {
                request::Error::Method => (405, "method not allowed\n"),
                request::Error::LengthRequired => (411, "length required\n"),
                request::Error::TooLarge => (413, "body too large\n"),
                request::Error::Bad => (400, "bad request\n"),
            };
            let _ = reply_fixed(tls, status, text);
            return;
        }
    };
    let Request { method, path, query, body_len, expect_continue } = request;
    let shown = String::from_utf8_lossy(&path).into_owned();
    // Body bytes that arrived with the headers go first into the ring.
    let carry = if method == Method::Post { (n - header_len).min(body_len as usize) } else { 0 };
    let fed = {
        let mut job = lock(&JOB);
        job.reset();
        job.method = method;
        job.req_len = body_len;
        job.ring.push_all(&buf[header_len..header_len + carry]);
        job.req_fed = carry as u32;
        job.path = path;
        job.query = query;
        job.phase = Phase::Request;
        job.req_fed
    };
    if method == Method::Post {
        info!("POST {shown}, {body_len} bytes");
    } else {
        info!("{} {shown}", method.name());
    }
    if expect_continue && fed < body_len && write_all(tls, b"HTTP/1.1 100 Continue\r\n\r\n").is_err() {
        return;
    }
    if serve(tls, method).is_err() {
        warn!("response failed for {shown}");
    }
}

pub fn run() {
    let mut cfg = os::server_config();
    let mut listener: Option<Listener> = None;
    let mut buf = vec![0u8; REQUEST_MAX];

    loop {
        if !os::net_up() {
            lock(&JOB).reset();
            listener = None;
            os::sleep_ms(200);
            continue;
        }
        if listener.is_none() {
            listener = Listener::open(PORT);
            if listener.is_none() {
                os::sleep_ms(1000);
                continue;
            }
            info!("TLS web server listening on {PORT}");
        }
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
        if !tls.is_tls13() {
            warn!("refusing non-TLS1.3");
        } else {
            os::set_nonblocking(tls.fd(), true);
            exchange(&tls, &mut buf);
        }
        drop(tls);
        lock(&JOB).reset();
    }
}
