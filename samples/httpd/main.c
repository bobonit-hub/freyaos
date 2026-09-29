/*
 * httpd - TLS web console for the ESP32-C6 web server.
 *
 * The C6 accepts one HTTPS connection on port 443 (TLS 1.3 only) and
 * parses the request. This program is the other half. It has two
 * screens. The first asks for the terminal password, and login.sh in
 * the docroot tests it with password_check(). The second has two tabs:
 * System, which is what sysinfo.sh prints, and Firmware, which uploads
 * a program image to /spi1/firmware.bin for update.sh to install.
 *
 *     run httpd.bin                 pages in /www on the card
 *     run httpd.bin /spi1/www       pages on the SPI NOR volume
 *
 * Wi-Fi credentials already stored on the C6 are used. Ctrl-C returns
 * to the shell. The routes:
 *
 *     GET  /                    index.html, the password screen (public)
 *     POST /login               the form; login.sh says ok or not
 *     GET  /logout?t=           forgets the session
 *     GET  /app.html?t=         the tabs; every other file or .sh page
 *                               under the docroot needs the token too
 *     POST /firmware?t=&sum=    stores the body in /spi1/firmware.bin
 *     GET  /firmware?t=         the log update.sh wrote, /spi1/update.log
 *
 * A session is one token, t= in the query of every request after the
 * login. The page keeps it. It lasts SESSION_MS from the last request,
 * and a new login replaces it.
 *
 * install() cannot run from a page while this program occupies the
 * program region, so the flash step is not done here. Once the upload
 * is stored and its byte sum matches the one the browser sent, httpd
 * answers the request, writes the sum to /spi1/firmware.sum and exits
 * with status 3. httpd.sh then sources update.sh, which checks the
 * stored file's checksum again, install()s it, removes the file, logs
 * the result to /spi1/update.log, and starts httpd again.
 */
#include "freya_api.h"

#define PAGE_MAX    4096
#define TOKEN_LEN   12
/* "t=" + token + "&sum=" + 8 hex digits has to fit in the query. */
_Static_assert(2 + TOKEN_LEN + 5 + 8 <= FREYA_WEB_QUERY,
               "session token leaves no room for the firmware checksum");
#define SESSION_MS  (15U * 60U * 1000U)
#define LOGIN_WAIT  2000U               /* ms between password attempts  */
#define FORM_MAX    256                 /* the login form body           */
#define EXIT_UPDATE 3                   /* a firmware waits in FW_FILE   */

#define FW_FILE     "/spi1/firmware.bin"
#define FW_SUM      "/spi1/firmware.sum"
#define FW_LOG      "/spi1/update.log"
#ifdef FREYA_APP_FLASH_SIZE
#define FW_MAX      FREYA_APP_FLASH_SIZE
#else
#define FW_MAX      FREYA_WEB_BODY_MAX
#endif

static char s_page[PAGE_MAX];
static char s_root[64] = "/www";
static char s_token[TOKEN_LEN + 1];
static uint32_t s_token_at;
static uint32_t s_fail_at;              /* the last wrong password       */
static uint32_t s_rng;
static int s_update;                    /* exit after this reply         */

/* ------------------------------------------------------------ strings */

static int slen(const char *s)
{
    int n = 0;

    while (s[n]) n++;
    return n;
}

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int ends(const char *s, const char *suf)
{
    int n = slen(s), m = slen(suf);

    if (n < m) return 0;
    return eq(s + n - m, suf);
}

/* Append src to a buffer of size cap that already holds *len bytes. */
static void add(char *buf, int cap, int *len, const char *src)
{
    while (*src && *len < cap - 1) buf[(*len)++] = *src++;
    buf[*len] = '\0';
}

static void add_u(char *buf, int cap, int *len, uint32_t v)
{
    char d[11];
    int i = 0;

    do { d[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i > 0 && *len < cap - 1) buf[(*len)++] = d[--i];
    buf[*len] = '\0';
}

/* Lowercase hex with no leading zeros, the way the shell's hex() prints
 * a checksum, so the browser, this program and update.sh agree. */
static void add_x(char *buf, int cap, int *len, uint32_t v)
{
    char d[9];
    int i = 0;

    do { d[i++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    while (i > 0 && *len < cap - 1) buf[(*len)++] = d[--i];
    buf[*len] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int n = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        int d = hex_nibble(*s++);

        if (d < 0 || ++n > 8) return -1;
        v = (v << 4) | (uint32_t)d;
    }
    if (!n) return -1;
    *out = v;
    return 0;
}

/* The value of key in a query or form body, percent-decoded, with '+'
 * as a space. 1 when found and it fits. */
static int field(const char *q, const char *key, char *out, int cap)
{
    while (*q) {
        const char *k = key;
        const char *p = q;

        while (*k && *p == *k) { p++; k++; }
        if (!*k && *p == '=') {
            int n = 0;

            p++;
            while (*p && *p != '&') {
                int c = (unsigned char)*p++;

                if (c == '+') c = ' ';
                else if (c == '%') {
                    int hi = hex_nibble(p[0]);
                    int lo = p[0] ? hex_nibble(p[1]) : -1;

                    if (hi < 0 || lo < 0) return 0;
                    c = (hi << 4) | lo;
                    p += 2;
                }
                if (n >= cap - 1) return 0;
                out[n++] = (char)c;
            }
            out[n] = '\0';
            return 1;
        }
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
    return 0;
}

/* ------------------------------------------------------------ tokens */

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* Fold a value into the generator: request timing, ADC noise. */
static void stir(uint32_t v)
{
    s_rng ^= v;
    if (!s_rng) s_rng = 0x9E3779B9UL;
    rnd();
}

static void new_token(const freya_api_t *api)
{
    int i;

    stir(api->ticks_ms());
    if (FREYA_API_HAS(api, adc_read)) {
        stir((uint32_t)api->adc_read(FREYA_ADC_TEMP));
        stir((uint32_t)api->adc_read(FREYA_ADC_VREF));
    }
    for (i = 0; i < TOKEN_LEN; i++)
        s_token[i] = "0123456789abcdef"[rnd() & 15];
    s_token[TOKEN_LEN] = '\0';
    s_token_at = api->ticks_ms();
}

/* 1 when the query carries the live token; the session is renewed. */
static int authed(const freya_api_t *api, const char *query)
{
    char t[TOKEN_LEN + 2];
    uint32_t now = api->ticks_ms();

    if (!s_token[0]) return 0;
    if ((uint32_t)(now - s_token_at) > SESSION_MS) {
        s_token[0] = '\0';
        return 0;
    }
    if (!field(query, "t", t, (int)sizeof t) || !eq(t, s_token)) return 0;
    s_token_at = now;
    return 1;
}

/* ------------------------------------------------------------ network */

/* Re-issue a nonblocking call until it finishes. */
static int poll_again(const freya_api_t *api, int rc)
{
    int pr;

    if (rc != FREYA_ERR_AGAIN) return rc;
    api->yield();
    pr = api->net_poll(20);
    return pr < 0 ? pr : FREYA_ERR_AGAIN;
}

static int wifi_up(const freya_api_t *api, freya_wifi_status_t *st)
{
    uint32_t start;
    int rc;

    do { rc = poll_again(api, api->wifi_on()); } while (rc == FREYA_ERR_AGAIN);
    if (rc != 0) return rc;
    do { rc = poll_again(api, api->wifi_connect()); } while (rc == FREYA_ERR_AGAIN);
    if (rc != 0) return rc;

    start = api->ticks_ms();
    for (;;) {
        do { rc = poll_again(api, api->wifi_status(st)); }
        while (rc == FREYA_ERR_AGAIN);
        if (rc != 0) return rc;
        if (st->state == FREYA_WIFI_CONNECTED) return 0;
        if (st->state == FREYA_WIFI_ERROR) return FREYA_ERR_IO;
        if ((uint32_t)(api->ticks_ms() - start) > 20000)
            return FREYA_ERR_TIMEOUT;
        api->net_poll(100);
    }
}

static int send_mem(const freya_api_t *api, int method, int status,
                    const char *type, const void *body, int len)
{
    const uint8_t *p = body;
    int rc, off = 0;

    if (len < 0) len = 0;
    do { rc = poll_again(api, api->web_begin(status, type, (uint32_t)len)); }
    while (rc == FREYA_ERR_AGAIN);
    if (rc != 0) return rc;
    if (method != FREYA_WEB_HEAD) {
        while (off < len) {
            int n = len - off;

            if (n > FREYA_WEB_CHUNK) n = FREYA_WEB_CHUNK;
            do { rc = poll_again(api, api->web_body(p + off, n)); }
            while (rc == FREYA_ERR_AGAIN);
            if (rc != 0) return rc;
            off += n;
        }
    }
    do { rc = poll_again(api, api->web_end()); } while (rc == FREYA_ERR_AGAIN);
    return rc;
}

static int send_text(const freya_api_t *api, int method, int status,
                     const char *text)
{
    return send_mem(api, method, status, "text/plain; charset=utf-8", text,
                    slen(text));
}

static int send_page(const freya_api_t *api, int method, int status)
{
    return send_mem(api, method, status, "text/html; charset=utf-8", s_page,
                    slen(s_page));
}

static int send_file(const freya_api_t *api, int method, const char *path,
                     const char *type)
{
    uint8_t chunk[FREYA_WEB_CHUNK];
    int32_t sz;
    uint32_t left;
    int fd, rc;

    fd = api->open(path, FREYA_O_RDONLY);
    if (fd < 0) return send_text(api, method, 404, "not found\n");
    sz = api->fsize(fd);
    if (sz < 0) {
        api->close(fd);
        return send_text(api, method, 404, "not found\n");
    }
    do { rc = poll_again(api, api->web_begin(200, type, (uint32_t)sz)); }
    while (rc == FREYA_ERR_AGAIN);
    if (rc != 0) {
        api->close(fd);
        return rc;
    }
    left = (uint32_t)sz;
    if (method != FREYA_WEB_HEAD) {
        while (left) {
            int n = api->read(fd, chunk,
                              left > FREYA_WEB_CHUNK ? FREYA_WEB_CHUNK : (int)left);

            if (n <= 0) {
                api->close(fd);
                return FREYA_ERR_IO;
            }
            do { rc = poll_again(api, api->web_body(chunk, n)); }
            while (rc == FREYA_ERR_AGAIN);
            if (rc != 0) {
                api->close(fd);
                return rc;
            }
            left -= (uint32_t)n;
        }
    }
    api->close(fd);
    do { rc = poll_again(api, api->web_end()); } while (rc == FREYA_ERR_AGAIN);
    return rc;
}

/* Source a script and send what it printed as the page. */
static int send_script(const freya_api_t *api, int method, const char *path,
                       const char *query)
{
    const char *mname = method == FREYA_WEB_HEAD ? "HEAD" : "GET";
    int n = 0;
    int st;

    st = api->shell_source_capture(path, mname, query, s_page,
                                   (int)sizeof s_page, &n);
    if (st != 0)
        return send_mem(api, method, 500, "text/plain; charset=utf-8",
                        n > 0 ? s_page : "script failed\n",
                        n > 0 ? n : 14);
    return send_mem(api, method, 200, "text/html; charset=utf-8", s_page, n);
}

/* Read the rest of a POST body and drop it, so a refused upload does
 * not leave the C6 waiting. */
static void drain_body(const freya_api_t *api)
{
    uint8_t chunk[FREYA_WEB_READ_MAX];
    int rc;

    for (;;) {
        do { rc = poll_again(api, api->web_read(chunk, (int)sizeof chunk, 0)); }
        while (rc == FREYA_ERR_AGAIN);
        if (rc <= 0) return;
    }
}

/* ------------------------------------------------------------ paths */

/* <docroot> + "/" + name into out. */
static int in_root(const char *name, char *out, int size)
{
    int n = 0;

    while (s_root[n] && n < size - 1) {
        out[n] = s_root[n];
        n++;
    }
    while (*name && n < size - 1) out[n++] = *name++;
    out[n] = '\0';
    return *name ? -1 : 0;
}

/* URL path -> <docroot> + path. '/' and a trailing slash select
 * index.html. The C6 already rejected '..'; this side checks again. */
static int map_path(const char *url, char *out, int size)
{
    int i = 0, n = 0, root_len = 0;
    const char *root = s_root;
    const char *idx = "index.html";

    if (!url || url[0] != '/') return -1;
    while (root[n] && n < size - 1) {
        out[n] = root[n];
        n++;
    }
    while (url[i]) {
        int start, seg;

        if (url[i] != '/') return -1;
        if (n >= size - 1) return -1;
        out[n++] = '/';
        i++;
        start = i;
        while (url[i] && url[i] != '/') {
            char c = url[i];

            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == '~'))
                return -1;
            if (n >= size - 1) return -1;
            out[n++] = c;
            i++;
        }
        seg = i - start;
        if (seg == 0)
            break;
        if ((seg == 1 && url[start] == '.') ||
            (seg == 2 && url[start] == '.' && url[start + 1] == '.'))
            return -1;
    }
    if (n > 0 && out[n - 1] == '/') {
        int k = 0;

        while (idx[k]) {
            if (n >= size - 1) return -1;
            out[n++] = idx[k++];
        }
    }
    out[n] = '\0';
    while (root[root_len]) root_len++;
    return n > root_len ? 0 : -1;
}

static const char *content_type(const char *path)
{
    const char *dot = 0;
    const char *s = path;

    while (*s) {
        if (*s == '/') dot = 0;
        else if (*s == '.') dot = s;
        s++;
    }
    if (!dot) return "application/octet-stream";
    if (eq(dot, ".html") || eq(dot, ".htm")) return "text/html; charset=utf-8";
    if (eq(dot, ".css")) return "text/css";
    if (eq(dot, ".js")) return "text/javascript";
    if (eq(dot, ".txt") || eq(dot, ".log")) return "text/plain; charset=utf-8";
    if (eq(dot, ".json")) return "application/json";
    if (eq(dot, ".svg")) return "image/svg+xml";
    if (eq(dot, ".png")) return "image/png";
    if (eq(dot, ".jpg") || eq(dot, ".jpeg")) return "image/jpeg";
    if (eq(dot, ".gif")) return "image/gif";
    if (eq(dot, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

/* ------------------------------------------------------------ pages */

/* A small page that says something and goes somewhere. */
static void notice_page(const char *title, const char *text, const char *url,
                        const char *link)
{
    int n = 0;

    s_page[0] = '\0';
    add(s_page, PAGE_MAX, &n, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width\"><title>Freya</title>");
    if (url) {
        add(s_page, PAGE_MAX, &n, "<meta http-equiv=\"refresh\" content=\"0; url=");
        add(s_page, PAGE_MAX, &n, url);
        add(s_page, PAGE_MAX, &n, "\">");
    }
    add(s_page, PAGE_MAX, &n, "<style>body{font-family:sans-serif;margin:3em auto;"
        "max-width:28em;padding:0 1em}</style></head><body><h1>");
    add(s_page, PAGE_MAX, &n, title);
    add(s_page, PAGE_MAX, &n, "</h1><p>");
    add(s_page, PAGE_MAX, &n, text);
    add(s_page, PAGE_MAX, &n, "</p><p><a href=\"");
    add(s_page, PAGE_MAX, &n, url ? url : "/");
    add(s_page, PAGE_MAX, &n, "\">");
    add(s_page, PAGE_MAX, &n, link);
    add(s_page, PAGE_MAX, &n, "</a></p></body></html>\n");
}

/* POST /login: the form field 'password' goes to login.sh as $query,
 * which tests it with password_check(). The word ok on the first line
 * opens a session. */
static void do_login(const freya_api_t *api, const freya_web_req_t *req)
{
    char form[FORM_MAX + 1];
    char pw[FREYA_WEB_QUERY + 2];
    char path[96];
    char out[64];
    uint32_t now = api->ticks_ms();
    int n = 0, rc, st, ok = 0;

    for (;;) {
        do { rc = poll_again(api, api->web_read(form + n, FORM_MAX - n + 1, 0)); }
        while (rc == FREYA_ERR_AGAIN);
        if (rc < 0) return;
        if (rc == 0) break;
        n += rc;
        if (n > FORM_MAX) {
            drain_body(api);
            send_text(api, req->method, 400, "form too long\n");
            return;
        }
    }
    form[n] = '\0';

    if (s_fail_at && (uint32_t)(now - s_fail_at) < LOGIN_WAIT) {
        notice_page("Wait", "Another attempt is possible in two seconds.",
                    0, "Back");
        send_page(api, req->method, 403);
        return;
    }
    if (field(form, "password", pw, (int)sizeof pw) && pw[0] &&
        in_root("/login.sh", path, (int)sizeof path) == 0) {
        st = api->shell_source_capture(path, "POST", pw, out, (int)sizeof out, &n);
        ok = st == 0 && n >= 2 && out[0] == 'o' && out[1] == 'k' &&
             (n == 2 || out[2] == '\r' || out[2] == '\n');
    }
    if (!ok) {
        s_fail_at = now ? now : 1;
        api->printf("httpd: wrong password\r\n");
        notice_page("Wrong password", "That is not the terminal password.",
                    0, "Try again");
        send_page(api, req->method, 403);
        return;
    }
    new_token(api);
    {
        char url[40];
        int m = 0;

        url[0] = '\0';
        add(url, (int)sizeof url, &m, "/app.html?t=");
        add(url, (int)sizeof url, &m, s_token);
        notice_page("Welcome", "Opening the console.", url, "Continue");
    }
    api->printf("httpd: login\r\n");
    send_page(api, req->method, 200);
}

/* POST /firmware?t=..&sum=..: the body goes to FW_FILE, its byte sum
 * has to be the sum the browser sent, and then update.sh takes over. */
static void do_firmware(const freya_api_t *api, const freya_web_req_t *req)
{
    uint8_t chunk[FREYA_WEB_READ_MAX];
    char hex[12];
    uint32_t want = 0, sum = 0, total = 0, left = 0;
    int fd, rc, n;

    if (!field(req->query, "sum", hex, (int)sizeof hex) ||
        parse_hex(hex, &want) != 0) {
        drain_body(api);
        send_text(api, req->method, 400, "sum=<hex byte sum> is required\n");
        return;
    }
    fd = api->open(FW_FILE, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd < 0) {
        drain_body(api);
        send_text(api, req->method, 500,
                  "cannot create " FW_FILE " - is /spi1 mounted?\n");
        return;
    }
    for (;;) {
        do { rc = poll_again(api, api->web_read(chunk, (int)sizeof chunk, &left)); }
        while (rc == FREYA_ERR_AGAIN);
        if (rc < 0) {
            api->close(fd);
            api->unlink(FW_FILE);
            api->printf("httpd: upload failed (%d)\r\n", rc);
            return;
        }
        if (rc == 0) break;
        if (total + (uint32_t)rc > FW_MAX) {
            api->close(fd);
            api->unlink(FW_FILE);
            drain_body(api);
            n = 0;
            s_page[0] = '\0';
            add(s_page, PAGE_MAX, &n, "the image is larger than the program region (");
            add_u(s_page, PAGE_MAX, &n, FW_MAX);
            add(s_page, PAGE_MAX, &n, " bytes)\n");
            send_mem(api, req->method, 413, "text/plain; charset=utf-8", s_page, n);
            return;
        }
        if (api->write(fd, chunk, rc) != rc) {
            api->close(fd);
            api->unlink(FW_FILE);
            drain_body(api);
            send_text(api, req->method, 500, "write failed - is /spi1 full?\n");
            return;
        }
        for (n = 0; n < rc; n++) sum += chunk[n];
        total += (uint32_t)rc;
    }
    api->close(fd);

    n = 0;
    s_page[0] = '\0';
    if (total == 0) {
        api->unlink(FW_FILE);
        send_text(api, req->method, 400, "the upload is empty\n");
        return;
    }
    if (sum != want) {
        api->unlink(FW_FILE);
        add(s_page, PAGE_MAX, &n, "checksum mismatch: the file sums to ");
        add_x(s_page, PAGE_MAX, &n, sum);
        add(s_page, PAGE_MAX, &n, ", the browser sent ");
        add_x(s_page, PAGE_MAX, &n, want);
        add(s_page, PAGE_MAX, &n, "; nothing stored\n");
        send_mem(api, req->method, 400, "text/plain; charset=utf-8", s_page, n);
        return;
    }

    /* The sum, as hex() prints it, for update.sh to compare against. */
    fd = api->open(FW_SUM, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd >= 0) {
        add_x(s_page, PAGE_MAX, &n, sum);
        add(s_page, PAGE_MAX, &n, "\n");
        rc = api->write(fd, s_page, n);
        api->close(fd);
        if (rc != n) fd = -1;
    }
    if (fd < 0) {
        api->unlink(FW_FILE);
        api->unlink(FW_SUM);
        send_text(api, req->method, 500, "cannot write " FW_SUM "\n");
        return;
    }

    n = 0;
    s_page[0] = '\0';
    add(s_page, PAGE_MAX, &n, "stored ");
    add_u(s_page, PAGE_MAX, &n, total);
    add(s_page, PAGE_MAX, &n, " bytes in " FW_FILE ", checksum ");
    add_x(s_page, PAGE_MAX, &n, sum);
    add(s_page, PAGE_MAX, &n, " ok.\nhttpd exits with status 3 so update.sh can "
        "install it. Log in again and open this tab for the result.\n");
    send_mem(api, req->method, 200, "text/plain; charset=utf-8", s_page, n);
    s_update = 1;
}

static void serve(const freya_api_t *api, const freya_web_req_t *req)
{
    char path[128];
    int rc = 0;

    if (req->method == FREYA_WEB_POST) {
        if (eq(req->path, "/login")) {
            do_login(api, req);
        } else if (eq(req->path, "/firmware")) {
            if (authed(api, req->query)) {
                do_firmware(api, req);
            } else {
                drain_body(api);
                send_text(api, req->method, 403, "log in first\n");
            }
        } else {
            drain_body(api);
            send_text(api, req->method, 404, "not found\n");
        }
        return;
    }

    if (eq(req->path, "/") || eq(req->path, "/index.html")) {
        if (in_root("/index.html", path, (int)sizeof path) != 0)
            rc = send_text(api, req->method, 500, "docroot too long\n");
        else
            rc = send_file(api, req->method, path, "text/html; charset=utf-8");
    } else if (eq(req->path, "/logout")) {
        s_token[0] = '\0';
        notice_page("Logged out", "The session is closed.", "/", "Log in");
        rc = send_page(api, req->method, 200);
    } else if (!authed(api, req->query)) {
        notice_page("Not logged in", "The session has ended or the link "
                    "carries no token.", 0, "Log in");
        rc = send_page(api, req->method, 403);
    } else if (eq(req->path, "/firmware")) {
        int fd = api->open(FW_LOG, FREYA_O_RDONLY);

        if (fd < 0) {
            rc = send_text(api, req->method, 200, "no firmware update yet\n");
        } else {
            api->close(fd);
            rc = send_file(api, req->method, FW_LOG, "text/plain; charset=utf-8");
        }
    } else if (map_path(req->path, path, (int)sizeof path) != 0) {
        rc = send_text(api, req->method, 403, "forbidden\n");
    } else if (ends(path, ".sh")) {
        rc = send_script(api, req->method, path, req->query);
    } else {
        rc = send_file(api, req->method, path, content_type(path));
    }
    if (rc != 0 && rc != FREYA_ERR_IO)
        api->printf("httpd: %s failed (%d)\r\n", req->path, rc);
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    freya_wifi_status_t st;
    int rc;

    if (!FREYA_API_HAS(api, web_read)) {
        api->puts("httpd: this kernel has no HTTPS file service with POST\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc > 2) {
        api->puts("usage: run httpd.bin [docroot]\r\n");
        return FREYA_EXIT_USAGE;
    }
    if (argc == 2) {
        const char *p = argv[1];
        int n = 0;

        if (p[0] != '/') {
            api->puts("httpd: docroot must be an absolute path\r\n");
            return FREYA_EXIT_USAGE;
        }
        while (p[n] && n < (int)sizeof s_root - 1) {
            s_root[n] = p[n];
            n++;
        }
        if (p[n]) {
            api->puts("httpd: docroot is too long\r\n");
            return FREYA_EXIT_USAGE;
        }
        while (n > 1 && s_root[n - 1] == '/') n--;
        s_root[n] = '\0';
    }
    stir(api->ticks_ms());
    stir(api->cpu_hz());
    rc = wifi_up(api, &st);
    if (rc != 0) {
        api->printf("httpd: wifi failed (%d)\r\n", rc);
        return FREYA_EXIT_FAIL;
    }
    api->printf("httpd: https://%u.%u.%u.%u/  TLS 1.3, docroot %s\r\n",
                (st.ip >> 24) & 255U, (st.ip >> 16) & 255U,
                (st.ip >> 8) & 255U, st.ip & 255U, s_root);

    for (;;) {
        freya_web_req_t req;

        do {
            rc = poll_again(api, api->web_take(&req));
            stir(api->ticks_ms());
        } while (rc == FREYA_ERR_AGAIN);
        if (rc != 0) {
            api->printf("httpd: link failed (%d)\r\n", rc);
            return FREYA_EXIT_FAIL;
        }
        serve(api, &req);
        if (s_update) {
            api->printf("httpd: firmware stored in " FW_FILE
                        " - source(\"/spi1/update.sh\") installs it\r\n");
            return EXIT_UPDATE;
        }
    }
}
