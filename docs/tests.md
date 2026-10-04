# Tests

What `make test` checks on the host, without a board: the filesystem and
XMODEM code, the samples that are worth testing, the exit status rule, the
timer, PWM, I2C, SPI and 1-Wire arithmetic, the cipher, the coder, the virtual
machine, BASIC, and the program image layout.

`make test` compiles the filesystem and XMODEM sources **unchanged** for the
host, points them at disk image files instead of a card, and checks the result
with the system's own FAT tools.

The FAT tests run against freshly formatted FAT16 and FAT32 images: directory
creation, small and multi-cluster files, read-back verification, seeking,
appending, long names and their generated 8.3 aliases, forty files in one
directory, deletion, a check that every allocated cluster is returned, and
rotation of `/freya.log` at 1 MiB via an atomic rename to `/freya.log.old`.
`fsck.vfat` then confirms the images are consistent. An interoperability pass
copies a 40 KB file that `mcopy` wrote, using only Freya calls, and verifies the
copy is byte identical when read back with mtools.

The USB stick test (`USB=1`) compiles `src/usbmsc.c`, `src/usbvol.c` and
`src/fat.c` against a simulated stick in place of `src/usbh.c`: it answers
enumeration, runs Bulk-Only Transport and SCSI over a FAT16 image, and checks
every data toggle it is sent. The test refuses a keyboard and a 4096 byte
block stick, mounts the stick at `/usb` beside a FAT32 card image, writes a
file on each in alternating 100 byte pieces, renames and lists on the stick,
follows a relative path into it, ejects and mounts it again, and pulls it out
while mounted. `fsck.vfat` checks both images, and mtools reads back a file
Freya wrote to the stick.

The USB headset test (`AUDIO=1`) compiles `src/uac.c` and `src/audio.c`
against a UAC1 configuration descriptor shaped like a common headset's - a
48 kHz stereo speaker, a 16/48 kHz microphone, a 24-bit setting it must
skip and a HID interface - and calls the 1 ms interrupt's two entry points a
simulated millisecond at a time. It checks which setting and rate is chosen
for 8 and 16 kHz, that the alternate setting and the frequency are set and no
control transfer happens while streaming, and sends tones through both
resamplers: the level must come through within 0.2 dB, the speaker's images
and the microphone's aliases must be more than 55 dB down. Priming,
underruns, overruns, gain, the calls' refusals and unplugging are checked
too.

The codec pack test (`CODECS=1`) builds libopus for the host as the pack
builds it - fixed point, the pseudostack - and checks G.711 against the
reference values, for monotonic coding and a 35 dB SNR on a tone; all 256
codes of both laws must decode as SoX decodes them. Opus encodes five seconds
of a tone at 16 kHz into an Ogg Opus file: the decoded tone must keep its
level within 1.5 dB, ffprobe must see 5.000 s of mono Opus in Ogg, and ffmpeg
must decode it to the same tone. Every mono stream at 8, 16 and 48 kHz, in
both modes and at three complexities, must stay 2 KiB inside the scratch.

The HTTP client test compiles `http/http.c` against a service table whose
network calls are a scripted server: every call answers `FREYA_ERR_AGAIN` at
random, a send takes only part of what it is given, and the response comes
back a few bytes at a time. It checks the request head byte for byte (the
request line, `Host` with and without a port, Basic authorization, a body
announced up front and written in parts), a name looked up for `http://` and
not for `https://` or an IPv4 address, bodies ended by a length, by chunks
with an extension and a trailer, and by the close, a skipped `100 Continue`,
`HEAD` and `204`, and the errors: a body cut short, a reply that is not HTTP,
bad URLs and headers, a name that does not resolve, a timeout and a cancel,
with every socket given back.

The cJSON test builds `third_party/cjson` as the program library builds it,
with `json/cjson_port.h` in front, against a service table that counts the
blocks it lends and, like the kernel, lends no more than 32: a document parses, reads back, prints compact and indented
and is freed whole; numbers print as cJSON prints them with a C library; `\u`
escapes and surrogate pairs decode; 2000 values fit in those 32 blocks; a
large print grows its buffer through `realloc()`; 300 strings of up to 600
bytes print and parse back; 300000 random `malloc()`, `realloc()` and
`free()` calls under a 160 KiB heap, from the heap and with a pool at an odd
address, keep every byte of every block and give every kernel block back;
a block followed by free room grows in place; JSON Patch and Merge Patch apply; broken JSON is refused; and no
block is left. `json/port.c`'s `%g` and `strtod()` are then checked against the
host C library's on random doubles, on 1 to 25 digits, at ties and at both
ends of the range.

The XMODEM tests drive `src/xmodem.c` with an emulated sender that answers the
receiver's own handshake: CRC mode and checksum fallback, 128 and 1024 byte
packets, a packet corrupted in transit and retransmitted, a duplicated packet,
line noise before the first packet, and the padding of the final block.

The `forth` sample is a program rather than kernel code, but it is the one
sample with enough behaviour to be worth testing, so it is built for the
host too — unchanged, against a service table that captures what it prints
— and driven a line at a time: arithmetic and the number bases, every
control structure, defining words, string literals, recursion, a source
file read through `include`, and each way the interpreter can fail. The
same binary talks to a terminal with `-i`, which is the quickest way to
try the language without a board.

The `rustdemo` sample is built for the host the same way, against the
Rust bindings, and run in a child process for each case, so that `exit()`
and a panic end it as they would on a board. The cases cover the console,
the heap, a file, a timer handler, a thread, a kernel table too short for a
call, `exit()` from a handler, and the panic status. The case is skipped
when `cargo` is not installed.

The `altair` sample is tested the same way. The 8080's flags, `DAA`,
timing, memory map, ports, loaders and tapes are always checked. The
CPU exercisers and Altair BASIC itself cannot be committed, so they run
only when `ALTAIR_TESTS`, `ALTAIR_BASIC` and `ALTAIR_MBL` point at them
(`samples/altair/README.md`). Ctrl-C with the console raw and not raw
is checked on `src/uart.c` itself, against a fake USART.

A run's exit status is decided in one place — `freya_exit_status()` in the ABI
header — so that the closing line of `run`, `$?`, the log line and a program
asking `last_exit()` can never disagree. That rule is a pure function of how
the run ended and what the program asked for, so it is checked on the host:
the truncation to a byte, the `128 + reason` statuses Freya synthesises for
Ctrl-C and the four faults, and the `FREYA_API_HAS` test a program uses on a
service table older than itself.

Neither a pin nor a timer exists on the host either, but the arithmetic behind
them does not need one, and it is the part that would be quietly wrong: a
period off by a factor of two looks like working code on the bench. So
`src/timer.c` and `src/pwm.c` are compiled unchanged and their dividers driven
over the whole range they accept — every period and every frequency at every
clock either board can run at, checked against what the prescaler and the
reload each chose will actually do. Periods come out inside 0.04% everywhere
and exact on the round numbers; frequencies inside 0.6%, which is the counts
running out at the top of the range rather than the arithmetic. The pin
encoding is checked beside them, and so is the board's table of PWM channels:
a hand written table whose two temptations are naming a pin the kernel keeps
and giving one timer channel to two pins. The I2C half-period gets the same
treatment: each half of the clock is a whole number of microseconds, rounded
up, so the bus is the rate that was asked for or a little slower and never
faster, and bus 1 of the pin table is PB6/PB7 on either board. The SPI
baud tap gets the same treatment: of the eight power-of-two divisions of
the bus clock, the one chosen is the fastest that does not exceed the
rate asked for, at 48, 36 and 32 MHz, and bus 1 is SCK/MISO/MOSI on
PB13/PB14/PB15 on either board. The 1-Wire
ROM search and its CRC-8 get the same treatment, against device ids planted
on the host: that walk is the part that would be quietly wrong.

The calendar RTC driver of `RTC=internal` is compiled for each board that
has one and run against a stand-in register block: the BCD and the weekday
it writes, the prescalers and the 24 hour format, the 2000..2099 range,
reading back what was written, and each way the RTC can be unusable - never
set, never in sync, no crystal, no init mode ([rtc.md](rtc.md)).

Ascon-AEAD128 is checked against the NIST SP 800-232 known answers.
`src/aead.c` and the reference in `third_party/ascon` are compiled
unchanged, an empty message, a byte with associated data and a 16-byte
block have to come out as the published ciphertexts, and a flipped tag
has to be refused with the output cleared. Built for the STM32F103, the
same test checks that both calls answer `FREYA_ERR_UNSUPPORTED`.
`tools/aead` generates a key from `/dev/urandom` and its `seal` has to
write the same ciphertext the board call writes.

heatshrink is checked against a stream the upstream tool wrote: `src/lz.c`
and the library are compiled unchanged, the encoder has to produce those
bytes and the decoder has to read them back, and then the bound, an output
that is exactly large enough, every refusal, and the stream calls fed in
odd-sized pieces. Built for the Blue Pill, the same test checks that both
calls answer `FREYA_ERR_UNSUPPORTED`.

The virtual machine is an instruction set, so what would be quietly wrong
about it is the addressing and the flags rather than the arithmetic.
`src/vm.c` is compiled unchanged and driven an instruction at a time: each
of the eight modes, against both the value it produces and the register it
stepped; the word a byte operand does not shorten on R6 and R7; the address
JMP and JSR take, which is neither; MOVB into a register, which is the one
byte instruction that reaches the whole of it; V and C on the shifts and
the subtractions; a dividend whose high word is negative and the two ways a
quotient can fail to exist; and every opcode the machine does not have,
each of which has to leave R7 where a caller can read it.

BASIC is checked in two layers. `basic/fpnat.c` is compiled natively
and 330 000 results of its arithmetic, functions and number
conversions are compared with libm: exactly for the arithmetic,
reading and printing of numbers, and to a few units in the last place
for the functions, with every overflow and domain error checked to
raise its fault. Then the programs under `tests/basic` run on the
interpreter compiled natively, each against its recorded output.
The host build uses the same arithmetic as the board, so those outputs
are what the board prints.

The flash programming itself cannot be reached from the host, which is the main
argument for keeping that driver small and its bounds check absolute. What can
be checked off the board is the part most likely to be quietly wrong: a last
pass builds a flash image and compares every field of the header
`apps/common/app_start.c` emits against the section addresses the linker
actually produced, and compares the regions declared in `include/freya_api.h`
against the ones the linker scripts describe. A linker script cannot include a
C header, so those two descriptions of the memory map are written down twice;
the kernel compares them at boot, and this compares them at build time.

```
132 checks, 0 failures     FAT16
132 checks, 0 failures     FAT32
11 checks, 0 failures     interoperability
18 checks, 0 failures     XMODEM
79 checks, 0 failures     forth
26 checks, 0 failures     exit status
168 checks, 0 failures    pins, timers, PWM, I2C, 1-Wire and SPI
136 checks, 0 failures    PDP-11 virtual machine
330204 checks, 0 failures BASIC float floating point; 11 programs natively
39 checks, 0 failures     program image layout
ALL TESTS PASSED
```
