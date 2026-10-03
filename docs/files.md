# Files and the card

What Freya keeps on the card, and how to get files onto it and off it: a card
reader, XMODEM over the console with `download` and `upload`,
`tools/send.py`, and `tools/fremote.py`.

## The filesystem

Freya reads SD and SDHC cards over SPI, and its FAT16 / FAT32 implementation
reads *and* writes: files, directories, long file names, MBR partitions. The
card is ordinary FAT, so a card reader works. On the Black Pill a SPI NOR chip
on the board's footprint is mounted at `/spi1` as LittleFS, with the same file
calls ([hardware.md](hardware.md#sd-card-and-spi-flash)). What the FAT code
does not do is in [limits.md](limits.md).

## XMODEM over the console

Freya receives files over the console with XMODEM / XMODEM-1K. Start the
receiver on Freya and then send from the host:

```
freya: download("hello.bin")
Ready to receive 'hello.bin' over XMODEM.
```

```sh
stty -F /dev/ttyUSB0 921600 raw -echo -crtscts             # sx uses the line as it finds it
sx -k build/apps/hello.bin < /dev/ttyUSB0 > /dev/ttyUSB0   # lrzsz
python3 tools/send.py /dev/ttyUSB0 build/apps/hello.bin    # no lrzsz needed, sets the rate itself
```

minicom, Tera Term and ExtraPuTTY can send XMODEM from their menus. Because
XMODEM has no length field, the sender pads the last packet; Freya strips that
padding, and `--raw` keeps it if you ever need the padded stream verbatim.
`upload("file")` sends a file the other way.

## fremote.py

`tools/fremote.py` is the other way round: it opens either the UART console
or the ESP32-C6 TLS console and drives the shell, in the same shape as
MicroPython's `mpremote`. A path with a leading `:` is on the card.

```sh
python3 tools/fremote.py                          # shell; Ctrl-X leaves it
python3 tools/fremote.py u0 fs ls :/
python3 tools/fremote.py --tls 192.0.2.10         # TLS shell; password is hidden
python3 tools/fremote.py --tls freya.local fs ls :/
python3 tools/fremote.py fs cp build/apps/hello.bin :/hello.bin
python3 tools/fremote.py fs cp :/notes.txt .
python3 tools/fremote.py exec "led blink" + fs df
```

`fs cp` and `fs cat` use `download --size` and `upload`, so the copy is the
same bytes as the file, including a trailing 0x1A. `fs ls`, `fs rm`,
`fs mkdir`, `fs df` and `fs cd` are the shell commands of the
same name. `fs mv` is the shell's `rename`. `u0` is `/dev/ttyUSB0`, `a0` is
`/dev/ttyACM0` and `c3` is `COM3`. With no port, the only USB serial device is
used. `--tls HOST` connects to port 8022 (`HOST:PORT` overrides it), prompts
for the eight-byte terminal password without echoing it, and verifies TLS 1.3
against the Freya certificate in `coprocessor/esp32c6/certs/freya.crt`. Set
the password first with `password("xxxxxxxx")` on the UART console. Later
UART connections also detect the board's `password:` request and prompt
locally without echoing the password. A TLS invocation opens only the TLS
connection; it does not open or mirror a UART. `--tls` and a UART port cannot
be selected together. The TLS terminal itself is described in
[network.md](network.md#terminal).
