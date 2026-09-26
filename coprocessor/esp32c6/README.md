# ESP32-C6 network coprocessor

This directory contains standalone ESP-IDF and MicroPython-based coprocessor
firmware for Freya. The C6 owns Wi-Fi, DHCP, DNS, ICMP, TCP/UDP and TLS; Freya
only transports fixed 512-byte RPC frames over SPI.

## Wiring

This pinout was bench-tested with the ESP-IDF firmware on an ESP32-C6FH4.
All signals are 3.3 V. Connect grounds; do not connect either board to 5 V.

- STM32 PB13 (SPI2 SCK) → ESP32-C6 GPIO6
- STM32 PB14 (SPI2 MISO) ← ESP32-C6 GPIO2
- STM32 PB15 (SPI2 MOSI) → ESP32-C6 GPIO7
- STM32 PB12 (CS) → ESP32-C6 GPIO14
- STM32 PB10 (READY) ← ESP32-C6 GPIO4
- 3.3 V and GND in common

READY is asserted only after a DMA transaction has been queued. A command
transaction is followed by a READY-triggered fetch transaction. Responses are
cached by sequence number, so retrying after a damaged frame does not repeat a
socket write or another side effect.

## Build and flash

The firmware is an ESP-IDF 5.5.x project:

```sh
cd coprocessor/esp32c6
. /path/to/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash
```

The ESP32 NVS namespace `freya` stores SSID/password blobs. RPC never provides
a credential-read operation. Entering a password through Freya still leaves
that command in Freya's in-memory shell history.

## TLS

`api->net_tls_connect()` upgrades an unconnected TCP socket and connects it
with ESP-TLS. The firmware fixes `esp_tls_cfg_t.tls_version` to TLS 1.3,
uses ESP-IDF's full certificate bundle, requires hostname verification, and
starts asynchronous SNTP before the first handshake. TLS 1.2 fallback and
insecure verification are intentionally unavailable.

All cryptographic operations, keys, certificates and TLS state remain on the
ESP32-C6. Freya sends and receives plaintext socket data over SPI, so the
board-to-board wiring is inside the trusted boundary and is not protected
against physical probing. Application sockets are still TLS clients only.
The firmware also accepts one terminal connection on TCP port 8022 and
one HTTPS connection on port 443. Both handshakes use the self-signed
P-256 certificate in `certs/freya.crt` (the matching key is `certs/freya.key`,
compiled into the image). The terminal login is the username `admin` and
the eight-byte password stored by Freya's `password` command. The web
server is TLS 1.3 only. It parses the request and asks the STM32 file
service (`samples/httpd`) for a static file or a shell-script page.
Console bytes then cross SPI in the clear, as with every other C6 payload.

The ESP-IDF pin assignment is in `main/freya_coprocessor.c`; the MicroPython
assignment is in `freya_link.c`. Keep both files and the wiring lists above
and in `docs/network.md` synchronized if a board requires different GPIOs.

## Protocol

Every SPI transaction is 512 bytes: magic `ESP1`, version, opcode, sequence,
payload length, signed status, CRC-32 and payload. The slave always has a DMA
transaction queued. After receiving a request it prepares and queues the
response, raises READY, and clocks that response during Freya's FETCH
transaction. Duplicate sequence numbers return the cached response, so a
lost FETCH is safe to retry.

Socket descriptors and payloads are bounded to four sockets and 480 bytes.
All C6 sockets are nonblocking. WLAN and socket readiness changes raise an
event through the same READY/fetch handshake. Wi-Fi uses DHCP; no
static-address RPC is provided in this version.

The shell `curl` command uses a separate bounded HTTP job on the C6. Redirects
and chunked transfer coding are handled by ESP-IDF, gzip responses are expanded
with the C6 ROM inflater, and at most 192 KiB of response body is retained.
Certificate verification remains the default; `curl --insecure` opts out for
that request only.
