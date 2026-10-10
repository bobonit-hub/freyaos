# USB sticks

With `make USB=1`, Freya is a USB host on the board's USB socket. It mounts
a mass storage stick as FAT16 / FAT32 at `/usb`, beside the card at `/`
and the SPI flash at `/spi1`. The FAT code is the same code that runs the
card, so files, directories, long names and every file call work the same
on the stick. The host is left out unless the build asks for it.

```sh
make BOARD=blackpill USB=1          # the stick alone
make BOARD=blackpill USB=1 SD=1     # the stick and the card
```

## Boards

| Board | USB host | Core | 48 MHz clock |
|---|---|---|---|
| Black Pill (STM32F411), STM32F401 | yes | OTG_FS | PLLQ |
| STM32F405, WeAct STM32F4 64-pin, APM32F407 | yes | OTG_FS | PLLQ |
| STM32U585 | yes | OTG_FS | PLL2Q |
| STM32H723 | yes | OTG_HS on its full speed PHY | PLL3Q |
| STM32H523, STM32H562 | not yet | USB DRD, a different core | |
| Blue Pill (STM32F103) | no | device only | |
| Black Pill 2 (AT32F403A) | no | device only | |

`USB=1` is refused on a board whose `board.mk` does not set `USB_HOST`.
The H5's USB controller can be a host too, but it is not the Synopsys core
`src/usbh.c` drives, and it has no driver yet.

The 48 MHz clock is made from the crystal. If the crystal does not start
and the board falls back to its internal oscillator, the USB clock is
only as accurate as that oscillator, and a stick may not keep up with it.

## Wiring and power

D- and D+ are PA11 and PA12, which on every board with the host are
already the USB-C socket's data lines. `USB=1` reserves those two pins, so
a program cannot claim them.

The core does not switch VBUS on these boards. **The stick needs 5 V from
somewhere else.** Each board feeds its regulator from the socket's VBUS,
not the other way round, so a stick plugged into the socket through an
OTG adapter gets no power from the board. Use an OTG adapter or a Y cable
that takes 5 V in and passes D-, D+ and that 5 V to the stick, with ground
shared with the board. Check your board's schematic before you feed 5 V
into the socket's VBUS pin.

### The WeAct STM32F4 64-pin board

On `weact_f405` (schematic V1.1) the socket's VBUS reaches VCC, the 3.3 V
regulator's input, through D4, a Schottky diode that blocks the other way.
Nothing switches VBUS, and CC1 and CC2 have 5.1 kOhm pull-downs (R9, R10),
which is how a device, not a host, marks its port. So as it ships the
board gives a stick or a headset no power. Two ways to give it some:

- **Close SB3.** The solder bridge sits beside D4, straight from VBUS to
  VCC; check whether yours is open (most WeAct bridges ship open). With it
  closed and the board fed 5 V on a VCC header pin (VCC takes 3.3 to 6 V),
  the device in the socket gets that 5 V and comes up with the board, so
  it is found at boot. There is no current limit or protection on that
  path, and VBUS is VCC both ways: do not plug the board into a PC or a
  charger while SB3 is closed and the board has 5 V of its own.
- **Leave the board as it is** and use an OTG Y cable or adapter that takes
  5 V in, as above. Nothing on the board changes and nothing can back-feed.

A USB-C headset on a C-to-C cable may stay off even with VBUS present: it
looks for a source's pull-up on CC and finds the board's pull-downs. A USB-A
headset through a C-to-A OTG adapter does not care.

There is one device and no hub: a stick or a headset plugged straight into
the port.

## What works

- Full speed (12 Mbit/s). A high speed stick falls back to full speed on
  these ports, as every stick does.
- Mass storage, SCSI transparent command set, Bulk-Only Transport: what
  practically every USB flash drive is. LUN 0 only.
- 512 byte blocks. A stick that reports any other block size is refused.
- The card's partition rules: an MBR's first FAT partition, or a
  superfloppy.

The stick is polled: transfers run in the calling thread, one 512 byte
block per SCSI command, so expect a few hundred KB/s. Nothing is cached
beyond the FAT layer's two sectors, and a file is written out when it is
closed or synced.

## Boot and commands

At boot the port is powered and given 0.3 s for a device to connect. The
device is enumerated and named from its string descriptors, and what its
configuration holds decides what it is: a stick is mounted, and with
`make USB=1 AUDIO=1` a headset becomes the audio calls' microphone and
speaker on the two STM32F405 boards, the APM32F407 board, the STM32U585
and the STM32H723
([audio.md](audio.md)).

```
[boot] USB        : SanDisk Cruzer Blade (0781:5567)
[boot] USB stick  : 7.5 GiB (SanDisk Cruzer Blade)
[boot] filesystem : FAT32 "STICK", cluster 4.0 KiB, mounted on /usb
```

A device plugged in later is taken with `usb("mount")`. Before unplugging
it, run `usb("eject")`: it closes every open file, writes out what is
cached, stops a headset's streams, and powers the port down.

| Command | What it does |
|---|---|
| `usb()` | the device: its name, VID:PID, and a stick's size and filesystem or a headset's formats |
| `usb("mount")` | find the device and take it: a stick is mounted on `/usb` (closes every open file first, as `mount()` does) |
| `usb("eject")` | write out, let go and power the port down |

`df()` and `sysinfo()` show the stick as well. `cd("/usb")` works as it
does for `/spi1`. A program reaches the stick through the ordinary file
calls with a `/usb/...` path; there is no new call in the program API.

A stick pulled out while mounted fails its file calls with an I/O error
and is marked gone; `usb("eject")` then cleans up, and `usb("mount")`
picks up the next one.

## Source

| File | What it is |
|---|---|
| `src/usbh.c` | the Synopsys OTG core in host mode: slave mode, polled, one packet at a time; with `AUDIO=1` also isochronous, from its interrupt |
| `src/usbdev.c` | enumeration, string descriptors, and telling a stick from a headset |
| `src/usbmsc.c` | Bulk-Only Transport and SCSI |
| `src/usbvol.c` | the second FAT volume: parks whichever of the card and the stick is not in use |
| `board_usb_init()` | in each board's `board.c`: the 48 MHz clock, PA11/PA12 and the PHY supply |

`make test` runs `src/usbdev.c`, `src/usbmsc.c`, `src/usbvol.c` and `src/fat.c` against a
simulated stick and a card image, writing to both at once, and has
`fsck.vfat` and mtools check the result ([tests.md](tests.md)).
`src/usbh.c` itself only runs on a board.
