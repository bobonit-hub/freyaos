#!/usr/bin/env python3
"""Pack a Freya kernel and one flash-resident program into a single image.

The program is a .xip.bin linked for the board's program flash region.  On
the Blue Pill that region starts at 0x08009C80 (immediately after the
128-byte auto-start slot), so the image written to the
module is:

    [kernel][0xFF up to the region][program][0xFF to the end of the region]

`make BOARD=bluepill flash PROGRAM=hello` runs this.  The addresses come
from the kernel ELF, which is where the linker script reserved the region.
The auto-start slot is left erased (flag off) unless `--autostart` is
passed; packing a program must not autorun on every reset unless asked.
"""
import argparse
import struct
import sys

FLASH_BASE_DEFAULT = 0x08000000
MAGIC = 0x41595246  # 'FRYA'
AUTOSTART_MAGIC = 0x31415946  # first word of the auto-start slot
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
    p.add_argument("--load-addr", required=True, help="program region base, e.g. 0x08009C80")
    p.add_argument("--region-end", required=True, help="first address after the region")
    p.add_argument("--flash-base", default=hex(FLASH_BASE_DEFAULT),
                   help="start of internal flash (default 0x08000000)")
    p.add_argument("--slot-addr",
                   help="auto-start slot base from the kernel ELF "
                        "(__autostart_start), e.g. 0x08009C00")
    p.add_argument("--slot-end",
                   help="first address after the auto-start slot "
                        "(__autostart_end)")
    p.add_argument("--autostart", action="store_true",
                   help="write the auto-start magic at the slot; default off")
    p.add_argument("--out", required=True, help="combined image to write")
    args = p.parse_args()

    base = parse_addr(args.flash_base)
    load = parse_addr(args.load_addr)
    end = parse_addr(args.region_end)
    if not (base < load < end):
        die(f"region {load:#x}..{end:#x} is not inside flash at {base:#x}")

    offset = load - base
    region = end - load
    slot = parse_addr(args.slot_addr) if args.slot_addr else None
    slot_end = parse_addr(args.slot_end) if args.slot_end else None

    if args.autostart and slot is None:
        die("--autostart needs --slot-addr from the kernel ELF")
    if slot is not None:
        if not (base <= slot < load):
            die(f"auto-start slot {slot:#x} is not between flash "
                f"{base:#x} and the program region {load:#x}")
        if slot_end is not None and not (slot < slot_end <= load):
            die(f"auto-start slot {slot:#x}..{slot_end:#x} is not "
                f"inside the gap before {load:#x}")
        if args.autostart:
            magic_end = slot + 4
            limit = slot_end if slot_end is not None else load
            if magic_end > limit:
                die(f"auto-start magic at {slot:#x} does not fit "
                    f"before {limit:#x}")

    with open(args.kernel, "rb") as f:
        kernel = f.read()
    with open(args.app, "rb") as f:
        app = f.read()

    kernel_limit = (slot - base) if slot is not None else offset
    if len(kernel) > kernel_limit:
        what = "auto-start slot" if slot is not None else "program region"
        die(f"kernel is {len(kernel)} bytes and the {what} starts at "
            f"offset {kernel_limit:#x} ({kernel_limit} bytes)")
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
    if args.autostart:
        struct.pack_into("<I", image, slot - base, AUTOSTART_MAGIC)

    with open(args.out, "wb") as f:
        f.write(image)

    extra = ", autostart on" if args.autostart else ""
    print(f"packed {args.app} at {load:#010x}: {image_size} bytes, "
          f"image {len(image)} bytes{extra} -> {args.out}")


if __name__ == "__main__":
    main()
