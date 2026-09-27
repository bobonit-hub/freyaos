#!/usr/bin/env python3
"""Two copies of Freya system settings.

The layout matches include/freya_api.h.  Each copy is 64 bytes, starts
with the marker, and stores a checksum of every other byte.  The names
and offsets are fixed.
"""
import struct
import sys

MARKER = 0x54455346  # 'F','S','E','T'
AUTOSTART_MAGIC = 0x31415946
BLOCK = 64
COPIES = 2
AREA = 1024
OFF_AUTOSTART = 4
OFF_LOGLEVEL = 8
OFF_RAMDUMP = 12
OFF_CKSUM = 16  # firmware control sum
OFF_PASSWORD = 20
PASSWORD_LEN = 8
OFF_SUM = 28  # checksum of this copy

NAMES = {
    "autostart": (OFF_AUTOSTART, 4),
    "loglevel": (OFF_LOGLEVEL, 4),
    "ramdump": (OFF_RAMDUMP, 4),
    "cksum": (OFF_CKSUM, 4),
    "password": (OFF_PASSWORD, PASSWORD_LEN),
}


def checksum(block):
    total = 0
    for i, b in enumerate(block[:BLOCK]):
        if OFF_SUM <= i < OFF_SUM + 4:
            continue
        total = (total + b) & 0xFFFFFFFF
    return total


def is_blank(block):
    return bytes(block[:BLOCK]) == b"\xff" * BLOCK


def is_valid(block):
    if len(block) < BLOCK or is_blank(block):
        return False
    if struct.unpack_from("<I", block, 0)[0] != MARKER:
        return False
    return struct.unpack_from("<I", block, OFF_SUM)[0] == checksum(block)


def seal(block):
    struct.pack_into("<I", block, 0, MARKER)
    struct.pack_into("<I", block, OFF_SUM, checksum(block))


def stamp(area, fw_sum, autostart=None):
    """Write the firmware sum into both copies.

    autostart is True, False, or None.  None keeps the flag from a valid
    copy.  Returns 'ok', or 'fail' when both copies are corrupt.  On
    'fail' the area, including both checksum words, is unchanged.
    """
    if len(area) < BLOCK * COPIES:
        raise ValueError("settings area is shorter than two copies")
    c0 = bytearray(area[:BLOCK])
    c1 = bytearray(area[BLOCK:BLOCK * 2])
    k0 = "blank" if is_blank(c0) else ("valid" if is_valid(c0) else "bad")
    k1 = "blank" if is_blank(c1) else ("valid" if is_valid(c1) else "bad")
    if k0 == "bad" and k1 == "bad":
        return "fail"
    if k0 == "valid":
        base = bytearray(c0)
    elif k1 == "valid":
        base = bytearray(c1)
    else:
        base = bytearray(b"\xff" * BLOCK)
    if autostart is True:
        struct.pack_into("<I", base, OFF_AUTOSTART, AUTOSTART_MAGIC)
    elif autostart is False:
        struct.pack_into("<I", base, OFF_AUTOSTART, 0xFFFFFFFF)
    struct.pack_into("<I", base, OFF_CKSUM, fw_sum & 0xFFFFFFFF)
    seal(base)
    area[:BLOCK] = base
    area[BLOCK:BLOCK * 2] = base
    return "ok"


def fresh(fw_sum, autostart):
    """A blank area with the firmware sum and the auto-start flag set."""
    area = bytearray(b"\xff" * AREA)
    if stamp(area, fw_sum, bool(autostart)) != "ok":
        raise RuntimeError("a blank settings area was rejected")
    return area


def self_test():
    area = fresh(0xA1B2C3D4, True)
    if not is_valid(area[:BLOCK]) or area[:BLOCK] != area[BLOCK:BLOCK * 2]:
        sys.exit("settings: fresh copies do not match")
    if struct.unpack_from("<I", area, OFF_AUTOSTART)[0] != AUTOSTART_MAGIC:
        sys.exit("settings: auto-start flag was not stored")
    if struct.unpack_from("<I", area, OFF_CKSUM)[0] != 0xA1B2C3D4:
        sys.exit("settings: firmware sum was not stored")
    bad = bytearray(area)
    bad[0] ^= 0xFF
    bad[BLOCK] ^= 0xFF
    before = bytes(bad)
    if stamp(bad, 0x11111111, None) != "fail" or bytes(bad) != before:
        sys.exit("settings: both-corrupt stamp touched the area")
    one = bytearray(area)
    one[BLOCK] ^= 0xFF
    if stamp(one, 0xA1B2C3D4, None) != "ok":
        sys.exit("settings: a single bad copy was not repaired")
    if not is_valid(one[:BLOCK]) or one[:BLOCK] != one[BLOCK:BLOCK * 2]:
        sys.exit("settings: repair did not rewrite both copies")
    print("settings self-test ok")


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        self_test()
    else:
        sys.exit("settings: pass --self-test, or import this module")
