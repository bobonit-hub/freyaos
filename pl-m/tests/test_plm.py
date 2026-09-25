"""Compile PL/M samples and, when the toolchain is present, link them."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PLM = os.path.join(ROOT, "pl-m")
sys.path.insert(0, PLM)

from plm import PlmError, compile_source  # noqa: E402

LIB = [os.path.join(PLM, "lib")]
GCC = shutil.which("arm-none-eabi-gcc")
LDSCRIPT = os.path.join(ROOT, "boards", "bluepill", "app_flash.ld")


def asm(text, name="t.plm"):
    return compile_source(text, name, LIB)


class LanguageTests(unittest.TestCase):
    def test_address_is_32_bit(self):
        src = asm("""
T: DO;
  DECLARE p ADDRESS;
  DECLARE n DWORD;
  n = SIZE(p);
END T;
""")
        self.assertIn("movs r0, #4", src)
        self.assertNotIn("vldr", src)
        self.assertNotIn("__aeabi", src)

    def test_integer_is_32_bit_and_signed_compare(self):
        src = asm("""
T: DO;
  DECLARE n INTEGER;
  n = -1;
  IF n < 0 THEN n = 1;
END T;
""")
        self.assertIn("movlt r0, #255", src)

    def test_no_float(self):
        with self.assertRaises(PlmError) as cm:
            asm("T: DO; DECLARE x REAL; END T;")
        self.assertIn("floating point", str(cm.exception))
        with self.assertRaises(PlmError) as cm:
            asm("T: DO; DECLARE x DWORD INITIAL (1.5); END T;")
        self.assertIn("floating point", str(cm.exception))

    def test_literally_and_data(self):
        src = asm("""
T: DO;
  DECLARE CR LITERALLY '0DH';
  DECLARE msg(*) BYTE DATA ('A', CR, 0);
END T;
""")
        self.assertIn(".byte 65, 13, 0", src)

    def test_flash_header(self):
        src = asm("HELLO: DO; END HELLO;")
        self.assertIn(".long 0x41595246", src)
        self.assertIn(".long 0x0800c080", src.lower())
        self.assertIn(".long 1", src)
        self.assertIn("app_main", src)
        self.assertIn("runflash", src)

    def test_fact_and_hello_compile(self):
        for name in ("hello.plm", "fact.plm"):
            path = os.path.join(PLM, "examples", name)
            with open(path, encoding="utf-8") as fh:
                listing = compile_source(fh.read(), path, LIB)
            self.assertIn("app_main", listing)


@unittest.skipUnless(GCC and os.path.isfile(LDSCRIPT), "arm-none-eabi-gcc required")
class LinkTests(unittest.TestCase):
    def _link(self, listing):
        tmp = tempfile.mkdtemp()
        src = os.path.join(tmp, "p.s")
        elf = os.path.join(tmp, "p.elf")
        with open(src, "w", encoding="utf-8") as fh:
            fh.write(listing)
        subprocess.check_call([
            GCC, "-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft",
            "-nostdlib", "-T", LDSCRIPT, "-Wl,--no-warn-rwx-segments",
            src, "-o", elf,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return elf

    def test_examples_link_for_program_flash(self):
        for name in ("hello.plm", "fact.plm"):
            path = os.path.join(PLM, "examples", name)
            with open(path, encoding="utf-8") as fh:
                elf = self._link(compile_source(fh.read(), path, LIB))
            out = subprocess.check_output(
                ["arm-none-eabi-objdump", "-s", "-j", ".header", elf],
                text=True)
            flat = out.lower().replace(" ", "")
            self.assertIn("46525941", flat)
            self.assertIn("80c00008", flat)


if __name__ == "__main__":
    unittest.main()
