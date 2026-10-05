#!/usr/bin/env python3
"""Host checks for tools/dfu_image.py that do not need a board."""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("dfu_image", os.path.join(ROOT, "tools", "dfu_image.py"))
dfu_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dfu_image)

fails = 0


def check(cond, what):
    global fails
    if cond:
        print(f"  ok    {what}")
    else:
        print(f"  FAIL  {what}")
        fails += 1


def dies(images):
    try:
        dfu_image.merge(images)
    except SystemExit:
        return True
    return False


print("dfu_image")

# Apart, the images stay separate elements, sorted.
out = dfu_image.merge([(0x08010000, b"\x02" * 4), (0x08000000, b"\x01" * 4)])
check(out == [(0x08000000, b"\x01" * 4), (0x08010000, b"\x02" * 4)],
      "separate images stay separate, in address order")

# A packed image passes over the settings as 0xFF; the settings land there.
packed = b"\x01" * 16 + b"\xFF" * 16 + b"\x03" * 16
out = dfu_image.merge([(0x08000000, packed), (0x08000010, b"\x02" * 8)])
check(out == [(0x08000000, b"\x01" * 16 + b"\x02" * 8 + b"\xFF" * 8 + b"\x03" * 16)],
      "an image over 0xFF filler is merged into one element")

# Overlap that would replace real bytes is refused.
check(dies([(0x08000000, b"\x01" * 16), (0x08000008, b"\x02" * 4)]),
      "an image over programmed bytes is refused")

# A later image may run past the end of the one it starts inside.
out = dfu_image.merge([(0x08000000, b"\x01" * 4 + b"\xFF" * 4), (0x08000004, b"\x02" * 8)])
check(out == [(0x08000000, b"\x01" * 4 + b"\x02" * 8)],
      "an image running past the filler extends the element")

# Several images inside one packed image, as on the Black Pill 2.
packed = b"\x01" * 8 + b"\xFF" * 56
out = dfu_image.merge([(0x08000000, packed), (0x08000020, b"\x03" * 8), (0x08000010, b"\x02" * 4)])
check(len(out) == 1 and out[0][1] == b"\x01" * 8 + b"\xFF" * 8 + b"\x02" * 4 + b"\xFF" * 12
      + b"\x03" * 8 + b"\xFF" * 24, "two images inside one packed image")

sys.exit(1 if fails else 0)
