# threads

Two threads. The program's main thread runs at priority 1. It creates
`blink` at priority 2, so `blink` runs first and the main thread only
gets the CPU while `blink` is asleep. Both print a few lines and then
the run ends, which stops whichever thread is still around.

```
freya: run threads.bin
main is thread 0
blink 0
blink is thread 2
main 0
blink 1
```

While it runs, a line typed at the console is read the next time a
thread blocks, or within a few milliseconds if one of them is spinning:

```
threads
stop blink
stop
```

`stop` with no name stops the whole run. `threads` lists every thread,
including the shell and idle. The calls are in [docs/threads.md](../../docs/threads.md).
