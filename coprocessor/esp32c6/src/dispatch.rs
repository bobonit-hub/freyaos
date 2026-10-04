//! The RPC operations: Wi-Fi, the four application sockets (plain or TLS
//! client) and the syslog socket, and the hand-offs to the ping, name
//! lookup, terminal and web services.

use core::ffi::c_int;
use core::mem::zeroed;
use std::time::{SystemTime, UNIX_EPOCH};

use esp_idf_svc::handle::RawHandle;
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use esp_idf_svc::sntp::EspSntp;
use esp_idf_svc::sys;
use esp_idf_svc::wifi::EspWifi;
use freya_c6::frame::PAYLOAD;
use freya_c6::status::{AGAIN, ARG, IO, NACK};
use freya_c6::{cstr, get16, get32, put16, put32};

use crate::os::{self, errno_status};
use crate::{dns, ping, term, web};

const MAX_SOCKETS: usize = 4;
const RECV_MAX: usize = 480;

#[allow(dead_code)]
mod op {
    pub const FETCH: u16 = 0;
    pub const WIFI_ON: u16 = 1;
    pub const WIFI_OFF: u16 = 2;
    pub const CREDENTIALS: u16 = 3;
    pub const CONNECT: u16 = 4;
    pub const DISCONNECT: u16 = 5;
    pub const STATUS: u16 = 6;
    pub const SCAN_START: u16 = 7;
    pub const SCAN_NEXT: u16 = 8;
    pub const PING_START: u16 = 9;
    pub const PING_RESULT: u16 = 10;
    pub const SOCKET: u16 = 11;
    pub const CLOSE: u16 = 12;
    pub const SOCK_CONNECT: u16 = 13;
    pub const BIND: u16 = 14;
    pub const LISTEN: u16 = 15;
    pub const ACCEPT: u16 = 16;
    pub const SEND: u16 = 17;
    pub const RECV: u16 = 18;
    pub const SENDTO: u16 = 19;
    pub const RECVFROM: u16 = 20;
    pub const TLS_CONNECT: u16 = 21;
    // 22..25 were the HTTP job of the shell's old curl; Freya's HTTP
    // client now runs on the STM32 over the sockets.
    pub const TERM: u16 = 26;
    pub const WEB: u16 = 27;
    pub const SYSLOG: u16 = 28;
    pub const RESOLVE: u16 = 29;
}
pub use op::FETCH;

/// A socket upgraded to a TLS 1.3 client connection.
struct TlsSlot {
    tls: *mut sys::esp_tls_t,
    cfg: Box<sys::esp_tls_cfg_t>,
    host: Vec<u8>,
    port: u16,
    connected: bool,
}

impl Drop for TlsSlot {
    fn drop(&mut self) {
        unsafe { sys::esp_tls_conn_destroy(self.tls) };
    }
}

/// [address (4), port (2), 2 spare] at `offset`, as a socket address.
fn endpoint(data: &[u8], offset: usize) -> Option<sys::sockaddr_in> {
    (data.len() >= offset + 8).then(|| os::sockaddr(get32(&data[offset..]), get16(&data[offset + 4..])))
}

fn esp_ok(err: sys::esp_err_t) -> i32 {
    if err == sys::ESP_OK {
        0
    } else {
        IO
    }
}

fn copy_ssid(to: &mut [u8], ssid: &[u8]) {
    let ssid = cstr(&ssid[..ssid.len().min(32)]);
    to[..ssid.len()].copy_from_slice(ssid);
}

pub struct Coproc {
    wifi: EspWifi<'static>,
    nvs: EspDefaultNvsPartition,
    wifi_started: bool,
    sockets: [c_int; MAX_SOCKETS],
    tls: [Option<TlsSlot>; MAX_SOCKETS],
    /// Remote syslog sends from this socket, not from one of the four.
    syslog_fd: c_int,
    scan: Vec<sys::wifi_ap_record_t>,
    scan_pos: usize,
    sntp: Option<EspSntp<'static>>,
}

impl Coproc {
    pub fn new(wifi: EspWifi<'static>, nvs: EspDefaultNvsPartition) -> Self {
        Self {
            wifi,
            nvs,
            wifi_started: false,
            sockets: [-1; MAX_SOCKETS],
            tls: Default::default(),
            syslog_fd: -1,
            scan: Vec::new(),
            scan_pos: 0,
            sntp: None,
        }
    }

    /// The first application socket readable (or with a pending accept),
    /// as the payload of an event frame.
    pub fn event(&self) -> Option<[u8; 2]> {
        let mut fds: Vec<sys::pollfd> = self
            .sockets
            .iter()
            .filter(|&&fd| fd >= 0)
            .map(|&fd| sys::pollfd { fd, events: sys::POLLIN as _, revents: 0 })
            .collect();
        if fds.is_empty() || unsafe { sys::lwip_poll(fds.as_mut_ptr(), fds.len() as _, 0) } <= 0 {
            return None;
        }
        let ready = fds.iter().find(|p| p.revents != 0)?;
        let slot = self.sockets.iter().position(|&fd| fd == ready.fd)?;
        Some([2, slot as u8])
    }

    fn wifi_start(&mut self) -> bool {
        if !self.wifi_started {
            self.wifi_started = unsafe { sys::esp_wifi_start() } == sys::ESP_OK;
        }
        self.wifi_started
    }

    fn close_slot(&mut self, slot: usize) {
        self.tls[slot] = None;
        os::close(self.sockets[slot]);
        self.sockets[slot] = -1;
    }

    fn free_slot(&self) -> Option<usize> {
        (0..MAX_SOCKETS).find(|&i| self.sockets[i] < 0 && self.tls[i].is_none())
    }

    /// The SSID and password saved by OP_CREDENTIALS.
    fn credentials(&self) -> Option<(Vec<u8>, Vec<u8>)> {
        let nvs = EspNvs::<NvsDefault>::new(self.nvs.clone(), "freya", false).ok()?;
        let mut ssid = [0u8; 33];
        let mut pass = [0u8; 65];
        let ssid = nvs.get_str("ssid", &mut ssid).ok()??.as_bytes().to_vec();
        let pass = nvs.get_str("pass", &mut pass).ok()??.as_bytes().to_vec();
        Some((ssid, pass))
    }

    /// OP_CREDENTIALS: [SSID, NUL, password, NUL] into NVS.
    fn set_credentials(&self, data: &[u8]) -> i32 {
        let ssid = cstr(data);
        if ssid.is_empty() || ssid.len() >= data.len() || ssid.len() > 32 {
            return ARG;
        }
        let rest = &data[ssid.len() + 1..];
        let pass = cstr(rest);
        if pass.len() >= rest.len() || pass.len() > 64 {
            return ARG;
        }
        let (Ok(ssid), Ok(pass)) = (core::str::from_utf8(ssid), core::str::from_utf8(pass)) else {
            return ARG;
        };
        let Ok(nvs) = EspNvs::<NvsDefault>::new(self.nvs.clone(), "freya", true) else { return IO };
        match nvs.set_str("ssid", ssid).and_then(|_| nvs.set_str("pass", pass)) {
            Ok(()) => 0,
            Err(_) => IO,
        }
    }

    fn connect(&mut self) -> i32 {
        let Some((ssid, pass)) = self.credentials() else { return ARG };
        if !self.wifi_start() {
            return IO;
        }
        let mut config: sys::wifi_config_t = unsafe { zeroed() };
        unsafe {
            config.sta.ssid[..ssid.len()].copy_from_slice(&ssid);
            config.sta.password[..pass.len()].copy_from_slice(&pass);
            if sys::esp_wifi_set_config(sys::wifi_interface_t_WIFI_IF_STA, &mut config) != sys::ESP_OK {
                return IO;
            }
        }
        match unsafe { sys::esp_wifi_connect() } {
            sys::ESP_OK | sys::ESP_ERR_WIFI_CONN => 0,
            _ => IO,
        }
    }

    /// [state, RSSI, address, gateway, mask, SSID (32 + 4 spare)].
    fn status(&self, reply: &mut [u8]) -> usize {
        let mut state = 0;
        let mut rssi = 0;
        let mut ip: sys::esp_netif_ip_info_t = unsafe { zeroed() };
        let mut ap: sys::wifi_ap_record_t = unsafe { zeroed() };
        if self.wifi_started {
            state = 1;
            if unsafe { sys::esp_wifi_sta_get_ap_info(&mut ap) } == sys::ESP_OK {
                state = 3;
                rssi = i32::from(ap.rssi);
                unsafe { sys::esp_netif_get_ip_info(self.wifi.sta_netif().handle(), &mut ip) };
            } else {
                ap = unsafe { zeroed() };
            }
        }
        put32(reply, state);
        put32(&mut reply[4..], rssi as u32);
        put32(&mut reply[8..], u32::from_be(ip.ip.addr));
        put32(&mut reply[12..], u32::from_be(ip.gw.addr));
        put32(&mut reply[16..], u32::from_be(ip.netmask.addr));
        copy_ssid(&mut reply[20..], &ap.ssid);
        56
    }

    fn scan_start(&mut self) -> i32 {
        if !self.wifi_start() {
            return IO;
        }
        self.scan = Vec::new();
        self.scan_pos = 0;
        let mut count: u16 = 0;
        unsafe {
            if sys::esp_wifi_scan_start(core::ptr::null(), true) != sys::ESP_OK
                || sys::esp_wifi_scan_get_ap_num(&mut count) != sys::ESP_OK
            {
                return IO;
            }
            if count == 0 {
                return 0;
            }
            let mut records = vec![zeroed::<sys::wifi_ap_record_t>(); usize::from(count)];
            if sys::esp_wifi_scan_get_ap_records(&mut count, records.as_mut_ptr()) != sys::ESP_OK {
                return IO;
            }
            records.truncate(usize::from(count));
            self.scan = records;
        }
        0
    }

    /// [RSSI, channel, auth mode, 2 spare, SSID (32 + 4 spare)].
    fn scan_next(&mut self, reply: &mut [u8]) -> (i32, usize) {
        let Some(ap) = self.scan.get(self.scan_pos) else {
            self.scan = Vec::new();
            return (NACK, 0);
        };
        put32(reply, i32::from(ap.rssi) as u32);
        reply[4] = ap.primary;
        reply[5] = ap.authmode as u8;
        copy_ssid(&mut reply[8..], &ap.ssid);
        self.scan_pos += 1;
        (0, 44)
    }

    fn wifi_off(&mut self) -> i32 {
        for slot in 0..MAX_SOCKETS {
            self.close_slot(slot);
        }
        os::close(self.syslog_fd);
        self.syslog_fd = -1;
        unsafe {
            sys::esp_wifi_disconnect();
            if self.wifi_started {
                sys::esp_wifi_stop();
            }
        }
        self.wifi_started = false;
        os::set_net_up(false);
        0
    }

    /// OP_SYSLOG: [address (4), port (2), datagram].
    fn syslog(&mut self, data: &[u8]) -> i32 {
        let Some(peer) = endpoint(data, 0) else { return ARG };
        if self.syslog_fd < 0 {
            let fd = unsafe {
                sys::lwip_socket(sys::AF_INET as c_int, sys::SOCK_DGRAM as c_int, sys::IPPROTO_UDP as c_int)
            };
            if fd < 0 {
                return IO;
            }
            os::set_nonblocking(fd, true);
            self.syslog_fd = fd;
        }
        let datagram = &data[6..];
        let n = unsafe {
            sys::lwip_sendto(
                self.syslog_fd,
                datagram.as_ptr().cast(),
                datagram.len(),
                0,
                os::sockaddr_ptr(&peer),
                os::SOCKADDR_LEN,
            )
        };
        if n < 0 {
            errno_status()
        } else {
            n as i32
        }
    }

    fn socket(&mut self, data: &[u8]) -> i32 {
        if data.len() != 6 {
            return ARG;
        }
        let (domain, kind) = (u32::from(get16(data)), u32::from(get16(&data[2..])));
        if domain != sys::AF_INET || (kind != sys::SOCK_STREAM && kind != sys::SOCK_DGRAM) {
            return ARG;
        }
        let Some(slot) = self.free_slot() else { return IO };
        let fd = unsafe { sys::lwip_socket(sys::AF_INET as c_int, kind as c_int, c_int::from(get16(&data[4..]))) };
        if fd < 0 {
            return IO;
        }
        os::set_nonblocking(fd, true);
        self.sockets[slot] = fd;
        slot as i32
    }

    /// OP_TLS_CONNECT: [slot (2), port (2), host name, NUL].  The plain TCP
    /// socket in the slot is replaced by an ESP-TLS connection; retried
    /// until the handshake completes.
    fn tls_connect(&mut self, slot: usize, data: &[u8]) -> i32 {
        if data.len() < 6 {
            return ARG;
        }
        let port = get16(&data[2..]);
        let host = cstr(&data[4..]);
        if port == 0 || host.is_empty() || host.len() >= data.len() - 4 || host.len() > 253 {
            return ARG;
        }
        // Certificates cannot be checked before the clock is right.
        let now = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs());
        if now < 1_700_000_000 {
            if self.sntp.is_none() {
                match EspSntp::new_default() {
                    Ok(sntp) => self.sntp = Some(sntp),
                    Err(_) => return IO,
                }
            }
            return AGAIN;
        }
        match &self.tls[slot] {
            None => {
                if self.sockets[slot] < 0 {
                    return ARG;
                }
                os::close(self.sockets[slot]);
                self.sockets[slot] = -1;
                let tls = unsafe { sys::esp_tls_init() };
                if tls.is_null() {
                    return IO;
                }
                let mut cfg: Box<sys::esp_tls_cfg_t> = Box::new(unsafe { zeroed() });
                cfg.non_block = true;
                cfg.timeout_ms = 10000;
                cfg.crt_bundle_attach = Some(sys::esp_crt_bundle_attach);
                cfg.skip_common_name = false;
                cfg.tls_version = sys::esp_tls_proto_ver_t_ESP_TLS_VER_TLS_1_3;
                self.tls[slot] = Some(TlsSlot { tls, cfg, host: host.to_vec(), port, connected: false });
            }
            Some(t) if t.port != port || t.host != host => return ARG,
            Some(_) => {}
        }
        let t = self.tls[slot].as_mut().unwrap();
        if t.connected {
            return 0;
        }
        let rc = unsafe {
            sys::esp_tls_conn_new_async(t.host.as_ptr().cast(), t.host.len() as c_int, c_int::from(t.port), &*t.cfg, t.tls)
        };
        match rc {
            1 => {
                t.connected = true;
                0
            }
            rc if rc < 0 => {
                self.tls[slot] = None;
                IO
            }
            _ => AGAIN,
        }
    }

    fn tls_io(&mut self, op: u16, slot: usize, data: &[u8], reply: &mut [u8]) -> (i32, usize) {
        let t = self.tls[slot].as_ref().unwrap();
        if !t.connected {
            return (AGAIN, 0);
        }
        let n = match op {
            op::SEND => unsafe { sys::esp_tls_conn_write(t.tls, data[2..].as_ptr().cast(), data.len() - 2) },
            op::RECV => {
                if data.len() < 4 {
                    return (ARG, 0);
                }
                let wanted = usize::from(get16(&data[2..]));
                if wanted > PAYLOAD {
                    return (ARG, 0);
                }
                unsafe { sys::esp_tls_conn_read(t.tls, reply.as_mut_ptr().cast(), wanted) }
            }
            _ => return (ARG, 0),
        };
        if os::tls_wants(n) {
            return (AGAIN, 0);
        }
        if n < 0 {
            self.tls[slot] = None;
            return (IO, 0);
        }
        (n as i32, if op == op::RECV { n as usize } else { 0 })
    }

    fn socket_io(&mut self, op: u16, slot: usize, data: &[u8], reply: &mut [u8]) -> (i32, usize) {
        let fd = self.sockets[slot];
        match op {
            op::SOCK_CONNECT | op::BIND => {
                let Some(address) = endpoint(data, 2) else { return (ARG, 0) };
                let rc = unsafe {
                    if op == op::BIND {
                        sys::lwip_bind(fd, os::sockaddr_ptr(&address), os::SOCKADDR_LEN)
                    } else {
                        sys::lwip_connect(fd, os::sockaddr_ptr(&address), os::SOCKADDR_LEN)
                    }
                };
                if rc == 0 || (op == op::SOCK_CONNECT && os::errno() as u32 == sys::EISCONN) {
                    (0, 0)
                } else {
                    (errno_status(), 0)
                }
            }
            op::LISTEN => {
                if data.len() < 4 {
                    return (ARG, 0);
                }
                let ok = unsafe { sys::lwip_listen(fd, c_int::from(get16(&data[2..]))) } == 0;
                (if ok { 0 } else { IO }, 0)
            }
            op::ACCEPT => {
                let Some((client, peer)) = os::accept(fd) else { return (errno_status(), 0) };
                let Some(client_slot) = self.free_slot() else {
                    os::close(client);
                    return (IO, 0);
                };
                os::set_nonblocking(client, true);
                self.sockets[client_slot] = client;
                let (address, port) = os::peer(&peer);
                put32(reply, address);
                put16(&mut reply[4..], port);
                (client_slot as i32, 8)
            }
            op::SEND | op::SENDTO => {
                let n = if op == op::SENDTO {
                    let Some(peer) = endpoint(data, 2) else { return (ARG, 0) };
                    let payload = &data[10..];
                    unsafe {
                        sys::lwip_sendto(
                            fd,
                            payload.as_ptr().cast(),
                            payload.len(),
                            0,
                            os::sockaddr_ptr(&peer),
                            os::SOCKADDR_LEN,
                        )
                    }
                } else {
                    let payload = &data[2..];
                    unsafe { sys::lwip_send(fd, payload.as_ptr().cast(), payload.len(), 0) }
                };
                (if n < 0 { errno_status() } else { n as i32 }, 0)
            }
            op::RECV | op::RECVFROM => {
                if data.len() < 4 {
                    return (ARG, 0);
                }
                let wanted = usize::from(get16(&data[2..]));
                if wanted > RECV_MAX {
                    return (ARG, 0);
                }
                if op == op::RECVFROM {
                    let mut peer: sys::sockaddr_in = unsafe { zeroed() };
                    let mut len = os::SOCKADDR_LEN;
                    let buf = &mut reply[8..8 + wanted];
                    let n = unsafe {
                        sys::lwip_recvfrom(fd, buf.as_mut_ptr().cast(), wanted, 0, os::sockaddr_mut(&mut peer), &mut len)
                    };
                    if n < 0 {
                        return (errno_status(), 0);
                    }
                    let (address, port) = os::peer(&peer);
                    put32(reply, address);
                    put16(&mut reply[4..], port);
                    (n as i32, n as usize + 8)
                } else {
                    let n = unsafe { sys::lwip_recv(fd, reply.as_mut_ptr().cast(), wanted, 0) };
                    if n < 0 {
                        (errno_status(), 0)
                    } else {
                        (n as i32, n as usize)
                    }
                }
            }
            _ => (ARG, 0),
        }
    }

    /// Runs one request.  The status, and how much of `reply` it filled.
    pub fn dispatch(&mut self, op: u16, data: &[u8], reply: &mut [u8; PAYLOAD]) -> (i32, usize) {
        let only = |status: i32| (status, 0);
        match op {
            op::WIFI_ON => return only(if self.wifi_start() { 0 } else { IO }),
            op::WIFI_OFF => return only(self.wifi_off()),
            op::TERM => return term::handle(data, reply),
            op::WEB => return web::handle(data, reply),
            op::SYSLOG => return only(self.syslog(data)),
            op::CREDENTIALS => return only(self.set_credentials(data)),
            op::CONNECT => return only(self.connect()),
            op::DISCONNECT => return only(esp_ok(unsafe { sys::esp_wifi_disconnect() })),
            op::STATUS => return (0, self.status(reply)),
            op::SCAN_START => return only(self.scan_start()),
            op::SCAN_NEXT => return self.scan_next(reply),
            op::PING_START => return only(ping::start(data)),
            op::PING_RESULT => return ping::result(reply),
            op::RESOLVE => return dns::handle(data, reply),
            op::SOCKET => return only(self.socket(data)),
            _ => {}
        }

        if data.len() < 2 {
            return only(ARG);
        }
        let slot = usize::from(get16(data));
        if slot >= MAX_SOCKETS || (self.sockets[slot] < 0 && self.tls[slot].is_none()) {
            return only(ARG);
        }
        match op {
            op::CLOSE => {
                self.close_slot(slot);
                only(0)
            }
            op::TLS_CONNECT => only(self.tls_connect(slot, data)),
            _ if self.tls[slot].is_some() => self.tls_io(op, slot, data, reply),
            _ => self.socket_io(op, slot, data, reply),
        }
    }
}
