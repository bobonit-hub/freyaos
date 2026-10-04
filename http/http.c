/*
 * Freya - the HTTP client library (see freya_http.h).
 *
 * The same source is the kernel's, for curl, and the program library's.
 * It calls the network only through the freya_api_t it is given, and
 * has its own string helpers, so it needs nothing else from either side.
 *
 * h->io is the one buffer: the request is gathered there and sent in
 * FREYA_NET_PAYLOAD_MAX pieces, and once it has gone the response is
 * received into it.
 */
#include <stddef.h>
#include "freya_http.h"

/* h->state */
enum { ST_IDLE, ST_HEAD, ST_BODY, ST_RESPONSE, ST_DONE };

/* h->mode: how the response body ends */
enum { BODY_LENGTH, BODY_CHUNKED, BODY_CLOSE };

/* h->flags */
#define F_HEAD_METHOD   0x01U   /* a HEAD request: no response body      */
#define F_BODY_METHOD   0x02U   /* POST, PUT, PATCH: always a length     */
#define F_CHUNK_CRLF    0x04U   /* a chunk's CRLF is still to be read    */

#define END             (-100)  /* the peer closed the connection        */

/* ------------------------------------------------------------ strings */
static uint32_t str_len(const char *s)
{
    uint32_t n = 0;

    while (s[n]) n++;
    return n;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* Whether s starts with prefix, ignoring case. */
static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
        if (lower(*s++) != lower(*prefix++)) return 0;
    return 1;
}

/* Whether the comma-separated list s holds word, ignoring case. */
static int has_word(const char *s, const char *word)
{
    uint32_t n = str_len(word);

    for (const char *p = s; *p; p++) {
        if ((p == s || p[-1] == ',' || p[-1] == ' ' || p[-1] == '\t') &&
            starts_with(p, word) &&
            (p[n] == '\0' || p[n] == ',' || p[n] == ' ' || p[n] == ';'))
            return 1;
    }
    return 0;
}

static const char *skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Whether s can go into the request head: no control characters. */
static int clean(const char *s)
{
    for (; *s; s++)
        if ((uint8_t)*s < 0x20 && *s != '\t') return 0;
    return 1;
}

/* ---------------------------------------------------------- the wait */
/*
 * After a call answered FREYA_ERR_AGAIN: polls the link, and says
 * whether to call again (0) or give up.
 */
static int wait_more(freya_http_t *h, uint32_t start)
{
    if (h->api->net_poll(20) < 0) return FREYA_ERR_IO;
    if (h->cancel && h->cancel(h->ctx)) return FREYA_ERR_IO;
    if ((uint32_t)(h->api->ticks_ms() - start) >= h->timeout_ms)
        return FREYA_ERR_TIMEOUT;
    return 0;
}

#define RETRY(h, rc, call)                                              \
    do {                                                                \
        uint32_t start_ = (h)->api->ticks_ms();                         \
        while (((rc) = (call)) == FREYA_ERR_AGAIN &&                    \
               ((rc) = wait_more((h), start_)) == 0)                    \
            ;                                                           \
    } while (0)

/* ----------------------------------------------------------- sending */
static int flush(freya_http_t *h)
{
    uint16_t done = 0;
    int rc;

    while (done < h->len) {
        RETRY(h, rc, h->api->net_send(h->socket, h->io + done,
                                      h->len - done));
        if (rc < 0) return rc;
        if (rc == 0) return FREYA_ERR_IO;
        done = (uint16_t)(done + rc);
    }
    h->len = 0;
    return 0;
}

static int put_bytes(freya_http_t *h, const void *data, uint32_t n)
{
    const uint8_t *p = data;
    int rc;

    while (n) {
        uint32_t room = sizeof h->io - h->len;
        if (room == 0) {
            rc = flush(h);
            if (rc) return rc;
            continue;
        }
        if (room > n) room = n;
        for (uint32_t i = 0; i < room; i++) h->io[h->len + i] = p[i];
        h->len = (uint16_t)(h->len + room);
        p += room;
        n -= room;
    }
    return 0;
}

static int put(freya_http_t *h, const char *s)
{
    return put_bytes(h, s, str_len(s));
}

static int put_number(freya_http_t *h, uint32_t v)
{
    char text[11];
    int i = sizeof text - 1;

    text[i] = '\0';
    do {
        text[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return put(h, text + i);
}

/* --------------------------------------------------------- receiving */
/* The next byte of the response, END, or a FREYA_ERR_*. */
static int next_byte(freya_http_t *h)
{
    int rc;

    if (h->pos == h->len) {
        RETRY(h, rc, h->api->net_recv(h->socket, h->io, sizeof h->io));
        if (rc < 0) return rc;
        if (rc == 0) return END;
        h->pos = 0;
        h->len = (uint16_t)rc;
    }
    return h->io[h->pos++];
}

/* One line into h->line, without its CR LF, cut to fit.  Its length,
 * or a FREYA_ERR_*: a connection that closes mid-line is FREYA_ERR_IO. */
static int read_line(freya_http_t *h)
{
    int n = 0, c;

    for (;;) {
        c = next_byte(h);
        if (c == END) return FREYA_ERR_IO;
        if (c < 0) return c;
        if (c == '\n') break;
        if (n < FREYA_HTTP_LINE - 1) h->line[n++] = (char)c;
    }
    if (n && h->line[n - 1] == '\r') n--;
    h->line[n] = '\0';
    return n;
}

/* --------------------------------------------------------------- URL */
/*
 * Splits url.  The host goes to h->line; the port, whether it is TLS and
 * where the path starts come back.
 */
static int parse_url(freya_http_t *h, const char *url, uint16_t *port,
                     int *tls, const char **path)
{
    const char *p;
    uint32_t n = 0;

    if (starts_with(url, "https://")) {
        *tls = 1;
        *port = 443;
        p = url + 8;
    } else if (starts_with(url, "http://")) {
        *tls = 0;
        *port = 80;
        p = url + 7;
    } else {
        return FREYA_ERR_ARG;
    }
    while (p[n] && p[n] != ':' && p[n] != '/' && p[n] != '?' && p[n] != '#') {
        if ((uint8_t)p[n] <= ' ' || p[n] == '@' || p[n] == '[') return FREYA_ERR_ARG;
        if (n >= 253) return FREYA_ERR_ARG;
        h->line[n] = p[n];
        n++;
    }
    if (n == 0) return FREYA_ERR_ARG;
    h->line[n] = '\0';
    p += n;
    if (*p == ':') {
        uint32_t v = 0;
        p++;
        if (*p < '0' || *p > '9') return FREYA_ERR_ARG;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p++ - '0');
            if (v > 65535) return FREYA_ERR_ARG;
        }
        if (v == 0 || (*p && *p != '/' && *p != '?' && *p != '#'))
            return FREYA_ERR_ARG;
        *port = (uint16_t)v;
    }
    for (n = 0; p[n] && p[n] != '#'; n++)
        if ((uint8_t)p[n] <= ' ' || p[n] == 0x7f) return FREYA_ERR_ARG;
    *path = p;
    return 0;
}

/* A dotted IPv4 address, in the order freya_net_addr_t keeps. */
static int parse_ipv4(const char *s, uint32_t *addr)
{
    uint32_t a = 0;

    for (int part = 0; part < 4; part++) {
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (uint32_t)(*s++ - '0');
            digits++;
        }
        if (!digits || v > 255) return -1;
        a = a << 8 | v;
        if (part < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    *addr = a;
    return 0;
}

/* Connects h->socket to the host in h->line. */
static int open_connection(freya_http_t *h, uint16_t port, int tls)
{
    const freya_api_t *api = h->api;
    freya_net_addr_t to;
    int rc;

    if (tls) {
        if (!FREYA_API_HAS(api, net_tls_connect)) return FREYA_ERR_UNSUPPORTED;
        RETRY(h, rc, api->net_tls_connect(h->socket, h->line, port));
        return rc;
    }
    to.port = port;
    to.reserved = 0;
    if (parse_ipv4(h->line, &to.addr) != 0) {
        if (!FREYA_API_HAS(api, net_resolve)) return FREYA_ERR_UNSUPPORTED;
        RETRY(h, rc, api->net_resolve(h->line, &to.addr));
        if (rc) return rc;
    }
    RETRY(h, rc, api->net_connect(h->socket, &to));
    return rc;
}

/* ------------------------------------------------------------ request */
void http_init(freya_http_t *h, const freya_api_t *api)
{
    uint8_t *p = (uint8_t *)h;

    for (uint32_t i = 0; i < sizeof *h; i++) p[i] = 0;
    h->api = api;
    h->timeout_ms = FREYA_HTTP_TIMEOUT_MS;
    h->socket = -1;
    h->length = -1;
}

int http_open(freya_http_t *h, const char *method, const char *url)
{
    const freya_api_t *api = h->api;
    const char *path;
    uint16_t port;
    int tls, rc;

    if (h->state != ST_IDLE || !method || !*method || !url ||
        !api || !FREYA_API_HAS(api, net_poll))
        return FREYA_ERR_ARG;
    for (const char *m = method; *m; m++)
        if ((uint8_t)*m <= ' ' || (uint8_t)*m >= 0x7f) return FREYA_ERR_ARG;
    rc = parse_url(h, url, &port, &tls, &path);
    if (rc) return rc;

    h->status = 0;
    h->length = -1;
    h->left = 0;
    h->pos = h->len = 0;
    h->flags = 0;
    if (starts_with(method, "HEAD") && !method[4]) h->flags |= F_HEAD_METHOD;
    if ((starts_with(method, "POST") && !method[4]) ||
        (starts_with(method, "PUT") && !method[3]) ||
        (starts_with(method, "PATCH") && !method[5]))
        h->flags |= F_BODY_METHOD;

    RETRY(h, rc, api->net_socket(FREYA_AF_INET, FREYA_SOCK_STREAM,
                                 FREYA_IPPROTO_TCP));
    if (rc < 0) return rc;
    h->socket = rc;
    h->state = ST_HEAD;
    rc = open_connection(h, port, tls);
    if (rc) goto fail;

    /* The request line, then the host, which is still in h->line. */
    rc = put(h, method);
    if (!rc) rc = put(h, " ");
    if (!rc && *path != '/') rc = put(h, "/");
    for (const char *p = path; !rc && *p && *p != '#'; p++)
        rc = put_bytes(h, p, 1);
    if (!rc) rc = put(h, " HTTP/1.1\r\nHost: ");
    if (!rc) rc = put(h, h->line);
    if (!rc && port != (tls ? 443 : 80)) {
        rc = put(h, ":");
        if (!rc) rc = put_number(h, port);
    }
    if (!rc) rc = put(h, "\r\nConnection: close\r\n");
    if (rc) goto fail;
    return 0;

fail:
    http_close(h);
    return rc;
}

int http_header(freya_http_t *h, const char *name, const char *value)
{
    int rc;

    if (h->state != ST_HEAD || !name || !*name || !value ||
        !clean(name) || !clean(value))
        return FREYA_ERR_ARG;
    for (const char *p = name; *p; p++)
        if (*p == ':' || *p == ' ' || *p == '\t') return FREYA_ERR_ARG;
    rc = put(h, name);
    if (!rc) rc = put(h, ": ");
    if (!rc) rc = put(h, value);
    if (!rc) rc = put(h, "\r\n");
    return rc;
}

int http_basic_auth(freya_http_t *h, const char *user_password)
{
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = (const uint8_t *)user_password;
    uint32_t n;
    int rc;

    if (h->state != ST_HEAD || !user_password || !clean(user_password))
        return FREYA_ERR_ARG;
    n = str_len(user_password);
    rc = put(h, "Authorization: Basic ");
    while (!rc && n) {
        uint32_t take = n < 3 ? n : 3;
        uint32_t v = (uint32_t)p[0] << 16 |
                     (take > 1 ? (uint32_t)p[1] << 8 : 0) |
                     (take > 2 ? p[2] : 0);
        char out[4];
        out[0] = digits[v >> 18 & 63];
        out[1] = digits[v >> 12 & 63];
        out[2] = take > 1 ? digits[v >> 6 & 63] : '=';
        out[3] = take > 2 ? digits[v & 63] : '=';
        rc = put_bytes(h, out, 4);
        p += take;
        n -= take;
    }
    if (!rc) rc = put(h, "\r\n");
    return rc;
}

int http_send(freya_http_t *h, const void *body, uint32_t len)
{
    int rc = 0;

    if (h->state != ST_HEAD) return FREYA_ERR_ARG;
    if (len || (h->flags & F_BODY_METHOD)) {
        rc = put(h, "Content-Length: ");
        if (!rc) rc = put_number(h, len);
        if (!rc) rc = put(h, "\r\n");
    }
    if (!rc) rc = put(h, "\r\n");
    if (rc) return rc;
    h->state = ST_BODY;
    h->left = len;
    return body ? http_write(h, body, len) : 0;
}

int http_write(freya_http_t *h, const void *data, uint32_t len)
{
    int rc;

    if (h->state != ST_BODY || len > h->left || (len && !data))
        return FREYA_ERR_ARG;
    rc = put_bytes(h, data, len);
    if (rc) return rc;
    h->left -= len;
    return 0;
}

/* ----------------------------------------------------------- response */
/* "HTTP/1.1 200 OK" -> 200, or FREYA_ERR_IO. */
static int status_code(const char *line)
{
    const char *p = line;
    int code = 0;

    if (!starts_with(p, "HTTP/")) return FREYA_ERR_IO;
    while (*p && *p != ' ') p++;
    p = skip_space(p);
    for (int i = 0; i < 3; i++) {
        if (p[i] < '0' || p[i] > '9') return FREYA_ERR_IO;
        code = code * 10 + (p[i] - '0');
    }
    if (p[3] && p[3] != ' ') return FREYA_ERR_IO;
    return code;
}

static int read_head(freya_http_t *h)
{
    int rc, chunked = 0;

    rc = read_line(h);
    if (rc < 0) return rc;
    rc = status_code(h->line);
    if (rc < 0) return rc;
    h->status = rc;
    h->length = -1;
    if (h->header) h->header(h->ctx, h->line);
    for (;;) {
        rc = read_line(h);
        if (rc < 0) return rc;
        if (rc == 0) break;
        if (h->header) h->header(h->ctx, h->line);
        if (starts_with(h->line, "Content-Length:")) {
            const char *p = skip_space(h->line + 15);
            uint32_t v = 0;
            if (*p < '0' || *p > '9') return FREYA_ERR_IO;
            while (*p >= '0' && *p <= '9') {
                if (v > 0x7fffffffU / 10) return FREYA_ERR_IO;
                v = v * 10 + (uint32_t)(*p++ - '0');
            }
            if (v > 0x7fffffffU) return FREYA_ERR_IO;
            h->length = (int32_t)v;
        } else if (starts_with(h->line, "Transfer-Encoding:")) {
            chunked = has_word(h->line + 18, "chunked");
        }
    }
    if ((h->flags & F_HEAD_METHOD) || h->status < 200 ||
        h->status == 204 || h->status == 304) {
        h->mode = BODY_LENGTH;
        h->left = 0;
    } else if (chunked) {
        h->mode = BODY_CHUNKED;
        h->length = -1;
        h->left = 0;
    } else if (h->length >= 0) {
        h->mode = BODY_LENGTH;
        h->left = (uint32_t)h->length;
    } else {
        h->mode = BODY_CLOSE;
    }
    return h->status;
}

int http_response(freya_http_t *h)
{
    int rc;

    if (h->state == ST_HEAD) {
        rc = http_send(h, NULL, 0);
        if (rc) return rc;
    }
    if (h->state != ST_BODY || h->left) return FREYA_ERR_ARG;
    rc = flush(h);
    if (rc) return rc;
    h->state = ST_RESPONSE;
    h->pos = h->len = 0;
    do {
        rc = read_head(h);
        if (rc < 0) return rc;
    } while (rc >= 100 && rc < 200);
    if (h->mode == BODY_LENGTH && h->left == 0) h->state = ST_DONE;
    return rc;
}

/* The size line of the next chunk into h->left; at the last chunk the
 * trailer is read too and the body is over. */
static int next_chunk(freya_http_t *h)
{
    uint32_t size = 0;
    int rc, digits = 0;

    if (h->flags & F_CHUNK_CRLF) {
        rc = read_line(h);
        if (rc < 0) return rc;
        if (rc != 0) return FREYA_ERR_IO;
        h->flags &= (uint8_t)~F_CHUNK_CRLF;
    }
    rc = read_line(h);
    if (rc < 0) return rc;
    for (const char *p = h->line; ; p++, digits++) {
        int c = lower(*p), v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else break;
        if (size > 0x0fffffffU) return FREYA_ERR_IO;
        size = size << 4 | (uint32_t)v;
    }
    if (!digits) return FREYA_ERR_IO;
    if (size == 0) {
        do {
            rc = read_line(h);
            if (rc < 0) return rc;
        } while (rc != 0);
        h->state = ST_DONE;
        return 0;
    }
    h->left = size;
    h->flags |= F_CHUNK_CRLF;
    return 0;
}

int http_read(freya_http_t *h, void *buf, int len)
{
    uint8_t *out = buf;
    uint32_t n;
    int rc;

    if (h->state == ST_DONE) return 0;
    if (h->state != ST_RESPONSE || !buf || len < 1) return FREYA_ERR_ARG;
    if (h->mode == BODY_CHUNKED && h->left == 0) {
        rc = next_chunk(h);
        if (rc) return rc;
        if (h->state == ST_DONE) return 0;
    }
    if (h->pos == h->len) {
        RETRY(h, rc, h->api->net_recv(h->socket, h->io, sizeof h->io));
        if (rc < 0) return rc;
        if (rc == 0) {
            if (h->mode != BODY_CLOSE) return FREYA_ERR_IO;
            h->state = ST_DONE;
            return 0;
        }
        h->pos = 0;
        h->len = (uint16_t)rc;
    }
    n = (uint32_t)(h->len - h->pos);
    if (n > (uint32_t)len) n = (uint32_t)len;
    if (h->mode != BODY_CLOSE && n > h->left) n = h->left;
    for (uint32_t i = 0; i < n; i++) out[i] = h->io[h->pos + i];
    h->pos = (uint16_t)(h->pos + n);
    if (h->mode != BODY_CLOSE) {
        h->left -= n;
        if (h->mode == BODY_LENGTH && h->left == 0) h->state = ST_DONE;
    }
    return (int)n;
}

void http_close(freya_http_t *h)
{
    if (h->socket >= 0) {
        /* Not cancelled: the close is what a cancel wants done.  One that
         * cannot finish still gives the slot up here; in a program the
         * kernel frees the C6's when the run ends. */
        uint32_t start = h->api->ticks_ms();
        while (h->api->net_close(h->socket) == FREYA_ERR_AGAIN &&
               h->api->net_poll(20) >= 0 &&
               (uint32_t)(h->api->ticks_ms() - start) < 2000U)
            ;
        h->socket = -1;
    }
    h->state = ST_IDLE;
    h->pos = h->len = 0;
}
