#!/usr/bin/env python3
"""Pack Freya images into one ST DfuSe file.

The ROM loader on the STM32F405 programs whatever this file names, and
nothing else.  The kernel stays at 0x08000000 and the extension stays at
the address the linker gave it, so the gap between them — the auto-start
slot and the program region — is not erased.

    python3 tools/dfu_image.py \
        --image 0x08000000:build/stm32f405/freya.bin \
        --image 0x08020000:build/stm32f405/freya-kext.bin \
        --out build/stm32f405/freya.dfu
"""
import argparse
import struct
import sys
import zlib

DFU_SUFFIX_LEN = 16
TARGET_PREFIX_LEN = 274
VID_ST = 0x0483
PID_DFU = 0xDF11


def die(msg):
    sys.exit(f"dfu: {msg}")


def parse_image(text):
    if ":" not in text:
        die(f"expected address:file, got {text}")
    addr_s, path = text.split(":", 1)
    try:
        addr = int(addr_s, 0)
    except ValueError:
        die(f"not an address: {addr_s}")
    if addr < 0 or addr > 0xFFFFFFFF:
        die(f"address out of range: {addr_s}")
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        die(str(e))
    if not data:
        die(f"{path} is empty")
    return addr, data


def build(images, vid, pid, device, alt, name):
    elements = b""
    for addr, data in images:
        elements += struct.pack("<II", addr, len(data)) + data

    name_b = name.encode("ascii", "strict")[:255]
    name_b = name_b + b"\x00" * (255 - len(name_b))
    target = (
        b"Target"
        + struct.pack("<BI", alt & 0xFF, 1)
        + name_b
        + struct.pack("<II", len(elements), len(images))
        + elements
    )
    if len(target) != TARGET_PREFIX_LEN + len(elements):
        die("target prefix is the wrong length")

    # Size field is the whole file, suffix included.  Filled in after the
    # suffix length is known; the CRC covers every byte except itself.
    body = b"DfuSe" + struct.pack("<BIB", 1, 0, 1) + target
    total = len(body) + DFU_SUFFIX_LEN
    body = b"DfuSe" + struct.pack("<BIB", 1, total, 1) + target

    # USB DFU suffix, CRC last.  dfu-suffix writes the same order.
    tail = struct.pack("<HHH", device, pid, vid) + struct.pack("<H3sB", 0x011A, b"UFD", 16)
    # dfu-suffix uses CRC-32 with the final inversion left off.
    crc = (zlib.crc32(body + tail) ^ 0xFFFFFFFF) & 0xFFFFFFFF
    return body + tail + struct.pack("<I", crc)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", action="append", required=True,
                   metavar="ADDR:FILE",
                   help="one image, repeated; ADDR is where it is flashed")
    p.add_argument("--vid", default=hex(VID_ST), help="USB vendor id (default ST)")
    p.add_argument("--pid", default=hex(PID_DFU), help="USB product id (default DFU)")
    p.add_argument("--device", default="0xFFFF",
                   help="bcdDevice, 0xFFFF matches any bootloader revision")
    p.add_argument("--alt", type=int, default=0,
                   help="DFU alternate setting (0 is internal flash)")
    p.add_argument("--name", default="Internal Flash",
                   help="target name stored in the file")
    p.add_argument("--out", required=True)
    args = p.parse_args()

    try:
        vid = int(args.vid, 0)
        pid = int(args.pid, 0)
        device = int(args.device, 0)
    except ValueError:
        die("vid, pid and device are integers")
    for n, label in ((vid, "vid"), (pid, "pid"), (device, "device")):
        if n < 0 or n > 0xFFFF:
            die(f"{label} does not fit in 16 bits")

    images = [parse_image(s) for s in args.image]
    images.sort(key=lambda item: item[0])
    for i in range(1, len(images)):
        prev, prev_data = images[i - 1]
        addr, _ = images[i]
        if addr < prev + len(prev_data):
            die(f"image at 0x{addr:08x} overlaps the one at 0x{prev:08x}")

    blob = build(images, vid, pid, device, args.alt, args.name)
    with open(args.out, "wb") as f:
        f.write(blob)
    print(f"dfu: {args.out} ({len(blob)} bytes, {len(images)} images)")


if __name__ == "__main__":
    main()
