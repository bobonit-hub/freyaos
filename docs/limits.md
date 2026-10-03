# Notes and limits

What Freya does not do, and the edges of what it does: the filesystem, the
clock, one program at a time, pins and interrupts, flash, and the console
during an install. Limits that belong to one board — the Blue Pill's SRAM,
the F4 sector layout, the flash size register — are in
[boards.md](boards.md).

* FAT12 is not supported (cards that small are rare); FAT16 and FAT32 are.
* Long file names are read and written as ASCII; UTF-16 beyond ASCII becomes
  `?` on display.
* File timestamps come from a software clock that starts at 2026-01-01 and is
  set with `date`. A build with `RTC=ds3231` also keeps that time on a DS3231
  wired to PB6 (SCL) and PB7 (SDA), and one with `RTC=internal` in the
  chip's own calendar RTC ([rtc.md](rtc.md)), for the years 2000 to 2099.
* One program at a time. Its main thread is the shell's stack; any thread
  it creates has a 1 KiB stack of its own. There is no MPU isolation.
* A pin interrupt is one of sixteen hardware lines, and line *n* serves pin
  *n* of one port at a time, so PA0 and PB0 cannot both have one. Three
  timers serve both the periodic interrupts and PWM, so a program wanting
  both has three between them, and the channels of one timer share its
  frequency. PWM reaches the eight pins that mean the same thing on every
  board, not every pin either chip could route. ADC1 provides synchronous
  12-bit reads; input capture is not implemented. Handlers all run at one
  priority and never nest, and only the console sits above them — which is
  what makes Ctrl-C work against a handler that loops.
* Every supported board has at least 128 KiB of flash; the Blue Pill size
  register and `samples/flashprobe` are in
  [boards.md](boards.md#flash-size).
* On the Blue Pill the 20 KiB of SRAM is the real limit, not the 128 KiB of
  flash; see [boards.md](boards.md#blue-pill-stm32f103c8t6).
* The F4 boards give the program flash region everything between the kernel
  and the kernel extension; the sector layout is in
  [boards.md](boards.md#the-f4-sector-layout).
* One program in flash at a time, as with RAM. `install` erases and rewrites
  the region; `uninstall` erases it. `make flash` writes only the kernel and
  leaves an installed program alone, which is convenient but does mean a stale
  image can outlive the kernel that installed it — the loader checks the
  header rather than trusting it. `make flash PROGRAM=<app>` is the other
  choice: the image it writes covers the whole program region, so the program
  packed in replaces whatever was there.
* Console input is lost while flash is being erased or programmed, and the
  software clock loses about the duration of the install. Both follow from
  there being no read-while-write, and neither is worth putting the console
  interrupt handler in RAM to avoid.
* `stop` with no name interrupts a program that is running, and at the
  prompt unloads the image and reports how the last run ended. `stop` with
  a thread name stops that thread. Ctrl-C stops the whole run.
