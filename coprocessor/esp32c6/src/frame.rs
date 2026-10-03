//! The 512-byte SPI frame: magic `ESP1`, version, opcode, sequence, payload
//! length, signed status and CRC-32, all little-endian, then the payload.

use crate::{get16, get32, put16, put32};

pub const SIZE: usize = 512;
pub const HEADER: usize = 20;
pub const PAYLOAD: usize = SIZE - HEADER;
pub const MAGIC: u32 = 0x3150_5345;
pub const VERSION: u16 = 1;
/// The opcode of an unsolicited event delivered by a FETCH.
pub const EVENT: u16 = 0x8000;

pub type Frame = [u8; SIZE];

/// CRC-32 (IEEE) of the whole frame, with the CRC field read as zero.
pub fn crc32(frame: &Frame) -> u32 {
    let mut crc = 0xffff_ffffu32;
    for (i, &byte) in frame.iter().enumerate() {
        crc ^= if (16..20).contains(&i) { 0 } else { u32::from(byte) };
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & (crc & 1).wrapping_neg());
        }
    }
    !crc
}

pub struct Header {
    pub opcode: u16,
    pub sequence: u32,
    pub length: usize,
}

/// The header of a well-formed frame, or `None`.
pub fn decode(frame: &Frame) -> Option<Header> {
    let length = usize::from(get16(&frame[12..]));
    let ok = get32(frame) == MAGIC
        && get16(&frame[4..]) == VERSION
        && length <= PAYLOAD
        && get32(&frame[16..]) == crc32(frame);
    ok.then(|| Header {
        opcode: get16(&frame[6..]),
        sequence: get32(&frame[8..]),
        length,
    })
}

pub fn encode(frame: &mut Frame, opcode: u16, sequence: u32, status: i32, payload: &[u8]) {
    frame.fill(0);
    put32(frame, MAGIC);
    put16(&mut frame[4..], VERSION);
    put16(&mut frame[6..], opcode);
    put32(&mut frame[8..], sequence);
    put16(&mut frame[12..], payload.len() as u16);
    put16(&mut frame[14..], status as i16 as u16);
    frame[HEADER..HEADER + payload.len()].copy_from_slice(payload);
    let crc = crc32(frame);
    put32(&mut frame[16..], crc);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let mut frame = [0u8; SIZE];
        encode(&mut frame, 17, 0x1234_5678, -8, b"hello");
        let header = decode(&frame).unwrap();
        assert_eq!((header.opcode, header.sequence, header.length), (17, 0x1234_5678, 5));
        assert_eq!(&frame[HEADER..HEADER + 5], b"hello");
        assert_eq!(get16(&frame[14..]) as i16, -8);
        frame[100] ^= 1;
        assert!(decode(&frame).is_none());
    }

    #[test]
    fn matches_binascii_crc() {
        // Plain CRC-32 over the frame with bytes 16..20 zeroed; the values
        // are Python's binascii.crc32 of the same bytes.
        assert_eq!(crc32(&[0u8; SIZE]), 0xb2aa_7578);
        let mut frame = [0u8; SIZE];
        encode(&mut frame, 17, 0x1234_5678, -8, b"hello");
        assert_eq!(get32(&frame[16..]), 0x257d_bb4c);
        frame[16..20].copy_from_slice(&[0xaa; 4]);
        assert_eq!(crc32(&frame), 0x257d_bb4c);
    }

    #[test]
    fn rejects_long_payload_field() {
        let mut frame = [0u8; SIZE];
        encode(&mut frame, 1, 1, 0, &[]);
        put16(&mut frame[12..], (PAYLOAD + 1) as u16);
        let crc = crc32(&frame);
        put32(&mut frame[16..], crc);
        assert!(decode(&frame).is_none());
    }
}
