//! The HTTPS console's request parsing: the request line, the path and
//! query that may reach the STM32 file service, and the headers a POST
//! needs.

use alloc::vec::Vec;

pub const PATH_MAX: usize = 96;
pub const QUERY_MAX: usize = 31;
pub const BODY_MAX: u32 = 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Method {
    Get = 1,
    Head = 2,
    Post = 3,
}

impl Method {
    pub fn name(self) -> &'static str {
        match self {
            Method::Get => "GET",
            Method::Head => "HEAD",
            Method::Post => "POST",
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct Request {
    pub method: Method,
    pub path: Vec<u8>,
    pub query: Vec<u8>,
    /// The Content-Length of a POST, zero otherwise.
    pub body_len: u32,
    pub expect_continue: bool,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Error {
    /// 400
    Bad,
    /// 405
    Method,
    /// 411: a POST without Content-Length, or chunked.
    LengthRequired,
    /// 413
    TooLarge,
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn hex(c: u8) -> Option<u8> {
    (c as char).to_digit(16).map(|d| d as u8)
}

fn unreserved(c: u8) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'-' | b'~')
}

/// Percent-decodes a path.  A decoded slash, backslash or NUL would hide a
/// segment boundary, so it is refused, as is a raw backslash.
pub fn decode_path(s: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(s.len());
    let mut i = 0;
    while i < s.len() {
        match s[i] {
            b'%' => {
                let hi = hex(*s.get(i + 1)?)?;
                let lo = hex(*s.get(i + 2)?)?;
                let c = hi << 4 | lo;
                if matches!(c, 0 | b'/' | b'\\') {
                    return None;
                }
                out.push(c);
                i += 3;
            }
            b'\\' => return None,
            c => {
                out.push(c);
                i += 1;
            }
        }
    }
    Some(out)
}

pub fn query_ok(s: &[u8]) -> bool {
    s.iter().all(|&c| unreserved(c) || matches!(c, b'=' | b'&' | b'%' | b'+'))
}

/// An absolute path of unreserved characters with no `.` or `..` segment
/// and no empty segment except a trailing one.
pub fn path_ok(s: &[u8]) -> bool {
    let Some(rest) = s.strip_prefix(b"/") else { return false };
    let segments: Vec<&[u8]> = rest.split(|&c| c == b'/').collect();
    let last = segments.len() - 1;
    segments.iter().enumerate().all(|(i, seg)| {
        if seg.is_empty() {
            return i == last;
        }
        *seg != b"." && *seg != b".." && seg.iter().all(|&c| unreserved(c))
    })
}

/// The value of one header in `headers`, the text after the request line,
/// trimmed and cut to 31 bytes.
pub fn header_value<'a>(headers: &'a [u8], name: &str) -> Option<&'a [u8]> {
    for line in headers.split(|&c| c == b'\n') {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        if line.is_empty() {
            break;
        }
        let Some(colon) = line.iter().position(|&c| c == b':') else { continue };
        if !line[..colon].eq_ignore_ascii_case(name.as_bytes()) {
            continue;
        }
        let value = &line[colon + 1..];
        let start = value.iter().position(|&c| c != b' ' && c != b'\t').unwrap_or(value.len());
        let end = value.iter().rposition(|&c| c != b' ' && c != b'\t').map_or(start, |e| e + 1);
        let value = &value[start..end.max(start)];
        return Some(&value[..value.len().min(31)]);
    }
    None
}

/// Parses the request line and headers, `buf` ending with the blank line.
pub fn parse(buf: &[u8]) -> Result<Request, Error> {
    let line_end = find(buf, b"\r\n").ok_or(Error::Bad)?;
    let (line, headers) = (&buf[..line_end], &buf[line_end + 2..]);
    let method = if line.starts_with(b"GET ") {
        Method::Get
    } else if line.starts_with(b"HEAD ") {
        Method::Head
    } else if line.starts_with(b"POST ") {
        Method::Post
    } else {
        return Err(Error::Method);
    };
    let sp = line.iter().position(|&c| c == b' ').ok_or(Error::Bad)?;
    let ver = line.iter().rposition(|&c| c == b' ').ok_or(Error::Bad)?;
    if ver == sp || !line[ver + 1..].starts_with(b"HTTP/1.") {
        return Err(Error::Bad);
    }
    let target = &line[sp + 1..ver];
    if target.first() != Some(&b'/') {
        return Err(Error::Bad);
    }
    let (raw_path, query) = match target.iter().position(|&c| c == b'?') {
        Some(q) => (&target[..q], &target[q + 1..]),
        None => (target, &target[..0]),
    };
    if query.len() > QUERY_MAX || !query_ok(query) {
        return Err(Error::Bad);
    }
    let path = decode_path(raw_path).ok_or(Error::Bad)?;
    if path.len() > PATH_MAX || !path_ok(&path) {
        return Err(Error::Bad);
    }

    let mut body_len = 0;
    let mut expect_continue = false;
    if method == Method::Post {
        if header_value(headers, "Transfer-Encoding").is_some() {
            return Err(Error::LengthRequired);
        }
        let digits = header_value(headers, "Content-Length")
            .filter(|v| !v.is_empty())
            .ok_or(Error::LengthRequired)?;
        for &c in digits {
            if !c.is_ascii_digit() {
                return Err(Error::Bad);
            }
            if body_len > BODY_MAX {
                return Err(Error::TooLarge);
            }
            body_len = body_len * 10 + u32::from(c - b'0');
        }
        if body_len > BODY_MAX {
            return Err(Error::TooLarge);
        }
        expect_continue = header_value(headers, "Expect")
            .is_some_and(|v| v.eq_ignore_ascii_case(b"100-continue"));
    }
    Ok(Request { method, path, query: query.to_vec(), body_len, expect_continue })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn get_with_query() {
        let r = parse(b"GET /cgi/run.sh?a=1&b=%20 HTTP/1.1\r\nHost: x\r\n\r\n").unwrap();
        assert_eq!(r.method, Method::Get);
        assert_eq!(r.path, b"/cgi/run.sh");
        assert_eq!(r.query, b"a=1&b=%20");
        assert_eq!(r.body_len, 0);
    }

    #[test]
    fn decodes_and_checks_path() {
        assert_eq!(parse(b"HEAD /a%2Db/ HTTP/1.0\r\n\r\n").unwrap().path, b"/a-b/");
        for bad in [
            &b"GET /a%2fb HTTP/1.1\r\n\r\n"[..],
            b"GET /a%00 HTTP/1.1\r\n\r\n",
            b"GET /a\\b HTTP/1.1\r\n\r\n",
            b"GET /../etc HTTP/1.1\r\n\r\n",
            b"GET /a/./b HTTP/1.1\r\n\r\n",
            b"GET /a//b HTTP/1.1\r\n\r\n",
            b"GET /%2e%2e/x HTTP/1.1\r\n\r\n",
            b"GET /a%4 HTTP/1.1\r\n\r\n",
            b"GET a HTTP/1.1\r\n\r\n",
            b"GET / HTTP/2\r\n\r\n",
            b"GET /\r\n\r\n",
            b"GET /?<x> HTTP/1.1\r\n\r\n",
            b"GET /?0123456789012345678901234567890123 HTTP/1.1\r\n\r\n",
        ] {
            assert_eq!(parse(bad), Err(Error::Bad), "{}", core::str::from_utf8(bad).unwrap());
        }
        assert_eq!(parse(b"PUT / HTTP/1.1\r\n\r\n"), Err(Error::Method));
        assert!(path_ok(b"/"));
        assert!(path_ok(b"/index.html"));
        assert!(path_ok(b"/.well-known/x"));
    }

    #[test]
    fn post_headers() {
        let r = parse(
            b"POST /upload HTTP/1.1\r\ncontent-length:  1234 \r\nExpect: 100-Continue\r\n\r\n",
        )
        .unwrap();
        assert_eq!((r.body_len, r.expect_continue), (1234, true));
        assert_eq!(
            parse(b"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n"),
            Err(Error::LengthRequired)
        );
        assert_eq!(parse(b"POST / HTTP/1.1\r\n\r\n"), Err(Error::LengthRequired));
        assert_eq!(parse(b"POST / HTTP/1.1\r\nContent-Length:\r\n\r\n"), Err(Error::LengthRequired));
        assert_eq!(parse(b"POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n"), Err(Error::Bad));
        assert_eq!(
            parse(b"POST / HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n"),
            Err(Error::TooLarge)
        );
        assert_eq!(
            parse(b"POST / HTTP/1.1\r\nContent-Length: 1048576\r\n\r\n").unwrap().body_len,
            BODY_MAX
        );
    }

    #[test]
    fn header_lookup() {
        let h = b"Host: a\r\nX-Long: 0123456789012345678901234567890123456789\r\n\r\nBody: no\r\n";
        assert_eq!(header_value(h, "host"), Some(&b"a"[..]));
        assert_eq!(header_value(h, "x-long").unwrap().len(), 31);
        assert_eq!(header_value(h, "Body"), None);
        assert_eq!(header_value(b"Hostname: b\r\n\r\n", "Host"), None);
    }
}
