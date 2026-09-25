#!/usr/bin/env python3
"""Assemble Freya VM text.

The text is the gas listing QBE emits for -t freya: PDP-11 mnemonics,
32-bit words, and the usual .text/.data/.byte/.int directives. The image
is a raw little-endian memory dump whose symbols are offsets from 0.

  python3 as.py prog.s -o prog.bin --entry main
"""

import argparse
import re
import struct
import sys

REGS = {
    "r0": 0, "r1": 1, "r2": 2, "r3": 3, "r4": 4, "r5": 5,
    "r6": 6, "r7": 7, "sp": 6, "fp": 5, "pc": 7,
}

DBL = {
    "mov": 0o010000, "movb": 0o110000,
    "cmp": 0o020000, "cmpb": 0o120000,
    "bit": 0o030000, "bitb": 0o130000,
    "bic": 0o040000, "bicb": 0o140000,
    "bis": 0o050000, "bisb": 0o150000,
    "add": 0o060000, "sub": 0o160000,
}
UNA = {
    "clr": 0o005000, "clrb": 0o105000,
    "com": 0o005100, "comb": 0o105100,
    "inc": 0o005200, "incb": 0o105200,
    "dec": 0o005300, "decb": 0o105300,
    "neg": 0o005400, "negb": 0o105400,
    "adc": 0o005500, "adcb": 0o105500,
    "sbc": 0o005600, "sbcb": 0o105600,
    "tst": 0o005700, "tstb": 0o105700,
    "ror": 0o006000, "rorb": 0o106000,
    "rol": 0o006100, "rolb": 0o106100,
    "asr": 0o006200, "asrb": 0o106200,
    "asl": 0o006300, "aslb": 0o106300,
    "swab": 0o000300, "sxt": 0o006700,
    "jmp": 0o000100,
}
BR = {
    "br": 0o000400, "bne": 0o001000, "beq": 0o001400,
    "bge": 0o002000, "blt": 0o002400, "bgt": 0o003000, "ble": 0o003400,
    "bpl": 0o100000, "bmi": 0o100400, "bhi": 0o101000, "blos": 0o101400,
    "bvc": 0o102000, "bvs": 0o102400,
    "bcc": 0o103000, "bhis": 0o103000,
    "bcs": 0o103400, "blo": 0o103400,
}
INV = {
    "br": None, "bne": "beq", "beq": "bne", "bge": "blt", "blt": "bge",
    "bgt": "ble", "ble": "bgt", "bpl": "bmi", "bmi": "bpl", "bhi": "blos",
    "blos": "bhi", "bvc": "bvs", "bvs": "bvc", "bcc": "bcs", "bhis": "blo",
    "bcs": "bcc", "blo": "bhis",
}
# reg field is the register, the operand is the other half
REGOP = {
    "ash": 0o072000, "ashc": 0o073000, "mul": 0o070000, "div": 0o071000,
}
XOR = 0o074000
JSR = 0o004000


def parse_number(s):
    s = s.strip()
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    if re.fullmatch(r"0[0-7]+", s):
        return int(s, 8)
    return int(s, 10)


def split_sym(s):
    """NAME, NAME+n or NAME-n. NAME may itself contain $ and ."""
    m = re.fullmatch(r"(.+?)([+-]\d+)$", s.strip())
    if m and not m.group(1).endswith("e") and not m.group(1).endswith("E"):
        return m.group(1), parse_number(m.group(2))
    return s.strip(), 0


def parse_operand(s):
    s = s.strip()
    deferred = False
    if s.startswith("@"):
        deferred = True
        s = s[1:]
    if s.startswith("#"):
        return ("imm", s[1:], deferred)
    if s.startswith("-(") and s.endswith(")"):
        return ("autodec", s[2:-1].strip(), deferred)
    if s.startswith("(") and s.endswith(")+"):
        return ("autoinc", s[1:-2].strip(), deferred)
    if s.startswith("(") and s.endswith(")"):
        return ("def", s[1:-1].strip(), deferred)
    if s.endswith(")") and "(" in s:
        disp, reg = s[:-1].split("(", 1)
        return ("index", disp.strip(), reg.strip(), deferred)
    return ("reg", s, deferred)


def encode_ea(opnd, syms, pc_after_index):
    """Return (mode, reg, [extra words as (abs or None, reloc or None)])."""
    kind = opnd[0]
    if kind == "reg":
        if opnd[2]:
            raise SystemExit("bad deferred register")
        return 0, REGS[opnd[1]], []
    if kind == "def":
        return (7 if opnd[2] else 1), REGS[opnd[1]], []
    if kind == "autoinc":
        return (3 if opnd[2] else 2), REGS[opnd[1]], []
    if kind == "autodec":
        return (5 if opnd[2] else 4), REGS[opnd[1]], []
    if kind == "imm":
        word = reloc_word(opnd[1], syms)
        mode = 3 if opnd[2] else 2
        return mode, 7, [word]
    if kind == "index":
        disp, reg, deferred = opnd[1], opnd[2], opnd[3]
        if disp == "":
            n = 0
            word = (n, None)
        else:
            word = reloc_word(disp, syms)
        return (7 if deferred else 6), REGS[reg], [word]
    raise SystemExit(f"bad operand {opnd}")


def reloc_word(text, syms):
    text = text.strip()
    if re.fullmatch(r"[+-]?(\d+|0x[0-9a-fA-F]+|0[0-7]+)", text):
        return (parse_number(text) & 0xffffffff, None)
    name, off = split_sym(text)
    return (off, name)


class Asm:
    def __init__(self):
        self.sec = "text"
        self.secs = {"text": bytearray(), "rodata": bytearray(), "data": bytearray(), "bss": bytearray()}
        self.syms = {}          # name -> (sec, off)
        self.globs = set()
        self.relocs = []        # (sec, off, name, addend) absolute words
        self.branches = []      # dicts
        self.locn = 0           # gas local label counter, assigned at parse
        self.pending_locals = []

    def here(self):
        return self.sec, len(self.secs[self.sec])

    def emit(self, data):
        self.secs[self.sec] += data

    def align(self, n):
        b = self.secs[self.sec]
        while len(b) % n:
            b.append(0)

    def word(self, value, reloc):
        sec, off = self.here()
        self.emit(struct.pack("<I", value & 0xffffffff))
        if reloc:
            self.relocs.append((sec, off, reloc, value))

    def label(self, name):
        if name in self.syms:
            raise SystemExit(f"duplicate label {name}")
        self.syms[name] = self.here()


def strip_comments(line):
    out = []
    i = 0
    while i < len(line):
        if line.startswith("/*", i):
            j = line.find("*/", i + 2)
            if j < 0:
                break
            i = j + 2
            continue
        out.append(line[i])
        i += 1
    return "".join(out).strip()


def parse_lines(text):
    asm = Asm()
    # gas numeric local labels: N:  defines, Nb / Nf reference
    local_def = {}
    local_uses = []  # filled after we know order; we rewrite to unique names
    # First, expand local labels into unique names in a linear scan.
    lines = []
    counters = {}
    seen = {}
    raw = []
    for line in text.splitlines():
        line = strip_comments(line)
        if not line:
            continue
        raw.append(line)
    # Assign each `N:` a unique id in order, and resolve Nf/Nb.
    defs_at = {}  # (num, occurrence index) 
    seq = []
    for line in raw:
        m = re.fullmatch(r"(\d+):", line)
        if m:
            n = int(m.group(1))
            k = counters.get(n, 0)
            counters[n] = k + 1
            seq.append(("def", n, k))
        else:
            seq.append(("line", line))
    # For each use, find nearest def.
    def_positions = {}
    pos = 0
    indexed = []
    for item in seq:
        if item[0] == "def":
            def_positions.setdefault(item[1], []).append(pos)
            indexed.append(item)
        else:
            indexed.append(item)
        pos += 1
    counters = {n: 0 for n in def_positions}
    out_lines = []
    cursor = 0
    for item in seq:
        if item[0] == "def":
            n, k = item[1], item[2]
            out_lines.append(f".Lloc_{n}_{k}:")
            cursor += 1
            continue
        line = item[1]
        cursor += 1

        def repl(m):
            num = int(m.group(1))
            direction = m.group(2)
            cands = def_positions.get(num, [])
            # cursor is the index in seq; defs before this line have smaller index
            # Recompute: we need the seq index of this line.
            return m.group(0)  # placeholder, second pass below

        out_lines.append(line)

    # Second pass with real indices.
    out_lines = []
    for idx, item in enumerate(seq):
        if item[0] == "def":
            n, k = item[1], item[2]
            out_lines.append(f".Lloc_{n}_{k}:")
            continue
        line = item[1]

        def repl(m, idx=idx):
            num = int(m.group(1))
            direction = m.group(2)
            cands = def_positions.get(num, [])
            if direction == "b":
                prev = [p for p in cands if p < idx]
                if not prev:
                    raise SystemExit(f"no backward label {num}")
                pick = prev[-1]
            else:
                nxt = [p for p in cands if p > idx]
                if not nxt:
                    raise SystemExit(f"no forward label {num}")
                pick = nxt[0]
            # k is the occurrence number stored alongside
            # def_positions values are seq indices; recover k
            k = 0
            seen_n = 0
            for j, it in enumerate(seq):
                if it[0] == "def" and it[1] == num:
                    if j == pick:
                        k = it[2]
                        break
            return f".Lloc_{num}_{k}"

        line = re.sub(r"(\d+)([fb])\b", repl, line)
        out_lines.append(line)
    return out_lines


def assemble(text):
    lines = parse_lines(text)
    # Items in the text section are either raw bytes or branch sites.
    # Other sections are raw bytes with relocations.
    items = []  # text: ("bytes", b) or ("br", mnem, target)
    data = {"rodata": bytearray(), "data": bytearray(), "bss": bytearray()}
    data_rel = []  # (sec, off, name, addend)
    syms = {}
    sec = "text"
    text_len = 0
    globs = set()

    def mark(name):
        if sec == "text":
            syms[name] = ("text", text_len)
        else:
            syms[name] = (sec, len(data[sec]))

    def add_bytes(blob):
        nonlocal text_len
        if sec == "text":
            items.append(("bytes", blob))
            text_len += len(blob)
        else:
            data[sec] += blob

    for line in lines:
        if line.endswith(":") and not line.startswith("."):
            mark(line[:-1])
            continue
        if line.startswith(".") and line.endswith(":") and " " not in line:
            mark(line[:-1])
            continue
        if line.startswith("."):
            parts = line.split(None, 1)
            dirn = parts[0]
            rest = parts[1] if len(parts) > 1 else ""
            if dirn in (".text",):
                sec = "text"
            elif dirn in (".data",):
                sec = "data"
            elif dirn in (".bss",):
                sec = "bss"
            elif dirn == ".section":
                name = rest.split(",")[0].strip()
                if name in (".text",):
                    sec = "text"
                elif name in (".data",):
                    sec = "data"
                elif name in (".bss", ".tbss"):
                    sec = "bss"
                elif "rodata" in name:
                    sec = "rodata"
                else:
                    sec = "data"
            elif dirn == ".rodata":
                sec = "rodata"
            elif dirn == ".globl":
                globs.add(rest.strip())
            elif dirn in (".type", ".size", ".file", ".ident"):
                pass
            elif dirn == ".balign":
                n = parse_number(rest.split(",")[0])
                if sec == "text":
                    pad = (n - text_len % n) % n
                    add_bytes(b"\0" * pad)
                else:
                    while len(data[sec]) % n:
                        data[sec].append(0)
            elif dirn == ".fill":
                a, b, c = [parse_number(x) for x in rest.split(",")]
                add_bytes(bytes([c & 0xff]) * (a * b))
            elif dirn == ".byte":
                add_bytes(bytes([parse_number(x) & 0xff for x in rest.split(",")]))
            elif dirn == ".short":
                for x in rest.split(","):
                    add_bytes(struct.pack("<H", parse_number(x) & 0xffff))
            elif dirn in (".int", ".long", ".word"):
                emit_int(add_bytes, data_rel, sec, text_len, data, rest)
            elif dirn == ".quad":
                emit_quad(add_bytes, data_rel, sec, text_len, data, rest)
            elif dirn == ".ascii" or dirn == ".asciz":
                blob = parse_ascii(rest)
                if dirn == ".asciz":
                    blob += b"\0"
                add_bytes(blob)
            elif dirn == ".comm":
                name, size = rest.split(",")[:2]
                name = name.strip()
                size = parse_number(size)
                syms[name] = ("bss", len(data["bss"]))
                data["bss"] += b"\0" * size
            else:
                raise SystemExit(f"unknown directive {dirn}")
            continue

        # instruction
        if sec != "text":
            raise SystemExit(f"instruction outside .text: {line}")
        mnem, args = split_insn(line)
        if mnem in BR:
            target = args.strip()
            items.append(("br", mnem, target))
            text_len += 4  # provisional, relaxed later
        else:
            blob = encode_insn(mnem, args, syms)
            # encode_insn returns bytes plus reloc placeholders
            items.append(("insn", blob))
            text_len += sum(len(b) if isinstance(b, (bytes, bytearray)) else 4 for b in blob)

    # Branch relaxation. Recompute text symbol offsets each pass.
    def layout():
        off = 0
        for it in items:
            it_off = off
            if it[0] == "bytes":
                off += len(it[1])
            elif it[0] == "br":
                off += 12 if it[-1] == "long" else 4
            else:
                off += sum(4 for _ in it[1])
            yield it, it_off
        return off

    # tag branches with a size flag stored by replacing tuples
    # Use a parallel list of sizes.
    longb = [False] * len(items)
    # Fix symbol offsets from the provisional text_len; recompute properly.
    changed = True
    guard = 0
    while changed and guard < 64:
        guard += 1
        changed = False
        # recompute text symbol positions
        off = 0
        # First rebuild syms for text labels. They were recorded with the
        # provisional length, so re-walk labels... labels are not in items.
        # Record labels as zero-length items instead. I didn't. Second
        # structure: syms currently has wrong text offsets.
        # Re-parse is simpler: store labels inside items.
        break

    raise SystemExit("internal: assembler layout is finished in assemble2")


def split_insn(line):
    parts = line.split(None, 1)
    mnem = parts[0].lower()
    args = parts[1] if len(parts) > 1 else ""
    return mnem, args


def parse_ascii(rest):
    rest = rest.strip()
    if not (rest.startswith('"') and rest.endswith('"')):
        raise SystemExit(f"bad .ascii {rest}")
    s = rest[1:-1]
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] != "\\":
            out.append(ord(s[i]))
            i += 1
            continue
        i += 1
        esc = s[i]
        i += 1
        mapping = {"n": 10, "t": 9, "r": 13, "0": 0, "\\": 92, '"': 34, "a": 7}
        if esc in mapping:
            out.append(mapping[esc])
        elif esc == "x":
            out.append(int(s[i:i + 2], 16))
            i += 2
        else:
            out.append(ord(esc))
    return bytes(out)


def emit_int(add_bytes, data_rel, sec, text_len, data, rest):
    for piece in split_args(rest):
        piece = piece.strip()
        if re.fullmatch(r"[+-]?(\d+|0x[0-9a-fA-F]+|0[0-7]+)", piece):
            add_bytes(struct.pack("<I", parse_number(piece) & 0xffffffff))
        else:
            name, off = split_sym(piece)
            if sec == "text":
                # recorded as a reloc item by the caller via add_bytes of zeros
                # and a side channel. Use a sentinel by appending after.
                pass
            add_bytes(struct.pack("<I", off & 0xffffffff))
            if sec == "text":
                at = text_len  # wrong once add_bytes advanced; caller handles
            else:
                at = len(data[sec]) - 4
            data_rel.append((sec, at, name, off))


def emit_quad(add_bytes, data_rel, sec, text_len, data, rest):
    for piece in split_args(rest):
        piece = piece.strip()
        if re.fullmatch(r"[+-]?(\d+|0x[0-9a-fA-F]+|0[0-7]+)", piece):
            add_bytes(struct.pack("<Q", parse_number(piece) & 0xffffffffffffffff))
        else:
            name, off = split_sym(piece)
            add_bytes(struct.pack("<Q", off & 0xffffffffffffffff))
            at = len(data[sec]) - 8 if sec != "text" else text_len
            data_rel.append((sec, at, name, off))


def split_args(s):
    args = []
    cur = []
    depth = 0
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        args.append("".join(cur))
    return args


def encode_insn(mnem, args, syms):
    """Return a list of words. A word is an int, or ('abs', name, addend)."""
    mnem = mnem.lower()
    ops = split_args(args) if args else []
    if mnem == "halt":
        return [0]
    if mnem == "clc":
        return [0o000241]
    if mnem == "ccc":
        return [0o000257]
    if mnem == "rts":
        return [0o000200 | REGS[ops[0].strip()]]
    if mnem == "jsr":
        reg = REGS[ops[0].strip()]
        mode, dreg, extra = encode_ea(parse_operand(ops[1]), syms, 0)
        word = JSR | (reg << 6) | (mode << 3) | dreg
        return [word] + words(extra)
    if mnem == "xor":
        reg = REGS[ops[0].strip()]
        mode, dreg, extra = encode_ea(parse_operand(ops[1]), syms, 0)
        return [XOR | (reg << 6) | (mode << 3) | dreg] + words(extra)
    if mnem in REGOP:
        mode, sreg, extra = encode_ea(parse_operand(ops[0]), syms, 0)
        reg = REGS[ops[1].strip()]
        return [REGOP[mnem] | (reg << 6) | (mode << 3) | sreg] + words(extra)
    if mnem in DBL:
        sm, sr, se = encode_ea(parse_operand(ops[0]), syms, 0)
        dm, dr, de = encode_ea(parse_operand(ops[1]), syms, 0)
        word = DBL[mnem] | (sm << 9) | (sr << 6) | (dm << 3) | dr
        return [word] + words(se) + words(de)
    if mnem in UNA:
        mode, reg, extra = encode_ea(parse_operand(ops[0]), syms, 0)
        if mnem == "jmp" and mode == 0:
            raise SystemExit("jmp on a register")
        return [UNA[mnem] | (mode << 3) | reg] + words(extra)
    raise SystemExit(f"unknown instruction {mnem}")


def words(extra):
    out = []
    for value, reloc in extra:
        if reloc:
            out.append(("abs", reloc, value))
        else:
            out.append(value & 0xffffffff)
    return out


def assemble_file(text):
    lines = parse_lines(text)
    items = []
    secs = {"text": [], "rodata": [], "data": [], "bss": []}
    sec = "text"
    syms = {}
    globs = set()

    def cur_off():
        return sum(item_size(it) for it in secs[sec])

    def add(item):
        secs[sec].append(item)

    for line in lines:
        lab = None
        if line.endswith(":") and " " not in line:
            lab = line[:-1]
            syms[lab] = (sec, cur_off())
            continue
        if line.startswith("."):
            parts = line.split(None, 1)
            dirn = parts[0]
            rest = parts[1] if len(parts) > 1 else ""
            if dirn == ".text":
                sec = "text"
            elif dirn == ".data":
                sec = "data"
            elif dirn == ".bss":
                sec = "bss"
            elif dirn == ".rodata":
                sec = "rodata"
            elif dirn == ".section":
                name = rest.split(",")[0].strip()
                if name == ".text":
                    sec = "text"
                elif name == ".bss":
                    sec = "bss"
                elif "rodata" in name:
                    sec = "rodata"
                elif name.startswith(".note"):
                    sec = "skip"
                else:
                    sec = "data"
            elif dirn == ".globl":
                globs.add(rest.split(",")[0].strip())
            elif dirn in (".type", ".size", ".file", ".ident"):
                pass
            elif sec == "skip":
                pass
            elif dirn == ".balign":
                n = parse_number(rest.split(",")[0])
                pad = (n - cur_off() % n) % n
                if pad:
                    add(("bytes", b"\0" * pad))
            elif dirn == ".fill":
                a, b, c = [parse_number(x) for x in rest.split(",")]
                add(("bytes", bytes([c & 0xff]) * (a * b)))
            elif dirn == ".byte":
                add(("bytes", bytes(parse_number(x) & 0xff for x in rest.split(","))))
            elif dirn == ".short":
                blob = b"".join(struct.pack("<H", parse_number(x) & 0xffff) for x in rest.split(","))
                add(("bytes", blob))
            elif dirn in (".int", ".long", ".word", ".quad"):
                wide = dirn == ".quad"
                for piece in split_args(rest):
                    piece = piece.strip()
                    if re.fullmatch(r"[+-]?(\d+|0x[0-9a-fA-F]+|0[0-7]+)", piece):
                        n = parse_number(piece)
                        blob = struct.pack("<Q" if wide else "<I", n & (0xffffffffffffffff if wide else 0xffffffff))
                        add(("bytes", blob))
                    else:
                        name, off = split_sym(piece)
                        add(("abs", name, off, 8 if wide else 4))
            elif dirn in (".ascii", ".asciz"):
                blob = parse_ascii(rest)
                if dirn == ".asciz":
                    blob += b"\0"
                add(("bytes", blob))
            elif dirn == ".comm":
                bits = [x.strip() for x in rest.split(",")]
                name, size = bits[0], parse_number(bits[1])
                # alignment is ignored beyond 4
                syms[name] = ("bss", sum(item_size(it) for it in secs["bss"]))
                secs["bss"].append(("bytes", b"\0" * size))
            else:
                raise SystemExit(f"unknown directive {line}")
            continue
        if sec != "text":
            raise SystemExit(f"instruction outside .text: {line}")
        mnem, args = split_insn(line)
        if mnem in BR:
            add(("br", mnem, args.strip(), False))
        else:
            add(("insn", encode_insn(mnem, args, syms)))

    # Relax branches, updating text label offsets.
    def text_labels():
        off = 0
        pos = {}
        for it in secs["text"]:
            # labels were stored in syms with old offsets; recompute from scratch
            off += item_size(it)
        return pos

    # Labels point at item boundaries. Store them as ("lab", name) items
    # so relaxation can move them. Re-scan: syms values for text are stale
    # once sizes change, and they were recorded against item sizes. A label
    # is not an item, so its offset was cur_off() at definition, which used
    # item_size. Branch items start short (4). When one grows, later labels
    # move. Recompute every text symbol from a parallel label list.
    return link(secs, syms)


def item_size(it):
    kind = it[0]
    if kind == "bytes":
        return len(it[1])
    if kind == "abs":
        return it[3]
    if kind == "br":
        return 12 if it[3] else 4
    if kind == "insn":
        return 4 * len(it[1])
    raise SystemExit(f"bad item {it}")


def link(secs, syms_in):
    # Rebuild text symbol offsets by replaying is not possible: labels are
    # only in syms_in. Record (name -> offset) at the time of short branches
    # and then adjust.
    #
    # Because a label's offset was computed with item_size, and branches
    # start short, syms_in text offsets match the short layout. Growing a
    # branch by 8 adds 8 to every later text symbol and every later item.
    long_at = set()
    text = secs["text"]

    def offsets():
        off = 0
        out = []
        for i, it in enumerate(text):
            out.append(off)
            if it[0] == "br" and i in long_at:
                off += 12
            else:
                off += item_size(it) if not (it[0] == "br") else 4
        return out, off

    def text_base_of_labels(item_offs):
        # syms_in text offsets correspond to the all-short layout. Map an
        # all-short offset to a relaxed offset by counting grown branches
        # before that point.
        short = []
        off = 0
        for it in text:
            short.append(off)
            off += 4 if it[0] == "br" else item_size(it)
        return short

    changed = True
    guard = 0
    while changed:
        guard += 1
        if guard > 100:
            raise SystemExit("branch relaxation did not converge")
        changed = False
        item_offs, _ = offsets()
        short_offs = text_base_of_labels(item_offs)
        # map short offset -> relaxed offset
        def relax_off(short_off):
            # find the item that starts at short_off, or the nearest preceding
            acc_s = 0
            acc_r = 0
            for i, it in enumerate(text):
                sz_s = 4 if it[0] == "br" else item_size(it)
                sz_r = 12 if (it[0] == "br" and i in long_at) else sz_s
                if acc_s == short_off:
                    return acc_r
                if acc_s > short_off:
                    break
                acc_s += sz_s
                acc_r += sz_r
            if acc_s == short_off:
                return acc_r
            raise SystemExit(f"label offset {short_off} is not on an instruction")

        for i, it in enumerate(text):
            if it[0] != "br" or i in long_at:
                continue
            mnem, target = it[1], it[2]
            if target not in syms_in:
                raise SystemExit(f"undefined label {target}")
            sec, off = syms_in[target]
            if sec != "text":
                dest = None
            else:
                dest = relax_off(off)
            src = item_offs[i] + 4  # PC after the branch word
            if dest is None:
                fits = False
            else:
                delta = (dest - src) // 4
                fits = -128 <= delta <= 127
            if not fits:
                long_at.add(i)
                changed = True

    item_offs, _ = offsets()
    short_offs_list = []
    acc = 0
    for it in text:
        short_offs_list.append(acc)
        acc += 4 if it[0] == "br" else item_size(it)

    def relax_off(short_off):
        acc_s = 0
        acc_r = 0
        for i, it in enumerate(text):
            sz_s = 4 if it[0] == "br" else item_size(it)
            sz_r = 12 if (it[0] == "br" and i in long_at) else sz_s
            if acc_s == short_off:
                return acc_r
            acc_s += sz_s
            acc_r += sz_r
        if acc_s == short_off:
            return acc_r
        raise SystemExit(f"bad label offset {short_off}")

    # Section bases: text, rodata, data, bss, each 4-aligned.
    blobs = {}
    bases = {}
    cursor = 0
    resolved = {}
    for name in ("text", "rodata", "data", "bss"):
        if cursor % 4:
            cursor += 4 - cursor % 4
        bases[name] = cursor
        raw = bytearray()
        for i, it in enumerate(secs[name]):
            if it[0] == "bytes":
                raw += it[1]
            elif it[0] == "abs":
                sym, add, width = it[1], it[2], it[3]
                # patched later
                raw += b"\0" * width
            elif it[0] == "insn":
                for w in it[1]:
                    if isinstance(w, tuple):
                        raw += b"\0\0\0\0"
                    else:
                        raw += struct.pack("<I", w & 0xffffffff)
            elif it[0] == "br":
                mnem, target, _ = it[1], it[2], it[3]
                if i in long_at:
                    if mnem == "br":
                        raw += b"\0" * 8  # jmp @#target
                    else:
                        raw += b"\0" * 12
                else:
                    raw += b"\0\0\0\0"
            else:
                raise SystemExit(f"bad item {it}")
        blobs[name] = raw
        cursor += len(raw)

    for name, (s, off) in syms_in.items():
        if s == "text":
            resolved[name] = bases["text"] + relax_off(off)
        else:
            resolved[name] = bases[s] + off

    def patch(blob, secname):
        off = 0
        out = bytearray(blob)
        for i, it in enumerate(secs[secname]):
            if it[0] == "bytes":
                off += len(it[1])
            elif it[0] == "abs":
                sym, add, width = it[1], it[2], it[3]
                if sym not in resolved and sym not in syms_in:
                    raise SystemExit(f"undefined symbol {sym}")
                val = resolved[sym] + add
                out[off:off + width] = (val & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
                off += width
            elif it[0] == "insn":
                for w in it[1]:
                    if isinstance(w, tuple):
                        _, sym, add = w
                        val = (resolved[sym] + add) & 0xffffffff
                        out[off:off + 4] = struct.pack("<I", val)
                    off += 4
            elif it[0] == "br":
                mnem, target = it[1], it[2]
                dest = resolved[target]
                if i in long_at:
                    if mnem == "br":
                        # jmp @#dest : mode 3, reg 7
                        word = 0o000100 | (3 << 3) | 7
                        out[off:off + 4] = struct.pack("<I", word)
                        out[off + 4:off + 8] = struct.pack("<I", dest & 0xffffffff)
                        off += 8
                    else:
                        inv = INV[mnem]
                        # inverted branch over the jmp: offset +2 words
                        word = BR[inv] | (2 & 0xff)
                        out[off:off + 4] = struct.pack("<I", word)
                        jmp = 0o000137  # jmp @#  == 000100 | 030 | 7
                        out[off + 4:off + 8] = struct.pack("<I", jmp)
                        out[off + 8:off + 12] = struct.pack("<I", dest & 0xffffffff)
                        off += 12
                else:
                    src = bases["text"] + off + 4
                    delta = (dest - src) // 4
                    if not -128 <= delta <= 127:
                        raise SystemExit(f"branch to {target} out of range")
                    word = BR[mnem] | (delta & 0xff)
                    out[off:off + 4] = struct.pack("<I", word)
                    off += 4
        return bytes(out)

    image = b""
    for name in ("text", "rodata", "data", "bss"):
        blob = patch(blobs[name], name)
        if len(image) % 4:
            image += b"\0" * (4 - len(image) % 4)
        assert len(image) == bases[name]
        image += blob
    return image, resolved


def main():
    ap = argparse.ArgumentParser(description="assemble Freya VM text")
    ap.add_argument("src", nargs="?", default="-")
    ap.add_argument("-o")
    ap.add_argument("--entry")
    args = ap.parse_args()
    if args.src == "-":
        text = sys.stdin.read()
    else:
        text = open(args.src).read()
    image, syms = assemble_file(text)
    if args.o:
        open(args.o, "wb").write(image)
        entry_out = sys.stdout
    else:
        sys.stdout.buffer.write(image)
        entry_out = sys.stderr
    if args.entry:
        if args.entry not in syms:
            raise SystemExit(f"no symbol {args.entry}")
        print(syms[args.entry], file=entry_out)


if __name__ == "__main__":
    main()
