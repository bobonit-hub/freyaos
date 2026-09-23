# altair16

An Altair 8800b Turnkey with 16 KiB of RAM: the same Intel 8080, Turnkey
Module and card loader as `samples/altair`, with the main RAM kept to
16 KiB. Original 8080 software runs on it unchanged. The Freya console
is its teletype.

16 KiB is enough for 4K and 8K BASIC. Extended BASIC's checksum loader
sits at 3F00h, inside this RAM, and leaves about 1.5 KiB of workspace.
Disk BASIC's loader is at 7E00h, above this RAM, and that tape is
refused.

The ports, the menu, the PROM sockets and the way a tape is read are
the same as the 48 KiB machine. This file covers what is different.
`samples/altair/README.md` covers the rest.

```
freya:/> run altair16.bin
--- altair16 starting (Ctrl-C stops it) ---
altair: 8800b Turnkey, 16 KiB RAM at 0000, Turnkey SRAM at F800, PROM at FC00
altair: full speed, starting at 0000.  Ctrl-] for the menu
```

## Build and run

```sh
make                                      # build/blackpill/samples/altair16.bin
                                          # and altair16.xip.bin
make flash PROGRAM=altair16               # packed into the module
make flash PROGRAM=altair16 AUTOSTART=1   # and started on every reset
```

The 48 KiB machine's memory fills the Black Pill's program region, so
that build is a flash image. Here the memory is 16 KiB and the
interpreter fits beside it, so the Makefile builds a RAM image and a
flash image. It is for the Black Pill only. The Blue Pill's window is
8 KiB, so `make BOARD=bluepill` skips this sample.

```
freya:/> run altair16.bin           the files in /altair/
freya:/> run altair16.bin 8k41.tap --boot 8k
freya:/> run altair16.bin 4k40.tap --boot 4k40
freya:/> install altair16.xip.bin   once, into program flash
freya:/> runflash
```

With no image named, it looks in `/altair/` for the same files as the
48 KiB machine: `xbasic.bin`, or else `xbasic.tap`, and the PROMs
`turmon.bin` and `mbl.bin`.

`--ram KB` maps 1 to 16 KiB of that RAM. The other arguments are
unchanged.

## Memory

| Address | What |
|---|---|
| 0000-3FFF | RAM, 16 KiB, in the program's RAM window |
| 4000-F7FF | empty: reads FFh, writes are lost |
| F800-FBFF | Turnkey Module SRAM, 1 KiB |
| FC00-FFFF | Turnkey PROM sockets: FD00 TURMON, FE00 MBL, FF00 DBL |

A BASIC tape whose loader is above 3FFFh is refused. That is Disk
BASIC. Extended BASIC loads, and answers `MEMORY SIZE?` with Enter to
use all 16 KiB.

Saving the image once it is in memory works as on the larger machine.
For 8K BASIC the length is 2000h, and for 4K BASIC it is 1000h:

```
altair> save /altair/8kbasic.bin 0 2000
```
