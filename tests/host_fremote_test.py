#!/usr/bin/env python3
"""Host checks for tools/fremote.py that do not need a board."""
import importlib.util
import os
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("fremote", os.path.join(ROOT, "tools", "fremote.py"))
fremote = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fremote)

fails = 0


def check(cond, what):
    global fails
    if cond:
        print(f"  ok    {what}")
    else:
        print(f"  FAIL  {what}")
        fails += 1


class Pipe:
    def __init__(self):
        self.buf = bytearray()
        self.cv = threading.Condition()

    def write(self, data):
        with self.cv:
            self.buf.extend(data)
            self.cv.notify_all()

    def read(self, n, timeout):
        with self.cv:
            end = time.time() + timeout
            while not self.buf and time.time() < end:
                self.cv.wait(max(0.0, end - time.time()))
            take = bytes(self.buf[:n])
            del self.buf[:n]
            return take


class FakePort:
    def __init__(self, data):
        self.data = bytearray(data)
        self.written = bytearray()
        self.timeout = 0

    def write(self, data):
        self.written.extend(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.data.clear()

    def close(self):
        pass

    def read(self, n):
        take = bytes(self.data[:n])
        del self.data[:n]
        return take


def test_parse():
    print("\npaths, listings, command chain")
    check(fremote.resolve_port("u0") == "/dev/ttyUSB0", "u0 is ttyUSB0")
    check(fremote.resolve_port("a1") == "/dev/ttyACM1", "a1 is ttyACM1")
    check(fremote.resolve_port("c3") == "COM3", "c3 is COM3")
    check(fremote.resolve_port("/dev/ttyUSB0") == "/dev/ttyUSB0", "a real path is kept")
    check(fremote.parse_remote(":notes.txt") == (True, "notes.txt"), "colon marks the card")
    check(fremote.parse_remote(":") == (True, "."), "a bare colon is the working directory")
    check(fremote.parse_remote("notes.txt") == (False, "notes.txt"), "no colon is the host")
    check(fremote.quote_path("my notes.txt") == '"my notes.txt"', "a space is quoted")
    try:
        fremote.quote_path('a"b')
        check(False, "a quote is refused")
    except fremote.RemoteError:
        check(True, "a quote is refused")
    rows = fremote.parse_ll(
        "/:\n"
        "  d---a      <DIR>  2026-09-21 20:14  apps\n"
        "  -w--a       2048  2026-09-21 20:31  notes.txt\n"
        "  1 file, 1 directory, 2.0 KiB total\n")
    check(rows == [("apps", True, 0), ("notes.txt", False, 2048)], "ll lines become names")
    groups = fremote.split_chain(["fs", "ls", "+", "exec", "led blink"])
    check(groups == [["fs", "ls"], ["exec", "led blink"]], "a plus chains commands")
    check(fremote.parse_tls_endpoint("192.0.2.1") == ("192.0.2.1", 8022),
          "TLS host uses the terminal port")
    check(fremote.parse_tls_endpoint("board.local:9443") == ("board.local", 9443),
          "TLS host may select a port")
    check(fremote.parse_tls_endpoint("[2001:db8::1]:8022") == ("2001:db8::1", 8022),
          "a bracketed IPv6 TLS endpoint is accepted")


def test_transports():
    print("\nUART and TLS transports")
    old_getpass = fremote.getpass.getpass
    fremote.getpass.getpass = lambda _prompt: "12345678"
    try:
        check(fremote.read_password() == b"12345678",
              "the hidden password prompt returns eight bytes")
    finally:
        fremote.getpass.getpass = old_getpass

    tls = fremote.TlsPort.__new__(fremote.TlsPort)
    prompts = iter((b"username: ", b"password: ", b"\r\n"))
    sent = []
    tls._read_until = lambda _marker, timeout=20: next(prompts)
    tls.write = sent.append
    tls._login(b"12345678")
    check(sent == [b"admin\n", b"12345678\n"],
          "TLS login sends admin and the configured password")

    made = []
    old_tls = fremote.TlsPort
    old_serial = fremote.serial

    class FakeTls:
        def __init__(self, endpoint, password):
            made.append(("tls", endpoint, password))

    class FakeSerialModule:
        @staticmethod
        def Serial(port, baud, timeout):
            made.append(("uart", port, baud, timeout))
            return FakePort(b"")

    fremote.TlsPort = FakeTls
    fremote.serial = FakeSerialModule
    try:
        fremote.Board("board.local", tls_password=b"12345678")
        fremote.Board("/dev/ttyUSB0")
    finally:
        fremote.TlsPort = old_tls
        fremote.serial = old_serial
    check(made[0] == ("tls", "board.local", b"12345678"),
          "TLS selection opens only the TLS transport")
    check(made[1][0] == "uart" and len(made) == 2,
          "UART selection opens only the serial transport")


def test_password_sync():
    print("\npassword-protected UART startup")

    class LockedPort(FakePort):
        def write(self, data):
            super().write(data)
            if data == b"12345678\r":
                self.data.extend(b"********\r\nok\r\nfreya:/> ")

    board = fremote.Board.__new__(fremote.Board)
    board.port = LockedPort(b"password: ")
    board._pending = b""
    board._login_password = None
    old_read_password = fremote.read_password
    asked = []
    fremote.read_password = lambda: asked.append(True) or b"12345678"
    try:
        text = board.sync()
    finally:
        fremote.read_password = old_read_password
    check(asked == [True], "UART asks for the password only when locked")
    check(board.port.written == b"\x0312345678\r",
          "UART sends the hidden password before synchronising")
    check(text.endswith("freya:/> "), "UART reaches the shell prompt after login")

    board = fremote.Board.__new__(fremote.Board)
    board.port = LockedPort(b"password: ")
    board._pending = b""
    board._login_password = b"12345678"
    fremote.read_password = lambda: (_ for _ in ()).throw(
        AssertionError("TLS password was requested twice"))
    try:
        text = board.sync()
    finally:
        fremote.read_password = old_read_password
    check(text.endswith("freya:/> "),
          "TLS reuses its hidden login password for the protected shell")


def test_transfer_start():
    print("\ntransfer framing")
    reply = (
        b'download("/x", "--size", 3); echo("FREMabcdef", $?)\r\n'
        b"Ready to receive '/x' over XMODEM.\r\n"
        b"Start the transfer on the host now (Ctrl-X twice on the host to abort).\r\n"
        b"C"
    )
    board = fremote.Board.__new__(fremote.Board)
    board.port = FakePort(reply)
    board._pending = b""
    old_token_hex = fremote.secrets.token_hex
    fremote.secrets.token_hex = lambda _n: "abcdef"
    try:
        marker, pre = board.begin_transfer('download("/x", "--size", 3)')
    finally:
        fremote.secrets.token_hex = old_token_hex
    check(marker == "FREMabcdef", "transfer marker is retained")
    check(pre.endswith(b"abort).\r\n"), "both informational lines are consumed")
    check(board.read(1, 0) == b"C", "the real CRC handshake remains unread")


def test_xmodem():
    print("\nXMODEM both ways, including a trailing 0x1A")
    payload = bytes([i & 0xFF for i in range(2500)]) + bytes([0x1A])
    for label, data in (("2501 bytes", payload), ("empty", b"")):
        left, right = Pipe(), Pipe()
        sender = fremote.Xmodem(left.read, right.write)
        recvr = fremote.Xmodem(right.read, left.write)
        box = {}

        def go():
            try:
                sender.send(data)
            except Exception as exc:
                box["err"] = exc

        th = threading.Thread(target=go)
        th.start()
        got = recvr.recv(len(data))
        th.join(5)
        check("err" not in box and got == data and not th.is_alive(), label)


def main():
    test_parse()
    test_transports()
    test_password_sync()
    test_transfer_start()
    test_xmodem()
    print(f"\n{fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
