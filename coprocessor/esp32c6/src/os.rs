//! Thin safe wrappers over the ESP-IDF and lwIP calls the services share:
//! sockets, the TLS server session, task creation and the link state.

use core::ffi::{c_int, c_void, CStr};
use core::mem::{size_of, zeroed};
use core::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use esp_idf_svc::hal::task::thread::ThreadSpawnConfiguration;
use esp_idf_svc::sys;
use freya_c6::status::{AGAIN, IO};

/// True from DHCP's address until a Wi-Fi disconnect or OP_WIFI_OFF.  The
/// terminal and web servers listen only while it is set.
static NET_UP: AtomicBool = AtomicBool::new(false);

pub fn net_up() -> bool {
    NET_UP.load(Ordering::Relaxed)
}

pub fn set_net_up(up: bool) {
    NET_UP.store(up, Ordering::Relaxed);
}

/// A lock that survives a panicked holder; the data stays usable.
pub fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

pub fn sleep_ms(ms: u64) {
    std::thread::sleep(Duration::from_millis(ms));
}

/// One FreeRTOS tick, the shortest wait.
pub fn yield_tick() {
    unsafe { sys::vTaskDelay(1) };
}

pub fn expired(start: Instant, ms: u64) -> bool {
    start.elapsed() > Duration::from_millis(ms)
}

/// Starts a thread as a FreeRTOS task with this name, stack and priority.
pub fn spawn<F>(name: &'static CStr, stack: usize, priority: u8, f: F) -> std::io::Result<JoinHandle<()>>
where
    F: FnOnce() + Send + 'static,
{
    let config = ThreadSpawnConfiguration {
        name: Some(name),
        stack_size: stack,
        priority,
        ..Default::default()
    };
    config.set().map_err(|_| std::io::Error::from(std::io::ErrorKind::Other))?;
    let result = std::thread::Builder::new().stack_size(stack).spawn(f);
    let _ = ThreadSpawnConfiguration::default().set();
    result
}

pub fn errno() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

/// AGAIN for the errors a nonblocking call retries, otherwise IO.
pub fn errno_status() -> i32 {
    match errno() as u32 {
        sys::EAGAIN | sys::EINPROGRESS | sys::ETIMEDOUT | sys::EALREADY => AGAIN,
        _ => IO,
    }
}

pub fn set_nonblocking(fd: c_int, on: bool) {
    unsafe {
        let flags = sys::lwip_fcntl(fd, sys::F_GETFL as c_int, 0);
        let flags = if on {
            flags | sys::O_NONBLOCK as c_int
        } else {
            flags & !(sys::O_NONBLOCK as c_int)
        };
        sys::lwip_fcntl(fd, sys::F_SETFL as c_int, flags);
    }
}

pub fn set_option(fd: c_int, level: u32, name: u32, value: c_int) {
    unsafe {
        sys::lwip_setsockopt(
            fd,
            level as c_int,
            name as c_int,
            (&value as *const c_int).cast(),
            size_of::<c_int>() as u32,
        );
    }
}

pub fn close(fd: c_int) {
    if fd >= 0 {
        unsafe { sys::lwip_close(fd) };
    }
}

/// An IPv4 socket address; `address` and `port` are in host order.
pub fn sockaddr(address: u32, port: u16) -> sys::sockaddr_in {
    let mut sa: sys::sockaddr_in = unsafe { zeroed() };
    sa.sin_len = size_of::<sys::sockaddr_in>() as u8;
    sa.sin_family = sys::AF_INET as _;
    sa.sin_port = port.to_be();
    sa.sin_addr.s_addr = address.to_be();
    sa
}

pub fn peer(sa: &sys::sockaddr_in) -> (u32, u16) {
    (u32::from_be(sa.sin_addr.s_addr), u16::from_be(sa.sin_port))
}

pub fn sockaddr_ptr(sa: &sys::sockaddr_in) -> *const sys::sockaddr {
    (sa as *const sys::sockaddr_in).cast()
}

pub fn sockaddr_mut(sa: &mut sys::sockaddr_in) -> *mut sys::sockaddr {
    (sa as *mut sys::sockaddr_in).cast()
}

pub const SOCKADDR_LEN: u32 = size_of::<sys::sockaddr_in>() as u32;

/// Accepts one pending connection: the descriptor and the peer.
pub fn accept(fd: c_int) -> Option<(c_int, sys::sockaddr_in)> {
    let mut sa: sys::sockaddr_in = unsafe { zeroed() };
    let mut len = SOCKADDR_LEN;
    let client = unsafe { sys::lwip_accept(fd, sockaddr_mut(&mut sa), &mut len) };
    (client >= 0).then_some((client, sa))
}

/// A nonblocking TCP listener on every address.
pub struct Listener(pub c_int);

impl Listener {
    pub fn open(port: u16) -> Option<Self> {
        let fd = unsafe {
            sys::lwip_socket(sys::AF_INET as c_int, sys::SOCK_STREAM as c_int, sys::IPPROTO_TCP as c_int)
        };
        if fd < 0 {
            return None;
        }
        let listener = Listener(fd);
        set_option(fd, sys::SOL_SOCKET, sys::SO_REUSEADDR, 1);
        let sa = sockaddr(0, port);
        let ok = unsafe { sys::lwip_bind(fd, sockaddr_ptr(&sa), SOCKADDR_LEN) == 0 && sys::lwip_listen(fd, 1) == 0 };
        if !ok {
            return None;
        }
        set_nonblocking(fd, true);
        Some(listener)
    }
}

impl Drop for Listener {
    fn drop(&mut self) {
        close(self.0);
    }
}

pub fn tls_wants(n: isize) -> bool {
    n == sys::ESP_TLS_ERR_SSL_WANT_READ as isize || n == sys::ESP_TLS_ERR_SSL_WANT_WRITE as isize
}

/// The self-signed console certificate and its key, NUL-terminated PEM as
/// mbedTLS wants it.  build.rs checks that the key file is there.
static SERVER_CERT: &str = concat!(include_str!("../certs/freya.crt"), "\0");
static SERVER_KEY: &str = concat!(include_str!("../certs/freya.key"), "\0");

pub fn server_config() -> sys::esp_tls_cfg_server_t {
    let mut cfg: sys::esp_tls_cfg_server_t = unsafe { zeroed() };
    cfg.__bindgen_anon_3.servercert_buf = SERVER_CERT.as_ptr();
    cfg.__bindgen_anon_4.servercert_bytes = SERVER_CERT.len() as u32;
    cfg.__bindgen_anon_5.serverkey_buf = SERVER_KEY.as_ptr();
    cfg.__bindgen_anon_6.serverkey_bytes = SERVER_KEY.len() as u32;
    cfg.tls_handshake_timeout_ms = 10000;
    cfg
}

/// A TLS server session on an accepted connection.  Dropping it ends the
/// session and closes the connection.
pub struct Session {
    tls: *mut sys::esp_tls_t,
    fd: c_int,
}

impl Session {
    /// Runs the handshake on a blocking `fd`, which the session then owns
    /// (it is closed if the handshake fails).
    pub fn handshake(cfg: &mut sys::esp_tls_cfg_server_t, fd: c_int) -> Option<Self> {
        let tls = unsafe { sys::esp_tls_init() };
        if tls.is_null() {
            close(fd);
            return None;
        }
        let session = Session { tls, fd };
        (unsafe { sys::esp_tls_server_session_create(cfg, fd, tls) } == 0).then_some(session)
    }

    pub fn fd(&self) -> c_int {
        self.fd
    }

    pub fn read(&self, buf: &mut [u8]) -> isize {
        unsafe { sys::esp_tls_conn_read(self.tls, buf.as_mut_ptr().cast(), buf.len()) }
    }

    pub fn write(&self, buf: &[u8]) -> isize {
        unsafe { sys::esp_tls_conn_write(self.tls, buf.as_ptr().cast(), buf.len()) }
    }

    pub fn is_tls13(&self) -> bool {
        let ssl = unsafe { sys::esp_tls_get_ssl_context(self.tls) };
        !ssl.is_null() && unsafe { CStr::from_ptr(sys::mbedtls_ssl_get_version(ssl.cast())) } == c"TLSv1.3"
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        unsafe { sys::esp_tls_server_session_delete(self.tls) };
        close(self.fd);
    }
}

/// A buffer the SPI DMA can reach, word-aligned.  It lives for the rest of
/// the run.
pub fn dma_buffer<const N: usize>() -> &'static mut [u8; N] {
    let p: *mut c_void = unsafe { sys::heap_caps_aligned_alloc(4, N, sys::MALLOC_CAP_DMA) };
    assert!(!p.is_null(), "no DMA memory for the SPI frames");
    unsafe {
        ptr::write_bytes(p.cast::<u8>(), 0, N);
        &mut *p.cast::<[u8; N]>()
    }
}
