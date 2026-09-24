# edit

A terminal text editor for Freya. It draws with ANSI / VT100 sequences on
the serial terminal and reads single keypresses from USART2. There is no
on-board display.

Use a terminal that understands VT100 (picocom, minicom, PuTTY, Tera Term)
at **921600 8N1**, at least 80 columns by 24 rows, with hardware flow
control off. Local echo must be off.

The whole file lives in one buffer in the program region. On the Blue Pill
that buffer is **3072 bytes**, because the region is 8 KiB and also holds
the program. On the Black Pill it is **32 KiB**. The status line shows
`used/capacity`. A file that does not fit is refused; it is not cut off.

```
freya: run edit.bin
freya: run edit.bin notes.txt
```

A relative path is taken from the shell's working directory. A name that
is not on the card yet starts an empty buffer, and writing creates the
file. Each component of a path is at most 63 characters, and the whole
path at most 127, which is what the filesystem stores.

## Keys

Arrow keys, Home, End, Page Up, Page Down and Delete are the usual VT100
sequences. A line longer than the screen scrolls sideways so the cursor
stays in view. `y` and `n` are accepted in either case.

| Key | Action |
|---|---|
| arrows | Move one character, or one line |
| `Home`, `Ctrl-A` | Start of the line |
| `End`, `Ctrl-E` | End of the line |
| `PgUp`, `PgDn` | Move by a screen |
| `Backspace` | Delete the character before the cursor |
| `Delete`, `Ctrl-D` | Delete the character at the cursor |
| `Enter` | Break the line |
| `Tab` | Insert a tab. Stops are every four columns |
| `Ctrl-K` | Delete to the end of the line, or the line break if already there |
| `Ctrl-U` | Delete back to the start of the line |
| `Ctrl-L` | Redraw the screen |
| `Ctrl-O` | Write the buffer. Asks for the path; Enter writes, Escape cancels |
| `Ctrl-X` | Quit. Asks first when the buffer has been changed |
| `Ctrl-C` | Stop the program (Freya, not an editor key) |

`Ctrl-C` is taken by the kernel and does not ask. Unsaved text is not
written. While the write prompt is up, Backspace deletes the last character,
`Ctrl-U` clears the name, `Ctrl-O` or Enter writes, and Escape or
`Ctrl-X` returns to the text. `y` or `n` answers the quit question.

Bytes already in a file are kept. A control byte is shown as `^X`, and a
byte above ASCII as `?`. On load, CR is dropped, so a CRLF file becomes
LF. Save writes LF and replaces the file.

## Build

```sh
make                   # Black Pill: build/blackpill/samples/edit.bin and .xip.bin
make BOARD=bluepill    # Blue Pill:  build/bluepill/samples/edit.bin and .xip.bin
```

The `.bin` loads into the program RAM region. The `.xip.bin` installs
into the program flash region.

## Run

Copy the `.bin` onto the card (or `download` it over XMODEM), then:

```
freya: run edit.bin notes.txt
```

Install into internal flash so it survives a power cycle and needs no card
to start (the file it edits still has to be on a card):

```
freya: install edit.xip.bin
freya: runflash notes.txt
```

Or pack it with the kernel when the module is programmed:

```sh
make BOARD=bluepill flash PROGRAM=edit
make BOARD=bluepill flash PROGRAM=edit AUTOSTART=1
```

Then `runflash` after boot, or `autostart on` so the next reset runs it.
