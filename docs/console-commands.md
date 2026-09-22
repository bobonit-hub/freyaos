# Console commands

The shell on USART2 is the same program on both boards.  `help` lists
whatever the running image was compiled with; after the Black Pill gained
a program flash region that is the full set on both.

## Blue Pill commands

Every command Freya implements.  The Black Pill now has the same list.

| Command | What it does |
|---|---|
| `help [command]` | list commands, or describe one |
| `sysinfo` | CPU, unique id, clocks, reset cause, uptime, log level, auto-start and ram-dump flags, card, filesystem |
| `meminfo` | flash and RAM usage: .data, .bss, heap, program region, stack |
| `mount` | initialise the card and mount the filesystem |
| `ls [-l] [path]` | list a directory |
| `ll [path]` | list with sizes, dates and attributes |
| `cd [path]`, `pwd` | move around |
| `mkdir <dir>...` | create directories |
| `rm [-r] <path>...` | remove files, empty directories, or whole trees |
| `rename <old> <new>`, `mv` | rename or move a file or directory (no data copy) |
| `download <file> [--raw]` | receive a file over XMODEM |
| `cat <file>` | print a file |
| `write <file> <text...>` | append a line to a file |
| `hexdump <file> [off] [len]` | dump a file in hex |
| `flashdump [file]` | write internal flash to a file on the card (default `/freya.flash`) |
| `df` | capacity, free and used space |
| `load <file>\|@flash` | load a program image into RAM, or bind the flash image |
| `run [file\|@flash] [args...]` | run the loaded program |
| `runflash [args...]` | run the program stored in internal flash |
| `stop` | stop, or unload, the program |
| `install <file>` | write a program into internal flash |
| `saveflash [file]` | copy the installed program from flash onto the card (default `/<name>.xip.bin`) |
| `uninstall` | erase the program flash region |
| `autostart [on\|off]` | run the flash program automatically at boot |
| `ramdump [on\|off]` | write SRAM to `/freya.ram` after a BusFault (default off) |
| `date [YYYY-MM-DD HH:MM:SS]` | show or set the clock used for file timestamps |
| `loglevel [level]` | show or set the file log level (`off`/`error`/`warn`/`info`/`debug`, or `0`..`4`) |
| `uptime`, `led`, `echo`, `clear`, `reboot` | the usual small change |

Ctrl-C stops a running program, Ctrl-U clears the input line, and the up and
down cursor keys walk the command history.

## Commands that were Blue Pill only

These lived behind `FREYA_APP_FLASH_ADDR`, which only the Blue Pill defined.
They are implemented on both boards now (Black Pill: 128-byte slot at the
end of sector 3, 64 KiB program in sector 4).  Acceptance: type any of
them at `freya:/>` on either module.

| Command | Why it was missing on the Black Pill |
|---|---|
| `runflash [args...]` | no program flash region |
| `install <file>` | no F4 flash programmer |
| `saveflash [file]` | nothing to copy out of flash |
| `uninstall` | nothing to erase |
| `autostart [on\|off]` | no auto-start slot |
| `ramdump [on\|off]` | flag lived in that same slot; dump was compiled out |

Related behaviour that followed the same `#ifdef`, and is now common too:
`load @flash` / `run @flash`, persistent `loglevel` in the slot, `sysinfo`
and `meminfo` lines for auto-start / ram-dump / program flash, and a
BusFault dump of SRAM to `/freya.ram` when the flag is on.
