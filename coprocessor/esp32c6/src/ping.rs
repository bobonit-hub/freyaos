//! One asynchronous ICMP echo at a time.  OP_PING_START resolves the name
//! and pings on a task of its own; OP_PING_RESULT collects the outcome.

use std::sync::Mutex;
use std::time::Duration;

use esp_idf_svc::ping::{Configuration, EspPing, Reply};
use freya_c6::status::{AGAIN, ARG, IO};
use freya_c6::{cstr, get32, put32};

use crate::dns::resolve;
use crate::os::{self, lock};

struct State {
    active: bool,
    finished: bool,
    address: u32,
    elapsed: u32,
    replies: u32,
}

static STATE: Mutex<State> =
    Mutex::new(State { active: false, finished: false, address: 0, elapsed: 0, replies: 0 });

fn worker(timeout_ms: u32, name: String) {
    if let Some(ip) = resolve(&name) {
        lock(&STATE).address = u32::from(ip);
        let config = Configuration {
            count: 1,
            interval: Duration::from_millis(10),
            timeout: Duration::from_millis(timeout_ms.into()),
            ..Default::default()
        };
        let (mut elapsed, mut replies) = (0u32, 0u32);
        let _ = EspPing::default().ping_details(ip, &config, |_, reply| {
            if let Reply::Success(info) = reply {
                elapsed += info.elapsed_time.as_millis() as u32;
                replies += 1;
            }
        });
        let mut state = lock(&STATE);
        state.elapsed = elapsed;
        state.replies = replies;
    }
    lock(&STATE).finished = true;
}

/// OP_PING_START: [timeout ms (4), host name, NUL].
pub fn start(data: &[u8]) -> i32 {
    let mut state = lock(&STATE);
    if state.active {
        return AGAIN;
    }
    if data.len() < 6 {
        return ARG;
    }
    let name = cstr(&data[4..]);
    if name.is_empty() || name.len() >= data.len() - 4 {
        return ARG;
    }
    let Ok(name) = String::from_utf8(name.to_vec()) else { return ARG };
    let timeout = get32(data);
    *state = State { active: true, finished: false, address: 0, elapsed: 0, replies: 0 };
    if os::spawn(c"freya_ping", 6144, 5, move || worker(timeout, name)).is_err() {
        state.active = false;
        return IO;
    }
    0
}

/// OP_PING_RESULT: [address, elapsed ms, replies, losses].
pub fn result(reply: &mut [u8]) -> (i32, usize) {
    let mut state = lock(&STATE);
    if !state.active || !state.finished {
        return (AGAIN, 0);
    }
    put32(reply, state.address);
    put32(&mut reply[4..], state.elapsed);
    put32(&mut reply[8..], state.replies);
    put32(&mut reply[12..], 1u32.wrapping_sub(state.replies));
    state.active = false;
    (0, 16)
}
