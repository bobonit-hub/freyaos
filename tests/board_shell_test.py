#!/usr/bin/env python3
"""On-board checks for the Freya shell language.

Talks to the serial console the way tools/fremote.py does. One port is
assumed when none is named; otherwise pass u0, /dev/ttyUSB0, and so on.

The suite does not reboot, install, or remove anything it did not create.
A function cannot be dropped, so function and thread checks are skipped
when all four slots are already taken, and otherwise reuse the name sht.
Variables the suite creates are unset before it finishes.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import fremote  # noqa: E402

fails = 0
checks = 0
board = None

# Names this run stored, so the board is left without them.
_ours = []


def check(cond, what, detail=""):
    global fails, checks
    checks += 1
    if cond:
        print(f"  ok    {what}", flush=True)
    else:
        fails += 1
        extra = f" ({detail})" if detail else ""
        print(f"  FAIL  {what}{extra}", flush=True)


def _body(text):
    """Drop the echoed command line and the prompt. Keep the command's text."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    nl = text.find("\n")
    if nl >= 0:
        text = text[nl + 1:]
    cut = text.rfind("freya:")
    if cut >= 0:
        text = text[:cut]
    return text


def sh(line, timeout=8):
    """Run one shell line. Return (status, output)."""
    if len(line) > 159:
        raise fremote.RemoteError(f"line is {len(line)} characters; the shell takes 159")
    board.write(b"\x15")
    board.write(line.encode() + b"\r")
    text = board._read_until_prompt(timeout)
    if "freya:" not in text:
        board.write(b"\x03")
        text += board._read_until_prompt(3)
        raise fremote.RemoteError(
            f"no prompt after {line!r}: {text!r}")
    out = _body(text)
    board.write(b"echo $?\r")
    st = board._read_until_prompt(5)
    m = re.search(r"(\d+)\s*$", _body(st).strip())
    if not m:
        raise fremote.RemoteError(f"no status after {line!r}: {st!r}")
    return int(m.group(1)), out


def ok(line, what, expect=None, timeout=8):
    try:
        status, out = sh(line, timeout)
    except fremote.RemoteError as exc:
        check(False, what, str(exc))
        return ""
    detail = out.strip().replace("\n", " | ")
    check(status == 0, what, f"status {status}: {detail}")
    if expect is not None and status == 0:
        check(out.strip() == expect, f"{what}: output", repr(out.strip()))
    return out


def bad(line, what, status=1, text=None):
    try:
        got, out = sh(line)
    except fremote.RemoteError as exc:
        check(False, what, str(exc))
        return
    check(got == status, what, f"status {got}, output {out.strip()!r}")
    if text is not None:
        check(text in out, f"{what}: message", repr(out.strip()))


def keep(name):
    if name not in _ours:
        _ours.append(name)


def main(argv):
    global board
    port = None
    for a in argv:
        if a in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        port = a
    if port is None:
        port = fremote.default_port()
    else:
        port = fremote.resolve_port(port)

    print(f"Freya shell language on {port}", flush=True)
    board = fremote.Board(port)
    try:
        hello = board.sync()
        check("freya:" in hello, "the console answers")
        if "freya:" not in hello:
            return 1
        run()
    finally:
        cleanup()
        board.close()

    print(f"\n{checks} checks, {fails} failures")
    return 1 if fails else 0


def cleanup():
    if board is None:
        return
    for name in _ours:
        try:
            sh(f"unset {name}")
        except fremote.RemoteError:
            pass
    try:
        board.write(b"\x03")
        board._read_until_prompt(2)
    except fremote.RemoteError:
        pass


def free_names(listing, want):
    """Pick short names that are not already variables."""
    taken = set(re.findall(r"^(\w+) = ", listing, re.M))
    picked = []
    for i in range(26):
        name = "z" + chr(ord("a") + i)
        if name not in taken:
            picked.append(name)
        if len(picked) == want:
            return picked
    return None


def run():
    print("\nvalues", flush=True)
    listing = ok("set", "set lists variables")
    names = free_names(listing, 6)
    check(names is not None, "six variable slots are free")
    if not names:
        print("  skip  the board already has eight variables", flush=True)
        return
    n, a, b, c, s, x = names
    for name in names:
        keep(name)

    ok(f"set {n} 1 + 2 * 3", "multiplication binds tighter than addition")
    ok(f"echo ${n}", "1 + 2 * 3 is 7", "7")
    ok(f"set {n} 7.5 / 2", "a float divides")
    ok(f"echo ${n}", "7.5 / 2 is 3.75", "3.75")
    ok(f"set {n} 1b + 2b", "bytes add as integers")
    ok(f"echo ${n}", "1b + 2b is 3", "3")
    ok(f"set {n} 0xFFFFFFFF", "hex keeps all 32 bits")
    ok(f"echo ${n}", "0xFFFFFFFF is -1", "-1")
    ok(f"set {n} -1 >> 1", "a shift fills with zero")
    ok(f"echo ${n}", "-1 >> 1 is 2147483647", "2147483647")
    ok(f"set {n} 1 == 1.0", "numbers match by value")
    ok(f"echo ${n}", "1 == 1.0", "1")
    ok(f"set {n} 1 == 1b", "a byte matches its integer")
    ok(f"echo ${n}", "1 == 1b", "1")
    ok(f"set {n} true == 1", "a bool does not match a number")
    ok(f"echo ${n}", "true == 1 is 0", "0")
    ok(f"set {n} empty == none", "empty is not none")
    ok(f"echo ${n}", "empty == none is 0", "0")
    ok(f"set {n} 1 < 2 < 3", "comparisons associate left to right")
    ok(f"echo ${n}", "1 < 2 < 3 is 1", "1")
    ok(f'set {s} "%d %s" 7 "items"', "a string formats the values after it")
    ok(f"echo ${s}", "the format is filled", "7 items")
    ok(f"set {n} 65b", "a byte is stored as a byte")
    listed = ok("set", "set shows the byte")
    check(f"{n} = 65b" in listed, "the listing prints 65b", listed.strip())
    bad(f"set {n} 1 / 0", "division by zero", text="division by zero")
    bad(f"set {n} true + 1", "a bool is not arithmetic", text="bad expression")
    bad("set nosuch", "a missing expression", text="usage:")

    print("\ncontrol", flush=True)
    ok("if true; echo yes; else; echo no; end", "if true takes the first branch", "yes")
    ok("if false; echo yes; else; echo no; end", "if false takes the else", "no")
    ok(f"set {n} 7", "store 7 for a comparison")
    ok(f"if ${n} == 7; echo yes; else; echo no; end", "a comparison is a condition", "yes")
    status, out = sh("if false; echo yes; end")
    check(status == 1 and out.strip() == "", "a false condition with no else leaves status 1",
          f"status {status}, output {out.strip()!r}")
    ok("loop 3; echo tick; end", "loop repeats", "tick\ntick\ntick")
    ok("loop 0; echo tick; end", "loop 0 runs nothing", "")
    ok("loop 3; echo x; break; end; echo z", "break leaves the loop", "x\nz")
    bad("break", "break outside a loop", text="unexpected break")
    bad("return 1", "return outside a function", text="unexpected return")
    ok("echo hi # there", "a comment does not print", "hi")
    ok('echo "a # b"', "quotes hide a hash", "a # b")
    ok('echo "a;b"', "quotes hide a semicolon", "a;b")
    bad("nosuchcmd", "an unknown word", status=127, text="command not found")

    print("\nconversion, patterns, numbers, clock", flush=True)
    ok(f'set {n} int("0x10")', "int reads hex text")
    ok(f"echo ${n}", "int of 0x10 is 16", "16")
    ok(f"set {n} int(\"0xFFFFFFFF\")", "int of 0xFFFFFFFF")
    ok(f"echo ${n}", "int of 0xFFFFFFFF is -1", "-1")
    ok(f"set {n} byte(255)", "byte of 255")
    ok(f"echo ${n}", "byte stays in range", "255")
    bad(f"set {n} byte(256)", "byte refuses 256", text="integer overflow")
    ok(f'set {n} bool("true")', "bool reads its text")
    ok(f"echo ${n}", "bool of true", "true")
    ok(f"set {n} hex(-1)", "hex of -1")
    ok(f"echo ${n}", "hex(-1) is ffffffff", "ffffffff")
    ok(f"set {n} hex(hex(255))", "hex round trip")
    ok(f"echo ${n}", "hex(hex(255)) is 255", "255")
    bad(f"set {n} int(empty)", "empty has no number", text="not a number")
    bad(f"set {n} int(none)", "none has no number", text="not a number")

    ok(f'set {s} match("abc-12", "%a+")', "match takes the letters")
    ok(f"echo ${s}", "match result", "abc")
    ok(f'set {a}, {b} match("abc-12", "(%a+)%-(%d+)")', "match returns captures")
    ok(f"echo ${a}", "first capture", "abc")
    ok(f"echo ${b}", "second capture", "12")
    ok(f'set {a}, {b} find("abc-12", "%d+")', "find returns the span")
    ok(f"echo ${a}", "match starts at 5", "5")
    ok(f"echo ${b}", "match ends at 6", "6")
    ok(f'set {s}, {n} gsub("a1b2", "%d", "x")', "gsub replaces each match")
    ok(f"echo ${s}", "digits became x", "axbx")
    ok(f"echo ${n}", "two replacements", "2")
    status, out = sh(f'set {s} match("zzz", "%d+")')
    check(status == 0, "a failed match succeeds", f"status {status}: {out.strip()}")
    ok(f"echo ${s}", "no match is none", "none")
    bad(f'set {s} match("abc", "[")', "a broken pattern", text="bad pattern")

    ok(f"set {n} srand(1)", "srand replaces the state")
    ok(f"set {n} rand()", "first rand after seed 1")
    ok(f"echo ${n}", "rand is 16838", "16838")
    ok(f"set {n} rand()", "second rand")
    ok(f"echo ${n}", "rand is 5758", "5758")
    ok(f"set {x} sin(pi() / 2)", "sine of a right angle")
    ok(f"echo ${x}", "sin(pi() / 2) is 1", "1")
    ok(f"set {x} cos(pi())", "cosine of pi")
    ok(f"echo ${x}", "cos(pi()) is -1", "-1")

    ok(f"set {n} time(2026, 1, 1, 0, 0, 0)", "time builds a count of seconds")
    ok(f"set {s} date(${n})", "date formats that count")
    ok(f"echo ${s}", "the epoch of 2026", "2026-01-01 00:00:00")
    ok(f"set {n} year(${n})", "year reads one field")
    ok(f"echo ${n}", "the year is 2026", "2026")
    ok(f"set {n} now()", "now reads the clock")
    ok(f"set {n} ticks()", "ticks reads milliseconds since boot")

    print("\narrays and dicts", flush=True)
    room = heap_largest()
    if room < 64:
        print(f"  skip  heap largest free block is {room} B; arrays need more", flush=True)
    else:
        arrays(n, a, b, c)

    print("\nfunctions", flush=True)
    listed = ok("fn", "fn lists functions")
    have = [ln.strip() for ln in listed.splitlines() if ln.strip() and ln.strip() != "no functions"]
    if room < 64:
        print(f"  skip  a function body is stored on the heap ({room} B free)", flush=True)
    elif len(have) >= 4 and "sht" not in have:
        print("  skip  all four function slots are in use", flush=True)
    else:
        functions(n, a, b, c, s)

    print("\nfiles and threads", flush=True)
    prompt = board.sync()
    if "(no fs)" in prompt:
        print("  skip  no filesystem is mounted", flush=True)
    else:
        files(n, s)
    # functions() may have been skipped; threads need sht.
    listed = ok("fn", "functions after the earlier checks")
    if "sht" in listed.split():
        threads(n)
    else:
        print("  skip  threads need a free function slot", flush=True)

    print("\nclean", flush=True)
    for name in (n, a, b, c, s, x):
        st, _out = sh(f"unset {name}")
        # unset of a name we never stored is a failure; either is fine here.
        if st == 0 and name in _ours:
            pass
    left = ok("set", "variables this suite stored are gone")
    for name in names:
        check(not re.search(rf"^{name} = ", left, re.M), f"{name} is unset", left.strip())


def heap_largest():
    _status, out = sh("meminfo")
    m = re.search(r"largest free block (\d+)", out)
    return int(m.group(1)) if m else 0


def arrays(n, a, b, c):
    ok(f"set {a} array(30, 10, 20)", "array stores the values")
    ok(f"set {n} min(${a})", "min is the least element")
    ok(f"echo ${n}", "min is 10", "10")
    ok(f"set {n} max(${a})", "max is the greatest element")
    ok(f"echo ${n}", "max is 30", "30")
    ok(f"set {b} sort(${a})", "sort returns a new array")
    ok(f"echo ${b}", "the copy is ordered", "[10, 20, 30]")
    ok(f"echo ${a}", "sort leaves the original", "[30, 10, 20]")
    ok(f"unset {b}", "unset frees the copy")
    ok(f"set {a}[2] 40", "an index past the end grows the array")
    ok(f"echo ${a}", "the gap was filled", "[30, 10, 40]")
    ok(f'set {c} dict("b", 2, "a", 1)', "dict keeps keys sorted")
    ok(f"echo ${c}", "keys are in order", '{"a": 1, "b": 2}')
    ok(f'set {n} ${c}["a"]', "a dict lookup")
    ok(f"echo ${n}", "the value for a is 1", "1")
    ok(f'set {c}["d"] 4', "a new key is inserted")
    ok(f'set {n} ${c}["d"]', "the new key reads back")
    ok(f"echo ${n}", "the value for d is 4", "4")
    bad(f'set {n} ${c}["z"]', "a missing key", text="no such key")
    bad(f"set {a} array(1, 2, 3, 4, 5, 6, 7, 8, 9)", "nine elements", text="array too long")
    bad(f'set {a}[0] "no"', "an array keeps one type", text="type mismatch")
    ok(f"unset {a}", "unset the array")
    ok(f"unset {c}", "unset the dict")


def functions(n, a, b, c, s):
    ok("fn sht; return $1 + $2; end", "define sht")
    ok(f"set {n} sht(2, 3)", "a call is an expression")
    ok(f"echo ${n}", "sht(2, 3) is 5", "5")
    ok(f"fn sht; if $1 /= 0; return 1, 2.5, \"ok\"; end; return 0, 0, \"no\"; end",
       "redefine sht to return three values")
    ok(f"set {a}, {b}, {c} sht(1)", "set stores one value per name")
    ok(f"echo ${a}", "first value", "1")
    ok(f"echo ${b}", "second value", "2.5")
    ok(f"echo ${c}", "third value", "ok")
    ok(f"set {s} sht(0)", "one name takes the first value")
    ok(f"echo ${s}", "the first of the false branch", "0")
    bad("fn if; return 1; end", "a reserved name", text="bad name")
    bad(f"set {a}, {b}, {c} sht(1); set {a}, {b}, {c}, {s}, {n} sht(1)",
        "five names need five values")
    # The line above may have stored some names before failing. That is fine.


def files(n, s):
    path = "/shtest.txt"
    ok(f'set {n} open("{path}", "w")', "open for writing")
    ok(f'set {s} write(${n}, "hi", 10b)', "write text and a newline")
    ok(f"set {s} close(${n})", "close the file")
    ok(f'set {n} open("{path}")', "open for reading")
    ok(f"set {s} read(${n})", "read the line")
    ok(f"echo ${s}", "the line is hi", "hi")
    ok(f"set {s} read(${n})", "the next read is the end")
    ok(f"echo ${s}", "end of file is empty", "empty")
    ok(f"set {s} close(${n})", "close the reader")
    ok(f"rm {path}", "remove the test file")


def threads(n):
    ok('fn sht; echo tick; return 1; end', "a thread body")
    status, out = sh('set %s spawn("sht", 1)' % n, timeout=5)
    check(status == 0 and "tick" in out, "spawn runs the function",
          f"status {status}, output {out.strip()!r}")
    ok(f"set {n} join(${n})", "join waits until it has finished", timeout=5)
    ok(f"echo ${n}", "join returns 0", "0")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except fremote.RemoteError as exc:
        print(f"board: {exc}", file=sys.stderr)
        sys.exit(1)
