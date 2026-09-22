#!/usr/bin/env python3
"""Create a relocatable installed-program image from an ARM ELF file."""

import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

PT_LOAD = 1
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 2
R_ARM_ABS32 = 2

HDR_MAGIC = 0
HDR_ABI = 4
HDR_LOAD_ADDR = 8
HDR_IMAGE_SIZE = 16
HDR_RELOC_OFFSET = 64
HDR_RELOC_COUNT = 68
HEADER_SIZE = 72
FREYA_APP_MAGIC = 0x41595246
FREYA_ABI_VERSION = 3


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def elf_relocation_offsets(elf_data, image, image_base):
    """Return image offsets of ABS32 words whose values point into the image."""
    ident = elf_data[:16]
    if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 1:
        raise ValueError("expected a 32-bit little-endian ELF file")

    eh = struct.unpack_from("<HHIIIIIHHHHHH", elf_data, 16)
    phoff, shoff = eh[4], eh[5]
    phentsize, phnum = eh[8], eh[9]
    shentsize, shnum = eh[10], eh[11]

    programs = []
    for i in range(phnum):
        p = struct.unpack_from("<IIIIIIII", elf_data, phoff + i * phentsize)
        if p[0] == PT_LOAD:
            programs.append({
                "offset": p[1], "paddr": p[3], "filesz": p[4]
            })

    sections = []
    for i in range(shnum):
        s = struct.unpack_from("<IIIIIIIIII", elf_data, shoff + i * shentsize)
        sections.append({
            "type": s[1], "flags": s[2], "addr": s[3], "offset": s[4],
            "size": s[5], "info": s[7], "entsize": s[9],
        })

    def section_lma(section):
        for p in programs:
            start = section["offset"]
            end = start + section["size"]
            if start >= p["offset"] and end <= p["offset"] + p["filesz"]:
                return p["paddr"] + start - p["offset"]
        raise ValueError("allocated section is not in a load segment")

    result = set()
    image_end = image_base + len(image)
    for relsec in sections:
        if relsec["type"] not in (SHT_REL, SHT_RELA):
            continue
        target = sections[relsec["info"]]
        if not (target["flags"] & SHF_ALLOC) or target["type"] == SHT_NOBITS:
            continue

        entry_size = relsec["entsize"] or (12 if relsec["type"] == SHT_RELA else 8)
        target_lma = section_lma(target)
        for pos in range(relsec["offset"],
                         relsec["offset"] + relsec["size"], entry_size):
            r_offset, r_info = struct.unpack_from("<II", elf_data, pos)
            if (r_info & 0xFF) != R_ARM_ABS32:
                continue

            # In an executable ELF r_offset is normally a VMA.  Accept a
            # section-relative value too, which makes this work with either
            # GNU ld representation.
            if target["addr"] <= r_offset < target["addr"] + target["size"]:
                within = r_offset - target["addr"]
            else:
                within = r_offset
            if within + 4 > target["size"]:
                raise ValueError("relocation lies outside its target section")

            image_offset = target_lma + within - image_base
            if image_offset < 0 or image_offset + 4 > len(image):
                continue
            value = u32(image, image_offset)
            if image_base <= (value & ~1) <= image_end:
                result.add(image_offset)

    # load_addr is a constant in the C initializer, so the linker has no
    # relocation record for it.  It describes the image and moves with it.
    result.add(HDR_LOAD_ADDR)
    return sorted(result)


def build_image(elf_path, output_path, objcopy):
    elf_data = Path(elf_path).read_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        raw_path = Path(tmp) / "image.bin"
        subprocess.run([objcopy, "-O", "binary", elf_path, raw_path],
                       check=True)
        image = bytearray(raw_path.read_bytes())

    if len(image) < HEADER_SIZE:
        raise ValueError("program image is smaller than its header")
    if u32(image, HDR_MAGIC) != FREYA_APP_MAGIC:
        raise ValueError("program image has no Freya header")
    if u32(image, HDR_ABI) != FREYA_ABI_VERSION:
        raise ValueError("program image does not use the current ABI")
    if u32(image, HDR_IMAGE_SIZE) != len(image):
        raise ValueError("ELF image size and header image_size disagree")

    base = u32(image, HDR_LOAD_ADDR)
    offsets = elf_relocation_offsets(elf_data, image, base)
    reloc_offset = (len(image) + 3) & ~3
    image.extend(b"\0" * (reloc_offset - len(image)))
    image.extend(struct.pack("<%dI" % len(offsets), *offsets))

    struct.pack_into("<I", image, HDR_IMAGE_SIZE, len(image))
    struct.pack_into("<I", image, HDR_RELOC_OFFSET, reloc_offset)
    struct.pack_into("<I", image, HDR_RELOC_COUNT, len(offsets))
    Path(output_path).write_bytes(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("elf")
    parser.add_argument("output")
    args = parser.parse_args()
    try:
        build_image(args.elf, args.output, args.objcopy)
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
