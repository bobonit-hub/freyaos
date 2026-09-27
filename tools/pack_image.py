#!/usr/bin/env python3
"""Pack a Freya kernel and one flash-resident image into a single file.

The payload is either a program (.xip.bin linked for the board's program
flash region) or a shell script.  The image written to the module is:

    [kernel][0xFF up to the region][payload][0xFF to the end of the region]

A script is stored the way `install` writes one: an 8-byte header
('SCRT' and the text length), the text, and a trailing NUL.

`make flash PROGRAM=hello` and `make flash SCRIPT=boot.sh` run this.
The addresses come from the kernel ELF, which is where the linker script
reserved the region.  System settings live at the end of flash, outside
this image; the auto-start flag is written with the settings area.
"""
import argparse
import struct
import sys

FLASH_BASE_DEFAULT = 0x08000000
MAGIC = 0x41595246  # 'FRYA'
SCRIPT_MAGIC = 0x54524353  # 'S','C','R','T'
XIP = 0x1
HDR = 72  # freya_app_header_t, ABI 3
SCRIPT_HDR = 8  # freya_script_header_t


def die(msg):
    sys.exit(f"pack: {msg}")


def u32(blob, off):
    return struct.unpack_from("<I", blob, off)[0]


def parse_addr(text):
    try:
        return int(text, 0)
    except ValueError:
        die(f"not an address: {text}")


def script_text_ok(data):
    """Same rule as script_text_ok() in the shell: printable ASCII, tab, CR, LF."""
    for c in data:
        if c != 0x09 and c != 0x0A and c != 0x0D and (c < 0x20 or c > 0x7E):
            return False
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kernel", required=True, help="freya.bin")
    p.add_argument("--app", help="program .xip.bin")
    p.add_argument("--script", help="shell script stored in the program region")
    p.add_argument("--load-addr", required=True, help="program region base, e.g. 0x0800C080")
    p.add_argument("--region-end", required=True, help="first address after the region")
    p.add_argument("--flash-base", default=hex(FLASH_BASE_DEFAULT),
                   help="start of internal flash (default 0x08000000)")
    p.add_argument("--out", required=True, help="combined image to write")
    args = p.parse_args()

    if bool(args.app) == bool(args.script):
        die("pass exactly one of --app or --script")

    base = parse_addr(args.flash_base)
    load = parse_addr(args.load_addr)
    end = parse_addr(args.region_end)
    if not (base < load < end):
        die(f"region {load:#x}..{end:#x} is not inside flash at {base:#x}")

    offset = load - base
    region = end - load

    with open(args.kernel, "rb") as f:
        kernel = f.read()

    kernel_limit = offset
    if len(kernel) > kernel_limit:
        what = "auto-start slot" if slot is not None else "program region"
        die(f"kernel is {len(kernel)} bytes and the {what} starts at "
            f"offset {kernel_limit:#x} ({kernel_limit} bytes)")

    if args.script:
        with open(args.script, "rb") as f:
            text = f.read()
        if not text:
            die(f"{args.script}: empty file is not a shell script")
        if not script_text_ok(text):
            die(f"{args.script}: not a shell script "
                "(printable ASCII, tab, CR and LF only)")
        payload = struct.pack("<II", SCRIPT_MAGIC, len(text)) + text + b"\x00"
        label = args.script
        nbytes = len(text)
    else:
        with open(args.app, "rb") as f:
            app = f.read()
        if len(app) < HDR:
            die(f"{args.app}: shorter than a program header")
        magic, abi, linked, entry, image_size = struct.unpack_from("<5I", app, 0)
        flags = u32(app, 48)
        if magic != MAGIC:
            die(f"{args.app}: not a Freya program (magic {magic:#x})")
        if abi < 3 or not (flags & XIP):
            die(f"{args.app}: not a flash image - link it with app_flash.ld "
                f"(the .xip.bin, not the RAM .bin)")
        if linked != load:
            die(f"{args.app}: linked for {linked:#x}, region is {load:#x}")
        if image_size == 0 or image_size > len(app):
            die(f"{args.app}: image_size {image_size} does not match the file "
                f"({len(app)} bytes)")
        if not (load <= entry < load + image_size):
            die(f"{args.app}: entry {entry:#x} is outside the image")
        payload = app[:image_size]
        label = args.app
        nbytes = image_size

    if len(payload) > region:
        die(f"{label}: {len(payload)} bytes does not fit the "
            f"{region // 1024} KiB program region")

    image = bytearray(b"\xFF" * (offset + region))
    image[:len(kernel)] = kernel
    image[offset:offset + len(payload)] = payload

    with open(args.out, "wb") as f:
        f.write(image)

    extra = ""
    kind = "script" if args.script else "program"
    print(f"packed {kind} {label} at {load:#010x}: {nbytes} bytes, "
          f"image {len(image)} bytes{extra} -> {args.out}")


if __name__ == "__main__":
    main()
