# log

Writes one Freya log line at each level so you can see the names in
`/freya.log` and how the filter drops quieter ones.

```
freya: loglevel debug
freya: run log.bin
freya: cat /freya.log
```

Optional argument sets the filter for this run (`off`, `error`, `warn`,
`info`, `debug`, or `0`..`4`):

```
run log.bin error     # only the ERROR line is stored
run log.bin debug     # all four lines are stored
```

Each file line looks like `2026-09-22 02:42:00 INFO sample info: tick 12 ms`.
Without a card the same text goes to the console.
