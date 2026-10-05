#!/usr/bin/env python3
"""Write a DfuSe file through dfu-util one flash page at a time.

Artery's ROM loader on the AT32F403A stalls a DfuSe request now and then,
erase, set-address or download alike, and dfu-util gives up on the first
stall.  Each page here is its own dfu-util run, retried until it goes in;
the loader comes back from a stall when the device is opened again.

The page size and the flash size come from the loader's own memory map.
When the file covers the whole flash, the chip is mass-erased first and
only the pages holding something other than 0xFF are written; otherwise
every page the file names is erased and written, and the rest of the
flash is left alone.  The last page also asks the loader to leave DFU.

    python3 tools/dfu_flash.py --device 2e3c:df11 build/blackpill2/freya.dfu
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

PREFIX_LEN = 11
TARGET_PREFIX_LEN = 274


def die(msg):
    sys.exit(f"dfu_flash: {msg}")


def parse_dfuse(blob):
    """Elements of a DfuSe file's alternate 0, as (address, data)."""
    if blob[:5] != b"DfuSe":
        die("not a DfuSe file")
    ntargets = blob[10]
    off = PREFIX_LEN
    for _ in range(ntargets):
        if blob[off:off + 6] != b"Target":
            die("bad target prefix")
        alt = blob[off + 6]
        nelements = struct.unpack_from("<I", blob, off + 270)[0]
        off += TARGET_PREFIX_LEN
        elements = []
        for _ in range(nelements):
            addr, size = struct.unpack_from("<II", blob, off)
            elements.append((addr, blob[off + 8:off + 8 + size]))
            off += 8 + size
        if alt == 0:
            return elements
    die("no image for alternate setting 0")


def parse_layout(name):
    """'@Internal Flash  /0x08000000/ 512*2Kg' -> (base, page_size, pages)."""
    if "," in name:
        die(f"{name!r}: only a flash of equal pages is supported")
    m = re.search(r"/(0x[0-9A-Fa-f]+)/\s*(\d+)\*(\d+)([KM ]?)", name)
    if not m:
        die(f"cannot read the flash layout from {name!r}")
    scale = {"K": 1024, "M": 1024 * 1024}.get(m.group(4), 1)
    return int(m.group(1), 16), int(m.group(3)) * scale, int(m.group(2))


def plan(elements, base, page_size, pages):
    """Return (mass_erase, [(address, page bytes)]) for the elements."""
    end = base + page_size * pages
    image = {}
    for addr, data in elements:
        if addr < base or addr + len(data) > end:
            die(f"element 0x{addr:08x}+{len(data)} is outside flash "
                f"0x{base:08x}..0x{end:08x}")
        for i, b in enumerate(data):
            image[addr + i] = b
    first = {a - (a - base) % page_size for a in image}
    whole = len(first) == pages and len(image) == page_size * pages
    out = []
    for page in sorted(first):
        data = bytes(image.get(a, 0xFF) for a in range(page, page + page_size))
        # A partly named page is read back as 0xFF outside the file, so
        # its other bytes would be lost; dfu-util erases it all the same.
        if whole and data.count(0xFF) == page_size:
            continue
        out.append((page, data))
    return whole, out


def dfu_util(args, device):
    cmd = ["dfu-util", "-d", device, "-a", "0"] + args
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode, r.stdout


def present(device):
    _, out = dfu_util(["-l"], device)
    return "Found DFU" in out


def retry(what, args, device, tries, leaving=False):
    for n in range(1, tries + 1):
        rc, out = dfu_util(args, device)
        if rc == 0:
            return n
        # Leaving resets the chip, and dfu-util may report that as an error.
        if leaving and not present(device):
            return n
        time.sleep(0.3)
    last = out.strip().splitlines()[-1] if out.strip() else f"exit {rc}"
    die(f"{what}: failed {tries} times, last: {last}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device", required=True, help="vid:pid of the loader")
    p.add_argument("--tries", type=int, default=30, help="attempts per request")
    p.add_argument("file", help="DfuSe file, as tools/dfu_image.py writes")
    args = p.parse_args()

    with open(args.file, "rb") as f:
        elements = parse_dfuse(f.read())

    rc, out = dfu_util(["-l"], args.device)
    m = re.search(r'alt=0, name="([^"]*)"', out)
    if not m:
        die(f"no DFU loader {args.device} found (hold BOOT0, tap NRST)")
    base, page_size, pages = parse_layout(m.group(1))

    whole, todo = plan(elements, base, page_size, pages)
    if not todo:
        die("nothing to write")
    if whole:
        print(f"dfu_flash: mass erase, then {len(todo)} of {pages} pages")
        retry("mass erase", ["-s", f"0x{base:08x}:mass-erase:force"],
              args.device, args.tries)
    else:
        print(f"dfu_flash: {len(todo)} pages of {page_size} bytes")

    stalls = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "page.bin")
        for i, (addr, data) in enumerate(todo):
            with open(path, "wb") as f:
                f.write(data)
            last = i == len(todo) - 1
            opt = f"0x{addr:08x}:leave" if last else f"0x{addr:08x}"
            stalls += retry(f"page 0x{addr:08x}", ["-s", opt, "-D", path],
                            args.device, args.tries, leaving=last) - 1
            print(f"\rdfu_flash: {i + 1}/{len(todo)} pages, {stalls} retried",
                  end="", flush=True)
    print("\ndfu_flash: done, the chip runs the new image")


if __name__ == "__main__":
    main()
