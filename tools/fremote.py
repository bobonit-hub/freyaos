#!/usr/bin/env python3
"""
Remote shell and SD card utility for a Freya board.

Talks to the serial console the way mpremote talks to MicroPython: an
interactive shell, one-shot commands, and copy/list/remove on the card.
A path with a leading ':' is on the board. Anything else is on the host.

    python3 tools/fremote.py                         # shell on the only port
    python3 tools/fremote.py u0                      # /dev/ttyUSB0, then the shell
    python3 tools/fremote.py fs ls :/
    python3 tools/fremote.py fs cp build/apps/hello.bin :/hello.bin
    python3 tools/fremote.py fs cp :/notes.txt .
    python3 tools/fremote.py exec "led blink" + fs df

'fs cp' uses XMODEM. 'download --size' and 'upload' on the board keep
the exact byte count, so a file that ends in 0x1A is not truncated.
Requires pyserial (pip install pyserial).
"""
import os
import re
import secrets
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

SOH, STX, EOT, ACK, NAK, CAN, SUB = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x1A
BAUD = 921600

_LL_LINE = re.compile(
    r"^  (\S)\S{4} +(\S+) +(\d{4}-\d{2}-\d{2} \d{2}:\d{2})  (.*)$")
_READY_SEND = re.compile(r"Ready to send '.*' \((\d+) bytes\) over XMODEM")
_PROMPT = re.compile(r"freya:[^\r\n]*> ")


class RemoteError(Exception):
    pass


def crc16(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def quote_path(path: str) -> str:
    """Quote a path for the Freya shell. The shell has no backslash escapes."""
    if any(c in path for c in "\"\r\n;"):
        raise RemoteError(f"path cannot contain quotes or ';' : {path}")
    if any(c.isspace() for c in path):
        return f'"{path}"'
    return path


def parse_remote(path: str):
    """Return (on_board, path). A leading ':' marks the card."""
    if path == ":":
        return True, "."
    if path.startswith(":"):
        return True, path[1:] or "."
    return False, path


def resolve_port(name: str) -> str:
    """u0 -> /dev/ttyUSB0, a0 -> /dev/ttyACM0, c3 -> COM3. Other names pass through."""
    m = re.fullmatch(r"([uacUAC])(\d+)", name or "")
    if not m:
        return name
    n = m.group(2)
    kind = m.group(1).lower()
    if kind == "c":
        return f"COM{n}"
    dev = "ttyUSB" if kind == "u" else "ttyACM"
    return f"/dev/{dev}{n}"


def default_port() -> str:
    if list_ports is None:
        raise RemoteError("pyserial is required: pip install pyserial")
    found = []
    for p in list_ports.comports():
        dev = p.device
        if "ttyUSB" in dev or "ttyACM" in dev or dev.upper().startswith("COM"):
            found.append(dev)
    if len(found) == 1:
        return found[0]
    if not found:
        raise RemoteError("no serial port found; pass one, for example u0 or /dev/ttyUSB0")
    raise RemoteError("several serial ports: " + ", ".join(found))


def parse_ll(text: str):
    """Entries from an 'ls -l' listing: (name, is_dir, size)."""
    rows = []
    for line in text.splitlines():
        m = _LL_LINE.match(line)
        if not m:
            continue
        is_dir = m.group(2) == "<DIR>"
        size = 0 if is_dir else int(m.group(2))
        rows.append((m.group(4), is_dir, size))
    return rows


def split_chain(argv):
    """Split a command list on a bare '+', the way mpremote chains commands."""
    groups = [[]]
    for arg in argv:
        if arg == "+":
            if not groups[-1]:
                raise RemoteError("'+' needs a command on each side")
            groups.append([])
        else:
            groups[-1].append(arg)
    if not groups[-1]:
        raise RemoteError("'+' needs a command on each side")
    return groups


# ---------------------------------------------------------------- XMODEM
class Xmodem:
    """Byte pipe with read(n, timeout) -> bytes and write(bytes)."""

    def __init__(self, read, write):
        self._read = read
        self._write = write

    def send(self, data: bytes, block=1024):
        deadline = time.time() + 60
        use_crc = None
        while time.time() < deadline:
            ch = self._read(1, 1.0)
            if ch == b"C":
                use_crc = True
                break
            if ch == bytes([NAK]):
                use_crc = False
                block = 128
                break
            if ch == bytes([CAN]):
                raise RemoteError("board cancelled the transfer")
        if use_crc is None:
            raise RemoteError("timed out waiting for the board to accept XMODEM")

        seq = 1
        off = 0
        while off < len(data):
            chunk = data[off:off + block].ljust(block, bytes([SUB]))
            self._packet(seq, chunk, use_crc)
            off += block
            seq = (seq + 1) & 0xFF
        self._eot()

    def recv(self, size: int) -> bytes:
        """Receive a file. The board waits for 'C'. Extra packet padding is dropped."""
        got = bytearray()
        expect = 1
        use_crc = True
        ch = b""
        for _ in range(60):
            self._write(b"C")
            ch = self._read(1, 1.0)
            if ch:
                break
        else:
            raise RemoteError("timed out waiting for the board to send")

        while True:
            if not ch:
                ch = self._read(1, 10.0)
            if not ch:
                raise RemoteError("timed out waiting for the board to send")
            if ch == bytes([EOT]):
                self._write(bytes([ACK]))
                break
            if ch == bytes([CAN]):
                raise RemoteError("board cancelled the transfer")
            if ch not in (bytes([SOH]), bytes([STX])):
                ch = b""
                continue
            length = 1024 if ch == bytes([STX]) else 128
            hdr = self._read_exact(2 + length + (2 if use_crc else 1))
            seq, nseq = hdr[0], hdr[1]
            payload = hdr[2:2 + length]
            bad = ((seq + nseq) & 0xFF) != 0xFF
            if use_crc and crc16(payload) != int.from_bytes(hdr[-2:], "big"):
                bad = True
            if bad:
                self._write(bytes([NAK]))
                ch = b""
                continue
            if seq == expect:
                got.extend(payload)
                expect = (expect + 1) & 0xFF
            self._write(bytes([ACK]))
            ch = b""
        return bytes(got[:size])

    def _packet(self, seq, payload, use_crc):
        header = bytes([STX if len(payload) == 1024 else SOH, seq & 0xFF, (~seq) & 0xFF])
        check = crc16(payload).to_bytes(2, "big") if use_crc else bytes([sum(payload) & 0xFF])
        for _ in range(10):
            self._write(header + payload + check)
            reply = self._read(1, 10.0)
            if reply == bytes([ACK]):
                return
            if reply == bytes([CAN]):
                raise RemoteError("board cancelled the transfer")
        raise RemoteError(f"packet {seq} was not acknowledged")

    def _eot(self):
        for _ in range(10):
            self._write(bytes([EOT]))
            if self._read(1, 10.0) == bytes([ACK]):
                return
        raise RemoteError("board did not acknowledge the end of the transfer")

    def _read_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self._read(n - len(buf), 10.0)
            if not chunk:
                raise RemoteError("timed out in the middle of a packet")
            buf.extend(chunk)
        return bytes(buf)


# ---------------------------------------------------------------- board
class Board:
    def __init__(self, port, baud=BAUD):
        if serial is None:
            raise RemoteError("pyserial is required: pip install pyserial")
        self.port = serial.Serial(port, baud, timeout=0.05)
        self.port.reset_input_buffer()
        self._pending = b""

    def close(self):
        self.port.close()

    def write(self, data: bytes):
        self.port.write(data)
        self.port.flush()

    def read(self, n, timeout):
        if self._pending:
            take = self._pending[:n]
            self._pending = self._pending[n:]
            return take
        deadline = time.time() + timeout
        buf = bytearray()
        while len(buf) < n and time.time() < deadline:
            left = deadline - time.time()
            self.port.timeout = max(left, 0.01)
            chunk = self.port.read(n - len(buf))
            if chunk:
                buf.extend(chunk)
        return bytes(buf)

    def pushback(self, data: bytes):
        self._pending = data + self._pending

    def sync(self):
        """Reach a fresh prompt. Ctrl-C abandons a half-typed block and
        stops a program the console is running."""
        self.write(b"\x03")
        text = self._read_until_prompt(5)
        if "freya:" not in text:
            raise RemoteError("no Freya prompt on this port")
        return text

    def run(self, line: str, timeout=60) -> str:
        """Run one shell line. Return what it printed. Raise on a bad status."""
        nonce = secrets.token_hex(3)
        marker = f"FREM{nonce}"
        self.write(b"\x15")
        self.write(f"{line}; echo {marker} $?\r".encode())
        raw = self._read_until(marker.encode(), timeout)
        status, _tail = self._status_after(raw, marker)
        body = self._strip_echo(raw, marker)
        if status != 0:
            detail = body.strip() or f"status {status}"
            raise RemoteError(detail)
        return body

    def begin_transfer(self, line: str, timeout=30):
        """Start download/upload. Return (nonce marker, text so far)."""
        nonce = secrets.token_hex(3)
        marker = f"FREM{nonce}"
        self.write(b"\x15")
        self.write(f"{line}; echo {marker} $?\r".encode())
        raw = self._read_until(b"Ready to ", timeout)
        extra = self._read_until(b"now", 5)
        return marker, raw + extra

    def finish_transfer(self, marker: str, timeout=30):
        raw = self._read_until(marker.encode(), timeout)
        status, _tail = self._status_after(raw, marker)
        if status != 0:
            raise RemoteError(self._strip_echo(raw, marker).strip() or f"status {status}")

    def _read_until(self, needle: bytes, timeout) -> bytes:
        buf = bytearray(self._pending)
        self._pending = b""
        deadline = time.time() + timeout
        while needle not in buf and time.time() < deadline:
            self.port.timeout = 0.2
            chunk = self.port.read(256)
            if chunk:
                buf.extend(chunk)
        idx = buf.find(needle)
        if idx < 0:
            raise RemoteError("timed out waiting for the board")
        end = idx + len(needle)
        self._pending = bytes(buf[end:])
        return bytes(buf[:end])

    def _read_until_prompt(self, timeout) -> str:
        buf = bytearray(self._pending)
        self._pending = b""
        deadline = time.time() + timeout
        while True:
            text = bytes(buf).decode("latin1")
            found = _PROMPT.search(text)
            if found:
                self._pending = text[found.end():].encode("latin1")
                return text[:found.end()]
            if time.time() >= deadline:
                return text
            self.port.timeout = 0.2
            chunk = self.port.read(64)
            if chunk:
                buf.extend(chunk)

    def _status_after(self, raw: bytes, marker: str):
        text = raw.decode("latin1")
        m = re.search(re.escape(marker) + r" (\d+)", text)
        if not m:
            raise RemoteError("board did not report a status")
        # Keep reading through the prompt so the next command starts clean.
        if not _PROMPT.search(text):
            text += self._read_until_prompt(5)
        return int(m.group(1)), text

    def _strip_echo(self, raw: bytes, marker: str) -> str:
        text = raw.decode("latin1").replace("\r\n", "\n").replace("\r", "\n")
        # Drop the echoed command line.
        nl = text.find("\n")
        if nl >= 0:
            text = text[nl + 1:]
        cut = text.find(marker)
        if cut >= 0:
            text = text[:cut]
        if text.endswith("\n"):
            # The marker was on its own line; leave the command's own newline.
            pass
        return text


def xmodem_of(board: Board) -> Xmodem:
    return Xmodem(board.read, board.write)


# ---------------------------------------------------------------- filesystem
def remote_isdir(board: Board, path: str) -> bool:
    try:
        board.run(f"ls -l {quote_path(path)}")
        return True
    except RemoteError:
        return False


def join_remote(directory: str, name: str) -> str:
    if directory in (".", ""):
        return name
    if directory.endswith("/"):
        return directory + name
    return directory + "/" + name


def put_file(board: Board, local: str, remote: str):
    with open(local, "rb") as fh:
        data = fh.read()
    marker, _pre = board.begin_transfer(
        f"download {quote_path(remote)} --size {len(data)}")
    xmodem_of(board).send(data)
    board.finish_transfer(marker)
    print(f"copied {local} -> :{remote} ({len(data)} bytes)")


def get_file(board: Board, remote: str, local: str):
    marker, pre = board.begin_transfer(f"upload {quote_path(remote)}")
    m = _READY_SEND.search(pre.decode("latin1"))
    if not m:
        raise RemoteError("board did not say how big the file is")
    size = int(m.group(1))
    data = xmodem_of(board).recv(size)
    board.finish_transfer(marker)
    if len(data) != size:
        raise RemoteError(f"short read: got {len(data)} of {size} bytes")
    with open(local, "wb") as fh:
        fh.write(data)
    print(f"copied :{remote} -> {local} ({size} bytes)")


def copy_tree(board: Board, src_remote, src_local, dst_remote, dst_local, recursive):
    """Copy one path. Exactly one of each src/dst pair is set."""
    if src_remote and dst_remote:
        import tempfile
        name = os.path.basename(src_remote.rstrip("/")) or "file"
        fd, tmp = tempfile.mkstemp(prefix="fremote-")
        os.close(fd)
        try:
            if remote_isdir(board, src_remote):
                if not recursive:
                    raise RemoteError(f"{src_remote} is a directory (pass -r)")
                _copy_remote_dir(board, src_remote, dst_remote)
            else:
                get_file(board, src_remote, tmp)
                put_file(board, tmp, dst_remote)
        finally:
            os.remove(tmp)
        return

    if src_local and dst_remote:
        if os.path.isdir(src_local):
            if not recursive:
                raise RemoteError(f"{src_local} is a directory (pass -r)")
            board.run(f"mkdir {quote_path(dst_remote)}")
            for name in sorted(os.listdir(src_local)):
                copy_tree(board, None, os.path.join(src_local, name),
                          join_remote(dst_remote, name), None, True)
        else:
            put_file(board, src_local, dst_remote)
        return

    if src_remote and dst_local:
        if remote_isdir(board, src_remote):
            if not recursive:
                raise RemoteError(f"{src_remote} is a directory (pass -r)")
            os.makedirs(dst_local, exist_ok=True)
            listing = board.run(f"ls -l {quote_path(src_remote)}")
            for name, is_dir, _size in parse_ll(listing):
                copy_tree(board, join_remote(src_remote, name), None,
                          None, os.path.join(dst_local, name), True)
        else:
            parent = os.path.dirname(dst_local)
            if parent:
                os.makedirs(parent, exist_ok=True)
            get_file(board, src_remote, dst_local)
        return

    raise RemoteError("fs cp needs a source and a destination")


def _copy_remote_dir(board, src, dst):
    board.run(f"mkdir {quote_path(dst)}")
    listing = board.run(f"ls -l {quote_path(src)}")
    for name, is_dir, _size in parse_ll(listing):
        child_s = join_remote(src, name)
        child_d = join_remote(dst, name)
        if is_dir:
            _copy_remote_dir(board, child_s, child_d)
        else:
            import tempfile
            fd, tmp = tempfile.mkstemp(prefix="fremote-")
            os.close(fd)
            try:
                get_file(board, child_s, tmp)
                put_file(board, tmp, child_d)
            finally:
                os.remove(tmp)


def cmd_fs(board: Board, args):
    if not args:
        raise RemoteError("usage: fs <ls|cat|cp|rm|mkdir|mv|df|cd|pwd> ...")
    op, rest = args[0], args[1:]
    if op == "ls":
        long_fmt = False
        if rest and rest[0] == "-l":
            long_fmt = True
            rest = rest[1:]
        path = rest[0] if rest else ":"
        _on, p = parse_remote(path if path.startswith(":") else ":" + path)
        flag = "-l " if long_fmt else ""
        print(board.run(f"ls {flag}{quote_path(p)}").rstrip())
        return
    if op == "cat":
        if len(rest) != 1:
            raise RemoteError("usage: fs cat <file>")
        _on, p = parse_remote(rest[0] if rest[0].startswith(":") else ":" + rest[0])
        marker, pre = board.begin_transfer(f"upload {quote_path(p)}")
        m = _READY_SEND.search(pre.decode("latin1"))
        if not m:
            raise RemoteError("board did not say how big the file is")
        data = xmodem_of(board).recv(int(m.group(1)))
        board.finish_transfer(marker)
        sys.stdout.buffer.write(data)
        return
    if op == "mkdir":
        if not rest:
            raise RemoteError("usage: fs mkdir <dir>...")
        for item in rest:
            _on, p = parse_remote(item if item.startswith(":") else ":" + item)
            print(board.run(f"mkdir {quote_path(p)}").rstrip())
        return
    if op == "rm":
        flags = []
        paths = []
        for item in rest:
            if item in ("-r", "-rf"):
                flags.append("-r")
            else:
                paths.append(item)
        if not paths:
            raise RemoteError("usage: fs rm [-r] <path>...")
        for item in paths:
            _on, p = parse_remote(item if item.startswith(":") else ":" + item)
            opt = "-r " if flags else ""
            print(board.run(f"rm {opt}{quote_path(p)}").rstrip())
        return
    if op == "mv":
        if len(rest) != 2:
            raise RemoteError("usage: fs mv <old> <new>")
        ends = []
        for item in rest:
            _on, p = parse_remote(item if item.startswith(":") else ":" + item)
            ends.append(p)
        print(board.run(f"rename {quote_path(ends[0])} {quote_path(ends[1])}").rstrip())
        return
    if op == "df":
        print(board.run("df").rstrip())
        return
    if op == "pwd":
        print(board.run("pwd").rstrip())
        return
    if op == "cd":
        path = rest[0] if rest else "/"
        _on, p = parse_remote(path if path.startswith(":") else ":" + path)
        board.run(f"cd {quote_path(p)}")
        print(board.run("pwd").rstrip())
        return
    if op == "cp":
        recursive = False
        items = []
        for item in rest:
            if item == "-r":
                recursive = True
            else:
                items.append(item)
        if len(items) < 2:
            raise RemoteError("usage: fs cp [-r] <src>... <dst>")
        sources, dest = items[:-1], items[-1]
        dest_remote, dest_path = parse_remote(dest)
        if len(sources) > 1 and not dest_remote and not os.path.isdir(dest_path):
            raise RemoteError("copying several files requires a directory destination")
        if len(sources) > 1 and dest_remote and not remote_isdir(board, dest_path):
            raise RemoteError("copying several files requires a directory destination")
        for src in sources:
            src_remote, src_path = parse_remote(src)
            if not src_remote and not dest_remote:
                raise RemoteError("fs cp copies to or from the board; both paths are local")
            name = os.path.basename(src_path.rstrip("/")) or "file"
            if src_remote and dest_remote:
                target = join_remote(dest_path, name) if len(sources) > 1 else dest_path
                copy_tree(board, src_path, None, target, None, recursive)
            elif src_remote:
                target = os.path.join(dest_path, name) if os.path.isdir(dest_path) else dest_path
                copy_tree(board, src_path, None, None, target, recursive)
            else:
                target = join_remote(dest_path, name) if (len(sources) > 1 or remote_isdir(board, dest_path)) else dest_path
                copy_tree(board, None, src_path, target, None, recursive)
        return
    raise RemoteError(f"unknown fs command {op}")


def cmd_repl(board: Board):
    import select
    import termios
    import tty

    if not sys.stdin.isatty():
        raise RemoteError("repl needs a terminal")
    board.sync()
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    print("remote shell, Ctrl-X exits", file=sys.stderr)
    try:
        tty.setraw(fd)
        while True:
            r, _, _ = select.select([fd, board.port], [], [])
            if fd in r:
                data = os.read(fd, 64)
                if b"\x18" in data:          # Ctrl-X, same exit as mpremote
                    break
                board.write(data)
            if board.port in r:
                data = board.port.read(board.port.in_waiting or 1)
                if data:
                    os.write(sys.stdout.fileno(), data)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        print(file=sys.stderr)


def dispatch(board, args):
    if not args or args[0] in ("repl", "shell"):
        cmd_repl(board)
        return
    op, rest = args[0], args[1:]
    if op == "exec":
        if not rest:
            raise RemoteError("usage: exec <command>")
        sys.stdout.write(board.run(" ".join(rest)))
        return
    if op == "reset":
        board.write(b"\x15reboot\r")
        print("rebooting")
        return
    if op == "fs":
        cmd_fs(board, rest)
        return
    if op == "connect":
        raise RemoteError("connect belongs before the other commands")
    raise RemoteError(f"unknown command {op}")


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    port = None
    baud = BAUD
    commands = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        if a in ("-p", "--port"):
            i += 1
            if i >= len(argv):
                raise RemoteError("--port needs a device")
            port = argv[i]
        elif a in ("-b", "--baud"):
            i += 1
            baud = int(argv[i])
        elif a == "connect":
            i += 1
            if i >= len(argv):
                raise RemoteError("connect needs a device")
            port = argv[i]
        elif re.fullmatch(r"[uacUAC]\d+", a) and not commands:
            port = a
        else:
            commands = argv[i:]
            break
        i += 1

    if port is None:
        port = default_port()
    else:
        port = resolve_port(port)

    groups = split_chain(commands) if commands else [[]]
    board = Board(port, baud)
    try:
        board.sync()
        for group in groups:
            dispatch(board, group)
    finally:
        board.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RemoteError as exc:
        print(f"fremote: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
