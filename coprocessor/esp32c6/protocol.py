import struct
import binascii

SIZE = 512
MAGIC = 0x31505345
VERSION = 1
HEADER = 20
PAYLOAD = SIZE - HEADER
FMT = "<IHHIHhI"


def crc32(frame):
    copy = bytearray(frame)
    copy[16:20] = b"\0\0\0\0"
    return binascii.crc32(copy) & 0xFFFFFFFF


def encode(opcode, sequence, status=0, payload=b""):
    if len(payload) > PAYLOAD:
        raise ValueError("payload")
    frame = bytearray(SIZE)
    struct.pack_into(FMT, frame, 0, MAGIC, VERSION, opcode, sequence,
                     len(payload), status, 0)
    frame[HEADER:HEADER + len(payload)] = payload
    struct.pack_into("<I", frame, 16, crc32(frame))
    return frame


def decode(frame):
    if len(frame) != SIZE:
        return None
    magic, version, opcode, sequence, length, status, crc = \
        struct.unpack_from(FMT, frame, 0)
    if magic != MAGIC or version != VERSION or length > PAYLOAD:
        return None
    if crc != crc32(frame):
        return None
    return opcode, sequence, status, bytes(frame[HEADER:HEADER + length])
