# WebSocket client and duplex call audio — design

Date: 2026-10-04. Status: approved in conversation, awaiting spec review.

## Goal

Two-way call audio between a Freya board's USB headset and a server, over a
WebSocket, with the codec pack's Opus. Freya is the client: it dials out to a
`ws://` or `wss://` URL (a media gateway, a PBX endpoint, a voice service, or
the host tool below).

Decided with the user:

- Freya is a WebSocket **client** only; no server role.
- On the wire, **one binary message per 20 ms Opus packet**, 16 kHz mono, the
  raw packet with no header of ours, the same in both directions.
- A sample, `samples/wsphone`, plus a host tool, `tools/wsaudio.py`, with an
  echo mode and a bridge mode (a real call with the PC's mic and speaker).
- Approach A: a new library `ws/` on top of `http/`, which does the opening
  handshake; `http/` gains one opt-in field.

Not in scope: a server role, `permessage-deflate` or any extension, UTF-8
validation of text messages, WebSocket in the ESP32-C6 firmware, a kernel
API change, a shell command.

## 1. The change to `http/`

A new public field in `freya_http_t`, set to NULL by `http_init()`:

```c
const char *upgrade;    /* "websocket" to ask for a protocol switch, or NULL */
```

When it is not NULL at `http_open()`:

- the request carries `Connection: Upgrade` and `Upgrade: <value>` in place
  of `Connection: close`;
- `http_response()` returns a `101` instead of skipping it as an interim 1xx;
  any other status is returned as now;
- after a `101` the struct is in a new state, ST_UPGRADED: `http_read()`
  returns `FREYA_ERR_ARG`, the socket stays open, and the bytes already read
  past the head stay in `io[pos..len)` for the caller to take;
- `http_close()` closes the socket as always.

With `upgrade` NULL nothing changes; `curl` is untouched. The field is
validated like a header value (no CR or LF, not empty).

## 2. The library: `ws/`

Files: `ws/freya_ws.h`, `ws/ws.c` (handshake, framing), `ws/sha1.c` (SHA-1
and base64, internal). Built as `build/<board>/ws/libfreya_ws.a` with the
programs' flags, on every board with the network link, by `make ws` and by
plain `make`, like `make http`. A program links both `libfreya_ws.a` and
`libfreya_http.a`.

### API

```c
#define FREYA_WS_TEXT     1
#define FREYA_WS_BINARY   2
#ifndef FREYA_WS_MESSAGE
#define FREYA_WS_MESSAGE  1024          /* make WS_MESSAGE= */
#endif

typedef struct freya_ws {
    freya_http_t http;          /* the handshake, then the socket and io buffer */
    const char  *protocol;      /* Sec-WebSocket-Protocol to ask for, or NULL */
    uint32_t     timeout_ms;    /* connect, one send, the close wait          */
    int          close_code;    /* the peer's close status, or 0              */
    /* the library's own: state, rx frame header and progress, the
     * message being joined, the PRNG state */
    uint8_t      msg[FREYA_WS_MESSAGE];
} freya_ws_t;                   /* about 2.3 KB */

void ws_init(freya_ws_t *ws, const freya_api_t *api);
int  ws_connect(freya_ws_t *ws, const char *url);
int  ws_send(freya_ws_t *ws, int type, const void *data, int len);
int  ws_recv(freya_ws_t *ws, int *type, const void **data);
int  ws_ping(freya_ws_t *ws);
int  ws_close(freya_ws_t *ws, int code);
```

- `ws_connect()` waits. It maps `ws://` to `http://` and `wss://` to
  `https://` and passes the URL to `http_open()` with `upgrade =
  "websocket"`, adds `Sec-WebSocket-Key` (16 random bytes, base64),
  `Sec-WebSocket-Version: 13` and, when set, `Sec-WebSocket-Protocol`. It
  requires status 101, `Upgrade: websocket`, `Connection` containing
  `Upgrade` (both case-insensitive), and `Sec-WebSocket-Accept` equal to
  base64(SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")). The headers are
  read through the `http` header callback. Bytes left in `io` after the head
  are the start of the frame stream.
- `ws_send()` waits until the frame is handed to the link. It sends one
  unfragmented frame, FIN set, masked with a fresh 4-byte key, header and
  payload built in `http.io` and sent in `FREYA_NET_PAYLOAD_MAX` pieces. Any
  `len` from 0 up is allowed.
- `ws_recv()` never waits. It moves whatever the socket has into the frame
  parser and returns the length of one whole message (with `*type` set and
  `*data` pointing into `msg`, valid until the next call), 0 when no whole
  message is there yet, or a `FREYA_ERR_*`. Fragments are joined. A ping is
  answered with a pong with the same payload; a pong is dropped. A close from
  the peer is answered with a close of the same code, `close_code` is set,
  the socket is closed and `FREYA_ERR_IO` is returned, now and on every later
  call.
- `ws_ping()` sends an empty ping.
- `ws_close()` sends a close with `code` (1000 if 0), waits up to 1 s for the
  peer's close while dropping data frames, then closes the socket. Safe at any
  point and twice.

### Randomness

There is no RNG call in the API. Masking keys and the handshake key come from
a xorshift32 generator seeded once per `ws_init()` from `ticks_ms()`, the
struct's address and the low bits of eight `adc_read(FREYA_ADC_TEMP)`
readings when the API has `adc_read`. It is not cryptographic. RFC 6455
masking only defends against confused intermediaries, which cannot see the
frames under `wss://`. The docs say this.

### Errors

As `http/`: `FREYA_ERR_ARG` for a bad URL, type or call order;
`FREYA_ERR_TIMEOUT` when the connect, a send or the close wait makes no
progress for `timeout_ms` (default 30 s, 1 s for the close wait);
`FREYA_ERR_IO` for a refused handshake, a protocol violation, a peer close or
a dropped link; `FREYA_ERR_UNSUPPORTED` without the network.

Protocol violations from the server make the client send close 1002 and close
the socket: a masked frame, a reserved bit set, an unknown opcode, a control
frame longer than 125 bytes or not FIN, a continuation with nothing to
continue, or a new data frame in the middle of a fragmented message. A message
longer than `FREYA_WS_MESSAGE` gets close 1009. A 64-bit length with the top
bit set is a violation. Text payloads are not checked for UTF-8.

### Limits

One WebSocket takes one of the C6's four sockets. `wss://` uses ESP-IDF's CA
bundle on the C6 with hostname verification, so the server needs a
certificate from a public CA. A self-signed test server works only over
`ws://`.

## 3. The sample: `samples/wsphone`

Built where `opusrec` is (USB audio and `CODECS=1`), as a flash image,
linking the codec pack, `libfreya_ws.a` and `libfreya_http.a`.

```
run("@flash", "ws://192.168.1.10:8765/")            # 24 kbit/s
run("@flash", "wss://example.org/call", "16000")    # bitrate
```

Opus: 16 kHz mono, `OPUS_APPLICATION_VOIP`, bitrate from the argument (24000
by default), complexity 3, 20 ms frames (320 samples). Encoder and decoder
come from `api->malloc()`.

```
mic ─► 10 ms timer ─► 512 ms ring ─► opus_encode ─► ws_send ─► server
spk ◄─ audio_write ◄─ jitter queue ◄─ opus_decode ◄─ ws_recv ◄─ server
```

- **Send.** A 10 ms timer moves `audio_read()` into an 8192-sample ring, as in
  `opusrec`. The main loop encodes and sends every whole 20 ms frame waiting.
  When more than 200 ms are waiting, the oldest frames are dropped and counted.
- **Receive.** `samples/wsphone/jitter.c` keeps up to 8 packets (160 ms).
  Playback starts once 3 are queued (60 ms). Whenever `audio_status()` shows
  less than 320 samples queued for the speaker, the next packet is decoded and
  written. If none is there, Opus concealment (`opus_decode(dec, NULL, 0, ...)`)
  fills up to 5 frames, then nothing is written and the queue refills to 3
  before playback resumes. A packet arriving to a full queue drops the oldest.
  The jitter queue only decides; the main loop does the Opus calls.
- **Opus calls** happen only in the main loop, never in the timer.
- **Text messages** from the server are printed to the console.
- **Ending.** A console key or Ctrl-C sends close 1000. A peer close or a lost
  link ends the run. At the end the program prints frames sent, received,
  concealed and dropped, the close code, and `audio_status()`'s underrun and
  overrun counters. The exit status is 0 after a clean close, else 1.

## 4. The host tool: `tools/wsaudio.py`

A WebSocket server on the Python standard library alone (asyncio, hashlib,
base64): the handshake, unmasking, fragments, ping/pong, close.

- `wsaudio.py echo [--port 8765] [--delay 300]` sends every message back,
  text and binary, after the delay. It does not decode Opus.
- `wsaudio.py bridge [--port 8765]` decodes the board's packets to the PC's
  speaker and encodes the PC's microphone to the board, 16 kHz mono 20 ms.
  It needs `opuslib` and `sounddevice`, imported only in this mode, with an
  install hint when they are missing.
- `--tls CERT KEY` serves `wss://`, for a certificate the C6 trusts.
- `--self-test` checks the server's framing against RFC 6455's examples.

## 5. Testing (`make test`)

1. `tests/host_ws_test.c`, against a scripted fake network in the style of
   `host_http_test.c` (random `FREYA_ERR_AGAIN`, bytes a few at a time): the
   handshake bytes; a correct, wrong and missing `Accept`; a non-101 status;
   a frame in the same read as the 101; 7-, 16- and 64-bit lengths; masking,
   checked by unmasking what was sent; fragmented messages with a ping in the
   middle (pong sent); an oversize message (1009); each violation (1002); a
   peer close (echoed, `close_code` set); `ws_recv()` returning 0 on partial
   frames; `ws_close()` at every state and twice.
2. SHA-1 against the FIPS 180 vectors ("abc", the 448-bit message, a million
   'a's) and the RFC 6455 key example (`dGhlIHNhbXBsZSBub25jZQ==` →
   `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`); base64 against RFC 4648's vectors.
3. `host_http_test` gains `upgrade` cases: the request head, a 101 returned
   with leftover bytes kept, and a 200 to an upgrade request returned as
   usual. The existing cases pass unchanged.
4. Loopback: `ws/` built for the host with a shim mapping `net_*` onto POSIX
   sockets, against `tools/wsaudio.py echo --delay 0` on a free port. It sends
   50 binary messages of sizes 0 to 1024 and one text message, checks every
   echo byte for byte, and closes with 1000. `wsaudio.py --self-test` runs
   too.
5. `tests/host_jitter_test.c` runs `samples/wsphone/jitter.c` through
   in-order, late, lost, burst and overflow sequences against the expected
   decode, conceal, wait and drop decisions.

Only a board can exercise the C6 link, real latency, and the CPU cost of Opus
encode plus decode on the F405 and U585. The docs say these are not yet
measured.

## 6. Docs and build

- `docs/websocket.md`: the API, randomness, limits, `wsphone`, `wsaudio.py`.
- `README.md`: a feature line and a documentation row.
- `docs/http.md` (`upgrade`), `docs/codecs.md` (`wsphone`),
  `docs/source-layout.md`, `docs/tests.md`, `docs/building.md` (`make ws`,
  `WS_MESSAGE=`), `RELEASE_NOTES.md`.
- `Makefile`: `WS_DIR`, `WS_LIB`, the `ws` target, `wsphone` in the codec
  samples with `SMPL_CFLAGS_wsphone` and `SMPL_LIBS_wsphone`.
- `tests/run_tests.sh`: the tests above.
