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
        -Iinclude -Isrc -Iboards/$BOARD $BOARD_DEF -DFREYA_HOST"

# shellcheck disable=SC2086
$CC $CFLAGS tests/host_fat_test.c src/fat.c src/log.c src/string.c src/print.c \
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

# The exit status rule: how a program's code, a Ctrl-C and a fault each
# become the number the shell reports as '$?'.
echo
echo "================= exit status ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_exit_test.c -o "$OUT/hostexit"
"$OUT/hostexit" || status=1

# Pins, timers and PWM: the pin numbering a program uses, the two
# dividers compiled for the host and asked for every period and every
# frequency they accept, and the board's table of PWM channels.
echo
echo "================= pins, timers and PWM ================="
# shellcheck disable=SC2086
$CC $CFLAGS tests/host_irq_test.c -o "$OUT/hostirq"
"$OUT/hostirq" || status=1

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
check "the header is 64 bytes" "$(macro hdr_size)" 64

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
    check "the kernel and the header agree on the autostart address" \
          "$(macro autostart_addr)" "$(sym "$kelf" __autostart_start)"
    check "the kernel and the header agree on the autostart size" \
          "$(macro autostart_size)" \
          "$(( $(sym "$kelf" __autostart_end) - $(sym "$kelf" __autostart_start) ))"
    check "the auto-start slot is 128 bytes" "$(macro autostart_size)" 128
    check "the ram-dump flag is the third word of the auto-start slot" \
          "$(macro ramdump_off)" 8
    check "the auto-start slot is 128-byte aligned" \
          0 "$(( $(macro autostart_addr) % 128 ))"
    check "the program flash region is 128-byte aligned" \
          0 "$(( $(macro app_flash_addr) % 128 ))"
    check "the autostart slot sits immediately before the program flash region" \
          "$(macro app_flash_addr)" "$(sym "$kelf" __autostart_end)"
    check "the kernel image ends below the autostart slot" \
          1 "$(( $(sym "$kelf" __kernel_flash_end) <= $(sym "$kelf" __autostart_start) ))"
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
    else
        check "packing the kernel and hello.xip.bin" 1 0
    fi

    # AUTOSTART=1 writes the magic at the slot; the rest of the gap stays erased.
    packed_as="$OUT/freya+hello+autostart.bin"
    slot=$(( $(macro autostart_addr) - 0x08000000 ))
    slot_end=$(( $(macro autostart_addr) + $(macro autostart_size) ))
    if python3 tools/pack_image.py \
            --kernel "$kbin" --app "$bin" \
            --load-addr "$(macro app_flash_addr)" --region-end "$end" \
            --slot-addr "$(macro autostart_addr)" --slot-end "$slot_end" \
            --autostart --out "$packed_as" >/dev/null; then
        check "the packed auto-start magic is at the slot" \
              "$(( 0x31415946 ))" "$(fld "$packed_as" "$slot")"
        gap=$( {
            dd if="$packed_as" bs=1 skip="$ksize" count=$((slot - ksize)) status=none
            dd if="$packed_as" bs=1 skip=$((slot + 4)) count=$((off - slot - 4)) status=none
          } | tr -d '\377' | wc -c | tr -d ' ')
        check "the rest of the gap stays erased when auto-start is packed" 0 "$gap"
    else
        check "packing hello.xip.bin with --autostart" 1 0
    fi

    if python3 tools/pack_image.py \
            --kernel "$kbin" --app "$ram" \
            --load-addr "$(macro app_flash_addr)" --region-end "$end" \
            --out "$OUT/rejected.bin" >/dev/null 2>&1; then
        check "a RAM image is refused" 1 0
    else
        check "a RAM image is refused" 1 1
    fi
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
