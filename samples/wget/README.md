# wget

Fetches one URL with the HTTP client library, `http/freya_http.h`
([docs/http.md](../../docs/http.md)), and prints the body or saves it to
a file. It is built on every board with the ESP32-C6 link and links
`build/<board>/http/libfreya_http.a`.

```
freya: wifi("on")
freya: wifi("connect")
freya: run("wget.bin", "https://example.com/")
freya: run("wget.bin", "http://192.168.1.10:8080/data.csv", "/data.csv")
```

The last line gives the status code, the byte count and the time. The exit
status is 0 for a 2xx response. Ctrl-C stops the transfer.
