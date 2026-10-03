# The shell language on Linux

`fsh` is the Freya shell built as an ordinary Linux program. It uses the
same interpreter as the boards, `src/shell.c`. The language is the one in
[shell.md](shell.md), with the same values, limits and messages, so a
script can be written and tried on a PC before it goes onto a card.
Everything that needs the board's hardware is left out of the build.

    make linux              # or: make -C linux
    build/linux/fsh                 interactive prompt
    build/linux/fsh script.fsh      run a script file
    build/linux/fsh -c 'echo(1 + 2)'
    build/linux/fsh < script.fsh    standard input that is not a terminal is a script

The exit status is the last command's status (`$?`), or the status given
to `exit()`.

## The prompt

On a terminal the prompt works the way it does over the UART: backspace,
Ctrl-U, the cursor-key history, a block left open carrying on to the next
line, Ctrl-C to stop a script or a `sleep`. The prompt shows the working
directory. Ctrl-D on an empty line, or `exit()`, leaves. As on the board,
a command typed at the prompt uses function syntax, `ls("-l")`. A script
may also write `ls -l`.

## run()

`run(program, arg, ...)` starts a program, found on `PATH` the way
`execvp()` finds it. Each argument is passed as one word, and no shell
is involved, so `*` and `$HOME` mean nothing to it. A number, a byte or a
bool argument is passed as its text. Arrays and dicts are refused.

Written as a statement on its own, `run()` is the console command. The
program shares the terminal and its exit status becomes `$?`:

    freya:/home/me> run("ls", "-l", "/tmp")
    ...
    freya:/home/me> $?
    0

Inside an expression, `run()` returns what the program wrote on its
standard output, exactly, with any final newline. A second value is the
exit status. Standard input and standard error stay the terminal's:

    set out run("date", "+%Y")
    set out, rc run("grep", "-c", "x", "file.txt")
    if $rc == 0
      echo $out
    end

A program that cannot be started is status 127, with a message on
standard error. A program ended by a signal is status 128 + the signal
number. Ctrl-C at the program also stops the script, the way it stops a
script on the board.

`run(...)` needs the shell to have room for what the program prints: the
whole output is one string on the heap.

## Files

Paths are the host's, relative to the process's working directory, which
`cd()` changes. `ls`, `cd`, `pwd`, `mkdir`, `rm`, `rename`, `cat`,
`write`, `hexdump` and `source` are there, as are `open`, `read`,
`write`, `close`, `seek`, `flush`, `file_read`, `file_write` and
`file_checksum`. The `write` command ends its line with `\n` instead of
the card's `\r\n`. A script holds at most 16 open files. A path may be
up to 1023 bytes long.

## Other commands

`help`, `echo`, `sleep`, `yield`, `date`, `uptime`, `log`, `loglevel`,
`threads`, `stop`, `status`, `unset`, `clear`, `exit`, plus every
built-in function that does not need the board's hardware.

* `date()` is the local time. `date("YYYY-MM-DD HH:MM:SS")` moves this
  process's clock only, so `now()` and `date()` read the new time. The
  system clock is not changed.
* `uptime()` and `ticks()` count from the start of the process.
* `log()` writes the line to standard error, with the clock and the
  level in front, when the level allows it.
* `threads()` and `stop("name")` show and stop the script threads that
  `spawn()` started. `status()` prints the last command's status.

## What is left out

Hardware and system commands: `sysinfo`, `meminfo`, `mount`, `power`,
`df`, `download`, `upload`, `flashdump`, `load`, `install`, `uninstall`,
`saveflash`, `runflash`, `autostart`, `ramdump`, `password`, `cksum`,
`led`, `pin`, `pwm`, `adc`, `i2c`, `spi`, `w1`, `wifi`, `ping`, `syslog`,
`curl`, `aead`, `compress`, `decompress`, `reboot`.

Built-in functions: `get`, `set` (a pin), `adc`, `pwm`, `timer`,
`tstart`, `tstop`, `tcount`, `tclose`, `tperiod`, `irq`, `wait`,
`flash_read`, `flash_write`, `ram_read`, `ram_write`, `ram_checksum`,
`password_check`.

A left-out command is `command not found` (status 127), and a left-out
function is `no such function`.

## How it is built

`linux/board.h` stands in for a board header. It defines `FREYA_LINUX`,
and `src/shell.c` leaves out the code for the commands and functions
above when that is defined. The firmware for each board is unchanged.
`linux/platform.c` provides what the kernel provides on a board: the
console (the terminal, in raw mode while the shell reads it), the clock,
the heap, the file calls and the log, and `run()`.

`make test` builds `fsh` and runs `linux/test.sh`.
