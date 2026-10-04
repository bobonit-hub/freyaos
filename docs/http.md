# The HTTP client library

`http/` is an HTTP/1.1 client in C, built on the network calls of
`freya_api.h` ([network.md](network.md)). It runs on the STM32: the
ESP32-C6 only carries the TCP connection, or the TLS 1.3 connection for
`https://`, which it terminates and verifies against ESP-IDF's
certificate bundle.

It is one source with two uses:

* The kernel compiles it into the extension, and the shell's `curl` is
  built on it ([console-commands.md](console-commands.md#network-coprocessor)).
* On every board with the ESP32-C6 link (all but the Blue Pill), `make`
  also builds it as `build/<board>/http/libfreya_http.a`, a library a
  program links. `samples/wget` is the example.

| | |
|---|---|
| Schemes | `http://` (TCP) and `https://` (TLS 1.3, verified) |
| Hosts | a name, or an IPv4 address; `user@` and IPv6 are refused |
| Methods | any token: `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, ... |
| Request | any headers, Basic authorization, a body of any length sent in pieces |
| Response | the status, every head line to a callback, `Content-Length`, a chunked or close-delimited body read in pieces |
| Not done | redirects (the caller follows them, as `curl --location` does), gzip, cookies, keep-alive, proxies |

A request is one connection: the library sends `Connection: close`. It
takes one of the four C6 sockets while it is open. Nothing is
allocated; the state is a `freya_http_t` of about 800 bytes that the
caller provides, the 480-byte I/O buffer included.

## Using it

In this tree a sample links it with two Makefile variables, as
`samples/wget` has them:

```make
SMPL_CFLAGS_mysample := $(HTTP_INC)
SMPL_LIBS_mysample   := $(HTTP_LIB)
```

Outside the tree, compile with `-Ihttp -Iinclude` and link
`libfreya_http.a`. The library calls only through the `api` it is given,
so it brings no other dependency, and it is about 3 KB of code.

```c
#include "freya_http.h"

static freya_http_t h;
static uint8_t buf[480];

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int n, status;

    http_init(&h, api);
    if (http_open(&h, "POST", "https://example.com/api") == 0 &&
        http_header(&h, "Content-Type", "application/json") == 0 &&
        http_send(&h, "{\"on\":1}", 8) == 0 &&
        (status = http_response(&h)) > 0) {
        api->printf("status %d\r\n", status);
        while ((n = http_read(&h, buf, sizeof buf)) > 0)
            ;                        /* buf[0..n) is the next of the body */
    }
    http_close(&h);
    return 0;
}
```

Wi-Fi has to be on and associated first (`wifi("on")`, `wifi("connect")`,
or `api->wifi_on()` and `api->wifi_connect()`).

## The calls

| Call | What it does |
|---|---|
| `http_init(h, api)` | clears `h`; sets `api`, `timeout_ms` (30 s), no callbacks |
| `http_open(h, method, url)` | connects and writes the request line, `Host` and `Connection` |
| `http_header(h, name, value)` | adds a header; CR, LF and other control characters are refused |
| `http_basic_auth(h, "user:password")` | adds `Authorization: Basic ...` |
| `http_send(h, body, len)` | ends the head, with `Content-Length` when `len` is not 0 and always for `POST`, `PUT` and `PATCH`; sends `body` when it is not NULL |
| `http_write(h, data, len)` | sends more of a body announced by `http_send(h, NULL, len)`, up to that length |
| `http_response(h)` | ends the head if the request had no body, reads the response head, skips `100 Continue`; returns the status |
| `http_read(h, buf, len)` | reads the body: a count, 0 at its end |
| `http_close(h)` | closes the connection; safe at any point and twice |

Every call waits until it is done, polling the link with `net_poll()`.
A negative return is a `FREYA_ERR_*`:

| Code | Meaning |
|---|---|
| `FREYA_ERR_ARG` | a bad URL, method or header, or a call out of order |
| `FREYA_ERR_TIMEOUT` | nothing moved for `timeout_ms` |
| `FREYA_ERR_IO` | the connection failed or closed early, the reply is not HTTP, the name has no address, or the call was cancelled |
| `FREYA_ERR_UNSUPPORTED` | no network on this board, or an `http://` name on a kernel without `net_resolve` |

Before `http_open()` the caller may set:

* `timeout_ms`: the longest wait without progress, 30000 by default.
* `cancel(ctx)`: called while waiting; non-zero stops the call with
  `FREYA_ERR_IO`. A program needs none: Ctrl-C already ends its waits.
* `header(ctx, line)`: called with each line of the response head,
  the status line first, without CR LF, and cut to 255 characters.
* `ctx`: passed to both.

After `http_response()`, `h.status` is the status code and `h.length`
the `Content-Length`, or -1 when the response did not give one.

## Host names

`https://` hands the host name to `net_tls_connect()`, and the C6 looks it
up itself. `http://` needs the address first. The library uses
`api->net_resolve()`, which the kernel gained with this library: the C6
answers with an `OP_RESOLVE` lookup on a task of its own, one at a time.
An IPv4 address in the URL needs no lookup, and works with an older
kernel or older C6 firmware too.
