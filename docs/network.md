# ESP32-C6 networking

Every board but the Blue Pill can use an ESP32-C6 as a network
coprocessor, on SPI2 with the same pins. The C6 owns Wi-Fi credentials, association, DHCP, DNS, ICMP and
the TCP/IP stack. Freya does not contain lwIP.

## Connection

Use the following bench-tested pinout with 3.3 V logic and a common ground:

- PB13 SCK → C6 GPIO6
- PB14 MISO ← C6 GPIO2
- PB15 MOSI → C6 GPIO7
- PB12 CS → C6 GPIO14
- PB10 READY ← C6 GPIO4

Build and flash the Rust firmware described in
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

## Remote syslog

Every line the file log keeps (`klog`, the shell's `log()`, a program's
`api->log`, a program exit) can also go to one syslog server over UDP.
There is no TCP or TLS transport.

```text
syslog("server", "192.168.1.10")         # port 514
syslog("server", "192.168.1.10", 5514)
syslog("on")
syslog()
remote syslog is on, server 192.168.1.10:5514 (UDP, settings at 0x0800c020)
syslog("off")
```

The server is an IPv4 address; a hostname is refused, so no DNS lookup
ever delays a log line. The on flag, the address and the port are system
settings, stored in both copies next to the auto-start flag. They take
the 12 bytes after each copy's checksum word, which that checksum has
always covered, so settings written by an older kernel stay valid and read
remote syslog as off. A kernel-only `make flash` keeps them, like the
password.

Each line is one RFC 3164 datagram,
`<PRI>Mmm dd hh:mm:ss freya freya: message`. The facility is user (1).
The severity follows the log level: error 3, warn 4, info 6, debug 7.
The time is Freya's clock, not UTC unless the clock is set to UTC. The
log level filters the remote copy too: a line `loglevel` drops is not
sent either.

Lines are sent only while the C6 transport is open (`wifi("on")`). Before
that, and while the C6 is not associated, they are not sent. A program's
request to the C6 that is waiting for its reply is left alone. A line
logged meanwhile waits in a four-line queue and is sent on the next log
line or the next console poll, and a full queue drops its oldest line. A
line logged from an interrupt handler is not sent. UDP gives no delivery
guarantee and Freya does not retry.

The C6 sends from a UDP socket of its own (`ESP_OP_SYSLOG`). It does not
use one of the four program sockets, and `wifi("off")` closes it. A C6
firmware older than this op refuses the request, and the line is dropped.

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

`fremote.py` is the normal interactive client. Set the eight-character
password once through UART, then connect by C6 address:

```sh
python3 tools/fremote.py u0 exec 'password("12345678")'
python3 tools/fremote.py --tls <c6-address>
python3 tools/fremote.py --tls <c6-address> exec "wifi status"
```

The TLS form prompts for the password without echoing it, requires TLS 1.3,
and verifies the server with `coprocessor/esp32c6/certs/freya.crt`. It opens
the network console only; it neither opens nor mirrors the UART transport.
The shell, `exec`, command chaining, and `fs` operations use the selected
transport in the same way. On a later password-protected UART connection,
`fremote.py` detects the board's login request and uses the same hidden local
prompt instead of waiting indefinitely for the shell prompt.

## Web server

The same certificate serves HTTPS on port 443. TLS 1.3 is required, and
nothing listens for cleartext HTTP. The C6 reads one request at a time
and asks the STM32 for the resource. GET, HEAD and POST are the methods.
A POST carries a `Content-Length` of at most 1 MiB (`FREYA_WEB_BODY_MAX`);
chunked bodies are refused with 411 and larger ones with 413. The C6 keeps
8 KiB of the body at a time and reads more as the STM32 takes it, so a
long upload runs at the pace of the file write on the STM32.

A program takes a request with `api->web_take()`, which fills
`freya_web_req_t` with the method, the path and the query. For a POST it
then reads the body with `api->web_read(buf, max, &left)`, at most
`FREYA_WEB_READ_MAX` (480) bytes a call, until the call returns 0; `left`
is how much is still to come. The response is `web_begin`, `web_body`,
`web_end` as for a GET. A body the program does not read to the end is
dropped when the response begins. `FREYA_API_HAS(api, web_read)` tells a
kernel that speaks POST from one that does not.

The file service is `samples/httpd`: a password screen tested by a shell
script with `password_check()`, then a system page from `sysinfo()` and a
firmware upload that a shell script installs. A name ending in `.sh` under
the docroot is run as a shell script and the printed text is the body.
Every other file is sent unchanged.

```text
curl --tlsv1.3 -k https://<c6-address>/
curl --tlsv1.3 -k -d 'password=12345678' https://<c6-address>/login
```
