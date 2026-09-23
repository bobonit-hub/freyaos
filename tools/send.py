#!/usr/bin/env python3
"""
XMODEM-1K sender for Freya's 'download' command.

Useful when lrzsz (sx) is not installed.  Run Freya's download command
first, then point this script at the same serial port:

    freya:/> download hello.bin
    $ python3 tools/send.py /dev/ttyUSB0 build/apps/hello.bin

The Altair menu receives the same stream into the 8080's memory.
`upload` takes a raw image, `upload hex` an Intel HEX file, and then
the same command.  `--block 128` is the fallback when the board cannot
spare a buffer for a 1K packet.

Requires pyserial (pip install pyserial).
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

SOH, STX, EOT, ACK, NAK, CAN, SUB = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x1A


def crc16(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def wait_handshake(port, timeout):
    """Freya sends 'C' for CRC mode, or NAK if it fell back to checksums."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ch = port.read(1)
        if ch == b"C":
            return True
        if ch and ch[0] == NAK:
            return False
    sys.exit("timed out waiting for the receiver - is 'download' running?")


def send_packet(port, seq, payload, use_crc, retries=10):
    size = len(payload)
    header = bytes([STX if size == 1024 else SOH, seq & 0xFF, ~seq & 0xFF])
    if use_crc:
        check = crc16(payload).to_bytes(2, "big")
    else:
        check = bytes([sum(payload) & 0xFF])

    for _ in range(retries):
        port.write(header + payload + check)
        port.flush()
        reply = port.read(1)
        if reply and reply[0] == ACK:
            return
        if reply and reply[0] == CAN:
            sys.exit("receiver cancelled the transfer")
    sys.exit(f"packet {seq} was not acknowledged after {retries} tries")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyUSB0")
    ap.add_argument("file", help="file to send")
    ap.add_argument("-b", "--baud", type=int, default=921600)
    ap.add_argument("-k", "--block", type=int, choices=(128, 1024), default=1024,
                    help="packet payload size (default 1024)")
    ap.add_argument("-t", "--timeout", type=float, default=60.0,
                    help="seconds to wait for the receiver")
    args = ap.parse_args()

    with open(args.file, "rb") as fh:
        data = fh.read()

    with serial.Serial(args.port, args.baud, timeout=2) as port:
        port.reset_input_buffer()
        print(f"waiting for Freya ... ", end="", flush=True)
        use_crc = wait_handshake(port, args.timeout)
        print("CRC mode" if use_crc else "checksum mode")

        total = (len(data) + args.block - 1) // args.block
        for index in range(total):
            chunk = data[index * args.block:(index + 1) * args.block]
            chunk = chunk.ljust(args.block, bytes([SUB]))
            send_packet(port, index + 1, chunk, use_crc)
            done = (index + 1) * 100 // total
            print(f"\r  {index + 1}/{total} packets ({done}%)", end="", flush=True)

        print()
        for _ in range(10):
            port.write(bytes([EOT]))
            port.flush()
            reply = port.read(1)
            if reply and reply[0] == ACK:
                break
        print(f"sent {len(data)} bytes in {total} packets")


if __name__ == "__main__":
    main()
