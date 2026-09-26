# ESP32-C6 networking

Black Pill and STM32F405 builds can use an ESP32-C6 as a network
coprocessor. The C6 owns Wi-Fi credentials, association, DHCP, DNS, ICMP and
the TCP/IP stack. Freya does not contain lwIP.

## Connection

Use the following bench-tested pinout with 3.3 V logic and a common ground:

- PB13 SCK → C6 GPIO6
- PB14 MISO ← C6 GPIO2
- PB15 MOSI → C6 GPIO7
- PB12 CS → C6 GPIO14
- PB10 READY ← C6 GPIO4

Flash and copy the firmware described in
[`coprocessor/esp32c6/README.md`](../coprocessor/esp32c6/README.md). Hardware
interoperability was verified with this pinout on an ESP32-C6FH4; verify signal
integrity again if the module, wiring length or SPI clock changes.

SPI2 is reserved while Wi-Fi is on. `wifi("off")` cancels DMA, deasserts CS,
returns PB10/PB12–PB15 to inputs and makes SPI2 available to `spi()` and
`api->spi_open()` again. Blue Pill exports the same API but returns
`FREYA_ERR_UNSUPPORTED`.

## Console

```text
wifi("on")
wifi("credentials", "ssid", "password")
wifi("connect")
wifi("status")
wifi("scan")
ping("example.com", 5000)
wifi("disconnect")
wifi("off")
```

Credentials are saved in ESP32 NVS and cannot be read through RPC. The command
used to enter them remains in Freya's RAM shell history until reset.
DHCP is always used; status shows the leased address, gateway and mask.

## Program API

The network fields are appended to `freya_api_t`, so the program image ABI is
unchanged. Check availability with `FREYA_API_HAS(api, net_poll)`.

All calls are nonblocking. `FREYA_ERR_AGAIN` means call `net_poll(timeout_ms)`
and retry the same operation. Resources are bounded to four C6 sockets and a
480-byte data fragment per call. Available operations cover Wi-Fi status and
scan, asynchronous ping, and IPv4 TCP/UDP socket, close, connect, bind,
listen, accept, send/receive and datagram send/receive.

When `FREYA_API_HAS(api, net_tls_connect)` is true, TLS client sockets use
`api->net_tls_connect(socket, hostname, port)` instead
of `net_connect`. Retry it after `FREYA_ERR_AGAIN` like every other
nonblocking operation. The hostname is mandatory: the C6 uses it for SNI and
certificate-name verification against ESP-IDF's built-in CA bundle. The C6
also obtains UTC with SNTP before its first handshake.

Only TLS 1.3 is enabled for these connections. TLS 1.2 and older peers are
rejected, and the program API cannot disable certificate verification. The
shell's `curl --insecure` is an explicit per-request diagnostic exception. TLS
terminates on the ESP32-C6: keys, certificates and cryptographic state never
enter STM32 memory, while application plaintext does cross the SPI connection.
The SPI link is therefore not confidential against physical probing.
The socket API has no TLS server or DTLS calls.

```c
int s = api->net_socket(FREYA_AF_INET, FREYA_SOCK_STREAM, FREYA_IPPROTO_TCP);
while (api->net_tls_connect(s, "voice.example", 443) == FREYA_ERR_AGAIN)
    api->net_poll(20);
/* net_send/net_recv now carry plaintext through the C6's TLS 1.3 session. */
```

An application's sockets and an application-opened transport are released
when its run exits, including Ctrl-C and faults. A transport opened at the
console remains available after a program exits.

The wire protocol is a versioned 512-byte frame with magic, sequence, opcode,
payload length, signed status and CRC-32. A request and its response use
separate SPI transactions. Duplicate request sequences return a cached
response, preventing retries from repeating a send or another side effect.

## Terminal

The coprocessor listens on TCP port 8022 for one console session. TLS ends
on the C6, using the self-signed certificate shipped in its firmware. After
the handshake the client sends a line `admin` and a line with the eight-byte
Freya terminal password. The session is then the USART2 console. `password()`
on the board has to be on first; with no password stored the login is refused.

```text
openssl s_client -connect <c6-address>:8022 -tls1_3 -servername freya
```

The certificate name is `freya`. It is not from a public authority, so the
client reports a verification failure unless that check is left non-fatal.
The OpenSSH client speaks a different protocol and will not complete this
handshake.

## Web server

The same certificate serves HTTPS on port 443. TLS 1.3 is required, and
nothing listens for cleartext HTTP. The C6 reads one request at a time
and asks the STM32 for the resource. The file service is `samples/httpd`:
it maps the URL onto `/www` on the card. A name ending in `.sh` is run as
a shell script and the printed text is the body. Every other file is sent
unchanged. GET and HEAD are the only methods.

```text
curl --tlsv1.3 -k https://<c6-address>/
curl --tlsv1.3 -k https://<c6-address>/status.sh
```
