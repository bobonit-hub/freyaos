# dictophone

A voice recorder for the WeAct STM32F4 64-pin board: press KEY to start
recording the USB headset's microphone, press it again to stop. Each take is
saved to `voice.opus` on the board's microSD card, an Ogg Opus file any
player opens, and the next take replaces it. The blue LED is lit while it
records.

```
install("dictophone.xip.bin")                # once: it runs from flash
run("@flash")                                # /voice.opus at 16 kbit/s
run("@flash", "/notes/voice.opus", "24000")  # another file, 24 kbit/s
```

A key on the console, Ctrl-C too, ends the program; a take in progress is
closed properly first. 16 kHz wideband, mono, Opus in VoIP mode with 20 ms
frames at complexity 3; a minute at 16 kbit/s is about 120 KB.

A 10 ms timer interrupt moves the microphone's samples into a half-second
buffer, so a slow write to the card costs no audio. When a take stops, what
is still in that buffer is encoded before the file is closed.

Build with `make BOARD=weact_f405 SD=1 USB=1 AUDIO=1 CODECS=1`: the headset
goes in the USB-C socket, the card in the board's own slot. The board does
not power the socket as it ships; close SB3 and feed it 5 V on VCC, or use an
OTG Y cable with its own 5 V (`docs/usb.md`). The headset has to be there
at power-up: the kernel looks for it once, at boot. KEY is PC13, which pulls the pin high when pressed; the
program turns on its pull-down. See `docs/codecs.md`.
