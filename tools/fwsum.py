#!/usr/bin/env python3
"""Firmware control sum for a Freya image.

The sum is the bytes of each span added into a 32-bit accumulator.  The
four bytes where the sum is stored are skipped, so the stored word is not
part of the sum.  The same rule is fw_sum_bytes() in src/cksum.c.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

SKIP_LEN = 4


def die(msg):
    sys.exit(f"fwsum: {msg}")


def parse_addr(text):
    try:
        return int(text, 0)
    except ValueError:
        die(f"not an address: {text}")


def sum_bytes(data, addr, skip_addr, skip_len):
    total = 0
    for i, b in enumerate(data):
        a = addr + i
        if skip_len and a >= skip_addr and (a - skip_addr) < skip_len:
            continue
        total = (total + b) & 0xFFFFFFFF
    return total


def load_spans(spans, skip_addr):
    total = 0
    for spec in spans:
        if ":" not in spec:
            die(f"span is ADDR:FILE, not {spec}")
        addr_s, path = spec.split(":", 1)
        addr = parse_addr(addr_s)
        with open(path, "rb") as f:
            data = f.read()
        total = (total + sum_bytes(data, addr, skip_addr, SKIP_LEN)) & 0xFFFFFFFF
    return total


def self_test():
    data = bytes([1, 2, 3, 4, 5])
    if sum_bytes(data, 0x100, 0, 0) != 15:
        die("self-test: plain sum")
    if sum_bytes(data, 0x100, 0x102, 2) != 8:
        die("self-test: skipped bytes")
    # A stored sum must not change the sum of the image that holds it.
    image = bytearray(b"\x11\x22\x33\x44" + b"\x00\x00\x00\x00" + b"\x55")
    store = 0x200 + 4
    got = sum_bytes(image, 0x200, store, SKIP_LEN)
    struct.pack_into("<I", image, 4, got)
    if sum_bytes(image, 0x200, store, SKIP_LEN) != got:
        die("self-test: storing the sum changed it")
    print("fwsum self-test ok")


def stamp_device(spans, store, page_base, page_size, st_opts):
    if not (page_base <= store and store + SKIP_LEN <= page_base + page_size):
        die(f"sum at {store:#x} is outside the page "
            f"{page_base:#x}+{page_size:#x}")
    total = load_spans(spans, store)
    fd, page = tempfile.mkstemp(prefix="fwsum-")
    os.close(fd)
    try:
        subprocess.check_call(["st-flash", *st_opts, "read", page,
                               hex(page_base), hex(page_size)])
        with open(page, "rb") as f:
            blob = bytearray(f.read())
        if len(blob) < page_size:
            blob.extend(b"\xFF" * (page_size - len(blob)))
        struct.pack_into("<I", blob, store - page_base, total)
        with open(page, "wb") as f:
            f.write(blob[:page_size])
        subprocess.check_call(["st-flash", *st_opts, "write", page, hex(page_base)])
    finally:
        os.unlink(page)
    print(f"firmware sum 0x{total:08x} at {store:#x}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--span", action="append", default=[],
                   help="ADDR:FILE, flash address of the first byte")
    p.add_argument("--store-addr", help="where the sum word is stored")
    p.add_argument("--patch", help="image to write the sum into")
    p.add_argument("--image-base", default="0x08000000",
                   help="flash address of byte 0 of --patch")
    p.add_argument("--page-base", help="erase page holding the sum, for --device")
    p.add_argument("--page-size", help="size of that page")
    p.add_argument("--device", action="store_true",
                   help="read-modify-write the sum into the attached board")
    p.add_argument("--st-opt", action="append", default=[],
                   help="extra argument for st-flash, before the subcommand")
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args()

    if args.self_test:
        self_test()
        return

    if not args.store_addr:
        die("--store-addr is required")
    store = parse_addr(args.store_addr)
    if args.device:
        if not args.page_base or not args.page_size:
            die("--device needs --page-base and --page-size")
        stamp_device(args.span, store, parse_addr(args.page_base),
                     parse_addr(args.page_size), args.st_opt)
        return

    total = load_spans(args.span, store)
    if args.patch:
        base = parse_addr(args.image_base)
        off = store - base
        with open(args.patch, "rb") as f:
            image = bytearray(f.read())
        if off < 0 or off + SKIP_LEN > len(image):
            die(f"{args.patch} does not cover the sum at {store:#x}")
        struct.pack_into("<I", image, off, total)
        with open(args.patch, "wb") as f:
            f.write(image)
        print(f"firmware sum 0x{total:08x} at {store:#x} in {args.patch}")
    else:
        print(f"0x{total:08x}")


if __name__ == "__main__":
    main()
