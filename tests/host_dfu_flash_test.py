#!/usr/bin/env python3
"""Host checks for tools/dfu_flash.py that do not need a board."""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


dfu_flash = load("dfu_flash")
dfu_image = load("dfu_image")

fails = 0


def check(cond, what):
    global fails
    if cond:
        print(f"  ok    {what}")
    else:
        print(f"  FAIL  {what}")
        fails += 1


print("dfu_flash")

check(dfu_flash.parse_layout("@Internal Flash  /0x08000000/ 512*2Kg") == (0x08000000, 2048, 512),
      "Artery memory map: 512 pages of 2 KiB")
try:
    dfu_flash.parse_layout("@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg")
    check(False, "an ST map of mixed sectors is refused")
except SystemExit:
    check(True, "an ST map of mixed sectors is refused")

elements = [(0x08000000, b"\x01" * 10), (0x08001000, b"\x02" * 4)]
blob = dfu_image.build(elements, 0x2E3C, 0xDF11, 0xFFFF, 0, "Internal Flash")
check(dfu_flash.parse_dfuse(blob) == elements, "reads back what dfu_image.py writes")

# A file that names part of the flash: every page it touches, blank or not.
whole, pages = dfu_flash.plan([(0x08000000, b"\x01" * 10), (0x08000400, b"\xFF" * 2048)],
                              0x08000000, 1024, 8)
check(not whole, "a partial image is not a mass erase")
check([a for a, _ in pages] == [0x08000000, 0x08000400, 0x08000800],
      "a partial image writes every page it touches")
check(pages[0][1] == b"\x01" * 10 + b"\xFF" * 1014, "a short page is padded with 0xFF")

# A file that covers the flash: mass erase, blank pages skipped.
image = b"\xFF" * 1024 + b"\x05" * 3 + b"\xFF" * (1024 * 7 - 3)
whole, pages = dfu_flash.plan([(0x08000000, image)], 0x08000000, 1024, 8)
check(whole, "a whole-flash image is a mass erase")
check([a for a, _ in pages] == [0x08000400], "blank pages are skipped after a mass erase")

try:
    dfu_flash.plan([(0x08002000, b"\x01")], 0x08000000, 1024, 8)
    check(False, "an element past the end of flash is refused")
except SystemExit:
    check(True, "an element past the end of flash is refused")

sys.exit(1 if fails else 0)
