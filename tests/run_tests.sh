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

CC=${CC:-cc}

# The sources under test pull in freya.h, which pulls in the board header;
# any board will do on the host, so use the one being built.
BOARD=${BOARD:-blackpill}
BOARD_DEF="-DFREYA_BOARD_$(echo "$BOARD" | tr '[:lower:]' '[:upper:]')"

CFLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-builtin \
        -Iinclude -Isrc -Iboards/$BOARD $BOARD_DEF"

# shellcheck disable=SC2086
$CC $CFLAGS tests/host_fat_test.c src/fat.c src/string.c src/print.c \
    -o "$OUT/hosttest"

# shellcheck disable=SC2086
$CC $CFLAGS tests/host_xmodem_test.c src/xmodem.c src/fat.c src/fs.c \
    src/string.c src/print.c -o "$OUT/hostxmodem"

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
if [ $status -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "TESTS FAILED"
fi
exit $status
