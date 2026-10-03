#!/bin/sh
# Writes the bootloader, the partition table and the firmware ELF given as
# $1 to the ESP32-C6 on $ESPPORT (default /dev/ttyACM0), then opens the
# serial monitor (not with FLASH_ONLY=1).  `cargo run --release` calls
# it.  It needs the activated ESP-IDF environment for esptool.py and the
# monitor.
set -e
elf=$1
out=$(dirname "$elf")
port=${ESPPORT:-/dev/ttyACM0}

esptool.py --chip esp32c6 elf2image -o "$out/freya-c6.bin" "$elf"
esptool.py --chip esp32c6 -p "$port" -b 460800 write_flash \
    0x0 "$out/build/bootloader.bin" \
    0x8000 "$out/build/partition-table.bin" \
    0x10000 "$out/freya-c6.bin"
[ -n "$FLASH_ONLY" ] || exec python -m esp_idf_monitor -p "$port" --target esp32c6 "$elf"
