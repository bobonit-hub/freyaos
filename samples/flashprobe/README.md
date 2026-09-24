# flashprobe

How much internal flash the chip really has, as opposed to how much it says it
has.

Every board Freya supports has at least 128 KiB of flash, and the Blue Pill
program region runs to the end of that. An STM32F103C8 often still reports
64 KiB. This program raises any smaller report to 128 KiB and then writes,
which is how to find out whether one chip has still more.

Reading the extra pages proves nothing: unimplemented flash reads back as
*something*, and on a part whose address decoder wraps it reads back as the
kernel. Only a write settles it. So `flashprobe` walks the erase units from the
end of that 128 KiB upwards and, for each one, programs a 256-byte block,
reads it back, compares it, and erases the unit again. The first unit that does
not compare is where the flash ends.

## Run

```
freya: run flashprobe.bin
--- flashprobe starting (Ctrl-C stops it) ---
flashprobe: STM32F103 reports 64 KiB (using 128), probing to 256 KiB
flashprobe: each step writes 256 B into an erase unit that reads blank,
            compares it, and erases the unit again
  0x0801fc00   reference  ok
  0x08020000     129 KiB  ok
  ...
flashprobe: at least 256 KiB of flash - 128 KiB more than the chip declares
flashprobe: that is where the probe was told to stop, not where the flash is
```

A chip that has only the 128 KiB every board is built for stops on the first
step past that, and says which of the several ways it stopped:

```
  0x08020000     129 KiB  reads back as 0x08000000 - a mirror, not new flash
flashprobe: 128 KiB of flash - the declared size is all of it
```

```sh
run flashprobe.bin        # probe up to twice the size used below
run flashprobe.bin 256    # probe up to 256 KiB
```

| Argument | |
|---|---|
| KiB | where to stop, 1024 at the most; the default is twice the size used, and nothing below 128 KiB |

## The first step is a control

The step before any of the interesting ones is the last erase unit inside the
128 KiB already assumed, and it is labelled `reference`. If the
write-read-compare procedure cannot pass there,
none of its answers higher up are worth believing. On the Blue Pill that unit
is the last page of the program flash region, so the step is skipped with
`not blank` when a program is installed; on the Black Pill it is sector 7,
which Freya never uses, and erasing 128 KiB of it takes a second or two.

## What a step actually does

1. Read the whole erase unit, and the address it would fold onto if the chip
   ignores the address bits above the declared size.
2. If the unit reads back as that fold address byte for byte, and that address
   is not blank, the decoder wraps: this is the kernel seen twice, not new
   flash. Stop.
3. If the unit reads as all zeros, nothing answers at that address. Stop.
4. If the unit is not blank, something is stored there. Stop, without erasing
   it.
5. Program 256 bytes, each word carrying its own address, so that a decoder
   dropping bits inside the block and a bus answering every read with the same
   constant both fail the compare.
6. Read the block back and compare.
7. Read the fold address again. If the pattern turned up down there too, the
   two addresses are one set of cells rather than two.
8. Erase the unit and check it came back blank.

## Why it cannot break anything

**A unit is written only if it, and whatever it might alias, read entirely
0xFF first.** Everything else follows from that. Flash programming can only
clear bits, so a write that lands somewhere unintended fails its own compare
rather than corrupting anything; and the erase — the operation that could
destroy something — is only ever aimed at a unit already proven blank, whose
possible mirror was proven blank in the same pass. Below `FREYA_APP_FLASH_ADDR`
the program refuses outright, the way `boards/<board>/flash.c` does.

The unit is erased again at the end of each step, so the chip is left exactly
as it was found, at a cost of one erase cycle per probed unit out of the ten
thousand they are rated for.

## Why it drives the controller itself

Freya's flash driver bounds every write to the program region on purpose, and
the service table has no flash call in it at all — a program cannot rewrite the
kernel running it. Probing is the one job that needs an address outside that
region, and a program is not isolated from the hardware, so `flashprobe`
programs the controller directly. It is a sample rather than a console command
for exactly that reason.

## Why it has to run from RAM

Neither the F103 nor the F411 has read-while-write: while the controller is
busy, every read of flash stalls, instruction fetch included. A RAM image's own
code is not in flash, so it keeps running; the `.xip.bin` of this same sample
would stall inside its first erase, and it refuses to start:

```
freya: runflash
flashprobe: this one has to run from RAM - an erase stalls
            instruction fetch from flash.  Use 'run flashprobe.bin'.
```

Interrupts are masked for the length of each erase and each word, because every
handler Freya has is in flash. That is the same trade `install` makes: console
input is dropped while the controller is busy, and the software clock loses
about the time the probe takes. Ctrl-C is answered between steps.

## Why there is no reset between steps

An earlier version of this sample reset the MCU when a step left the flash
controller wedged, and kept its progress in SRAM so the next run could carry
on. None of that survived, for three reasons.

A reset cannot change the answer. The probe stops at the first unit that does
not compare, and that stop *is* the result; resuming past it does not extend
the size, which only grows on consecutive passing steps.

Neither part needs one. Every error either family reports is write-1-to-clear
in `FLASH_SR`, and a lock/unlock re-arms the controller: a write to an address
that is not there sets `PGERR` on the F103 or `PGSERR`/`PGAERR` on the F411 and
the controller goes back to idle. The one F4 condition that really does latch
until reset is a *wrong* key sequence written to `FLASH_KEYR`, and the probe
never writes one.

And the failure that genuinely would need a reset — a BusFault reading flash
that is not there — ends the run before the program could do anything about
it. Freya contains it and comes back to the shell.

What is left is one line. If a poll for `BSY` ever runs out, the step says so
and suggests the shell's own `reboot`, which is what the program's
`SYSRESETREQ` would have done anyway:

```
  0x08014000      81 KiB  the write was refused (controller timeout)
flashprobe: the controller never went idle - 'reboot' before anything else
            writes flash
```

## Reading past the end of the flash

On the Black Pill the first read above 512 KiB usually ends the run with a
BusFault rather than a compare failure, because the F411's flash aperture stops
being answered rather than answering with something. Freya contains it, prints
the faulting address and comes back to the shell — and that is the answer, just
delivered by the fault handler instead of by the summary line. Each step prints
its address *before* touching it for that reason.

## Build

```sh
make                   # build/blackpill/samples/flashprobe.bin
make BOARD=bluepill    # build/bluepill/samples/flashprobe.bin
```

Under 3 KiB, so it fits the program RAM region on either board. Copy the `.bin`
onto the card (or `download` it over XMODEM) and `run` it.

## Afterwards

Nothing resizes Freya from what it finds. The program region in
`include/freya_api.h` and `boards/<board>/freya.ld` is a build-time constant
that the kernel checks against the linker script at boot. On the Blue Pill
that constant already covers the 128 KiB every one of these boards has, past
the 64 KiB the size register often still reports.
