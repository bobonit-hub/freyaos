//! The gzip member header (RFC 1952), so the deflate data after it can go to
//! the ROM inflater.

/// Where the deflate data of a single-member gzip body starts.  The body
/// must also have room for the 8-byte trailer after it.
pub fn deflate_offset(data: &[u8]) -> Option<usize> {
    if data.len() < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8 {
        return None;
    }
    let flags = data[3];
    let mut pos = 10;
    if flags & 4 != 0 {
        let extra = usize::from(*data.get(pos)?) | usize::from(*data.get(pos + 1)?) << 8;
        pos += 2 + extra;
    }
    for flag in [8, 16] {
        if flags & flag != 0 {
            pos += data.get(pos..)?.iter().position(|&b| b == 0)? + 1;
        }
    }
    if flags & 2 != 0 {
        pos += 2;
    }
    (pos + 8 <= data.len()).then_some(pos)
}

#[cfg(test)]
mod tests {
    use super::deflate_offset;

    #[test]
    fn plain_header() {
        let mut body = [0u8; 30];
        body[..4].copy_from_slice(&[0x1f, 0x8b, 8, 0]);
        assert_eq!(deflate_offset(&body), Some(10));
        body[2] = 9;
        assert_eq!(deflate_offset(&body), None);
    }

    #[test]
    fn optional_fields() {
        // FEXTRA (2 bytes), FNAME "a", FCOMMENT "", FHCRC.
        let mut body = alloc::vec![0x1f, 0x8b, 8, 4 | 8 | 16 | 2, 0, 0, 0, 0, 0, 3];
        body.extend_from_slice(&[2, 0, 0xee, 0xee, b'a', 0, 0, 0xcc, 0xcc]);
        let start = body.len();
        body.extend_from_slice(&[0; 8]);
        assert_eq!(deflate_offset(&body), Some(start));
        body.pop();
        assert_eq!(deflate_offset(&body), None);
    }

    #[test]
    fn unterminated_name() {
        let mut body = alloc::vec![0x1f, 0x8b, 8, 8, 0, 0, 0, 0, 0, 3];
        body.extend_from_slice(&[b'x'; 20]);
        assert_eq!(deflate_offset(&body), None);
    }
}
