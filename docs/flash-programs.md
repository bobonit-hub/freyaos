# Flash-resident programs on the Blue Pill

The design note this feature was built from. It is kept because it records
*why* the arrangement is what it is — the reasoning behind the memory split,
the ABI extension and the RAM-resident flasher — which the code itself can
only hint at.

It was implemented as written, with four deviations, all of them noted in
place below: the region is sized 40/24 as recommended; `.data` is
re-initialised per *run* rather than per load, because a flash image is
immutable and so it can be; `md <addr>` was not added, because `install`
verifies the whole region against the file and eyeballing a hex dump adds
nothing; and the kernel grew 4 KiB rather than the 1.5 KiB guessed at here,
leaving 7 KiB of headroom instead of 11.

**The boundary has since moved.** The kernel spent the headroom this note
allowed, then spent it again: the split is 48 KiB of kernel plus a 128-byte
slot at `0x0800C000`. The program region grew with the flash along the way —
every supported board has at least 128 KiB — so a Blue Pill program gets
29568 bytes, through `0x080133FF`, rather than the 20352 bytes that fit in a
64 KiB part. The last 4 KiB of what used to be the program region is the
virtual machine, in the kernel extension. Everything below describes 40/24,
which is where it started;
`include/freya_api.h` and `boards/bluepill/freya.ld` are where it is now, and
`make test` compares the two.

**Execution has since moved to RAM.** ABI 3 appends an explicit relocation
table to each `.xip.bin`. The loader copies an installed image to the top of
the program RAM window when it fits, leaves its `.data` and `.bss` at the
bottom, and rebases every recorded code or constant pointer before calling it.
Numeric values that happen to look like flash addresses are not changed.
Larger installed programs retain the XIP behaviour described below.

Today a Freya program is a RAM image: `load` copies the file from the card
into the program region, zeroes its `.bss` and `run` branches into it. On the
Blue Pill that region is 8 KiB, because 8 KiB is what a 20 KiB SRAM can spare
after the kernel's `.data`/`.bss`, a heap worth having and a 6 KiB stack. The
64 KiB of flash, by contrast, is ~36 KiB used and the rest idle.

This note describes making a program live in that idle flash instead: copied
there once from the card, executed in place, surviving a power cycle. On the
Blue Pill it raises the ceiling on program size from 8 KiB to around 24 KiB and
makes a program runnable with no card in the socket at all.

## What is already true

Executing from flash needs nothing. The Cortex-M3 is Harvard only in its bus
topology — separate I-Code and D-Code paths into the flash controller — over a
single unified address map. An address in `0x0800xxxx` is fetchable exactly the
way an address in `0x2000xxxx` is, so `app_run()`'s

    entry = (entry_fn)(uintptr_t)(g_app.entry | 1UL);

works unchanged whether `g_app.entry` points into SRAM or into flash. There is
no mode to enter and no window to map.

Fault containment also carries over untouched. `freya_fault_handler()` decides
between a program fault and a kernel panic with

    int in_app = g_app.running && from_thread;

which is a question about state, not about where the faulting PC lies. A
flash-resident program that touches `0xF0000000` is reported and killed the same
way `spin.bin fault` is today.

What is missing is everything on the write side: Freya has no flash programming
code at all. `FLASH_TypeDef` is declared in `boards/bluepill/stm32f103.h`, but
only `ACR` is ever touched, for wait states and the prefetch buffer.

## Flash memory map

The F103C8T6 erases in 1 KiB pages — 64 of them, page *n* at
`0x08000000 + n * 1024`. That is a considerably kinder granularity than the
Black Pill's, where the sectors run 16, 16, 16, 16, 64, 128, 128, 128 KiB and
the smallest erasable unit past the kernel is 64 KiB.

The kernel was 29732 bytes when this was written and is 34048 now that the
flash driver and the installer are in it, so it ends inside page 33. The
proposal is to split the flash 40/24 and reserve the top 24 KiB, pages 40 to
63, for one program image:

```
0x08000000  +--------------------------------+
            |  Freya kernel (34048 used)     |  40 KiB, pages 0..39
            |  ~7 KiB headroom               |
0x0800A000  +--------------------------------+
            |  program flash region          |  24 KiB, pages 40..63
0x08010000  +--------------------------------+

0x20000000  +--------------------------------+
            |  kernel .data + .bss (~4 KiB)  |
            |  system heap (~2 KiB)          |
0x20001800  +--------------------------------+
            |  program RAM region (8 KiB)    |  .data + .bss only, for a
0x20003800  +--------------------------------+  flash-resident program
            |  main stack (6 KiB)            |
0x20005000  +--------------------------------+
```

The RAM map does not change. For a flash-resident program the existing 8 KiB
window stops holding the image and holds only its `.data` and `.bss`, which is
the whole point: code and constants move to flash, and the RAM budget buys
variables instead of instructions.

**Decision to review.** 40/24 leaves the kernel 11 KiB of growth and gives a
program 3× what it has now. 48/16 is more conservative on the kernel and only
2× on the program; 32/32 is the other way about and leaves barely 2 KiB of
kernel headroom, which the flash driver itself will eat into. 40/24 is the
recommendation.

**The upper 64 KiB is part of the map.** Most STM32F103C8 dies are physically
the 128 KiB part, and `FLASHSIZE_BASE` still often reports 64 KiB. Freya no
longer stays inside that report. Every supported board is built as having at
least 128 KiB. The Blue Pill program region runs to `0x080133FF`; the last
51 KiB is the kernel extension. The size used by `sysinfo`,
`meminfo` and `flashdump` is at least 128 KiB.

`samples/flashprobe` still walks erase units above that floor, programs a
256-byte block into each one, reads it back and erases it again, and prints
where the flash actually stops. It only ever writes to a unit that reads blank.
The kernel does not resize itself from the answer — the region bounds are
build-time constants, checked against the linker script at boot.

## ABI changes

A flash-resident program cannot be one contiguous image the way a RAM program
is, because `.data` has to be writable. It needs the ordinary split: `.text`
and `.rodata` execute from flash, `.data` is copied from a flash initialiser
into RAM, `.bss` is zeroed in RAM. That is three addresses the header does not
currently carry.

`freya_app_header_t` is 48 bytes today. The proposal appends to it and leaves
every existing offset alone:

```c
typedef struct {
    uint32_t magic;        /* FREYA_APP_MAGIC                         */
    uint32_t abi_version;  /* 1 = RAM image, 2 = either               */
    uint32_t load_addr;    /* address the image was linked for        */
    uint32_t entry;        /* absolute address of app_main (thumb)    */
    uint32_t image_size;   /* bytes of the file image                 */
    uint32_t bss_start;    /* zero initialised region, absolute       */
    uint32_t bss_end;
    uint32_t stack_need;   /* bytes of stack the program requires     */
    char     name[16];     /* informational, NUL padded               */
    /* ---------------------------------------- appended in ABI 2 --- */
    uint32_t flags;        /* FREYA_APP_F_XIP                         */
    uint32_t data_src;     /* flash address of the .data initialiser  */
    uint32_t data_start;   /* RAM destination, absolute               */
    uint32_t data_end;
} freya_app_header_t;      /* 64 bytes */
```

`FREYA_APP_F_XIP` set means `load_addr` is a flash address and the three new
fields are meaningful. Clear means the image is exactly what it is today, and
the new fields are zero.

Because the v1 header is a byte-for-byte prefix of the v2 header, the version
check can stay permissive where it costs nothing: accept ABI 1 or 2 for a RAM
image, require 2 for an XIP image. Every `hello.bin` already sitting on a card —
including `/autorun.bin` — keeps working. Without that concession the ABI bump
silently invalidates them, and the failure appears as a confusing message at
boot.

The `.data` copy belongs in the loader, not in the program. `app_load()`
already zeroes `.bss` on the program's behalf, so doing the `.data` copy in the
same place is consistent, keeps `apps/common/app_start.c` free of a
hand-written crt0, and means a program cannot get its own startup wrong.
`app_start.c` only grows the new header fields, filled in from linker symbols.

One consequence worth stating plainly: a flash-resident program's `.data` is
re-initialised on every `run`, so running it twice does not carry state over.
A RAM program cannot behave that way — its `.data` *is* the loaded image, and
reinitialising means loading it again — so the two differ here, and the
asymmetry is in `app_run()` with a comment saying why.

## Two linker scripts

`boards/bluepill/app.ld` stays as it is, for RAM images. A new
`boards/bluepill/app_flash.ld` describes the split:

```
MEMORY
{
    APPFLASH (rx)  : ORIGIN = 0x0800A000, LENGTH = 24K
    APPRAM   (rwx) : ORIGIN = 0x20001800, LENGTH = 8K
}
```

with `.header`, `.text` and `.rodata` in `APPFLASH`, then `.data` in `APPRAM`
at a load address in `APPFLASH` (the same `AT` trick `freya.ld` already uses for
the kernel's own `.data`), then `.bss` in `APPRAM`. `__data_load__`,
`__data_start__`, `__data_end__`, `__bss_start__` and `__bss_end__` feed the
header, and two `ASSERT`s catch a program that outgrows either region.

The Makefile builds both variants for every app and sample — it is two link
steps against the same objects, and having `hello` available both ways makes the
whole thing testable without writing a new test program. Suggested naming:
`build/bluepill/apps/hello.bin` and `build/bluepill/apps/hello.xip.bin`.

**A seam to be careful about.** The region addresses will exist in three places
that cannot include each other: `freya.ld`, `app_flash.ld` and the
`FREYA_BOARD_*` block in `include/freya_api.h`. `freya.ld` can `ASSERT` its own
consistency, but nothing checks it against the header. The cheap fix is a
runtime check at boot — compare the linker symbol against the macro and panic on
a mismatch — which is in the same spirit as the existing linker `ASSERT`s and
costs a few bytes.

## The flash driver

A new `src/flash.c`, with the register bits added to
`boards/bluepill/stm32f103.h` (`FLASH_SR`: `BSY`, `PGERR`, `WRPRTERR`, `EOP`;
`FLASH_CR`: `PG`, `PER`, `MER`, `STRT`, `LOCK`; and the two unlock keys
`0x45670123` / `0xCDEF89AB`).

The F1 sequence is short. Unlock by writing the keys to `KEYR`. To erase: wait
for `BSY` to clear, set `PER`, put the page address in `AR`, set `STRT`, wait
for `BSY`, check `EOP` and the error bits, clear `PER`. To program: set `PG`,
store a **halfword** — the F1 has no byte or word programming — wait for `BSY`,
check, clear `PG`. Lock by setting `LOCK`.

Timings from the datasheet: a page erase is around 20 ms typical and 40 ms
worst case, a halfword program 40 to 70 µs. Installing a full 24 KiB region is
therefore roughly 24 × 20 ms of erase plus 12288 × 60 µs of programming, call it
1.5 to 2 seconds. Only the pages an image actually needs get touched.

The driver must refuse, unconditionally and in one place, any address outside
`0x0800A000 .. 0x0800FFFF`. A mistake in a page-address calculation is the
difference between a failed install and a bricked board, and this is the check
that makes the difference cheap. The flash write path must also never be
reachable from `freya_api_t`: a program cannot be allowed to rewrite the kernel
it is running on.

## The bus-stall problem

The F103 has no read-while-write. While `BSY` is set the flash controller
stalls bus reads, which means code fetching from flash simply stops until the
operation finishes.

This has two separate consequences, and it is worth not conflating them.

**Correctness.** Whether the core may fetch instructions from flash while the
FPEC is busy is not something to rely on from a reading of the manual. In
practice the bus stalls rather than faults, and ST's own HAL programs F1 flash
from code executing in flash. Still, the robust arrangement is to put the
erase/program inner loop in SRAM. The tidy way to do that here costs no
permanent RAM: give it a `.ramfunc` output section linked for
`__app_ram_start` with its load address in flash, and copy it there when an
install begins. An install cannot overlap a loaded program anyway, so the
program RAM region is free scratch at exactly the moment it is needed — which
matters when the alternative is spending 250-odd bytes of a 2 KiB heap
permanently.

**Responsiveness, which the ramfunc does not fix.** The USART2 interrupt
handler lives in flash, so it stalls with everything else even if the flasher
does not. A 40 ms stall overruns the receive data register and loses console
bytes regardless of where the flasher runs. So:

* mask interrupts around each individual operation rather than across the whole
  install, so the handler gets to drain the ring buffer between pages;
* flush the receive buffer when the install finishes, and say on the console
  that input during an install is dropped;
* accept that the software clock loses roughly the install duration, since
  SysTick latches only one pending tick. About a second of drift on a full-region
  install, on a clock that already starts at 2026-01-01 and is set by hand.

Wrap the whole install in `app_guard_enter()` / `app_guard_leave()`. That
mechanism exists so a Ctrl-C cannot land in the middle of an SD block write, and
an abort between the erase and the program of a page is the same class of
problem with a worse outcome.

## Shell and boot

* `install <file>` — validate the header, erase the pages the image needs,
  stream it from the card 512 bytes at a time, verify by read-back, report
  pages and bytes written. Skip the write entirely if the region already holds
  an identical image; flash endurance is 10k cycles and there is no reason to
  spend one per `run`.
* `uninstall` — erase the region.
* `run @flash [args...]` — run what is installed. Treating the installed image
  as a pseudo-path rather than adding a command means `load`, `run`, `stop` and
  the `g_app` bookkeeping all work as they do now.
* `runflash [args...]` — the same run, with `@flash` filled in. A program
  packed into the module by `make flash PROGRAM=<app>` is started this way,
  and so is one installed from the card.
* `meminfo` — one more line for the program flash region and what occupies it.
* `boot_autorun()` — try the installed image when `/autorun.bin` is absent, or
  when there is no card at all. This is the part that makes the feature feel
  like an operating system rather than a trick: a Blue Pill with nothing in the
  card socket boots Freya and starts a program.

A verification aid seemed worth adding at the same time — there is no way to
look at an arbitrary address from the shell, only `hexdump` on a file — but it
was not, in the end. `install` verifies each halfword as it writes it and then
reads the whole region back against the file, which is a stronger statement
than a hex dump a human has to check, and `meminfo` reports what the installed
header says. `md <addr> [len]` can be added when something actually wants it.

## Testing

The on-target path cannot be reached by the host tests, which is a real gap and
the main argument for keeping the driver small and its bounds check absolute.
What can be tested off-target is the part most likely to be quietly wrong — the
agreement between the linker script and the header — and that is what the
"program image layout" pass in `tests/run_tests.sh` does: it builds
`hello.xip.bin`, reads the section addresses and linker symbols out of the ELF,
and asserts they match the header fields word for word, then compares the
regions in `include/freya_api.h` against the ones the linker scripts describe.
Twenty checks, and between them they catch every copy-paste error in a memory
map. `hello` was given a `.data` variable and a `.bss` variable specifically so
that the loader's copy and clear are on a path something exercises; two of the
checks assert it still has them. Six more pack `hello.xip.bin` behind the
kernel the way `make flash PROGRAM=hello` does, and check that the program
lands at the region address, that the kernel is undisturbed, that the gap and
the remainder of the region are erased, and that a RAM image is refused. A
second pack with `--autostart` checks that the magic sits at the slot and
that the rest of the gap is still erased.

What is left to verify on the board, in order: `install` a program and confirm
the read-back verifies; power-cycle and `run @flash`; check that a program with
initialised globals sees correct values on a second consecutive run (the
`.data` re-copy — `hello` prints exactly this); run `spin.xip.bin`'s fault case
and confirm the shell still comes back; confirm an ABI 1 `hello.bin` built
before this change still loads and runs from RAM; and confirm `install` of an
image already in flash writes nothing.

## Order of work

The flash driver with its bounds check and the `.ramfunc` plumbing came first
and is independently verifiable, with nothing yet executing from the region.
The ABI extension, the second linker script and the loader's `.data` copy came
second. `run @flash` was then a small change, and autorun-from-flash smaller
still.

The risk is concentrated almost entirely in the first step, and the way to keep
it there is to not let a single flash write happen outside one function that
has already checked its bounds.
