# Compression

A program can compress and decompress with heatshrink, the LZSS coder
written for small embedded systems. The stream is the one the host
`heatshrink` tool writes with `-w 8 -l 4`, so a file packed on a PC
unpacks on the board and the other way round. The console drives the
same code with `compress` and `decompress`.

The Blue Pill has no room for the coder: there both calls return
`FREYA_ERR_UNSUPPORTED` and the two commands say so. The Black Pill and
the STM32F405 have it.

`samples/compress` checks a round trip of a built-in text, or packs and
unpacks a file on the card. Build it with `make` and run
`run("compress.bin")`.

## Calls

```c
int (*compress)(const void *in, int in_len, void *out, int out_cap);
int (*decompress)(const void *in, int in_len, void *out, int out_cap);
```

Both take a whole buffer and return the number of bytes written to
`out`, or an error below zero. The caller owns both buffers; nothing is
kept between calls.

```c
char text[] = "tick tock tick tock tick tock tick tock";
uint8_t packed[FREYA_COMPRESS_BOUND(sizeof text)];
char back[sizeof text];
int n, m;

n = api->compress(text, sizeof text, packed, sizeof packed);
m = api->decompress(packed, n, back, sizeof back);
/* m == sizeof text and back is text */
```

`FREYA_COMPRESS_BOUND(n)` is the most `compress()` writes for `n` input
bytes: `n + (n + 7) / 8`, since a byte that finds nothing to refer back
to costs nine bits. An output buffer that large never runs out. There is
no such bound for `decompress()`: the caller knows how long the original
was, or tries a larger buffer when the call returns `FREYA_ERR_ARG`.

* `FREYA_ERR_ARG`: a length below zero, a null pointer with a length
  above zero, `in` and `out` overlapping, or `out` too small for the
  result. Nothing useful is in `out` after that.
* `FREYA_ERR_IO`: `decompress()` was handed bytes that are not a
  heatshrink stream, or a stream that stops short.
* `FREYA_ERR_BUSY`: the heap had no room for the coder's state, 526
  bytes to compress and 302 to decompress. It is freed before the call
  returns.
* `FREYA_ERR_HANDLER`: the call came from a pin or timer handler. The
  coder allocates and runs for a while, so it is refused there like the
  file calls are.
* `FREYA_ERR_UNSUPPORTED`: the board has no coder.

A length of zero is allowed: compressing nothing gives nothing, and so
does decompressing nothing.

These calls were appended to the service table. A program built against
this header and handed an older kernel checks before it calls:

```c
if (!FREYA_API_HAS(api, decompress)) {
    api->puts("this kernel has no coder\r\n");
    return FREYA_EXIT_FAIL;
}
```

## What a call does

heatshrink is LZSS: the output is a bit stream of literals, each a `0`
bit and eight bits of data, and back references, each a `1` bit, an
index into the bytes just written and a count. The window is
`FREYA_COMPRESS_WINDOW_BITS` (8), so a reference reaches back 256 bytes
and costs 8 bits of index; the lookahead is
`FREYA_COMPRESS_LOOKAHEAD_BITS` (4), so one reference copies at most 16
bytes and costs 4 bits of count. A reference is 13 bits. Prose shrinks
to about 70%, text made of repeated words to a fifth or less, and random
bytes grow by an eighth. The stream has no header, no length and no checksum, which is
why a wrong buffer is only ever caught as `FREYA_ERR_IO` or by a wrong
length coming back, and why the caller records the original size if it
needs it.

The kernel runs the coder in `src/lz.c` as a stream: `lz_open()`,
`lz_sink()`, `lz_poll()`, `lz_finish()`, `lz_close()`. The service
calls above feed a whole buffer through that stream; the console
commands feed a file through it 128 bytes at a time, so a file of any
size fits. The library is `third_party/heatshrink`, version 0.4.1, ISC
licensed, compiled with `src/heatshrink_config.h`.

## The commands

```
freya: compress
heatshrink LZSS, 8-bit window, 4-bit lookahead
usage: compress <in> <out>
freya: compress("/notes.txt", "/notes.hs")
compress: 2048 -> 1433 B (69%)
freya: decompress("/notes.hs", "/notes.txt")
decompress: 1433 -> 2048 B
```

With no arguments either command names the coder, prints its usage and
succeeds. Otherwise both take two paths, read the first and create or
truncate the second; the two have to name different files. The number
after `compress` is the packed size as a percentage of the original.
Ctrl-C stops the copy and leaves whatever was already written. A file
that is not a heatshrink stream fails with `bad stream`.
