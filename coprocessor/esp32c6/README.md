# ESP32-C6 network coprocessor

This directory contains the standalone ESP-IDF C firmware used by Freya. The
C6 owns Wi-Fi, DHCP, DNS, ICMP, TCP/UDP and TLS; Freya only transports fixed
512-byte RPC frames over SPI.

## Wiring

All signals are 3.3 V. Connect grounds; do not connect either board to 5 V.

- STM32 PB13 (SPI2 SCK) → ESP32-C6 GPIO6
- STM32 PB14 (SPI2 MISO) ← ESP32-C6 GPIO2
- STM32 PB15 (SPI2 MOSI) → ESP32-C6 GPIO7
- STM32 PB12 (CS) → ESP32-C6 GPIO10
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
against physical probing. This version supports TLS clients only; accepted
server sockets and UDP remain plaintext.

The pin assignment is centralized in `freya_link.c`; change both that file and
the wiring list if a particular C6 board cannot expose these GPIOs.

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
