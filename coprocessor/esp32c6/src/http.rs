//! The bounded HTTP(S) job behind the shell's `curl`.  OP_HTTP_START runs
//! one request on a task of its own; the response's status, headers and
//! up to 192 KiB of body (gzip expanded with the ROM inflater) then stay
//! here for OP_HTTP_INFO and OP_HTTP_READ.

use core::ffi::{c_char, c_int, c_void, CStr};
use std::ffi::CString;
use std::sync::Mutex;

use esp_idf_svc::sys;
use freya_c6::status::{AGAIN, ARG, IO};
use freya_c6::{fields, get16, get32, gzip, put16, put32};

use crate::os::{self, lock};

const HEADERS_MAX: usize = 400;
const BODY_MAX: usize = 192 * 1024;
const INFO_LEN: usize = 8 + HEADERS_MAX;

const ACCEPT_GZIP: u8 = 1;
const INSECURE: u8 = 2;

extern "C" {
    /// miniz's inflater in the ESP32-C6 ROM.
    fn tinfl_decompress_mem_to_mem(
        out: *mut c_void,
        out_len: usize,
        src: *const c_void,
        src_len: usize,
        flags: c_int,
    ) -> usize;
}
const TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: c_int = 4;

struct State {
    active: bool,
    finished: bool,
    error: i32,
    status: u16,
    headers: Vec<u8>,
    body: Vec<u8>,
    pos: usize,
}

const IDLE: State =
    State { active: false, finished: false, error: 0, status: 0, headers: Vec::new(), body: Vec::new(), pos: 0 };

static STATE: Mutex<State> = Mutex::new(IDLE);

/// What the event handler gathers while the request runs.
#[derive(Default)]
struct Response {
    error: i32,
    gzip: bool,
    headers: Vec<u8>,
    body: Vec<u8>,
}

impl Response {
    fn header(&mut self, key: &[u8], value: &[u8]) {
        if key.eq_ignore_ascii_case(b"Content-Encoding") && value.eq_ignore_ascii_case(b"gzip") {
            self.gzip = true;
        }
        let room = HEADERS_MAX.saturating_sub(self.headers.len() + 1);
        let line = [key, b": ", value, b"\r\n"].concat();
        self.headers.extend_from_slice(&line[..line.len().min(room)]);
    }

    fn data(&mut self, data: &[u8]) -> sys::esp_err_t {
        let needed = self.body.len() + data.len();
        if needed > BODY_MAX {
            self.error = IO;
            return sys::ESP_FAIL;
        }
        if needed > self.body.capacity() {
            let mut capacity = self.body.capacity().max(2048) * 2;
            while capacity < needed {
                capacity *= 2;
            }
            let capacity = capacity.min(BODY_MAX);
            if self.body.try_reserve_exact(capacity - self.body.len()).is_err() {
                self.error = IO;
                return sys::ESP_ERR_NO_MEM;
            }
        }
        self.body.extend_from_slice(data);
        sys::ESP_OK
    }

    /// Replaces a gzip body with what it expands to.
    fn gunzip(&mut self) -> Result<(), ()> {
        let offset = gzip::deflate_offset(&self.body).ok_or(())?;
        let len = self.body.len();
        let out_len = get32(&self.body[len - 4..]) as usize;
        if out_len > BODY_MAX || out_len + len > BODY_MAX + 65536 {
            return Err(());
        }
        let mut out: Vec<u8> = Vec::new();
        out.try_reserve_exact(out_len.max(1)).map_err(|_| ())?;
        let src = &self.body[offset..len - 8];
        let made = unsafe {
            tinfl_decompress_mem_to_mem(
                out.as_mut_ptr().cast(),
                out_len,
                src.as_ptr().cast(),
                src.len(),
                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF,
            )
        };
        if made != out_len {
            return Err(());
        }
        unsafe { out.set_len(made) };
        self.body = out;
        Ok(())
    }
}

unsafe extern "C" fn on_event(event: *mut sys::esp_http_client_event_t) -> sys::esp_err_t {
    let event = &*event;
    let response = &mut *event.user_data.cast::<Response>();
    match event.event_id {
        sys::esp_http_client_event_id_t_HTTP_EVENT_ON_HEADER => {
            let key = CStr::from_ptr(event.header_key).to_bytes();
            let value = CStr::from_ptr(event.header_value).to_bytes();
            response.header(key, value);
            sys::ESP_OK
        }
        sys::esp_http_client_event_id_t_HTTP_EVENT_ON_DATA if event.data_len > 0 => {
            response.data(core::slice::from_raw_parts(event.data.cast::<u8>(), event.data_len as usize))
        }
        _ => sys::ESP_OK,
    }
}

fn opt(s: &Option<CString>) -> *const c_char {
    s.as_ref().map_or(core::ptr::null(), |s| s.as_ptr())
}

/// Runs the request in `job`: [flags, url, user agent, user:password,
/// form data], the strings NUL-terminated and empty when unused.
fn perform(job: &[u8], response: &mut Response) -> Result<u16, i32> {
    let flags = job[0];
    let [url, agent, basic, data] = fields::<4>(&job[1..]).ok_or(ARG)?;
    let (user, password) = if basic.is_empty() {
        (None, None)
    } else {
        let colon = basic.iter().position(|&c| c == b':').ok_or(ARG)?;
        (Some(&basic[..colon]), Some(&basic[colon + 1..]))
    };
    let c = |s: &[u8]| CString::new(s).map_err(|_| ARG);
    let url = c(url)?;
    let agent = c(if agent.is_empty() { b"Freya-curl/1.0" } else { agent })?;
    let user = user.map(c).transpose()?;
    let password = password.map(c).transpose()?;
    let insecure = flags & INSECURE != 0;

    let config = sys::esp_http_client_config_t {
        url: url.as_ptr(),
        username: opt(&user),
        password: opt(&password),
        auth_type: if user.is_some() {
            sys::esp_http_client_auth_type_t_HTTP_AUTH_TYPE_BASIC
        } else {
            sys::esp_http_client_auth_type_t_HTTP_AUTH_TYPE_NONE
        },
        user_agent: agent.as_ptr(),
        method: if data.is_empty() {
            sys::esp_http_client_method_t_HTTP_METHOD_GET
        } else {
            sys::esp_http_client_method_t_HTTP_METHOD_POST
        },
        timeout_ms: 20000,
        event_handler: Some(on_event),
        user_data: (response as *mut Response).cast(),
        skip_cert_common_name_check: insecure,
        crt_bundle_attach: if insecure { None } else { Some(sys::esp_crt_bundle_attach) },
        tls_version: sys::esp_http_client_proto_ver_t_ESP_HTTP_CLIENT_TLS_VER_TLS_1_3,
        ..Default::default()
    };
    let client = unsafe { sys::esp_http_client_init(&config) };
    if client.is_null() {
        return Err(IO);
    }
    unsafe {
        if flags & ACCEPT_GZIP != 0 {
            sys::esp_http_client_set_header(client, c"Accept-Encoding".as_ptr(), c"gzip".as_ptr());
        }
        if !data.is_empty() {
            sys::esp_http_client_set_header(
                client,
                c"Content-Type".as_ptr(),
                c"application/x-www-form-urlencoded".as_ptr(),
            );
            sys::esp_http_client_set_post_field(client, data.as_ptr().cast(), data.len() as c_int);
        }
    }
    let err = unsafe { sys::esp_http_client_perform(client) };
    let status = unsafe { sys::esp_http_client_get_status_code(client) } as u16;
    unsafe { sys::esp_http_client_cleanup(client) };
    if err != sys::ESP_OK || response.error != 0 {
        return Err(if response.error != 0 { response.error } else { IO });
    }
    if flags & ACCEPT_GZIP != 0 && response.gzip {
        response.gunzip().map_err(|_| IO)?;
    }
    Ok(status)
}

fn worker(job: Vec<u8>) {
    let mut response = Response::default();
    let result = perform(&job, &mut response);
    let mut state = lock(&STATE);
    match result {
        Ok(status) => state.status = status,
        Err(e) => state.error = e,
    }
    state.headers = response.headers;
    state.body = response.body;
    state.finished = true;
}

pub fn start(data: &[u8]) -> i32 {
    if data.len() < 5 {
        return ARG;
    }
    let mut state = lock(&STATE);
    if state.active && !state.finished {
        return AGAIN;
    }
    *state = State { active: true, ..IDLE };
    let job = data.to_vec();
    if os::spawn(c"freya_http", 10240, 5, move || worker(job)).is_err() {
        state.active = false;
        return IO;
    }
    0
}

/// [body length (4), status (2), header length (2), headers], always 408
/// bytes.
pub fn info(reply: &mut [u8]) -> (i32, usize) {
    let state = lock(&STATE);
    if !state.active {
        return (ARG, 0);
    }
    if !state.finished {
        return (AGAIN, 0);
    }
    if state.error != 0 {
        return (state.error, 0);
    }
    reply[..INFO_LEN].fill(0);
    put32(reply, state.body.len() as u32);
    put16(&mut reply[4..], state.status);
    put16(&mut reply[6..], state.headers.len() as u16);
    reply[8..8 + state.headers.len()].copy_from_slice(&state.headers);
    (0, INFO_LEN)
}

/// [wanted (2)]; the reply is the next body bytes.
pub fn read(data: &[u8], reply: &mut [u8]) -> (i32, usize) {
    let mut state = lock(&STATE);
    if !state.active || !state.finished || data.len() != 2 {
        return (ARG, 0);
    }
    if state.error != 0 {
        return (state.error, 0);
    }
    let wanted = usize::from(get16(data));
    if wanted == 0 || wanted > reply.len() {
        return (ARG, 0);
    }
    let n = wanted.min(state.body.len() - state.pos);
    reply[..n].copy_from_slice(&state.body[state.pos..state.pos + n]);
    state.pos += n;
    (n as i32, n)
}

pub fn close() -> i32 {
    let mut state = lock(&STATE);
    if !state.active {
        return 0;
    }
    if !state.finished {
        return AGAIN;
    }
    *state = IDLE;
    0
}
