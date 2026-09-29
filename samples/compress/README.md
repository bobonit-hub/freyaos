# compress

The worked example for the compressor. With no arguments it feeds a
built-in text through `api->compress()` and `api->decompress()` and
checks that the same bytes come back.

With two paths it reads one file whole, compresses it and writes the
other; `-d` decompresses instead. The file has to fit in the heap twice,
once as read and once as written, so it is for small files: the console
commands `compress` and `decompress` stream a file of any size.

The calls are described in `docs/compress.md`.

## Run

```
freya: run("compress.bin")
compress: self-test ok, 378 -> 154 -> 378 B

freya: run("compress.bin", "/notes.txt", "/notes.hs")
compress: 2048 -> 1433 B

freya: run("compress.bin", "-d", "/notes.hs", "/notes.txt")
compress: 1433 -> 2048 B
```

The two paths have to differ; the output is created or truncated. On a
board without the compressor the program says so and fails.
