# The codec pack

`make CODECS=1` builds the codec pack: `build/<board>/codecs/libfreya_codecs.a`,
a library a program links to turn the samples of the audio calls
([audio.md](audio.md)) into what a call or a recording carries, and back.

| Codec | What it is | Typical use |
|---|---|---|
| G.711 A-law and mu-law | ITU-T G.711, one byte per sample | 8 kHz telephony, 64 kbit/s, no delay |
| Opus | libopus 1.5.2, fixed point, encoder and decoder | wideband calls at 12–32 kbit/s, recordings |
| Ogg Opus | a writer for `.opus` files (RFC 7845) | recordings any player opens |

The pack is not part of the kernel. A program that does not link it does not
carry it, and the kernel's flash map is unchanged. It is built with the
programs' compiler flags for the board, and `-O2`. `CODECS=1` builds on every
board; `samples/opusrec` is built where there is USB audio: the two STM32F405 boards, the
STM32U585 and the STM32H723.

```sh
make BOARD=stm32u585 USB=1 AUDIO=1 CODECS=1
```

## Using it

A program includes `freya_codecs.h` and links the library. In this tree that
is two variables in the Makefile, as `samples/opusrec` has them:

```make
SMPL_CFLAGS_mysample := $(CODEC_INC)
SMPL_LIBS_mysample   := $(CODEC_LIB) -Wl,--gc-sections
```

With `--gc-sections` the program keeps only what it calls: an Opus encoder
alone is about 130 KB of flash. That is more than a RAM window holds on most
boards, so a program that uses Opus is built as a flash image (`.xip.bin`)
and installed.

### G.711

```c
uint8_t g711_alaw_encode(int16_t pcm);    int16_t g711_alaw_decode(uint8_t code);
uint8_t g711_ulaw_encode(int16_t pcm);    int16_t g711_ulaw_decode(uint8_t code);
void    g711_alaw_encode_buf(const int16_t *pcm, uint8_t *code, int n);   /* and the rest */
```

The segment coding of the ITU-T reference (G.191): encoding truncates to the
reference's decision levels, and every code decodes to the reference value.

### Opus

The pack is libopus with its own API, `opus.h`, unchanged: `opus_encode()`,
`opus_decode()`, `opus_encoder_ctl()` and the rest. Two things differ from a
desktop build:

- **Nothing is allocated.** `opus_encoder_create()` and
  `opus_decoder_create()` return NULL. A program takes
  `opus_encoder_get_size(channels)` bytes itself - `api->malloc()` is the
  easy way - and calls `opus_encoder_init()` on them.
- **Scratch is static.** libopus's temporary arrays come from one buffer of
  `OPUS_SCRATCH` bytes (24 KiB) in the library's `.bss`, not from the stack,
  which is a few kilobytes on these boards. 24 KiB covers every mono stream at
  8, 16 or 48 kHz at any complexity (`make test` checks all of them); stereo at
  48 kHz needs about 40 KiB: `make CODECS=1 OPUS_SCRATCH=40960`. Because the
  buffer is shared, make one Opus call at a time, never from a pin or timer
  handler while the program is in another.

A wideband call at 16 kHz:

```c
OpusEncoder *enc = api->malloc(opus_encoder_get_size(1));
opus_encoder_init(enc, 16000, 1, OPUS_APPLICATION_VOIP);
opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));

int16_t pcm[320];                          /* 20 ms from audio_read() */
uint8_t pkt[256];
int n = opus_encode(enc, pcm, 320, pkt, sizeof(pkt));
```

libopus uses the Cortex-M's DSP multiplies through its inline assembly. How
much complexity a board affords in real time has not been measured on a
board yet. The 520 MHz STM32H723 has room for 10. For the 160 MHz STM32U585
and the 168 MHz STM32F405, start at 3, the default of `opusrec`, and raise it
while `audio_status()` shows no overruns.

### Ogg Opus files

```c
int freya_oggopus_open(freya_oggopus_t *o, freya_ogg_write_fn write, void *ctx,
                       int channels, uint32_t input_rate, uint16_t pre_skip,
                       uint32_t serial);
int freya_oggopus_packet(freya_oggopus_t *o, const uint8_t *pkt, int len,
                         uint32_t samples48);
int freya_oggopus_close(freya_oggopus_t *o);
```

`write(ctx, buf, len)` is the program's, usually `api->write()` on a file.
`open()` writes the OpusHead and OpusTags pages. `pre_skip` is the encoder's
`OPUS_GET_LOOKAHEAD` in 48 kHz samples (times 3 at 16 kHz). `packet()` adds
one Opus packet of `samples48` samples (960 for 20 ms) and writes a page about
once a second, so a recording cut short by a power loss keeps everything but
its last second. `close()` writes the last page with the end-of-stream flag.
The struct holds a page being built, about 4.4 KB.

## samples/opusrec

Records the headset's microphone to an Ogg Opus file on the SPI flash:

```
install("opusrec.xip.bin")
run("@flash")                                    # 10 s to /spi1/rec.opus
run("@flash", "/spi1/memo.opus", "30", "24000")  # 30 s at 24 kbit/s
```

A 10 ms timer interrupt moves the microphone's samples into a 512 ms buffer,
so the pauses while the SPI flash erases a sector do not overflow the audio
ring. The loop encodes 20 ms frames from that buffer and writes the pages. At
16 kbit/s a minute takes about 120 KB of the flash. `/spi1` is on the
STM32U585 and the STM32H723. On the STM32F405 boards, give a path on the
card: `run("@flash", "/rec.opus")` with `SD=1`, which on the WeAct board is
its own microSD slot.

## samples/dictophone

A voice recorder for the WeAct STM32F4 64-pin board (`BOARD=weact_f405`,
built there with `CODECS=1`): KEY (PC13) starts and stops a take, the blue
LED shows it is recording, and each take is saved to `/voice.opus` on the
board's microSD card, as `opusrec` would write it. A console key ends the
program, closing a take in progress first.

```
install("dictophone.xip.bin")
run("@flash")                                  # /voice.opus at 16 kbit/s
```

## Source and licence

| File | What it is |
|---|---|
| `codecs/freya_codecs.h` | the pack's header |
| `codecs/g711.c` | G.711 |
| `codecs/oggopus.c` | the Ogg Opus writer, with Ogg's CRC-32 |
| `codecs/opus_glue.c` | what libopus needs from a C library: `memcpy`, `memset`, `memmove`, `abs`, the scratch |
| `codecs/opus/config.h` | libopus's configuration: fixed point, pseudostack, no allocation |
| `third_party/opus/` | libopus 1.5.2, the C sources of CELT, SILK (fixed point) and the Opus layer, unchanged |

libopus is under the BSD licence in `third_party/opus/COPYING`. The pack does
not use its neural network parts (DRED, OSCE, deep PLC), which are left out.

`make test` builds libopus for the host as the pack builds it. It checks G.711
against the reference values and SoX's decoders, encodes and decodes a tone,
checks the scratch every mono stream uses, and hands a written Ogg Opus file
to ffprobe and ffmpeg ([tests.md](tests.md)).
