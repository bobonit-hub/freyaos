//! The parts of the coprocessor firmware that do not touch ESP-IDF: the SPI
//! frame format, the byte rings and the request parsing of the web server.
//! They build for the host as well, so `cargo test` runs them there (see
//! the README).

#![no_std]

extern crate alloc;
#[cfg(test)]
extern crate std;

pub mod frame;
pub mod request;
pub mod ring;

/// Status codes carried in a frame header.  They are Freya's `FREYA_ERR_*`.
pub mod status {
    pub const ARG: i32 = -3;
    pub const NACK: i32 = -5;
    pub const IO: i32 = -7;
    pub const AGAIN: i32 = -8;
}

pub fn get16(p: &[u8]) -> u16 {
    u16::from_le_bytes([p[0], p[1]])
}

pub fn get32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

pub fn put16(p: &mut [u8], value: u16) {
    p[..2].copy_from_slice(&value.to_le_bytes());
}

pub fn put32(p: &mut [u8], value: u32) {
    p[..4].copy_from_slice(&value.to_le_bytes());
}

/// The bytes before the first NUL, or all of them.
pub fn cstr(p: &[u8]) -> &[u8] {
    match p.iter().position(|&b| b == 0) {
        Some(n) => &p[..n],
        None => p,
    }
}

/// Splits `p` into `N` NUL-terminated fields.  `None` when a field has no
/// terminator inside `p`.
pub fn fields<const N: usize>(p: &[u8]) -> Option<[&[u8]; N]> {
    let mut out = [&p[..0]; N];
    let mut rest = p;
    for field in out.iter_mut() {
        let n = rest.iter().position(|&b| b == 0)?;
        *field = &rest[..n];
        rest = &rest[n + 1..];
    }
    Some(out)
}

/// The terminal password check.  It looks at every byte whatever the input,
/// so its time does not tell how much of a guess was right.
pub fn password_matches(expect: &[u8; 8], on: bool, got: Option<&[u8]>) -> bool {
    let Some(got) = got else { return false };
    let mut diff = (got.len() ^ expect.len()) as u8 | u8::from(got.len() > 255);
    for (i, e) in expect.iter().enumerate() {
        diff |= e ^ got.get(i).copied().unwrap_or(0);
    }
    on && diff == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splits_fields() {
        let [a, b, c] = fields::<3>(b"url\0\0x\0tail").unwrap();
        assert_eq!((a, b, c), (&b"url"[..], &b""[..], &b"x"[..]));
        assert!(fields::<2>(b"one\0two").is_none());
    }

    #[test]
    fn checks_password() {
        let pass = *b"12345678";
        assert!(password_matches(&pass, true, Some(b"12345678")));
        assert!(!password_matches(&pass, false, Some(b"12345678")));
        assert!(!password_matches(&pass, true, Some(b"1234567")));
        assert!(!password_matches(&pass, true, Some(b"123456789")));
        assert!(!password_matches(&pass, true, Some(b"12345679")));
        assert!(!password_matches(&pass, true, None));
    }
}
