# echo

A phone network's echo test on a USB headset: what the microphone hears
comes back on the speaker after a delay, through the same audio calls a
softphone would use. Once a second it prints the microphone's peak and
the stream's underrun, overrun and lost packet counts. Ctrl-C ends it.

```
run("echo.bin")                  # 300 ms, 16 kHz wideband
run("echo.bin", "500", "8000")   # 500 ms, 8 kHz narrowband
```

Needs a kernel built with `make USB=1 AUDIO=1` and a UAC1 headset in the
board's USB socket. See `docs/audio.md`.
