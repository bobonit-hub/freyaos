#!/bin/sh
#
# Freya - filesystem regression test.
#
# Builds the FAT code for the host, runs it against freshly formatted
# FAT16 and FAT32 images, and then has fsck.vfat and mtools confirm that
# what Freya wrote is a valid filesystem that other systems can read.
#
set -e

cd "$(dirname "$0")/.."
OUT=build/tests
mkdir -p "$OUT"

# The FAT images are hundreds of megabytes and only needed while a run
# is in progress. Drop them on the way out, including after a failure.
cleanup() {
    rm -f "$OUT/fat16.img" "$OUT/fat32.img" "$OUT/interop.img" "$OUT/xmodem.img" \
          "$OUT/spiflash.img" "$OUT/usb-card.img" "$OUT/usb-stick.img"
}
trap cleanup EXIT

CC=${CC:-cc}

# The sources under test pull in freya.h, which pulls in the board header;
# any board will do on the host, so use the one being built.  The card and
# FAT code are optional in the kernel (SD=1); the tests build them in.
BOARD=${BOARD:-blackpill}
BOARD_DEF="-DFREYA_BOARD_$(echo "$BOARD" | tr '[:lower:]' '[:upper:]')"

CFLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-builtin \
        -Iinclude -Isrc -Ithird_party/littlefs -Ithird_party/heatshrink \
        -Iboards/$BOARD $BOARD_DEF -DFREYA_HOST -DFREYA_SD \
        -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
        -DLFS_NAME_MAX=63"

# heatshrink, as the kernel builds it.  The encoder's one fall-through is
# upstream's and deliberate.
HS_SRC="third_party/heatshrink/heatshrink_encoder.c third_party/heatshrink/heatshrink_decoder.c"
HS_CFLAGS="-Wno-implicit-fallthrough"

# shellcheck disable=SC2086
$CC $CFLAGS tests/host_fat_test.c src/fat.c src/fs.c src/log.c src/string.c src/print.c \
    -o "$OUT/hosttest"

# shellcheck disable=SC2086
$CC $CFLAGS tests/host_xmodem_test.c src/xmodem.c src/fat.c src/fs.c \
    src/string.c src/print.c -o "$OUT/hostxmodem"

if grep -q "^SPIFLASH *:= *1" "boards/$BOARD/board.mk"; then
    # shellcheck disable=SC2086
    $CC $CFLAGS tests/host_spiflash_test.c src/spiflash.c src/lfsvol.c src/fat.c src/fs.c \
        third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c \
        src/string.c src/print.c -o "$OUT/hostspiflash"
    # The same volume in a kernel built without SD=1: src/nosd.c in place
    # of the card and the FAT code.
    # shellcheck disable=SC2086
    $CC $(echo "$CFLAGS" | sed 's/-DFREYA_SD//') tests/host_spiflash_test.c \
        src/spiflash.c src/lfsvol.c src/nosd.c src/fs.c \
        third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c \
        src/string.c src/print.c -o "$OUT/hostspiflash-nosd"
fi

status=0

for fs in 16 32; do
    case $fs in
        16) size=32768  ;;   # 32 MiB  -> FAT16
        32) size=262144 ;;   # 256 MiB -> FAT32
    esac

    img="$OUT/fat$fs.img"
    echo
    echo "================= FAT$fs ================="
    rm -f "$img"
    dd if=/dev/zero of="$img" bs=1024 count=$size status=none
    mkfs.vfat -F "$fs" -n FREYA "$img" >/dev/null

    if ! "$OUT/hosttest" "$img"; then
        status=1
    fi

    echo
    echo "--- fsck.vfat ---"
    if fsck.vfat -n -v "$img" 2>&1 | tail -n 12; then
        :
    else
        echo "fsck reported problems"
        status=1
    fi
done

# A round trip through mtools: a file Freya wrote must be readable by a
# foreign FAT implementation, and vice versa.
echo
echo "================= SPI flash ================="
if grep -q "^SPIFLASH *:= *1" "boards/$BOARD/board.mk"; then
    if ! "$OUT/hostspiflash"; then
        status=1
    fi
    echo "--- built without SD=1 ---"
    if ! "$OUT/hostspiflash-nosd"; then
        status=1
    fi
fi

echo
echo "================= interoperability ================="
img="$OUT/interop.img"
rm -f "$img"
dd if=/dev/zero of="$img" bs=1024 count=65536 status=none
mkfs.vfat -F 32 -n FREYA "$img" >/dev/null

head -c 40000 /dev/urandom > "$OUT/from_host.bin"
MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$OUT/from_host.bin" ::/from_host.bin
MTOOLS_SKIP_CHECK=1 mmd -i "$img" ::/hostdir

"$OUT/hosttest" "$img" interop || status=1

echo
echo "--- mtools reads back what Freya wrote ---"
MTOOLS_SKIP_CHECK=1 mdir -i "$img" -/ :: | head -n 20
rm -f "$OUT/roundtrip.bin"
MTOOLS_SKIP_CHECK=1 mcopy -i "$img" ::/by_freya.bin "$OUT/roundtrip.bin"
if cmp -s "$OUT/from_host.bin" "$OUT/roundtrip.bin"; then
    echo "  ok    the 40000 byte round trip is byte identical"
else
    echo "  FAIL  the round trip differs"
    status=1
fi
if MTOOLS_SKIP_CHECK=1 mtype -i "$img" "::/hostdir/Written By Freya.txt" \
       | grep -q "freya was here"; then
    echo "  ok    Linux reads the long name Freya created"
else
    echo "  FAIL  the long name is not readable by mtools"
    status=1
fi
fsck.vfat -n "$img" >/dev/null 2>&1 || { echo "  FAIL  interop image is inconsistent"; status=1; }

# The USB stick (USB=1): enumeration, Bulk-Only Transport and SCSI against
# a simulated stick, as a second FAT volume at /usb beside the card.  The
# hardware layer, src/usbh.c, is the one part that only runs on a board.
echo
echo "================= USB stick ================="
# shellcheck disable=SC2086
$CC $CFLAGS -DFREYA_USB tests/host_usb_test.c src/usbdev.c src/usbmsc.c src/usbvol.c \
    src/fat.c src/fs.c src/string.c src/print.c -o "$OUT/hostusb"
card="$OUT/usb-card.img"
stick="$OUT/usb-stick.img"
rm -f "$card" "$stick"
dd if=/dev/zero of="$card" bs=1024 count=65536 status=none
mkfs.vfat -F 32 -n CARD "$card" >/dev/null
dd if=/dev/zero of="$stick" bs=1024 count=32768 status=none
mkfs.vfat -F 16 -n STICK "$stick" >/dev/null
"$OUT/hostusb" "$card" "$stick" || status=1
fsck.vfat -n "$card" >/dev/null 2>&1 || { echo "  FAIL  the card is inconsistent"; status=1; }
fsck.vfat -n "$stick" >/dev/null 2>&1 || { echo "  FAIL  the stick is inconsistent"; status=1; }
if MTOOLS_SKIP_CHECK=1 mtype -i "$stick" ::/sub/rel.txt | grep -q relative; then
    echo "  ok    Linux reads the file Freya wrote to the stick"
else
    echo "  FAIL  the stick's file is not readable by mtools"
    status=1
fi

# USB headsets (AUDIO=1): a UAC1 configuration descriptor, and tones
# through the speaker's and the microphone's resamplers, measured on the
# far side, with the USB interrupt simulated a millisecond at a time.
echo
echo "================= USB headset ================="
# shellcheck disable=SC2086
$CC $CFLAGS -DFREYA_USB -DFREYA_AUDIO tests/host_audio_test.c src/uac.c src/audio.c \
    src/usbdev.c src/string.c src/print.c -lm -o "$OUT/hostaudio"
"$OUT/hostaudio" || status=1

# The codec pack (CODECS=1): G.711 against the reference coding and SoX's
# decoders, libopus built as the pack builds it, and an Ogg Opus file that
# ffprobe must take for 5 s of 48 kHz Opus and ffmpeg must decode.
echo
echo "================= codec pack ================="
CODEC_OUT="$OUT/codecs"
CODEC_FLAGS="-std=gnu11 -O2 -g -DHAVE_CONFIG_H -DFREYA_HOST -Icodecs/opus -Icodecs \
             -Ithird_party/opus/include -Ithird_party/opus/celt -Ithird_party/opus/silk \
             -Ithird_party/opus/silk/fixed"
mkdir -p "$CODEC_OUT/opus"
# shellcheck disable=SC2086
ls third_party/opus/celt/*.c third_party/opus/silk/*.c third_party/opus/silk/fixed/*.c \
   third_party/opus/src/*.c |
    xargs -P "$(nproc 2>/dev/null || echo 4)" -I{} sh -c \
        "$CC $CODEC_FLAGS -w -c {} -o $CODEC_OUT/opus/\$(echo {} | tr / _).o" || status=1
rm -f "$CODEC_OUT/libopus.a"
ar rcs "$CODEC_OUT/libopus.a" "$CODEC_OUT"/opus/*.o
# shellcheck disable=SC2086
$CC $CODEC_FLAGS -Wall -Wextra -Wno-unused-parameter tests/host_codecs_test.c \
    codecs/g711.c codecs/oggopus.c codecs/opus_glue.c "$CODEC_OUT/libopus.a" -lm \
    -o "$OUT/hostcodecs" || status=1
"$OUT/hostcodecs" "$CODEC_OUT/test.opus" || status=1

if command -v sox >/dev/null 2>&1; then
    python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)))" > "$CODEC_OUT/codes"
    "$OUT/hostcodecs" --g711-decode "$CODEC_OUT/codes" "$CODEC_OUT/ours.al" "$CODEC_OUT/ours.ul"
    sox -t al -r 8000 -c 1 "$CODEC_OUT/codes" -t raw -e signed -b 16 "$CODEC_OUT/sox.al"
    sox -t ul -r 8000 -c 1 "$CODEC_OUT/codes" -t raw -e signed -b 16 "$CODEC_OUT/sox.ul"
    if cmp -s "$CODEC_OUT/ours.al" "$CODEC_OUT/sox.al" &&
       cmp -s "$CODEC_OUT/ours.ul" "$CODEC_OUT/sox.ul"; then
        echo "  ok    all 256 A-law and mu-law codes decode as SoX decodes them"
    else
        echo "  FAIL  G.711 decoding differs from SoX"
        status=1
    fi
else
    echo "  --    sox not found, skipping the G.711 cross-check"
fi

if command -v ffprobe >/dev/null 2>&1 && command -v ffmpeg >/dev/null 2>&1; then
    probe=$(ffprobe -v error -show_entries stream=codec_name,sample_rate,channels \
            -show_entries format=format_name,duration -of csv=p=0 "$CODEC_OUT/test.opus" |
            tr '\n' ' ')
    case "$probe" in
        "opus,48000,1 ogg,5.000000 ") echo "  ok    ffprobe: an Ogg file of 5.000 s of mono Opus" ;;
        *) echo "  FAIL  ffprobe says: $probe"; status=1 ;;
    esac
    if ffmpeg -v error -i "$CODEC_OUT/test.opus" -f s16le -ar 16000 -ac 1 -y \
            "$CODEC_OUT/test.raw" && python3 - "$CODEC_OUT/test.raw" <<'PY'
import math, struct, sys
d = open(sys.argv[1], 'rb').read()
x = struct.unpack('<%dh' % (len(d) // 2), d)[16000:64000]
def level(f):
    w = 2 * math.pi * f / 16000; c = 2 * math.cos(w); s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return 2 * math.sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / len(x)
db = 20 * math.log10(level(440) / 4800)
print("  ok    ffmpeg decodes the 440 Hz tone at %.1f dB of its level" % db
      if abs(db) < 1.5 else "  FAIL  ffmpeg decodes the tone at %.1f dB" % db)
sys.exit(0 if abs(db) < 1.5 else 1)
PY
    then :; else status=1; fi
else
    echo "  --    ffmpeg not found, skipping the Ogg Opus file check"
fi

# XMODEM receiver against an emulated sender.
echo
echo "================= XMODEM ================="
img="$OUT/xmodem.img"
rm -f "$img"
dd if=/dev/zero of="$img" bs=1024 count=65536 status=none
mkfs.vfat -F 32 -n FREYA "$img" >/dev/null
"$OUT/hostxmodem" "$img" || status=1
fsck.vfat -n "$img" >/dev/null 2>&1 || { echo "  FAIL  image inconsistent after downloads"; status=1; }

echo
echo "================= fremote ================="
python3 tests/host_fremote_test.py || status=1

echo
echo "================= dfu_image ================="
python3 tests/host_dfu_image_test.py || status=1
python3 tests/host_dfu_flash_test.py || status=1

# The forth sample: its interpreter, its compiler and the machine that
# runs what the compiler produced, driven line by line with the output
# captured.  FREYA_APP_XIP picks the memory budget of a flash resident
# image, which is the only one the Blue Pill builds.  -no-pie keeps the
# test's static data inside the low 4 GiB, because Forth cells are 32
# bits wide and the program hands out real addresses.
echo
echo "================= forth ================="
# shellcheck disable=SC2086
$CC $CFLAGS -DFREYA_APP_XIP -no-pie tests/host_forth_test.c -o "$OUT/hostforth"
"$OUT/hostforth" || status=1

# The Altair sample: the 8080 instruction by instruction, the Turnkey
# memory map, the serial ports and the file formats.  ALTAIR_TESTS=dir
# adds the CP/M CPU exercisers found there, ALTAIR_BASIC=file a session
# with that Altair BASIC image; neither file is part of Freya.
echo
echo "================= altair ================="
# shellcheck disable=SC2086
$CC $CFLAGS -O2 -DFREYA_APP_XIP tests/host_altair_test.c -o "$OUT/hostaltair"
"$OUT/hostaltair" || status=1

# The Rust bindings: samples/rustdemo built unchanged for the host
# against rust/freya, and run against a service table that captures what
# it prints.  Needs cargo; without it the case is skipped, as the
# Makefile skips the Rust samples.
echo
echo "================= rust ================="
CARGO=${CARGO:-$(command -v cargo || ls "$HOME/.cargo/bin/cargo" 2>/dev/null || true)}
if [ -n "$CARGO" ]; then
    if "$CARGO" build --quiet --release --manifest-path samples/rustdemo/Cargo.toml \
            --target-dir "$OUT/rust"; then
        # shellcheck disable=SC2086
        $CC $CFLAGS tests/host_rust_test.c "$OUT/rust/release/librustdemo.a" \
            -o "$OUT/hostrust"
        "$OUT/hostrust" || status=1
    else
        echo "  FAIL  samples/rustdemo does not build for the host"
        status=1
    fi
else
    echo "  skipped: cargo not found"
fi

# The exit status rule: how a program's code, a Ctrl-C and a fault each
# become the number the shell reports as '$?'.
echo
echo "================= exit status ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_exit_test.c -o "$OUT/hostexit"
"$OUT/hostexit" || status=1

# Card power: what is closed and unmounted around the rail.  The pin
# itself is the board's; this is the decision in src/power.c.
echo
echo "================= board power ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_power_test.c src/power.c -o "$OUT/hostpower"
"$OUT/hostpower" || status=1

# Ctrl-C at the console: a stop for a running program, a key for one
# that asked for a raw console, and the shell's own key otherwise.
echo
echo "================= console Ctrl-C ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_uart_test.c -o "$OUT/hostuart"
"$OUT/hostuart" || status=1

# Thread names, priorities and stop.  The PendSV switch does not run on
# the host; the table and the scheduling decision do.
echo
echo "================= threads ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_thread_test.c src/thread.c src/string.c src/print.c \
    -o "$OUT/hostthread"
"$OUT/hostthread" || status=1

# The shell commands whose names are a single word, and help, which has
# to print that name.  Registers the commands read are planted in the
# host address space; the card and the clock are stubs.
echo
echo "================= shell commands ================="
# The C library already owns __data_start and __bss_start.  Rename the
# linker symbols shell.c subtracts so the test can plant its own.
# shellcheck disable=SC2086
$CC $CFLAGS -c src/shell.c -o "$OUT/shell_host.o" \
    -D__data_start=freya_test_data_start \
    -D__data_end=freya_test_data_end \
    -D__bss_start=freya_test_bss_start \
    -D__bss_end=freya_test_bss_end \
    -D__heap_start=freya_test_heap_start \
    -D__stack_limit=freya_test_stack_limit \
    -D__stack_top=freya_test_stack_top \
    -D__ram_start=freya_test_ram_start \
    -D__ram_end=freya_test_ram_end \
    -D__kernel_flash_end=freya_test_kernel_flash_end
# shellcheck disable=SC2086
$CC $CFLAGS $HS_CFLAGS -Ithird_party/ascon tests/host_shell_test.c \
    "$OUT/shell_host.o" src/print.c src/cksum.c src/lz.c src/aead.c \
    third_party/ascon/aead.c $HS_SRC -o "$OUT/hostshell"
"$OUT/hostshell" || status=1

echo
echo "================= DS3231 ================="
# The driver is optional.  Compile it for every board: PB6/PB7 have to be
# that board's I2C bus 1, and the register coding does not depend on which.
for b in blackpill bluepill stm32f405 weact_f405 apm32f407 blackpill2; do
    bdef="-DFREYA_BOARD_$(echo "$b" | tr '[:lower:]' '[:upper:]')"
    # shellcheck disable=SC2086
    $CC -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-builtin \
        -Iinclude -Isrc -Iboards/$b $bdef -DFREYA_HOST -DFREYA_RTC_DS3231 \
        tests/host_ds3231_test.c src/ds3231.c -o "$OUT/hostds3231-$b"
    "$OUT/hostds3231-$b" || status=1
done

echo
echo "================= calendar RTC ================="
# Optional too, and only for the boards whose chip has the calendar RTC.
for b in blackpill stm32f405 weact_f405 apm32f407 stm32u585 stm32h523 stm32h562 stm32h723; do
    bdef="-DFREYA_BOARD_$(echo "$b" | tr '[:lower:]' '[:upper:]')"
    # shellcheck disable=SC2086
    $CC -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-builtin \
        -Iinclude -Isrc -Iboards/$b $bdef -DFREYA_HOST -DFREYA_RTC_INTERNAL \
        tests/host_rtc_test.c src/rtc.c -o "$OUT/hostrtc-$b"
    "$OUT/hostrtc-$b" > "$OUT/hostrtc-$b.log" || { cat "$OUT/hostrtc-$b.log"; status=1; }
    echo "  $b: $(tail -1 "$OUT/hostrtc-$b.log")"
done

echo
echo "================= firmware sum ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_cksum_test.c src/cksum.c -o "$OUT/hostcksum"
"$OUT/hostcksum" || status=1
python3 tools/fwsum.py --self-test || status=1

echo
echo "================= system settings ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_settings_test.c src/settings.c src/cksum.c -o "$OUT/hostsettings"
"$OUT/hostsettings" || status=1

echo
echo "================= remote syslog ================="
# Only the boards with the ESP32-C6 link build it.
if [ "$BOARD" != bluepill ]; then
    # shellcheck disable=SC2086
    $CC $CFLAGS tests/host_syslog_test.c src/settings.c src/cksum.c src/print.c \
        -o "$OUT/hostsyslog"
    "$OUT/hostsyslog" || status=1
else
    echo "  --    the Blue Pill has no network, so no remote syslog"
fi

echo
echo "================= network framing ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_esp_link_test.c src/string.c -o "$OUT/hostnetframe"
"$OUT/hostnetframe" || status=1
# The network calls are built only with the ESP32-C6 link.  Without it
# src/net.c is the assembler stub that answers unsupported.
if [ "$BOARD" != bluepill ]; then
    # shellcheck disable=SC2086
    $CC $CFLAGS tests/host_net_test.c -o "$OUT/hostnet"
    "$OUT/hostnet" || status=1
else
    echo "  --    the Blue Pill has no network, so no network calls"
fi

# The HTTP client library (http/) against a scripted server behind the
# network calls of freya_api_t.  It is board-independent C.
echo
echo "================= HTTP client ================="
# shellcheck disable=SC2086
$CC $CFLAGS -Ihttp tests/host_http_test.c http/http.c -o "$OUT/hosthttp"
"$OUT/hosthttp" || status=1

# cJSON as the program library builds it: the upstream sources with
# json/cjson_port.h in front, against json/port.c, and its numbers
# against the host C library's.
echo
echo "================= cJSON ================="
JSON_FLAGS="-Ijson -Ithird_party/cjson"
# shellcheck disable=SC2086
$CC $CFLAGS $JSON_FLAGS -include json/cjson_port.h -c third_party/cjson/cJSON.c \
    -o "$OUT/cJSON.o" &&
$CC $CFLAGS $JSON_FLAGS -include json/cjson_port.h -c third_party/cjson/cJSON_Utils.c \
    -o "$OUT/cJSON_Utils.o" &&
$CC $CFLAGS $JSON_FLAGS tests/host_json_test.c json/port.c "$OUT/cJSON.o" \
    "$OUT/cJSON_Utils.o" -lm -o "$OUT/hostjson" || status=1
"$OUT/hostjson" || status=1

# Single precision on the Cortex-M3: the helpers in src/softfp.c against
# the host FPU, then a soft-float link that must not need libgcc for them.
echo
echo "================= single precision ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_softfp_test.c src/softfp.c -o "$OUT/hostsoftfp"
"$OUT/hostsoftfp" || status=1

CROSS_FP=${CROSS:-arm-none-eabi-}
if command -v "${CROSS_FP}gcc" >/dev/null 2>&1; then
    if "${CROSS_FP}gcc" -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -Os \
            -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
            -nostdlib -Wl,--gc-sections -Wl,-e,softfp_link \
            tests/softfp_link.c src/softfp.c -o "$OUT/softfp_link.elf"; then
        echo "  ok    Cortex-M3 float code links against src/softfp.c"
    else
        echo "  FAIL  Cortex-M3 float code did not link against src/softfp.c"
        status=1
    fi
else
    echo "  --    ${CROSS_FP}gcc not found, skipping the soft-float link"
fi

# Pins, timers, PWM, ADC, I2C, 1-Wire and SPI: the pin numbering a program
# uses, the timer dividers, the I2C half-period and the SPI baud tap
# compiled for the host and asked for every value they accept, the
# board's pin tables, and the 1-Wire ROM search against ids planted
# in place of a pin.
echo
echo "================= pins, timers, PWM, ADC, I2C, 1-Wire and SPI ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_irq_test.c -o "$OUT/hostirq"
"$OUT/hostirq" || status=1

# heatshrink.  The stream the host tool writes, round trips, the bound,
# and every refusal, compiled unchanged from src/lz.c and the library.
# A board without the code answers unsupported, and that is checked too.
echo
echo "================= heatshrink ================="
# shellcheck disable=SC2086
$CC $CFLAGS $HS_CFLAGS tests/host_compress_test.c src/lz.c $HS_SRC \
    -o "$OUT/hostcompress"
"$OUT/hostcompress" || status=1

# Ascon-AEAD128.  The NIST known answers, a round trip and every refusal,
# compiled unchanged from src/aead.c and the reference.  The STM32F103
# build answers unsupported, and that is checked on its own even when
# the rest of this run is another board.  Keys are made by tools/aead,
# which is the PC side of the same reference code.
echo
echo "================= Ascon-AEAD128 ================="
for b in blackpill bluepill; do
    bdef="-DFREYA_BOARD_$(echo "$b" | tr '[:lower:]' '[:upper:]')"
    # shellcheck disable=SC2086
    $CC -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-builtin \
        -Iinclude -Isrc -Ithird_party/ascon -Iboards/$b $bdef -DFREYA_HOST \
        tests/host_aead_test.c src/aead.c third_party/ascon/aead.c \
        -o "$OUT/hostaead-$b"
    "$OUT/hostaead-$b" || status=1
done
# shellcheck disable=SC2086
$CC -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Ithird_party/ascon \
    tools/aead.c third_party/ascon/aead.c -o "$OUT/aead"
key=$("$OUT/aead" key) || status=1
nonce=$("$OUT/aead" nonce) || status=1
if [ "${#key}" -eq 32 ] && [ "${#nonce}" -eq 32 ]; then
    echo "  ok    tools/aead key and nonce are 16 bytes"
else
    echo "  FAIL  tools/aead key and nonce are 16 bytes"
    status=1
fi
printf ' ' > "$OUT/aead-pt"
"$OUT/aead" seal 000102030405060708090A0B0C0D0E0F \
    101112131415161718191A1B1C1D1E1F "$OUT/aead-pt" "$OUT/aead-ct" >/dev/null \
    || status=1
got=$(od -An -tx1 "$OUT/aead-ct" | tr -d ' \n')
if [ "$got" = "e8dd576aba1cd3e6fc704de02aedb79588" ]; then
    echo "  ok    tools/aead seal matches the published ciphertext"
else
    echo "  FAIL  tools/aead seal matches the published ciphertext"
    status=1
fi
"$OUT/aead" open 000102030405060708090A0B0C0D0E0F \
    101112131415161718191A1B1C1D1E1F "$OUT/aead-ct" "$OUT/aead-back" >/dev/null \
    || status=1
if cmp -s "$OUT/aead-pt" "$OUT/aead-back"; then
    echo "  ok    tools/aead open restores the byte"
else
    echo "  FAIL  tools/aead open restores the byte"
    status=1
fi

# PDP-11 opcodes on 32-bit registers, compiled unchanged from src/vm.c.
echo
echo "================= PDP-11 virtual machine ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_vm_test.c src/vm.c -o "$OUT/hostvm"
"$OUT/hostvm" || status=1

# BASIC: first its arithmetic against libm, then the BASIC programs
# under tests/basic on the interpreter compiled natively.
echo
echo "================= BASIC ================="
# shellcheck disable=SC2086
$CC $CFLAGS -fno-math-errno -ffp-contract=off tests/host_fpnat_test.c -lm -o "$OUT/hostfpnat"
"$OUT/hostfpnat" || status=1
# The same arithmetic on integers, as the virtual machine's BASIC has it,
# has to give the FPU's bits: one hash of every result each way.
# shellcheck disable=SC2086
$CC $CFLAGS -O2 -fno-math-errno -ffp-contract=off tests/host_fpsoft_test.c -lm -o "$OUT/hostfpfloat"
# shellcheck disable=SC2086
$CC $CFLAGS -O2 -fno-math-errno -ffp-contract=off -DBAS_SOFTFLOAT tests/host_fpsoft_test.c -lm -o "$OUT/hostfpsoft"
fp_float=$("$OUT/hostfpfloat") || status=1
fp_soft=$("$OUT/hostfpsoft") || { echo "$fp_soft"; status=1; }
fp_soft_hash=$(echo "$fp_soft" | tail -n 1)
echo "$fp_soft" | grep "^  ok" || true
if [ "$fp_float" = "$fp_soft_hash" ]; then
    echo "  ok      fpsoft: the integer arithmetic gives the FPU's bits ($fp_float)"
else
    echo "  FAIL  fpsoft: the integer arithmetic differs: $fp_float with the FPU, $fp_soft_hash without"
    status=1
fi
sh tests/basic_tests.sh || status=1

# ---------------------------------------------------------------------
# Program image layout.
#
# The one part of the flash program support that can be checked off the
# board: that the header app_start.c emits agrees with where the linker
# actually put things, and that the regions in freya_api.h are the regions
# the linker scripts describe.  Every field below is derived from a linker
# symbol by one side and read out of the built image by the other, so a
# copy-paste error in a memory map fails here instead of on the bench.
echo
echo "================= program image layout ================="

# shellcheck disable=SC2086
$CC $CFLAGS tests/host_regions.c -o "$OUT/hostregions"
"$OUT/hostregions" > "$OUT/regions.txt"

macro() { awk -v k="$1" '$1 == k { print $2 }' "$OUT/regions.txt"; }

checks=0
fails=0
check() {   # description expected actual
    checks=$((checks + 1))
    if [ "$2" = "$3" ]; then
        echo "  ok    $1"
    else
        echo "  FAIL  $1: expected $2, got $3"
        fails=$((fails + 1))
        status=1
    fi
}

check "the ABI 2 fields are appended after the 48 byte ABI 1 header" \
      "$(macro hdr_v1_size)" 48
check "the ABI 3 fields are appended after the 64 byte ABI 2 header" \
      "$(macro hdr_v2_size)" 64
check "the header is 72 bytes" "$(macro hdr_size)" 72

CROSS=${CROSS:-arm-none-eabi-}
if ! command -v "${CROSS}nm" >/dev/null 2>&1; then
    echo "  --    ${CROSS}nm not found, skipping the image checks"
elif [ ! -f "boards/$BOARD/app_flash.ld" ]; then
    echo "  --    $BOARD keeps no program in flash, nothing more to check"
else
    kelf="build/$BOARD/freya.elf"
    elf="build/$BOARD/apps/hello.xip.elf"
    bin="build/$BOARD/apps/hello.xip.bin"
    [ -f "$bin" ] && [ -f "$kelf" ] || make BOARD="$BOARD" >/dev/null

    # A linker symbol's value, as a decimal number.
    sym() {
        v=$("${CROSS}nm" "$1" | awk -v n="$2" '$3 == n { print $1 }')
        if [ -z "$v" ]; then echo "no-symbol-$2"; else echo $(( 0x$v )); fi
    }
    # A little endian uint32 out of the built image, as a decimal number.
    fld() { od -A n -t u4 -j "$2" -N 4 "$1" | tr -d ' \n'; }

    check "the kernel and the header agree on the flash region address" \
          "$(macro app_flash_addr)" "$(sym "$kelf" __app_flash_start)"
    check "the kernel and the header agree on the flash region size" \
          "$(macro app_flash_size)" \
          "$(( $(sym "$kelf" __app_flash_end) - $(sym "$kelf" __app_flash_start) ))"
    check "the kernel and the header agree on the settings address" \
          "$(macro settings_addr)" "$(sym "$kelf" __settings_start)"
    check "the kernel and the header agree on the settings size" \
          "$(macro settings_size)" \
          "$(( $(sym "$kelf" __settings_end) - $(sym "$kelf" __settings_start) ))"
    check "the system settings area is 1 KiB" "$(macro settings_size)" 1024
    check "a settings copy is 64 bytes" "$(macro settings_block)" 64
    check "system settings keep two copies" "$(macro settings_copies)" 2
    check "the ram-dump flag follows the log level" \
          "$(macro ramdump_off)" 12
    check "the firmware sum follows the ram-dump flag" \
          "$(macro cksum_off)" 16
    check "the settings checksum follows the password" \
          "$(macro sum_off)" 28
    check "the terminal password is eight bytes after the firmware sum" \
          "$(macro password_off)" 20
    check "the terminal password is eight bytes" \
          "$(macro password_len)" 8
    check "the remote syslog flag follows the settings checksum" \
          "$(macro syslog_off)" 32
    check "the syslog server address follows the syslog flag" \
          "$(macro syslog_addr_off)" 36
    check "the syslog server port follows its address" \
          "$(macro syslog_port_off)" 40
    check "the system settings are 128-byte aligned" \
          0 "$(( $(macro settings_addr) % 128 ))"
    check "the program flash region is 128-byte aligned" \
          0 "$(( $(macro app_flash_addr) % 128 ))"
    # The flash map: the settings, the program region and the extension
    # each have erase units of their own, in the order the board fixes.
    # Blue Pill: kernel, program, extension, settings in the last page.
    # F4: kernel, settings in sector 3, program, extension in the last
    # sector, so the extension's end is the end of flash.
    # Black Pill 2: kernel, settings in a page of their own, extension in
    # the zero wait state flash, program to the end of flash.
    # STM32U585, STM32H523 and STM32H562: the Black Pill 2's order, in
    # 8 KiB pages, to the end of 2 MiB, of 512 KiB and of 1 MiB.
    case "$BOARD" in
        bluepill)  flash_end=$((0x08020000)) ;;
        blackpill) flash_end=$((0x08080000)) ;;
        stm32u585) flash_end=$((0x08200000)) ;;
        stm32h523) flash_end=$((0x08080000)) ;;
        stm32h562) flash_end=$((0x08100000)) ;;
        stm32h723) flash_end=$((0x08100000)) ;;
        *)         flash_end=$((0x08100000)) ;;
    esac
    check "the kernel image ends at or before the program region" \
          1 "$(( $(sym "$kelf" __kernel_flash_end) <= $(sym "$kelf" __app_flash_start) ))"
    check "the kernel extension ends inside internal flash" \
          1 "$(( $(sym "$kelf" __kext_end) <= flash_end ))"
    check "system settings end inside internal flash" \
          1 "$(( $(macro settings_addr) + $(macro settings_size) <= flash_end ))"
    if [ "$BOARD" = bluepill ]; then
        check "system settings are the last bytes of internal flash" \
              "$flash_end" \
              "$(( $(macro settings_addr) + $(macro settings_size) ))"
        check "the kernel extension ends at or before system settings" \
              1 "$(( $(sym "$kelf" __kext_end) <= $(sym "$kelf" __settings_start) ))"
        check "the program region ends at or before the kernel extension" \
              1 "$(( $(sym "$kelf" __app_flash_end) <= $(sym "$kelf" __kext_start) ))"
    elif [ "$BOARD" = blackpill2 ]; then
        check "system settings start the 2 KiB page after the kernel" \
              "$((0x0800C000))" "$(macro settings_addr)"
        check "the kernel extension starts after the settings page" \
              1 "$(( $(sym "$kelf" __kext_start) >= 0x0800C800 ))"
        check "the kernel extension ends in the zero wait state flash" \
              1 "$(( $(sym "$kelf" __kext_end) <= 0x08040000 ))"
        check "the kernel extension ends at or before the program region" \
              1 "$(( $(sym "$kelf" __kext_end) <= $(sym "$kelf" __app_flash_start) ))"
        check "the program region starts on a 2 KiB page" \
              0 "$(( $(macro app_flash_addr) % 2048 ))"
        check "the program region runs to the end of flash" \
              "$flash_end" "$(sym "$kelf" __app_flash_end)"
    elif [ "$BOARD" = stm32h723 ]; then
        # The F4's order in 128 KiB sectors: settings alone in sector 1.
        check "system settings are sector 1, between the kernel and the program" \
              "$((0x08020000))" "$(macro settings_addr)"
        check "the program region starts at sector 2" \
              "$((0x08040000))" "$(macro app_flash_addr)"
        check "the program region ends where the kernel extension starts" \
              "$(sym "$kelf" __kext_start)" "$(sym "$kelf" __app_flash_end)"
        check "the kernel extension is the last 128 KiB sector" \
              "$(( flash_end - 0x20000 ))" "$(sym "$kelf" __kext_start)"
    elif [ "$BOARD" = stm32u585 ] || [ "$BOARD" = stm32h523 ] ||
         [ "$BOARD" = stm32h562 ]; then
        check "system settings start the 8 KiB page after the kernel" \
              "$((0x0800C000))" "$(macro settings_addr)"
        check "the kernel extension starts after the settings page" \
              1 "$(( $(sym "$kelf" __kext_start) >= 0x0800E000 ))"
        check "the kernel extension ends at or before the program region" \
              1 "$(( $(sym "$kelf" __kext_end) <= $(sym "$kelf" __app_flash_start) ))"
        check "the program region starts on an 8 KiB page" \
              0 "$(( $(macro app_flash_addr) % 8192 ))"
        check "the program region runs to the end of flash" \
              "$flash_end" "$(sym "$kelf" __app_flash_end)"
    else
        check "system settings are in sector 3, between the kernel and the program" \
              1 "$(( $(macro settings_addr) >= 0x0800C000 && \
                     $(macro settings_addr) + $(macro settings_size) <= 0x08010000 ))"
        check "the program region starts at sector 4" \
              "$((0x08010000))" "$(macro app_flash_addr)"
        check "the program region ends where the kernel extension starts" \
              "$(sym "$kelf" __kext_start)" "$(sym "$kelf" __app_flash_end)"
        check "the kernel extension is the last 128 KiB sector" \
              "$(( flash_end - 0x20000 ))" "$(sym "$kelf" __kext_start)"
    fi
    check "the RAM resident flash routines sit in the program RAM region" \
          1 "$(( $(sym "$kelf" __ramfunc_start) == $(macro app_load_addr) ))"

    check "the flash image is linked for the flash region" \
          "$(macro app_flash_addr)" "$(fld "$bin" 8)"
    check "the flash image declares ABI $(macro abi_version)" \
          "$(macro abi_version)" "$(fld "$bin" 4)"
    check "the flash image sets the XIP flag" 1 "$(fld "$bin" 48)"
    check "image_size is the size of the file" \
          "$(wc -c < "$bin" | tr -d ' ')" "$(fld "$bin" 16)"
    check "entry is app_main with the Thumb bit set" \
          "$(( $(sym "$elf" app_main) | 1 ))" "$(fld "$bin" 12)"
    check "data_src is the .data initialiser in flash" \
          "$(sym "$elf" __data_load__)" "$(fld "$bin" 52)"
    check "data_start is in the program RAM region" \
          "$(sym "$elf" __data_start__)" "$(fld "$bin" 56)"
    check "data_end is in the program RAM region" \
          "$(sym "$elf" __data_end__)" "$(fld "$bin" 60)"
    check "the relocation table follows the linked program image" \
          "$(sym "$elf" __image_size__)" "$(fld "$bin" 64)"
    check "the relocation table is not empty" \
          1 "$(( $(fld "$bin" 68) > 0 ))"
    check "bss_start is in the program RAM region" \
          "$(sym "$elf" __bss_start__)" "$(fld "$bin" 20)"
    check "bss_end is in the program RAM region" \
          "$(sym "$elf" __bss_end__)" "$(fld "$bin" 24)"

    # If hello had neither, the loader's copy and clear would go untested.
    check "hello has initialised data for the loader to copy" \
          1 "$(( $(fld "$bin" 60) > $(fld "$bin" 56) ))"
    check "hello has a .bss for the loader to clear" \
          1 "$(( $(fld "$bin" 24) > $(fld "$bin" 20) ))"

    # The RAM variant of the same program must still be a RAM image.
    ram="build/$BOARD/apps/hello.bin"
    check "the RAM image is still linked for the RAM region" \
          "$(macro app_load_addr)" "$(fld "$ram" 8)"
    check "the RAM image does not set the XIP flag" 0 "$(fld "$ram" 48)"

    # A program that starts no threads may run on into their stacks.  On
    # the Blue Pill they follow the RAM window; on the F4 boards they are
    # below it, so there the larger size is the window itself.
    check "the kernel and the header agree on the program RAM window" \
          "$(macro app_region_size)" \
          "$(( $(sym "$kelf" __app_ram_end) - $(sym "$kelf" __app_ram_start) ))"
    if [ "$BOARD" = bluepill ]; then
        check "the thread stacks follow the program RAM window" \
              "$(sym "$kelf" __app_ram_end)" "$(sym "$kelf" __worker_stacks)"
        check "a program without threads may use the window and their stacks" \
              "$(( $(macro app_load_addr) + $(macro app_nothreads_size) ))" \
              "$(( $(sym "$kelf" __worker_stacks) + \
                   $(sym "$kelf" __worker_count) * $(sym "$kelf" __worker_stack_size) ))"
    else
        check "a program without threads has the same window" \
              "$(macro app_region_size)" "$(macro app_nothreads_size)"
    fi
    check "hello does not say it starts no threads" 0 "$(( $(fld "$bin" 48) & 2 ))"
    forth="build/$BOARD/samples/forth.xip.bin"
    check "forth says it starts no threads" 2 "$(( $(fld "$forth" 48) & 2 ))"
    check "forth's .bss fits the window a program without threads has" \
          1 "$(( $(fld "$forth" 24) <= $(macro app_load_addr) + $(macro app_nothreads_size) ))"
    if [ "$BOARD" = bluepill ]; then
        check "forth's .bss runs on into the thread stacks" \
              1 "$(( $(fld "$forth" 24) > $(macro app_load_addr) + $(macro app_region_size) ))"
    fi

    # Simulate the loader's exact relocation pass.  flashprobe contains the
    # program-flash base both as a pointer and as a numeric safety boundary;
    # only the former may move.
    probe="build/$BOARD/samples/flashprobe.xip.bin"
    if python3 - "$probe" "$(macro app_load_addr)" "$(macro app_region_size)" <<'PY'
import struct
import sys

path, ram_base, ram_size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
image = bytearray(open(path, "rb").read())
word = lambda off: struct.unpack_from("<I", image, off)[0]
flash_base = word(8)
reloc_offset, reloc_count = word(64), word(68)
relocs = struct.unpack_from("<%dI" % reloc_count, image, reloc_offset)
assert word(16) == len(image)
assert 8 in relocs and 12 in relocs and 52 in relocs

# This is probe_unit()'s scalar lower bound.  A value-based scan would
# relocate it and make every real flash page look like kernel memory.
scalars = [off for off in range(0, reloc_offset - 3, 4)
           if word(off) == flash_base and off not in relocs]

copy = (ram_base + ram_size - len(image)) & ~3
assert max(word(60), word(24)) <= copy
delta = (copy - flash_base) & 0xffffffff
for off in relocs:
    value = word(off)
    assert flash_base <= (value & ~1) <= flash_base + reloc_offset
    struct.pack_into("<I", image, off, (value + delta) & 0xffffffff)
assert word(8) == copy
assert copy <= (word(12) & ~1) < ram_base + ram_size
assert all(word(off) == flash_base for off in scalars)
PY
    then
        check "flashprobe pointers relocate to RAM without changing its flash boundary" 1 1
    else
        check "flashprobe pointers relocate to RAM without changing its flash boundary" 1 0
    fi

    # The image `make flash PROGRAM=hello` writes: kernel, erased gap, then
    # the program at the region the linker reserved, erased through the end.
    kbin="build/$BOARD/freya.bin"
    packed="$OUT/freya+hello.bin"
    off=$(( $(macro app_flash_addr) - 0x08000000 ))
    end=$(( $(macro app_flash_addr) + $(macro app_flash_size) ))
    ksize=$(wc -c < "$kbin" | tr -d ' ')
    asize=$(wc -c < "$bin" | tr -d ' ')
    if python3 tools/pack_image.py \
            --kernel "$kbin" --app "$bin" \
            --load-addr "$(macro app_flash_addr)" --region-end "$end" \
            --out "$packed" >/dev/null; then
        got=$(cmp -s -n "$ksize" "$kbin" "$packed" && echo 1 || echo 0)
        check "the packed image starts with the kernel" 1 "$got"
        gap=$(dd if="$packed" bs=1 skip="$ksize" count=$((off - ksize)) status=none \
              | tr -d '\377' | wc -c | tr -d ' ')
        check "the gap up to the program region is erased" 0 "$gap"
        dd if="$packed" bs=1 skip="$off" count="$asize" status=none > "$OUT/slot.bin"
        got=$(cmp -s "$OUT/slot.bin" "$bin" && echo 1 || echo 0)
        check "the program sits at the start of its flash region" 1 "$got"
        tail=$(dd if="$packed" bs=1 skip=$((off + asize)) status=none \
               | tr -d '\377' | wc -c | tr -d ' ')
        check "the rest of the program region is erased" 0 "$tail"
        check "the packed image covers the kernel and the whole program region" \
              "$(( end - 0x08000000 ))" "$(wc -c < "$packed" | tr -d ' ')"
        kext_addr=$(sym "$kelf" __kext_start)
        if python3 tools/fwsum.py \
                --span "0x08000000:$kbin" \
                --span "$kext_addr:build/$BOARD/freya-kext.bin" \
                --settings-out "$OUT/settings.bin" >/dev/null; then
            want=$(python3 tools/fwsum.py \
                --span "0x08000000:$kbin" \
                --span "$kext_addr:build/$BOARD/freya-kext.bin" \
                --store-addr 0)
            check "both settings copies hold the firmware sum" \
                  "$((want))" "$(fld "$OUT/settings.bin" "$(macro cksum_off)")"
            check "the second settings copy holds the same firmware sum" \
                  "$((want))" "$(fld "$OUT/settings.bin" $(( $(macro settings_block) + $(macro cksum_off) )))"
            check "both settings copies start with the marker" \
                  "$((0x54455346))" "$(fld "$OUT/settings.bin" 0)"
            check "the second settings copy starts with the marker" \
                  "$((0x54455346))" "$(fld "$OUT/settings.bin" "$(macro settings_block)")"
        else
            check "writing the firmware sum into system settings" 1 0
        fi
    else
        check "packing the kernel and hello.xip.bin" 1 0
    fi

    # AUTOSTART=1 writes the magic into both settings copies.  The packed
    # program image does not contain that area.
    kext_addr=$(sym "$kelf" __kext_start)
    if python3 tools/fwsum.py \
            --span "0x08000000:$kbin" \
            --span "$kext_addr:build/$BOARD/freya-kext.bin" \
            --autostart --settings-out "$OUT/settings-on.bin" >/dev/null; then
        check "auto-start is on in the first settings copy" \
              "$(( 0x31415946 ))" "$(fld "$OUT/settings-on.bin" 4)"
        check "auto-start is on in the second settings copy" \
              "$(( 0x31415946 ))" "$(fld "$OUT/settings-on.bin" $(( $(macro settings_block) + 4 )))"
    else
        check "writing system settings with auto-start on" 1 0
    fi

    if python3 tools/pack_image.py \
            --kernel "$kbin" --app "$ram" \
            --load-addr "$(macro app_flash_addr)" --region-end "$end" \
            --out "$OUT/rejected.bin" >/dev/null 2>&1; then
        check "a RAM image is refused" 1 0
    else
        check "a RAM image is refused" 1 1
    fi

    # make flash SCRIPT= stores the text the way install does: 'SCRT',
    # the length, the bytes, and a NUL.  The auto-start flag is in the
    # settings area, not in this image.
    printf 'echo fromflash\n' > "$OUT/boot.sh"
    packed_sh="$OUT/freya+boot.bin"
    if python3 tools/pack_image.py \
            --kernel "$kbin" --script "$OUT/boot.sh" \
            --load-addr "$(macro app_flash_addr)" --region-end "$end" \
            --out "$packed_sh" >/dev/null; then
        check "a packed script has the SCRT magic" \
              "$(( 0x54524353 ))" "$(fld "$packed_sh" "$off")"
        check "a packed script records its text length" \
              15 "$(fld "$packed_sh" $((off + 4)))"
        printf 'echo fromflash\n\0' > "$OUT/expect-script.bin"
        dd if="$packed_sh" bs=1 skip=$((off + 8)) count=16 status=none \
            > "$OUT/got-script.bin"
        got=$(cmp -s "$OUT/expect-script.bin" "$OUT/got-script.bin" && echo 1 || echo 0)
        check "a packed script is the text plus a NUL" 1 "$got"
        tail=$(dd if="$packed_sh" bs=1 skip=$((off + 8 + 16)) status=none \
               | tr -d '\377' | wc -c | tr -d ' ')
        check "the rest of the region stays erased after a script" 0 "$tail"
    else
        check "packing a shell script into the program region" 1 0
    fi

    if python3 tools/pack_image.py \
            --kernel "$kbin" --script "$kbin" \
            --load-addr "$(macro app_flash_addr)" --region-end "$end" \
            --out "$OUT/rejected-script.bin" >/dev/null 2>&1; then
        check "a binary file is refused as a script" 1 0
    else
        check "a binary file is refused as a script" 1 1
    fi
fi

echo
echo "================= shell on Linux ================="
# The same interpreter built as a Linux program, with the hardware left out.
if make -s --no-print-directory -C linux >/dev/null; then
    sh linux/test.sh build/linux/fsh || status=1
else
    echo "  FAIL  build/linux/fsh did not build"
    status=1
fi

echo
echo "$checks checks, $fails failures"

echo
if [ $status -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "TESTS FAILED"
fi
exit $status
