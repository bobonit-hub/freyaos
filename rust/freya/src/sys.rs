//! Raw bindings to `include/freya_api.h`.
//!
//! Every name here is the C name, so the header's comments describe these
//! too.  The build script compiles the header with arm-none-eabi-gcc and
//! checks each field offset, each struct size and each constant below
//! against what C sees, so the two cannot drift apart: a field appended to
//! the C table and not here, or a constant changed on one side, fails the
//! build.  Board-dependent constants (the load address, the flash region)
//! are left out; `apps/common/app_start.c` writes the header that needs
//! them.

#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};

pub const FREYA_APP_MAGIC: u32 = 0x4159_5246;
pub const FREYA_ABI_VERSION: u32 = 3;
pub const FREYA_ABI_MIN_VERSION: u32 = 1;
pub const FREYA_APP_F_XIP: u32 = 0x0000_0001;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_app_header_t {
    pub magic: u32,
    pub abi_version: u32,
    pub load_addr: u32,
    pub entry: u32,
    pub image_size: u32,
    pub bss_start: u32,
    pub bss_end: u32,
    pub stack_need: u32,
    pub name: [c_char; 16],
    pub flags: u32,
    pub data_src: u32,
    pub data_start: u32,
    pub data_end: u32,
    pub reloc_offset: u32,
    pub reloc_count: u32,
}

/* How a run ended. */
pub const FREYA_STOP_NONE: c_int = 0;
pub const FREYA_STOP_EXIT: c_int = 1;
pub const FREYA_STOP_CTRLC: c_int = 2;
pub const FREYA_STOP_HARDFAULT: c_int = 3;
pub const FREYA_STOP_MEMFAULT: c_int = 4;
pub const FREYA_STOP_BUSFAULT: c_int = 5;
pub const FREYA_STOP_USAGEFAULT: c_int = 6;

/* Exit status. */
pub const FREYA_EXIT_OK: c_int = 0;
pub const FREYA_EXIT_FAIL: c_int = 1;
pub const FREYA_EXIT_USAGE: c_int = 2;
pub const FREYA_EXIT_NOEXEC: c_int = 126;
pub const FREYA_EXIT_NOTFOUND: c_int = 127;
pub const FREYA_EXIT_KILLED: c_int = 128;
pub const FREYA_EXIT_STOPPED: c_int = FREYA_EXIT_KILLED + FREYA_STOP_CTRLC;
pub const FREYA_EXIT_MAX: c_int = 255;

/// `freya_exit_status()`: the status a run reports.
pub const fn freya_exit_status(reason: c_int, code: c_int) -> c_int {
    if reason <= FREYA_STOP_EXIT || reason > FREYA_STOP_USAGEFAULT {
        code & FREYA_EXIT_MAX
    } else {
        FREYA_EXIT_KILLED + reason
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_exit_t {
    pub status: i32,
    pub reason: i32,
    pub run_ms: u32,
    pub name: [c_char; 20],
}

/* open() flags */
pub const FREYA_O_RDONLY: c_int = 0x01;
pub const FREYA_O_WRONLY: c_int = 0x02;
pub const FREYA_O_RDWR: c_int = 0x03;
pub const FREYA_O_CREATE: c_int = 0x04;
pub const FREYA_O_TRUNC: c_int = 0x08;
pub const FREYA_O_APPEND: c_int = 0x10;

/* seek() whence */
pub const FREYA_SEEK_SET: c_int = 0;
pub const FREYA_SEEK_CUR: c_int = 1;
pub const FREYA_SEEK_END: c_int = 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_stat_t {
    pub name: [c_char; 64],
    pub size: u32,
    pub is_dir: u8,
    pub pad: [u8; 3],
}

/* log() levels */
pub const FREYA_LOG_OFF: c_int = 0;
pub const FREYA_LOG_ERROR: c_int = 1;
pub const FREYA_LOG_WARN: c_int = 2;
pub const FREYA_LOG_INFO: c_int = 3;
pub const FREYA_LOG_DEBUG: c_int = 4;
pub const FREYA_LOG_MAX_SIZE: u32 = 1024 * 1024;

/* Pins: FREYA_PIN(port, n). */
pub const fn freya_pin(port: c_int, n: c_int) -> c_int {
    ((port & 0x0F) << 4) | (n & 0x0F)
}
pub const fn freya_pin_port(pin: c_int) -> c_int {
    (pin >> 4) & 0x0F
}
pub const fn freya_pin_num(pin: c_int) -> c_int {
    pin & 0x0F
}

pub const FREYA_PIN_IN: c_int = 0;
pub const FREYA_PIN_IN_PULLUP: c_int = 1;
pub const FREYA_PIN_IN_PULLDOWN: c_int = 2;
pub const FREYA_PIN_OUT: c_int = 3;
pub const FREYA_PIN_OUT_OD: c_int = 4;
pub const FREYA_PIN_ANALOG: c_int = 5;

pub const FREYA_EDGE_RISING: c_int = 1;
pub const FREYA_EDGE_FALLING: c_int = 2;
pub const FREYA_EDGE_BOTH: c_int = FREYA_EDGE_RISING | FREYA_EDGE_FALLING;
pub const FREYA_EDGE_DEBOUNCE: c_int = 4;
pub const FREYA_DEBOUNCE_MS: c_int = 20;

pub const FREYA_TIMER_ONESHOT: c_int = 0x01;
pub const FREYA_TIMER_MIN_US: u32 = 10;
pub const FREYA_TIMER_MAX_US: u32 = 40_000_000;

pub const FREYA_PWM_FULL: u32 = 10_000;
pub const FREYA_PWM_MIN_HZ: u32 = 1;
pub const FREYA_PWM_MAX_HZ: u32 = 1_000_000;

pub const FREYA_I2C_MIN_HZ: u32 = 10_000;
pub const FREYA_I2C_MAX_HZ: u32 = 400_000;
pub const FREYA_I2C_MAX_LEN: c_int = 255;

pub const FREYA_W1_BUSES: c_int = 4;
pub const FREYA_W1_ROM_LEN: c_int = 8;
pub const FREYA_W1_MAX_LEN: c_int = 64;

pub const FREYA_SPI_MODE0: c_int = 0;
pub const FREYA_SPI_MODE1: c_int = 1;
pub const FREYA_SPI_MODE2: c_int = 2;
pub const FREYA_SPI_MODE3: c_int = 3;
pub const FREYA_SPI_MIN_HZ: u32 = 187_500;
pub const FREYA_SPI_MAX_HZ: u32 = 24_000_000;
pub const FREYA_SPI_MAX_LEN: c_int = 4096;

pub const FREYA_ADC_MAX: c_int = 4095;
pub const FREYA_ADC_TEMP: c_int = 0x100;
pub const FREYA_ADC_VREF: c_int = 0x101;

pub const FREYA_CRYPT_ROUNDS: c_int = 32;
pub const FREYA_CRYPT_KEY_LEN: c_int = 16;
pub const FREYA_CRYPT_NONCE_LEN: c_int = 8;
pub const FREYA_CRYPT_BLOCK: c_int = 8;
pub const FREYA_CRYPT_MAX_LEN: c_int = 4096;

pub const FREYA_COMPRESS_WINDOW_BITS: c_int = 8;
pub const FREYA_COMPRESS_LOOKAHEAD_BITS: c_int = 4;
/// `FREYA_COMPRESS_BOUND(n)`: the most compress() can write for n bytes.
pub const fn freya_compress_bound(n: usize) -> usize {
    n + (n + 7) / 8
}

pub const FREYA_AEAD_KEY_LEN: c_int = 16;
pub const FREYA_AEAD_NONCE_LEN: c_int = 16;
pub const FREYA_AEAD_TAG_LEN: c_int = 16;
pub const FREYA_AEAD_MAX_LEN: c_int = 4096;

/* What the pin, timer, bus and coprocessor calls return. */
pub const FREYA_ERR_PIN: c_int = -1;
pub const FREYA_ERR_BUSY: c_int = -2;
pub const FREYA_ERR_ARG: c_int = -3;
pub const FREYA_ERR_HANDLER: c_int = -4;
pub const FREYA_ERR_NACK: c_int = -5;
pub const FREYA_ERR_TIMEOUT: c_int = -6;
pub const FREYA_ERR_IO: c_int = -7;
pub const FREYA_ERR_AGAIN: c_int = -8;
pub const FREYA_ERR_UNSUPPORTED: c_int = -9;

/* Network. */
pub const FREYA_NET_SOCKETS: c_int = 4;
pub const FREYA_NET_PAYLOAD_MAX: c_int = 480;
pub const FREYA_WEB_GET: c_int = 1;
pub const FREYA_WEB_HEAD: c_int = 2;
pub const FREYA_WEB_POST: c_int = 3;
pub const FREYA_WEB_PATH: usize = 96;
pub const FREYA_WEB_QUERY: usize = 31;
pub const FREYA_WEB_TYPE: c_int = 40;
pub const FREYA_WEB_CHUNK: c_int = 400;
pub const FREYA_WEB_READ_MAX: c_int = 480;
pub const FREYA_WEB_BODY_MAX: u32 = 1024 * 1024;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_web_req_t {
    pub method: i32,
    pub path: [c_char; FREYA_WEB_PATH + 1],
    pub query: [c_char; FREYA_WEB_QUERY + 1],
}

pub const FREYA_WIFI_SSID_MAX: usize = 32;
pub const FREYA_WIFI_PASS_MAX: usize = 63;
pub const FREYA_AF_INET: c_int = 2;
pub const FREYA_SOCK_STREAM: c_int = 1;
pub const FREYA_SOCK_DGRAM: c_int = 2;
pub const FREYA_IPPROTO_TCP: c_int = 6;
pub const FREYA_IPPROTO_UDP: c_int = 17;

pub const FREYA_WIFI_OFF: c_int = 0;
pub const FREYA_WIFI_IDLE: c_int = 1;
pub const FREYA_WIFI_CONNECTING: c_int = 2;
pub const FREYA_WIFI_CONNECTED: c_int = 3;
pub const FREYA_WIFI_ERROR: c_int = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_net_addr_t {
    pub addr: u32,
    pub port: u16,
    pub reserved: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_wifi_status_t {
    pub state: i32,
    pub rssi: i32,
    pub ip: u32,
    pub gateway: u32,
    pub netmask: u32,
    pub ssid: [c_char; FREYA_WIFI_SSID_MAX + 1],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_wifi_scan_t {
    pub rssi: i32,
    pub channel: u8,
    pub auth: u8,
    pub reserved: [u8; 2],
    pub ssid: [c_char; FREYA_WIFI_SSID_MAX + 1],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_ping_result_t {
    pub addr: u32,
    pub elapsed_ms: u32,
    pub replies: u32,
    pub lost: u32,
}

pub type freya_irq_fn = Option<unsafe extern "C" fn(source: c_int, arg: *mut c_void)>;

/* Threads. */
pub const FREYA_THREAD_NAME_MAX: c_int = 16;
pub const FREYA_THREAD_STACK: u32 = 1024;
pub const FREYA_PRIO_MIN: c_int = 0;
pub const FREYA_PRIO_MAX: c_int = 7;
pub const FREYA_PRIO_NORMAL: c_int = 1;

pub type freya_thread_fn = Option<unsafe extern "C" fn(arg: *mut c_void)>;

pub const FREYA_PWR_SD: c_int = 1;

/* The 32-bit PDP-11. */
pub const FREYA_VM_NREGS: usize = 8;
pub const FREYA_VM_SP: c_int = 6;
pub const FREYA_VM_PC: c_int = 7;
pub const FREYA_VM_C: u32 = 0x1;
pub const FREYA_VM_V: u32 = 0x2;
pub const FREYA_VM_Z: u32 = 0x4;
pub const FREYA_VM_N: u32 = 0x8;
pub const FREYA_VM_HALT: c_int = 1;
pub const FREYA_VM_TRAP: c_int = 2;
pub const FREYA_VM_FAULT: c_int = 3;
pub const FREYA_VM_ILLEGAL: c_int = 4;
pub const FREYA_VM_LIMIT: c_int = 5;
pub const FREYA_VM_MAX_STEPS: u32 = 1_000_000;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct freya_vm_t {
    pub r: [u32; FREYA_VM_NREGS],
    pub psw: u32,
}

pub const FREYA_RTC_MIN_YEAR: c_int = 1980;
pub const FREYA_RTC_MAX_YEAR: c_int = 2199;

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct freya_rtc_t {
    pub year: u16,
    pub mon: u8,
    pub day: u8,
    pub hour: u8,
    pub min: u8,
    pub sec: u8,
}

/// The service table.  Fields are only ever appended; a field past `size`
/// is not there on the running kernel, which `api_has!` checks.
#[repr(C)]
pub struct freya_api_t {
    pub size: u32,
    pub version: u32,

    /* console */
    pub putc: unsafe extern "C" fn(c: c_char),
    pub puts: unsafe extern "C" fn(s: *const c_char),
    pub printf: unsafe extern "C" fn(fmt: *const c_char, ...) -> c_int,
    pub getc: unsafe extern "C" fn() -> c_int,
    pub getc_timeout: unsafe extern "C" fn(ms: u32) -> c_int,
    pub kbhit: unsafe extern "C" fn() -> c_int,

    /* memory */
    pub malloc: unsafe extern "C" fn(size: u32) -> *mut c_void,
    pub free: unsafe extern "C" fn(p: *mut c_void),

    /* time */
    pub ticks_ms: unsafe extern "C" fn() -> u32,
    pub delay_ms: unsafe extern "C" fn(ms: u32),

    /* flow control */
    pub should_stop: unsafe extern "C" fn() -> c_int,
    pub yield_: unsafe extern "C" fn(),
    pub exit: unsafe extern "C" fn(code: c_int) -> !,

    /* filesystem */
    pub open: unsafe extern "C" fn(path: *const c_char, flags: c_int) -> c_int,
    pub close: unsafe extern "C" fn(fd: c_int) -> c_int,
    pub read: unsafe extern "C" fn(fd: c_int, buf: *mut c_void, len: c_int) -> c_int,
    pub write: unsafe extern "C" fn(fd: c_int, buf: *const c_void, len: c_int) -> c_int,
    pub seek: unsafe extern "C" fn(fd: c_int, off: i32, whence: c_int) -> c_int,
    pub tell: unsafe extern "C" fn(fd: c_int) -> i32,
    pub fsize: unsafe extern "C" fn(fd: c_int) -> i32,
    pub unlink: unsafe extern "C" fn(path: *const c_char) -> c_int,
    pub mkdir: unsafe extern "C" fn(path: *const c_char) -> c_int,
    pub opendir: unsafe extern "C" fn(path: *const c_char) -> c_int,
    pub readdir: unsafe extern "C" fn(dd: c_int, st: *mut freya_stat_t) -> c_int,
    pub closedir: unsafe extern "C" fn(dd: c_int) -> c_int,

    /* raw board access */
    pub led: unsafe extern "C" fn(on: c_int),
    pub cpu_hz: unsafe extern "C" fn() -> u32,

    pub rename: unsafe extern "C" fn(old_path: *const c_char, new_path: *const c_char) -> c_int,

    pub log: unsafe extern "C" fn(level: c_int, fmt: *const c_char, ...),
    pub get_log_level: unsafe extern "C" fn() -> c_int,
    pub set_log_level: unsafe extern "C" fn(level: c_int) -> c_int,

    pub last_exit: unsafe extern "C" fn(st: *mut freya_exit_t) -> c_int,
    pub exit_reason_str: unsafe extern "C" fn(reason: c_int) -> *const c_char,

    pub pin_mode: unsafe extern "C" fn(pin: c_int, mode: c_int) -> c_int,
    pub pin_read: unsafe extern "C" fn(pin: c_int) -> c_int,
    pub pin_write: unsafe extern "C" fn(pin: c_int, value: c_int) -> c_int,
    pub pin_toggle: unsafe extern "C" fn(pin: c_int) -> c_int,

    pub pin_irq_attach:
        unsafe extern "C" fn(pin: c_int, edge: c_int, f: freya_irq_fn, arg: *mut c_void) -> c_int,
    pub pin_irq_detach: unsafe extern "C" fn(pin: c_int) -> c_int,
    pub pin_irq_count: unsafe extern "C" fn(pin: c_int) -> u32,

    pub timer_open:
        unsafe extern "C" fn(period_us: u32, flags: c_int, f: freya_irq_fn, arg: *mut c_void) -> c_int,
    pub timer_close: unsafe extern "C" fn(timer: c_int) -> c_int,
    pub timer_start: unsafe extern "C" fn(timer: c_int) -> c_int,
    pub timer_stop: unsafe extern "C" fn(timer: c_int) -> c_int,
    pub timer_period: unsafe extern "C" fn(timer: c_int, period_us: u32) -> c_int,
    pub timer_count: unsafe extern "C" fn(timer: c_int) -> u32,

    pub irq_count: unsafe extern "C" fn() -> u32,
    pub irq_wait: unsafe extern "C" fn(ms: u32) -> c_int,

    pub pwm_open: unsafe extern "C" fn(pin: c_int, freq_hz: u32, duty: u32) -> c_int,
    pub pwm_close: unsafe extern "C" fn(pwm: c_int) -> c_int,
    pub pwm_duty: unsafe extern "C" fn(pwm: c_int, duty: u32) -> c_int,
    pub pwm_pulse_us: unsafe extern "C" fn(pwm: c_int, us: u32) -> c_int,
    pub pwm_freq: unsafe extern "C" fn(pwm: c_int, freq_hz: u32) -> c_int,

    pub i2c_open: unsafe extern "C" fn(bus: c_int, hz: u32) -> c_int,
    pub i2c_close: unsafe extern "C" fn(bus: c_int) -> c_int,
    pub i2c_write: unsafe extern "C" fn(bus: c_int, addr: c_int, buf: *const c_void, len: c_int) -> c_int,
    pub i2c_read: unsafe extern "C" fn(bus: c_int, addr: c_int, buf: *mut c_void, len: c_int) -> c_int,
    pub i2c_transfer: unsafe extern "C" fn(
        bus: c_int,
        addr: c_int,
        tx: *const c_void,
        txlen: c_int,
        rx: *mut c_void,
        rxlen: c_int,
    ) -> c_int,

    pub w1_open: unsafe extern "C" fn(pin: c_int) -> c_int,
    pub w1_close: unsafe extern "C" fn(pin: c_int) -> c_int,
    pub w1_reset: unsafe extern "C" fn(pin: c_int) -> c_int,
    pub w1_write: unsafe extern "C" fn(pin: c_int, buf: *const c_void, len: c_int) -> c_int,
    pub w1_read: unsafe extern "C" fn(pin: c_int, buf: *mut c_void, len: c_int) -> c_int,
    pub w1_search: unsafe extern "C" fn(pin: c_int, rom: *mut c_void) -> c_int,
    pub w1_pullup: unsafe extern "C" fn(pin: c_int, on: c_int) -> c_int,
    pub w1_crc: unsafe extern "C" fn(buf: *const c_void, len: c_int) -> c_int,

    pub thread_create: unsafe extern "C" fn(
        name: *const c_char,
        priority: c_int,
        f: freya_thread_fn,
        arg: *mut c_void,
    ) -> c_int,
    pub thread_exit: unsafe extern "C" fn() -> !,
    pub thread_yield: unsafe extern "C" fn(),
    pub thread_sleep: unsafe extern "C" fn(ms: u32) -> c_int,
    pub thread_self: unsafe extern "C" fn() -> c_int,

    pub spi_open: unsafe extern "C" fn(bus: c_int, hz: u32, mode: c_int) -> c_int,
    pub spi_close: unsafe extern "C" fn(bus: c_int) -> c_int,
    pub spi_transfer: unsafe extern "C" fn(bus: c_int, tx: *const c_void, rx: *mut c_void, len: c_int) -> c_int,
    pub spi_write: unsafe extern "C" fn(bus: c_int, buf: *const c_void, len: c_int) -> c_int,
    pub spi_read: unsafe extern "C" fn(bus: c_int, buf: *mut c_void, len: c_int) -> c_int,

    pub crypt: unsafe extern "C" fn(
        key: *const c_void,
        nonce: *const c_void,
        off: u32,
        input: *const c_void,
        out: *mut c_void,
        len: c_int,
    ) -> c_int,

    pub console_raw: unsafe extern "C" fn(on: c_int) -> c_int,

    pub power: unsafe extern "C" fn(domain: c_int, on: c_int) -> c_int,

    pub adc_read: unsafe extern "C" fn(source: c_int) -> c_int,

    pub vm_reset: unsafe extern "C" fn(vm: *mut freya_vm_t) -> c_int,
    pub vm_step: unsafe extern "C" fn(vm: *mut freya_vm_t, mem: *mut c_void, size: u32) -> c_int,
    pub vm_run: unsafe extern "C" fn(
        vm: *mut freya_vm_t,
        mem: *mut c_void,
        size: u32,
        steps: u32,
        ran: *mut u32,
    ) -> c_int,

    pub wifi_on: unsafe extern "C" fn() -> c_int,
    pub wifi_off: unsafe extern "C" fn() -> c_int,
    pub wifi_credentials: unsafe extern "C" fn(ssid: *const c_char, password: *const c_char) -> c_int,
    pub wifi_connect: unsafe extern "C" fn() -> c_int,
    pub wifi_disconnect: unsafe extern "C" fn() -> c_int,
    pub wifi_status: unsafe extern "C" fn(status: *mut freya_wifi_status_t) -> c_int,
    pub wifi_scan_start: unsafe extern "C" fn() -> c_int,
    pub wifi_scan_next: unsafe extern "C" fn(entry: *mut freya_wifi_scan_t) -> c_int,
    pub ping_start: unsafe extern "C" fn(host: *const c_char, timeout_ms: u32) -> c_int,
    pub ping_result: unsafe extern "C" fn(result: *mut freya_ping_result_t) -> c_int,
    pub net_socket: unsafe extern "C" fn(domain: c_int, ty: c_int, protocol: c_int) -> c_int,
    pub net_close: unsafe extern "C" fn(socket: c_int) -> c_int,
    pub net_connect: unsafe extern "C" fn(socket: c_int, addr: *const freya_net_addr_t) -> c_int,
    pub net_bind: unsafe extern "C" fn(socket: c_int, addr: *const freya_net_addr_t) -> c_int,
    pub net_listen: unsafe extern "C" fn(socket: c_int, backlog: c_int) -> c_int,
    pub net_accept: unsafe extern "C" fn(socket: c_int, peer: *mut freya_net_addr_t) -> c_int,
    pub net_send: unsafe extern "C" fn(socket: c_int, buf: *const c_void, len: c_int) -> c_int,
    pub net_recv: unsafe extern "C" fn(socket: c_int, buf: *mut c_void, len: c_int) -> c_int,
    pub net_sendto: unsafe extern "C" fn(
        socket: c_int,
        buf: *const c_void,
        len: c_int,
        to: *const freya_net_addr_t,
    ) -> c_int,
    pub net_recvfrom: unsafe extern "C" fn(
        socket: c_int,
        buf: *mut c_void,
        len: c_int,
        from: *mut freya_net_addr_t,
    ) -> c_int,
    pub net_poll: unsafe extern "C" fn(timeout_ms: u32) -> c_int,

    pub net_tls_connect: unsafe extern "C" fn(socket: c_int, hostname: *const c_char, port: u16) -> c_int,

    pub web_take: unsafe extern "C" fn(req: *mut freya_web_req_t) -> c_int,
    pub web_begin: unsafe extern "C" fn(status: c_int, ty: *const c_char, length: u32) -> c_int,
    pub web_body: unsafe extern "C" fn(data: *const c_void, len: c_int) -> c_int,
    pub web_end: unsafe extern "C" fn() -> c_int,

    pub shell_source_capture: unsafe extern "C" fn(
        path: *const c_char,
        method: *const c_char,
        query: *const c_char,
        buf: *mut c_char,
        cap: c_int,
        out_len: *mut c_int,
    ) -> c_int,

    pub settings_block: unsafe extern "C" fn(buf: *mut c_void, len: c_int) -> c_int,
    pub settings_area_size: unsafe extern "C" fn() -> u32,

    pub web_read: unsafe extern "C" fn(data: *mut c_void, max: c_int, left: *mut u32) -> c_int,

    pub compress: unsafe extern "C" fn(input: *const c_void, in_len: c_int, out: *mut c_void, out_cap: c_int) -> c_int,
    pub decompress:
        unsafe extern "C" fn(input: *const c_void, in_len: c_int, out: *mut c_void, out_cap: c_int) -> c_int,

    pub aead_encrypt: unsafe extern "C" fn(
        key: *const c_void,
        nonce: *const c_void,
        ad: *const c_void,
        ad_len: c_int,
        input: *const c_void,
        in_len: c_int,
        out: *mut c_void,
        out_cap: c_int,
    ) -> c_int,
    pub aead_decrypt: unsafe extern "C" fn(
        key: *const c_void,
        nonce: *const c_void,
        ad: *const c_void,
        ad_len: c_int,
        input: *const c_void,
        in_len: c_int,
        out: *mut c_void,
        out_cap: c_int,
    ) -> c_int,

    pub rtc_get: unsafe extern "C" fn(t: *mut freya_rtc_t) -> c_int,
    pub rtc_set: unsafe extern "C" fn(t: *const freya_rtc_t) -> c_int,
}

/// `FREYA_API_HAS(api, field)`: whether the running kernel's table reaches
/// past `field`.  `api` is a `&freya_api_t`.
#[macro_export]
macro_rules! api_has {
    ($api:expr, $field:ident) => {{
        let api: &$crate::sys::freya_api_t = $api;
        let end = ::core::mem::offset_of!($crate::sys::freya_api_t, $field)
            + ::core::mem::size_of::<usize>();
        api.size as usize >= end
    }};
}

/* The filesystem calls return these, from src/fat.h.  They are not part
 * of freya_api.h, but they are what a program is handed back. */
pub const FAT_ERR_IO: c_int = -1;
pub const FAT_ERR_NOFS: c_int = -2;
pub const FAT_ERR_NOENT: c_int = -3;
pub const FAT_ERR_EXIST: c_int = -4;
pub const FAT_ERR_NOSPC: c_int = -5;
pub const FAT_ERR_INVAL: c_int = -6;
pub const FAT_ERR_NOTDIR: c_int = -7;
pub const FAT_ERR_ISDIR: c_int = -8;
pub const FAT_ERR_NOTEMPTY: c_int = -9;
pub const FAT_ERR_NOFILE: c_int = -10;
pub const FAT_ERR_RDONLY: c_int = -11;
pub const FAT_MAX_PATH: usize = 128;
