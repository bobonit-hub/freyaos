# More program RAM and flash on the Blue Pill

A proposal. It lists ways to make the Blue Pill's program regions
larger, what each one gains, and what it costs. R1 has been built, and
two of the F2 items have been done; each says so in place. The rest are
still proposals.

## Where the board stands

Measured from `make BOARD=bluepill` at 0a41c2b:

| Region                                  | Size     | Used     | Free        |
|-----------------------------------------|----------|----------|-------------|
| Kernel image `0x08000000`               | 48 KiB   | 48916 B  | 236 B       |
| Program flash `0x0800C000`              | 24 KiB   | -        | -           |
| Kernel extension `0x08012000`           | 55 KiB   | 52716 B  | 3604 B      |
| System settings `0x0801FC00`            | 1 KiB    | -        | -           |
| Kernel `.data` + `.bss` `0x20000000`    | 7 KiB    | 6548 B   | 616 B heap  |
| Program RAM window `0x20001C00`         | 7 KiB    | -        | -           |
| Thread stacks `0x20003800`              | 2 KiB    | -        | -           |
| Shell stack `0x20004000`                | 2560 B   | -        | -           |
| Interrupt stack `0x20004A00`            | 1536 B   | -        | -           |

The totals are fixed: 128 KiB of flash and 20 KiB of SRAM. A
kilobyte more for a program is a kilobyte less for the kernel, the
extension or a stack. Both kernel images are close to full, so every
proposal for more space has to take it from something.

The largest kernel buffers in `.bss`:

| Symbol         | File          | Bytes |
|----------------|---------------|-------|
| `s_hist`       | `shell.c`     | 1120  |
| `s_fn`         | `shell.c`     | 768   |
| `s_rx`         | `uart.c`      | 512   |
| `s_fat`        | `fat.c`       | 512   |
| `s_buf`        | `fat.c`       | 512   |
| `s_line`       | `shell.c`     | 320   |
| `s_idle_stack` | `thread.c`    | 256   |
| `s_thr`        | `thread.c`    | 240   |

## Program RAM

### R1. A program without threads gets the thread stacks: 7 to 9 KiB

The two 1 KiB thread stacks start at `0x20003800`, the first byte after the
program window. The two areas are contiguous, so a program that starts no
threads could use all of `0x20001C00 .. 0x20003FFF`.

**Built.** How it came out:

- A header flag, `FREYA_APP_F_NOTHREADS`, and `FREYA_APP_NOTHREADS_SIZE`
  in `include/freya_api.h`: 9 KiB on the Blue Pill. On the F4 boards the
  thread stacks are below the window, so there the size is the window and
  the flag only refuses threads.
- A program opts in through `NOTHREADS` in the Makefile. That compiles it
  with `-DFREYA_APP_NOTHREADS`, which sets the flag in `app_start.c`, and
  links it with `__app_nothreads__` defined, which lets `app.ld` and
  `app_flash.ld` allow 9 KiB instead of 7. forth is the one program
  listed. Its dictionary grew from 6144 to 8192 bytes.
- The loader takes each image's limit from its flags, and records in
  `g_app.ram_end` how far the program's RAM reaches. An installed image
  copied to RAM goes to the top of the 7 KiB window when it fits there,
  and to the top of the 9 KiB only when it does not.
- `thread_create()` returns `FREYA_ERR_UNSUPPORTED` to a flagged
  program.
- The shell's scratch, 816 bytes, lives in the thread stacks. While a
  program's RAM reaches into them (`app_holds_thread_stacks()`):
  - `shell_source_capture()` returns `FREYA_ERR_BUSY`.
  - The console calls allowed during a run are refused. Ctrl-C still
    stops the program.
  - If the shell needs its scratch while the program is only loaded, it
    unloads the program and says so. `run forth.bin` works as one
    command; `load` and then `run` may not.
- The kernel image lost 96 bytes to this (140 left). The rest went into
  the extension.

### R2. Measure the stacks, then shrink them

The reset handler paints the stacks with `0xDEADBEEF`, and `meminfo` reports
the high-water mark. Run the heaviest work the board does and record the
peaks:

- nested shell scripts and functions,
- FAT writes and directory walks,
- `samples/edit`, `samples/tetris` and the BASIC samples,
- pin interrupts and timer events arriving together.

The interrupt stack is 1536 B. Cortex-M3 handlers in Freya are short, so
768 to 1024 B is likely enough. The shell stack is also the program stack,
so it can only shrink as far as the largest `stack_need` among the
samples.

Each 512 B saved can move `__app_ram_end` up and widen the window.

### R3. Smaller kernel buffers on this board

| Buffer         | Now      | Proposal                                    | Saves   |
|----------------|----------|---------------------------------------------|---------|
| `s_hist`       | 1120 B   | 3 lines, or a ring of variable-length lines | 500-650 |
| `s_fn`         | 768 B    | fewer function slots                        | ~400    |
| `s_rx`         | 512 B    | 256 B; the ring is interrupt-driven         | 256     |
| `s_buf`+`s_fat`| 1024 B   | one shared sector buffer                    | 512     |

About 1.5 to 2 KiB in total. A shared FAT buffer makes allocation slower,
because a cluster chain walk and a directory read then evict each other.

The space can go to one of two places:

- **The heap.** It is 616 B now, small enough that a script or a file
  operation can run out. Giving it the space costs no compatibility.
- **The program window.** `FREYA_APP_LOAD_ADDR` moves down, and every
  Blue Pill program has to be rebuilt. ABI 3 relocates code and constant
  pointers into flash only, not into RAM, so an old RAM image does not
  rebase itself.

Recommended: the first kilobyte to the heap, the rest to the window, and
both in one change so programs are rebuilt once.

### R4. Lend the shell's line buffers to the program

Not recommended. `s_hist`, `s_line` and the script buffers are idle while a
program runs, but they are not contiguous with the window, and Ctrl-C,
`status` and a program run from a script all need them back intact.

### R5. Use XIP for large programs

No kernel change. A program installed in flash keeps its code and
constants there and uses the RAM window for `.data` and `.bss` only. The
loader copies an installed image to RAM only when it fits, which is a
matter of speed. The documentation and the sample READMEs could say so
more plainly: on this board, a program that needs more than a few KiB of
variables should be installed, not loaded.

## Program flash

### F1. Give the extension's slack to the program region: 24 to 27 KiB

The extension uses 52716 of its 56320 bytes. Shrunk to 52 KiB and moved
up to `0x08012C00`, it still ends at `0x0801FC00`, and the program region
grows to `0x0800C000 .. 0x08012BFF`.

- `boards/bluepill/freya.ld`: `KEXT` origin and length,
  `__app_flash_end`.
- `boards/bluepill/app_flash.ld`: `APPFLASH` length.
- `include/freya_api.h`: `FREYA_APP_FLASH_SIZE`. `make test` and the boot
  check compare it with the linker script.
- An existing `.xip.bin` keeps its link address and still installs.
- Flashing the extension uses the new address (`BOOTLOADER_KEXT` takes it
  from the build).

The cost is the extension's headroom: about 530 B is left. On its own this
is only worth doing if nothing new is planned for the extension on this
board. Paired with F2 it is safe.

### F2. Build fewer features into the Blue Pill extension

The board already leaves out the VM, Ascon and the compressor with
`BOARD_VM`, `BOARD_AEAD` and `BOARD_COMPRESS`. XTEA has since been
removed from every board; its slot in the service table stays and
returns `FREYA_ERR_UNSUPPORTED`. Candidates
for the same treatment, each to be measured from
`build/bluepill/freya.map` before it is chosen:

- XMODEM. A card can be moved to a PC instead.
- The kernel's copy of `softfp`. **Done.** `BOARD_SHELL_FLOAT` is 0 on
  the Blue Pill: the shell has no float values there, and a float
  literal, `float()`, `sin()`, `cos()`, `pi()` or `%f` reports that floats
  are not supported. `src/softfp.c` is built into no kernel now, only
  into programs. The extension went from 53032 to 49528 bytes, so 6792
  are free.
- The DS3231 driver.
- `net.o` and `curl.o`, if they still emit code with
  `BOARD_NET_SUPPORTED` at 0.

Every whole kilobyte freed is a kilobyte added to the program region by
moving the extension up, as in F1. Pages are 1 KiB, so nothing is lost
to rounding.

### F3. Link-time optimisation

`-flto` usually takes 5 to 15 percent off an `-Os` image, about 5 to 10 KiB
across both kernel images here. The linker scripts place code in the
extension by object file (`*thread.o(.text .text.*)` and so on), and LTO
removes those object boundaries. The extension's contents would have to
be named in the source instead, for example with a `FREYA_KEXT` macro that
expands to `__attribute__((section(".kext.text")))`. It is mechanical
work, but it touches every function in the extension, and it should be a
separate change with its own measurements.

### F4. A compressed program image

Rejected. XIP cannot run compressed code, and a copy-to-RAM image is at
most 7 KiB, so compression gains nothing that matters. The decompressor
also does not fit (`BOARD_COMPRESS` is 0).

## Other hardware

No STM32F103 in the LQFP48 package has more than 20 KiB of SRAM. A
GD32F303CCT6 (256 KiB flash, 48 KiB SRAM) is close to pin-compatible, and
an STM32F103RC board has 256 KiB and 48 KiB. Either would remove the
problem, but each is a new board port with its own flash timing and page
layout, not a change to this one.

## Recommended order

| Step | Change     | Gain                                       |
|------|------------|--------------------------------------------|
| 1    | R1 (built) | +2 KiB RAM for a program without threads   |
| 2    | R2, R3     | +1.5 to 3 KiB, heap first, then the window |
| 3    | F2, F1     | +3 to 6 KiB program flash                  |
| 4    | F3         | if more flash is still needed              |

Each step should come with the numbers it changes, measured the way the
table at the top was, and the README board table and
`docs/flash-programs.md` updated to match.
