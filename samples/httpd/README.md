# httpd

TLS web console split across the two chips. The ESP32-C6 accepts the
connection. This program, on the STM32, is the file service behind it.

Only TLS 1.3 is accepted, on port 443. There is no cleartext port. The
methods are GET, HEAD and POST; a POST body is at most 1 MiB.

## Two screens

The first screen asks for the terminal password in a hidden input. The
form goes to `POST /login`, and httpd runs `login.sh` from the docroot
with the password as `$query`. The script tests it with
`password_check()`, so the eight bytes never leave the kernel and httpd
never compares them itself. The word `ok` on the first line opens a
session: a token in the query of every later request, kept by the page,
good for fifteen minutes from the last request. A wrong password costs a
two-second wait before the next attempt.

`password("xxxxxxxx")` has to be set at the console first; with the
password off, `password_check()` is false for every text and nobody can
log in. The password is handed to the script as a shell string, so it
cannot contain a quote.

The second screen, `app.html`, has two tabs:

* **System** shows what `sysinfo.sh` prints — `sysinfo()`, so the
  version, board, clocks, uptime, card and program. Refresh reruns it.
* **Firmware** takes a program image (`.xip.bin`) or a shell script for
  the program flash region. The browser adds the file's bytes into a
  32-bit sum, the one `file_checksum()` and `cksum()` use, and posts the
  file to `/firmware?t=…&sum=…`. httpd streams the body into
  `/spi1/firmware.bin`, refuses it when the sum differs or the file is
  larger than the program region, writes the sum to
  `/spi1/firmware.sum`, answers, and exits with status 3.

`install()` cannot run from a page: it needs the program region the
serving program occupies, so the flash step is a shell script that runs
once httpd has exited. `update.sh` checks the stored file against the
sum, `install()`s it and appends one line to `/spi1/update.log`.
The file and the sum are removed after a successful install, and when
the checksum does not match. An install that fails leaves them, so
`source("/spi1/update.sh")` can try again. `httpd.sh` starts httpd,
sources `update.sh` after an exit with status 3, and starts httpd
again. The Firmware tab shows that log on its next visit, after a new
login.

## Files

| File | Where it goes | What it is |
|---|---|---|
| `httpd.bin` | `/spi1/httpd.bin` | this program (`build/<board>/samples/httpd.bin`) |
| `httpd.sh` | `/spi1/httpd.sh` | the loop: `run`, then `update.sh` on status 3 |
| `update.sh` | `/spi1/update.sh` | the firmware update program |
| `www/index.html` | `/spi1/www/index.html` | the password screen |
| `www/app.html` | `/spi1/www/app.html` | the two tabs |
| `www/login.sh` | `/spi1/www/login.sh` | `password_check($query)` |
| `www/sysinfo.sh` | `/spi1/www/sysinfo.sh` | `sysinfo()` |

`update.sh` and `httpd.sh` live outside the docroot, so a browser cannot
run them as pages. The upload, its sum and the log are always under
`/spi1`; the docroot is the argument:

```
freya: mount()
freya: mkdir("/spi1/www")
freya: password("12345678")
freya: source("/spi1/httpd.sh")
--- httpd starting (Ctrl-C stops it) ---
httpd: https://192.168.1.20/  TLS 1.3, docroot /spi1/www
```

`run("httpd.bin")` alone serves `/www` on the card and still stores an
upload under `/spi1`; after its exit with status 3 the console says which
script installs it. Ctrl-C on the console returns to the shell from either
form.

From another machine, leaving certificate verification non-fatal because
the name `freya` is self-signed:

```
curl --tlsv1.3 -k https://192.168.1.20/
curl --tlsv1.3 -k -d 'password=12345678' https://192.168.1.20/login
curl --tlsv1.3 -k 'https://192.168.1.20/sysinfo.sh?t=<token>'
curl --tlsv1.3 -k --data-binary @blink.xip.bin \
     'https://192.168.1.20/firmware?t=<token>&sum=<hex>'
```

The token is in the page the login answers with. The sum is
`python3 -c 'import sys;print("%x"%(sum(open(sys.argv[1],"rb").read())&0xffffffff))' blink.xip.bin`,
or `hex(file_checksum("/spi1/blink.xip.bin"))` at the console.

## Routes

| Request | Answer |
|---|---|
| `GET /`, `GET /index.html` | the password screen, without a token |
| `POST /login` | `password=…`; a page that opens `/app.html?t=…`, or 403 |
| `GET /logout?t=…` | forgets the token |
| `GET /app.html?t=…` | the tabs; every other file or `.sh` page under the docroot needs the token too |
| `POST /firmware?t=…&sum=…` | stores the body in `/spi1/firmware.bin` and exits with status 3 |
| `GET /firmware?t=…` | `/spi1/update.log` |

A `.sh` page is run with `source`, and what it prints is the page.
`$method` is `GET` or `HEAD` and `$query` is the query string, at most 31
characters. A page must not `run`, `load`, `install`, or `reboot`; those
are refused while the script is the response.

The session token comes from a small generator stirred with request
timing and ADC noise. It keeps a neighbour on the LAN from guessing the
link, which is what a sample can promise; the TLS connection is what
keeps the password and the token from being read on the way.
