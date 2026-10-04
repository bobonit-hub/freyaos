# ESP32-C6 network coprocessor

This directory is the coprocessor firmware for Freya, written in Rust on
ESP-IDF (`esp-idf-svc`, with the Rust standard library). The C6 owns Wi-Fi,
DHCP, DNS, ICMP, TCP/UDP and TLS; Freya only transports fixed 512-byte RPC
frames over SPI.

## Wiring

This pinout was bench-tested on an ESP32-C6FH4 with the earlier C firmware,
which the Rust firmware replaces with the same pins and wire protocol.
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

The pins are constants at the top of `src/main.rs`. Keep them, the list
above and `docs/network.md` the same if a board needs different GPIOs.

## Toolchain

- Rust from [rustup](https://rustup.rs). `rust-toolchain.toml` selects
  nightly with `rust-src`: the `riscv32imac-esp-espidf` target has no
  prebuilt standard library, so Cargo builds it (`build-std`).
- `ldproxy`, the linker wrapper: `cargo install ldproxy`.
- ESP-IDF 5.5 (tested with v5.5.5). With an ESP-IDF environment active
  (`. ~/esp-idf/export.sh`), the build uses that tree and its toolchain.
  Without one, `esp-idf-sys` downloads v5.5.5 and its tools into
  `~/.espressif` on the first build.
- libclang for `bindgen`, which generates the ESP-IDF bindings. libclang
  22 makes some newlib types opaque and the build fails in `esp-idf-sys`
  (`no field _stdin on type _reent`); point `LIBCLANG_PATH` at an older
  one, for example `export LIBCLANG_PATH=/usr/lib64/llvm20/lib64` on
  Fedora.

```sh
rustup toolchain install nightly --component rust-src
cargo install ldproxy
export PATH=$HOME/.cargo/bin:$PATH
```

The `cargo` that runs must be rustup's (`~/.cargo/bin/cargo`). A
distribution's own cargo, such as Fedora's `/usr/bin/cargo`, ignores
`rust-toolchain.toml` and the `build-std` setting, and the build fails
with ``can't find crate for `core` `` on the first dependency.
`cargo --version` should report a nightly.

## Certificate

The terminal and web servers present the self-signed P-256 certificate
`certs/freya.crt`, whose name is `freya`; `tools/fremote.py` trusts exactly
that certificate. Its private key `certs/freya.key` is compiled into the
firmware but is not in the repository (`.gitignore` keeps it out), and the
build stops if it is missing. To start over with a new pair, which every
client then has to be given:

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -days 3650 -subj /CN=freya -keyout certs/freya.key -out certs/freya.crt
```

## Build and flash

```sh
cd coprocessor/esp32c6
. ~/esp-idf/export.sh
cargo build --release
ESPPORT=/dev/ttyACM0 cargo run --release   # flash, then the serial monitor
```

The first build compiles ESP-IDF and the standard library and takes a few
minutes. `cargo run` calls `flash.sh`, which turns the ELF into an image
with `esptool.py` and writes the ESP-IDF bootloader, the partition table and
the image; `FLASH_ONLY=1` skips the monitor. The partition table is ESP-IDF's
single large app: NVS at 0x9000, then a 1.5 MiB factory app at 0x10000.
The firmware is about 1.3 MiB.

ESP-IDF options are in `sdkconfig.defaults`. ESP-IDF keeps the values of
an existing configuration, so after changing an option there run `cargo
clean` before building.

`cargo test-host` runs the unit tests of the parts that do not touch
ESP-IDF (`src/lib.rs`: frames, rings, request parsing)
on the build machine. The alias is for x86-64 Linux.

## Source

| File | Contents |
|------|----------|
| `src/main.rs` | start-up, the SPI slave and the frame exchange |
| `src/dispatch.rs` | the RPC operations: Wi-Fi, NVS credentials, sockets, TLS clients, syslog |
| `src/ping.rs` | the asynchronous ping |
| `src/dns.rs` | name lookups for plain TCP connections (`OP_RESOLVE`) |
| `src/term.rs` | the TLS terminal on port 8022 |
| `src/web.rs` | the HTTPS server on port 443 |
| `src/os.rs` | lwIP sockets, the TLS server session, tasks, the link-up flag |
| `src/lib.rs`, `src/frame.rs`, `src/ring.rs`, `src/request.rs` | the host-testable parts |

The ESP32 NVS namespace `freya` stores the SSID and password as strings
(`ssid`, `pass`). RPC never provides a credential-read operation. Entering
a password through Freya still leaves that command in Freya's in-memory
shell history. The MicroPython firmware this directory used to carry stored
them as blobs, so after moving from it set them again with
`wifi("credentials", ...)`.

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
one HTTPS connection on port 443, both with the certificate above. The
terminal login is the username `admin` and the eight-byte password stored
by Freya's `password` command. The web server is TLS 1.3 only. It parses
the request and asks the STM32 file service (`samples/httpd`) for a static
file or a shell-script page. A POST body of up to 1 MiB is passed on in
pieces of at most 480 bytes as the STM32 asks for them, through an 8 KiB
ring; `Expect: 100-continue` is answered, chunked bodies are refused with
411. Console bytes then cross SPI in the clear, as with every other C6
payload.

## Protocol

Every SPI transaction is 512 bytes: magic `ESP1`, version, opcode, sequence,
payload length, signed status, CRC-32 and payload. The slave always has a DMA
transaction queued. After receiving a request it prepares and queues the
response, raises READY, and clocks that response during Freya's FETCH
transaction. Duplicate sequence numbers return the cached response, so a
lost FETCH is safe to retry.

Socket descriptors and payloads are bounded to four sockets and 480 bytes.
Remote syslog (`OP_SYSLOG`: IPv4 address, port, then the datagram) sends
from a fifth UDP socket that only the firmware holds; `OP_WIFI_OFF` closes
it. All C6 sockets are nonblocking. WLAN and socket readiness changes raise an
event through the same READY/fetch handshake. Wi-Fi uses DHCP; no
static-address RPC is provided in this version.

`OP_RESOLVE` (a host name) looks the name up on a task of its own and
answers AGAIN until it is done, then the IPv4 address. Freya's HTTP client
(`http/` in the Freya tree, behind the shell's `curl`) uses it for `http://`
hosts; it runs on the STM32 over the ordinary sockets. The C6's own HTTP job,
ops 22 to 25, is gone, and those numbers are not reused.
