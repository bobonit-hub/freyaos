# jsonget

Fetches a JSON document with the HTTP client library
([docs/http.md](../../docs/http.md)) and prints it with cJSON
([docs/json.md](../../docs/json.md)), whole and indented, or the one value
a JSON Pointer (RFC 6901) names. Built on every board with the ESP32-C6
link; it links `libfreya_http.a` and `libfreya_cjson.a`.

```
freya: wifi("on")
freya: wifi("connect")
freya: run("jsonget.bin", "https://api.github.com/repos/DaveGamble/cJSON")
freya: run("jsonget.bin", "https://api.github.com/repos/DaveGamble/cJSON", "/license/spdx_id")
MIT
```

A string is printed without quotes; a number, object or array as JSON.
The body may be up to 64 KiB, kept in the heap while it is parsed. The
exit status is 0 when a value was printed.
