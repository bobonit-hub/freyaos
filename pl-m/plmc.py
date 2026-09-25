#!/usr/bin/env python3
"""Compile a PL/M module to a Cortex-M3 assembler listing for Freya."""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from plm import PlmError, compile_source


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", help="PL/M source file")
    p.add_argument("-o", "--output", help="assembler listing (default: source with .s)")
    p.add_argument("-I", dest="includes", action="append", default=[],
                   help="directory searched by $INCLUDE")
    p.add_argument("--stack", type=int, default=2048,
                   help="stack_need stored in the Freya header (default 2048)")
    args = p.parse_args(argv)
    out = args.output
    if not out:
        base, _ = os.path.splitext(args.source)
        out = base + ".s"
    lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")
    includes = [os.path.dirname(os.path.abspath(args.source)), lib] + args.includes
    try:
        with open(args.source, "r", encoding="utf-8") as fh:
            text = fh.read()
        listing = compile_source(text, args.source, includes, args.stack)
    except PlmError as exc:
        print(f"plmc: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"plmc: {exc}", file=sys.stderr)
        return 1
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(listing)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
