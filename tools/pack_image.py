#!/usr/bin/env python3
"""Pack a Freya kernel and one flash-resident program into a single image.

The program is a .xip.bin linked for the board's program flash region.  On
the Blue Pill that region starts at 0x0800A000, so the image written to the
module is:

    [kernel][0xFF up to the region][program][0xFF to the end of the region]

`make BOARD=bluepill flash PROGRAM=hello` runs this.  The addresses come
from the kernel ELF, which is where the linker script reserved the region.
"""
import argparse
import struct
import sys

FLASH_BASE_DEFAULT = 0x08000000
MAGIC = 0x41595246  # 'FRYA'
XIP = 0x1
HDR = 64  # freya_app_header_t, ABI 2


def die(msg):
    sys.exit(f"pack: {msg}")


def u32(blob, off):
    return struct.unpack_from("<I", blob, off)[0]


def parse_addr(text):
    try:
        return int(text, 0)
    except ValueError:
        die(f"not an address: {text}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kernel", required=True, help="freya.bin")
    p.add_argument("--app", required=True, help="program .xip.bin")
    p.add_argument("--load-addr", required=True, help="program region base, e.g. 0x0800A000")
    p.add_argument("--region-end", required=True, help="first address after the region")
    p.add_argument("--flash-base", default=hex(FLASH_BASE_DEFAULT),
                   help="start of internal flash (default 0x08000000)")
    p.add_argument("--out", required=True, help="combined image to write")
    args = p.parse_args()

    base = parse_addr(args.flash_base)
    load = parse_addr(args.load_addr)
    end = parse_addr(args.region_end)
    if not (base < load < end):
        die(f"region {load:#x}..{end:#x} is not inside flash at {base:#x}")

    offset = load - base
    region = end - load

    with open(args.kernel, "rb") as f:
        kernel = f.read()
    with open(args.app, "rb") as f:
        app = f.read()

    if len(kernel) > offset:
        die(f"kernel is {len(kernel)} bytes and the program region starts at "
            f"offset {offset:#x} ({offset} bytes)")
    if len(app) < HDR:
        die(f"{args.app}: shorter than a program header")

    magic, abi, linked, entry, image_size = struct.unpack_from("<5I", app, 0)
    flags = u32(app, 48)

    if magic != MAGIC:
        die(f"{args.app}: not a Freya program (magic {magic:#x})")
    if abi < 2 or not (flags & XIP):
        die(f"{args.app}: not a flash image - link it with app_flash.ld "
            f"(the .xip.bin, not the RAM .bin)")
    if linked != load:
        die(f"{args.app}: linked for {linked:#x}, region is {load:#x}")
    if image_size == 0 or image_size > len(app):
        die(f"{args.app}: image_size {image_size} does not match the file "
            f"({len(app)} bytes)")
    if image_size > region:
        die(f"{args.app}: {image_size} bytes does not fit the "
            f"{region // 1024} KiB program region")
    if not (load <= entry < load + image_size):
        die(f"{args.app}: entry {entry:#x} is outside the image")

    image = bytearray(b"\xFF" * (offset + region))
    image[:len(kernel)] = kernel
    image[offset:offset + image_size] = app[:image_size]

    with open(args.out, "wb") as f:
        f.write(image)

    print(f"packed {args.app} at {load:#010x}: {image_size} bytes, "
          f"image {len(image)} bytes -> {args.out}")


if __name__ == "__main__":
    main()
