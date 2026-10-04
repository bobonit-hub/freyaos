# opusrec

Records the USB headset's microphone to an Ogg Opus file: 16 kHz wideband,
mono, Opus VoIP mode in 20 ms frames, which any player opens.

```
install("opusrec.xip.bin")                     # once: it runs from flash
run("@flash")                                  # 10 s to /spi1/rec.opus
run("@flash", "/spi1/memo.opus", "30", "24000")  # 30 s at 24 kbit/s
run("@flash", "/spi1/memo.opus", "0")          # until a key
```

Arguments: the file, the seconds (0 records until a key is pressed), the
bitrate in bit/s (6000 to 64000, default 16000) and the encoder complexity
(0 to 10, default 3; the STM32H723 has room for 10). Any key, Ctrl-C too,
stops it early and the file is still closed with its last page.

A 10 ms timer interrupt moves the microphone's samples into a half-second
buffer, so the pauses while the SPI flash erases cost no audio. At 16 kbit/s
a minute is about 120 KB.

Needs a kernel built with `make USB=1 AUDIO=1 CODECS=1`, on the STM32U585 or
the STM32H723 for `/spi1` (another path works too: `/` on the card, `/usb`
is not possible while the headset holds the port). It is built as a flash
image only, `opusrec.xip.bin`: about 150 KB of it is libopus, more than a
RAM window holds. See
`docs/codecs.md`.
