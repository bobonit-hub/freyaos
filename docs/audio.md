# Audio

With `make USB=1 AUDIO=1`, a USB headset in the board's USB socket is the
board's microphone and speaker. Programs get six calls for a telephone call's
audio: mono 16-bit samples at 16 kHz (wideband, the default) or 8 kHz
(narrowband), whatever the headset itself runs at. The headset is a USB Audio
Class 1 device, which practically every USB headset, speakerphone and USB
sound dongle is.

```sh
make BOARD=blackpill USB=1 AUDIO=1
```

`AUDIO=1` needs `USB=1`, and a board with more than 512 KiB of flash, which
leaves room for the codecs a call needs ([codecs.md](codecs.md)): the
two STM32F405 boards (`stm32f405`, `weact_f405`), the APM32F407 board
(`apm32f407`), the STM32U585 and the STM32H723, whose `board.mk` sets
`USB_AUDIO`. The Black Pill (512 KiB) keeps USB sticks but not audio. A
kernel without `AUDIO=1` still has the six calls, which return
`FREYA_ERR_UNSUPPORTED`.

## The headset

The port has one device and no hub, so it holds either a stick or a headset.
At boot, or with `usb("mount")`, the kernel enumerates what is there; a
headset is announced with the formats it will be used at:

```
[boot] USB        : Logitech USB Headset (046d:0a44)
[boot] USB audio  : mic 16 kHz mono, speaker 48 kHz stereo
```

Freya takes the 16-bit PCM settings of the headset's streaming interfaces,
the one with an IN endpoint as the microphone and the one with an OUT
endpoint as the speaker. For each it picks the program's rate times 1, 2, 3
or 6 - the smallest the headset offers - so a 16 kHz call runs a 48 kHz
headset at exactly three samples per sample. A headset that only does
44.1 kHz cannot be resampled by a whole ratio and is refused. Stereo is mixed
down from the microphone and the one channel is sent to both ears.

At attach, every feature unit with a mute control is unmuted. The headset's
hardware volume is left where it powers up; `audio_gain()` scales in software.

## How it runs

```
program ── audio_write() ──► speaker ring ──► upsample ──► 1 ms packet ──► headset
program ◄── audio_read() ─── mic ring ◄──── downsample ◄── 1 ms packet ◄── headset
```

- Each direction has a ring of `FREYA_AUDIO_RING` = 1024 samples: 64 ms at
  16 kHz, 128 ms at 8 kHz. That is three 20 ms codec frames with room to
  spare; a jitter buffer for the network side is the program's business.
- The USB core's interrupt runs at every 1 ms start of frame. It queues the
  speaker's next packet and the microphone's next IN for the following frame,
  and takes the microphone's data as it arrives. Its priority is just below
  the console's and above every program handler.
- The speaker starts once 20 ms are queued. If the queue runs dry it plays
  silence, counts one underrun, and waits for the next 20 ms - a short gap
  rather than a stutter.
- When the microphone ring is full, new samples are dropped and counted as
  overruns: read at least every 60 ms.
- The resampling filters are one Kaiser-windowed FIR per ratio, 24 taps per
  phase, Q15 (`tools/firgen.py`). They are flat to 0.03 dB up to 0.85 of the
  program's Nyquist rate (6.8 kHz at 16 kHz), and images and aliases are down
  more than 60 dB. A 48 kHz headset costs about 2,300 multiply-adds a
  millisecond in the interrupt (the microphone's filter is folded, being
  symmetric, and `src/audio.c` is built `-O2`): by estimate, not yet
  measured on a board, about 5 % of the 168 MHz STM32F405, and less on the
  faster boards.

Freya is the clock: it sends exactly the nominal rate to the speaker. That
is what a synchronous or adaptive endpoint wants. An asynchronous speaker's
feedback endpoint is not read; its buffer absorbs the difference over a call
and an occasional sample may slip.

## The calls

```c
int audio_open(uint32_t rate, int dirs);       /* 8000, 16000 or 0 = 16000 */
int audio_close(void);
int audio_read(int16_t *buf, int count);       /* microphone, returns count read */
int audio_write(const int16_t *buf, int count);/* speaker, returns count taken */
int audio_status(freya_audio_status_t *st);
int audio_gain(int dirs, int gain);            /* 256 = x1, 0 = mute, up to 1024 */
```

`dirs` is `FREYA_AUDIO_MIC`, `FREYA_AUDIO_SPK` or both. Neither read nor
write waits: 0 means nothing to read, or no room yet, and a call loop sleeps a
millisecond or two and tries again. Both may be called from a pin or timer
handler. `audio_open()` returns `FREYA_ERR_IO` when there is no headset,
`FREYA_ERR_BUSY` when audio is already open, and `FREYA_ERR_UNSUPPORTED` when
the headset has no usable format for a direction. Once the headset is
unplugged, read and write return `FREYA_ERR_IO`. The kernel closes audio when
the run that opened it ends, however it ends.

`audio_status()` fills `freya_audio_status_t`: both rates, channel counts,
samples waiting and queued, the gains, the frame count, and the underrun,
overrun and lost packet counters - the numbers to watch on a bad call.

A 20 ms frame loop, as a softphone would have it:

```c
freya_audio_status_t st;
int16_t frame[320];                    /* 20 ms at 16 kHz */

api->audio_open(16000, FREYA_AUDIO_MIC | FREYA_AUDIO_SPK);
for (;;) {
    if (api->audio_status(&st) == 0 && st.mic_avail >= 320) {
        api->audio_read(frame, 320);
        encode_and_send(frame);        /* the network side */
    }
    if (receive_and_decode(frame))
        api->audio_write(frame, 320);
    api->thread_sleep(2);
}
```

`samples/echo` is a complete program: a phone network's echo test, what the
microphone hears played back after 300 ms. To encode what `audio_read()`
returns - G.711 or Opus for a call, an `.opus` file for a recording - link
the codec pack ([codecs.md](codecs.md)); `samples/opusrec` records to a file.

## The shell

| Command | What it does |
|---|---|
| `audio()` | the headset and its formats, and the stream if one is open |
| `audio("tone", hz [, seconds [, rate]])` | a sine on the speaker, -6 dBFS |
| `audio("loop" [, seconds [, rate]])` | the microphone on the speaker, with its peak each second |

Both run for 5 seconds unless told otherwise, Ctrl-C stops them, and both
print the counters at the end. `audio("loop")` exercises the whole path a
call takes, resampling included.

## Source

| File | What it is |
|---|---|
| `src/uac.c` | the UAC1 descriptors, choosing a setting and a rate, unmuting, alternate settings |
| `src/audio.c` | the rings, the resampling filters, the calls; only stubs without `AUDIO=1` |
| `src/usbh.c` | the isochronous channels and the 1 ms interrupt |
| `src/usbdev.c` | enumeration, and telling a stick from a headset |
| `tools/firgen.py` | generates and checks the filter tables |

`make test` runs `src/uac.c` and `src/audio.c` against a realistic headset
descriptor, with the 1 ms interrupt simulated: tones are sent through both
resamplers and measured on the far side for level, images and aliases, and
priming, underruns, overruns, gain and unplugging are checked
([tests.md](tests.md)). The isochronous transfers themselves only run on a
board.
