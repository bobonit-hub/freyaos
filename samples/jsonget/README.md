# jsonget

Fetches a JSON document with the HTTP client library
([docs/http.md](../../docs/http.md)) and prints it with cJSON
([docs/json.md](../../docs/json.md)), whole and indented, or the one value
a JSON Pointer (RFC 6901) names. Built where cJSON is, the boards with the
ESP32-C6 link and 192 KiB of SRAM or more; it links `libfreya_http.a` and
`libfreya_cjson.a`.

```
freya: wifi("on")
freya: wifi("connect")
freya: run("jsonget.bin", "https://api.github.com/repos/DaveGamble/cJSON")
freya: run("jsonget.bin", "https://api.github.com/repos/DaveGamble/cJSON", "/license/spdx_id")
MIT
```

A string is printed without quotes; a number, object or array as JSON.
The body may be up to 24 KiB. It and cJSON's values are kept in the
program's own RAM, a 24 KiB buffer and a 112 KiB pool given to
`freya_cjson_init_pool()`, so that with its 22 KiB of code it fits the
smallest RAM window it is built for (168 KiB, STM32H523) and leaves the kernel heap
alone; a document larger than the pool goes on into the heap. The exit
status is 0 when a value was printed.
