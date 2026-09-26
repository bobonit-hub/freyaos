/*
 * httpd - TLS file service for the ESP32-C6 web server.
 *
 * The C6 accepts one HTTPS connection on port 443 (TLS 1.3 only) and
 * parses the request. This program is the other half: it maps the URL
 * onto /www on the card. A path that ends in .sh is a shell script.
 * The script's printed output is the page. Every other file is sent
 * unchanged. Nothing else is served.
 *
 *     run httpd.bin                 pages in /www on the card
 *     run httpd.bin /spi1/www       pages on the SPI NOR volume
 *
 * Wi-Fi credentials already stored on the C6 are used. Ctrl-C returns
 * to the shell. A path that is only the docroot name is not served;
 * the URL is appended to that directory:
 *
 *     /www/index.html     static
 *     /www/status.sh      dynamic
 */
#include "freya_api.h"

#define PAGE_MAX    4096

static char s_page[PAGE_MAX];
static char s_root[64] = "/www";

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int ends(const char *s, const char *suf)
{
    int n = 0, m = 0;

    while (s[n]) n++;
    while (suf[m]) m++;
    if (n < m) return 0;
    return eq(s + n - m, suf);
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
    if (eq(dot, ".txt")) return "text/plain; charset=utf-8";
    if (eq(dot, ".json")) return "application/json";
    if (eq(dot, ".svg")) return "image/svg+xml";
    if (eq(dot, ".png")) return "image/png";
    if (eq(dot, ".jpg") || eq(dot, ".jpeg")) return "image/jpeg";
    if (eq(dot, ".gif")) return "image/gif";
    if (eq(dot, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

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
    int n = 0;

    while (text[n]) n++;
    return send_mem(api, method, status, "text/plain; charset=utf-8", text, n);
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

static void serve(const freya_api_t *api, const freya_web_req_t *req)
{
    char path[128];
    int rc;

    if (map_path(req->path, path, (int)sizeof path) != 0) {
        send_text(api, req->method, 403, "forbidden\n");
        return;
    }
    if (ends(path, ".sh"))
        rc = send_script(api, req->method, path, req->query);
    else
        rc = send_file(api, req->method, path, content_type(path));
    if (rc != 0 && rc != FREYA_ERR_IO)
        api->printf("httpd: %s failed (%d)\r\n", req->path, rc);
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    freya_wifi_status_t st;
    int rc;

    if (!FREYA_API_HAS(api, shell_source_capture) ||
        !FREYA_API_HAS(api, web_take)) {
        api->puts("httpd: this kernel has no HTTPS file service\r\n");
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

        do { rc = poll_again(api, api->web_take(&req)); }
        while (rc == FREYA_ERR_AGAIN);
        if (rc != 0) {
            api->printf("httpd: link failed (%d)\r\n", rc);
            return FREYA_EXIT_FAIL;
        }
        serve(api, &req);
    }
}
