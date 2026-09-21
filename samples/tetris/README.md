# tetris

A 10×20 console Tetris for Freya. It draws with ANSI / VT100 sequences on
the serial terminal and reads single keypresses from USART2. There is no
on-board display and no extra hardware.

Use a terminal that understands VT100 (picocom, minicom, PuTTY, Tera Term)
at **921600 8N1**, with hardware flow control off. Local echo must be off.

## Keys

These are hardcoded in `main.c`. Letters are accepted in either case.
Arrow keys are ignored (escape sequences are drained so they do not leak
into later commands).

| Key | Action |
|---|---|
| `A` | Move left |
| `D` | Move right |
| `S` | Soft drop (one row; +1 score) |
| `W` | Rotate clockwise |
| `Space` | Hard drop (lock on landing; +2 per row) |
| `P` | Pause / resume |
| `R` | Restart |
| `Q` | Quit to the Freya shell |
| `Ctrl-C` | Stop the program (Freya, not a game key) |

`Ctrl-C` is taken by the kernel. Do not bind it as a game control.

## Build

```sh
make BOARD=bluepill
```

Images:

- `build/bluepill/samples/tetris.bin` — load into the 8 KiB RAM region
- `build/bluepill/samples/tetris.xip.bin` — install into the 24 KiB flash region

The flash image is the better fit on the Blue Pill: code stays in flash and
the RAM window is only `.data` / `.bss`.

## Run

Copy the `.bin` onto the card (or `download` it over XMODEM), then:

```
freya:/> run tetris.bin
```

Install into internal flash so it survives a power cycle and needs no card:

```
freya:/> install tetris.xip.bin
freya:/> runflash
```

Or pack it with the kernel when the module is programmed:

```sh
make BOARD=bluepill flash PROGRAM=tetris
```

Then `runflash` after boot, or `autostart on` so the next reset runs it.

## Play

The well is ten cells wide and twenty high. The seven tetrominoes come from
a shuffled bag. A faint ghost (`::`) shows where the piece will land. Level
rises every ten lines and gravity speeds up. Line scores are 100 / 300 /
500 / 800 times (level + 1). Soft and hard drops add a little extra.

On game over, `R` starts a new game and `Q` returns to the shell. Quit also
prints the last score, lines and level.
