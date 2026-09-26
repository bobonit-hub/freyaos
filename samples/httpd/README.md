# httpd

TLS web server split across the two chips. The ESP32-C6 accepts the
connection. This program, on the STM32, is the file service.

Only TLS 1.3 is accepted, on port 443. There is no cleartext port.
A request is either a static file under `/www` on the card, or a shell
script whose name ends in `.sh`. The script is run with `source`, and
what it prints is the page. `$method` is `GET` or `HEAD`. `$query` is
the query string, at most 31 characters. Nothing else is served: no
upload, no directory listing, no other methods.

```
freya: mkdir("/www")
freya: run httpd.bin
httpd: https://192.168.1.20/  TLS 1.3, docroot /www
```

With no card, the same pages go on the SPI NOR volume and the docroot
is named on the command line: `run httpd.bin /spi1/www`.

Copy `samples/httpd/www/` into that directory first (`index.html`,
`hello.txt`, `status.sh`). From another machine, leaving certificate
verification non-fatal because the name `freya` is self-signed:

```
curl --tlsv1.3 -k https://192.168.1.20/
curl --tlsv1.3 -k https://192.168.1.20/status.sh
```

Ctrl-C on the console returns to the shell. A page must not `run`,
`load`, `install`, or `reboot`; those are refused while the script is
the response.
