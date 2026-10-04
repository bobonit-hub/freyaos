//! Name lookups.  OP_RESOLVE gives Freya's HTTP client the IPv4 address of
//! a host for a plain TCP connection (a TLS connection looks its host up
//! itself).  One lookup runs at a time, on a task of its own: the first
//! request for a name starts it and answers AGAIN, and the same request
//! answers AGAIN until the lookup ends, then the address or IO, once.

use std::net::{IpAddr, Ipv4Addr, ToSocketAddrs};
use std::sync::Mutex;

use freya_c6::status::{AGAIN, ARG, IO};
use freya_c6::{cstr, put32};

use crate::os::{self, lock};

struct State {
    busy: bool,
    finished: bool,
    name: Vec<u8>,
    address: Option<u32>,
}

static STATE: Mutex<State> = Mutex::new(State { busy: false, finished: false, name: Vec::new(), address: None });

/// The first IPv4 address of `name`, through lwIP's resolver.
pub fn resolve(name: &str) -> Option<Ipv4Addr> {
    (name, 0).to_socket_addrs().ok()?.find_map(|a| match a.ip() {
        IpAddr::V4(ip) => Some(ip),
        IpAddr::V6(_) => None,
    })
}

fn worker(name: String) {
    let address = resolve(&name).map(u32::from);
    let mut state = lock(&STATE);
    state.address = address;
    state.finished = true;
}

/// OP_RESOLVE: [host name, NUL] -> [address (4)], host byte order like
/// every other address on the link.
pub fn handle(data: &[u8], reply: &mut [u8]) -> (i32, usize) {
    let name = cstr(data);
    if name.is_empty() || name.len() >= data.len() || name.len() > 253 {
        return (ARG, 0);
    }
    let mut state = lock(&STATE);
    if state.busy {
        if state.name == name && state.finished {
            state.busy = false;
            return match state.address {
                Some(address) => {
                    put32(reply, address);
                    (0, 4)
                }
                None => (IO, 0),
            };
        }
        // A lookup still running holds the task; one nobody collected
        // (its caller gave up) gives way to the new name.
        if !state.finished {
            return (AGAIN, 0);
        }
    }
    let Ok(text) = String::from_utf8(name.to_vec()) else { return (ARG, 0) };
    *state = State { busy: true, finished: false, name: name.to_vec(), address: None };
    if os::spawn(c"freya_dns", 4096, 5, move || worker(text)).is_err() {
        state.busy = false;
        return (IO, 0);
    }
    (AGAIN, 0)
}
