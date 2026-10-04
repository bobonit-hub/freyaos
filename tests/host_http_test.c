/*
 * The HTTP client library (http/) on the host, against a freya_api_t
 * whose network calls are a scripted server: what the library sends is
 * kept, the response is handed back a few bytes at a time, and every
 * call answers FREYA_ERR_AGAIN now and then, as the C6 link does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freya_http.h"

static char sent[8192];
static int sent_len;
static const char *reply;
static int reply_len, reply_pos;
static int sockets_open, connects, tls_connects, resolves, recv_stall;
static uint32_t connect_addr;
static uint16_t connect_port;
static char tls_host[64];
static uint32_t now;
static unsigned seed = 1;

static int roll(int n) { seed = seed * 1103515245U + 12345U; return (int)((seed >> 16) % (unsigned)n); }
static int again(void) { return roll(3) == 0; }

static uint32_t m_ticks(void) { return now; }
static int m_poll(uint32_t ms) { now += ms; return 0; }
static int m_socket(int d, int t, int p)
{
    if (again()) return FREYA_ERR_AGAIN;
    sockets_open++;
    return 2;
}
static int m_close(int s) { if (again()) return FREYA_ERR_AGAIN; sockets_open--; return 0; }
static int m_connect(int s, const freya_net_addr_t *a)
{
    if (again()) return FREYA_ERR_AGAIN;
    connects++;
    connect_addr = a->addr;
    connect_port = a->port;
    return 0;
}
static int m_tls(int s, const char *host, uint16_t port)
{
    if (again()) return FREYA_ERR_AGAIN;
    tls_connects++;
    strncpy(tls_host, host, sizeof tls_host - 1);
    connect_port = port;
    return 0;
}
static int m_resolve(const char *host, uint32_t *addr)
{
    if (again()) return FREYA_ERR_AGAIN;
    resolves++;
    if (strcmp(host, "nowhere.test") == 0) return FREYA_ERR_IO;
    *addr = 0xC0A80105U;
    return 0;
}
static int m_send(int s, const void *buf, int len)
{
    if (len < 1 || len > FREYA_NET_PAYLOAD_MAX) return FREYA_ERR_ARG;
    if (again()) return FREYA_ERR_AGAIN;
    len = 1 + roll(len);                     /* a part of it, as TCP may */
    memcpy(sent + sent_len, buf, (size_t)len);
    sent_len += len;
    return len;
}
static int m_recv(int s, void *buf, int len)
{
    int n;

    if (len < 1 || len > FREYA_NET_PAYLOAD_MAX) return FREYA_ERR_ARG;
    if (recv_stall || again()) return FREYA_ERR_AGAIN;
    n = reply_len - reply_pos;
    if (n > len) n = len;
    if (n > 7) n = 1 + roll(7);
    memcpy(buf, reply + reply_pos, (size_t)n);
    reply_pos += n;
    return n;
}

static freya_api_t api = {
    .size = sizeof(freya_api_t),
    .ticks_ms = m_ticks,
    .net_socket = m_socket,
    .net_close = m_close,
    .net_connect = m_connect,
    .net_tls_connect = m_tls,
    .net_resolve = m_resolve,
    .net_send = m_send,
    .net_recv = m_recv,
    .net_poll = m_poll,
};

static int checks, fails;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("  FAIL  %s\n", what); }
    else printf("  ok    %s\n", what);
}

static void serve(const char *response)
{
    sent_len = 0;
    reply = response;
    reply_len = (int)strlen(response);
    reply_pos = 0;
    connects = tls_connects = resolves = 0;
    tls_host[0] = '\0';
}

/* The whole body into out, NUL-terminated; its length or an error. */
static int read_all(freya_http_t *h, char *out, int cap)
{
    int total = 0, n;

    while ((n = http_read(h, out + total, 1 + roll(cap - total - 1))) > 0)
        total += n;
    out[total] = '\0';
    return n < 0 ? n : total;
}

static int lines;
static char first_line[64];
static void on_header(void *ctx, const char *line)
{
    if (lines++ == 0) strncpy(first_line, line, sizeof first_line - 1);
}

static int cancel_now;
static int on_cancel(void *ctx) { return cancel_now; }

int main(int argc, char **argv)
{
    freya_http_t h;
    char body[4096];
    int rc;

    if (argc > 1) seed = (unsigned)atoi(argv[1]);   /* another interleaving */

    /* GET over TLS, a fragment dropped, a Content-Length body. */
    serve("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
          "content-length: 11\r\n\r\nhello world");
    http_init(&h, &api);
    h.header = on_header;
    rc = http_open(&h, "GET", "https://example.com/path?q=1#frag");
    check(rc == 0 && tls_connects == 1 && strcmp(tls_host, "example.com") == 0 &&
          connect_port == 443 && resolves == 0,
          "https:// connects with TLS to the host, on 443, without a lookup");
    rc = http_response(&h);
    sent[sent_len] = '\0';
    check(strcmp(sent, "GET /path?q=1 HTTP/1.1\r\nHost: example.com\r\n"
                       "Connection: close\r\n\r\n") == 0,
          "the request head is the request line, Host and Connection");
    check(rc == 200 && h.status == 200 && h.length == 11,
          "the status and Content-Length are read, in any case");
    check(lines == 3 && strcmp(first_line, "HTTP/1.1 200 OK") == 0,
          "every head line reaches the callback, the status line first");
    rc = read_all(&h, body, sizeof body);
    check(rc == 11 && strcmp(body, "hello world") == 0,
          "the body comes back whole from small pieces");
    check(http_read(&h, body, 10) == 0, "the end of the body reads as 0");
    http_close(&h);
    check(sockets_open == 0 && h.socket == -1, "close gives the socket back");
    http_close(&h);
    check(sockets_open == 0, "a second close does nothing");

    /* POST over TCP to a name and a port, auth, a header, a body. */
    serve("HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok");
    http_init(&h, &api);
    rc = http_open(&h, "POST", "http://api.example:8080/form");
    check(rc == 0 && resolves == 1 && connects == 1 &&
          connect_addr == 0xC0A80105U && connect_port == 8080,
          "http:// looks the name up and connects to the port given");
    check(http_basic_auth(&h, "Aladdin:open sesame") == 0 &&
          http_header(&h, "X-Test", "1") == 0, "headers are accepted");
    check(http_header(&h, "X-Bad", "a\r\nInjected: 1") == FREYA_ERR_ARG &&
          http_header(&h, "Bad Name", "x") == FREYA_ERR_ARG &&
          http_header(&h, "", "x") == FREYA_ERR_ARG,
          "CR, LF and a bad name are refused in a header");
    check(http_send(&h, "a=1&b=2", 7) == 0, "the body is sent");
    rc = http_response(&h);
    sent[sent_len] = '\0';
    check(strcmp(sent, "POST /form HTTP/1.1\r\nHost: api.example:8080\r\n"
                       "Connection: close\r\n"
                       "Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==\r\n"
                       "X-Test: 1\r\nContent-Length: 7\r\n\r\na=1&b=2") == 0,
          "Host keeps the port; Basic is base64; the body has its length");
    check(rc == 201 && read_all(&h, body, sizeof body) == 2, "the reply is read");
    http_close(&h);

    /* An IPv4 address, a chunked body with an extension and a trailer. */
    serve("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, Chunked\r\n\r\n"
          "5;ext=1\r\nHello\r\n1A\r\n, chunked world, in parts!\r\n"
          "0\r\nX-Trailer: t\r\n\r\n");
    http_init(&h, &api);
    rc = http_open(&h, "GET", "http://10.0.0.1?x");
    check(rc == 0 && resolves == 0 && connect_addr == 0x0A000001U &&
          connect_port == 80, "an IPv4 address needs no lookup");
    rc = http_response(&h);
    sent[sent_len] = '\0';
    check(strncmp(sent, "GET /?x HTTP/1.1\r\nHost: 10.0.0.1\r\n", 34) == 0,
          "a query with no path gets the path /");
    check(rc == 200 && h.length == -1, "a chunked body has no length");
    rc = read_all(&h, body, sizeof body);
    check(rc == 31 && strcmp(body, "Hello, chunked world, in parts!") == 0,
          "a chunked body is decoded");
    check(reply_pos == reply_len, "the trailer is read to its end");
    http_close(&h);

    /* 100 Continue, then a body that ends when the connection does. */
    serve("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.0 200 OK\r\nServer: x\r\n\r\n"
          "until the end");
    http_init(&h, &api);
    rc = http_open(&h, "PUT", "http://10.0.0.1/f");
    check(rc == 0 && http_send(&h, NULL, 600) == 0, "a body announced up front");
    memset(body, 'b', 600);
    check(http_write(&h, body, 250) == 0 && http_write(&h, body, 350) == 0,
          "and written in parts larger than a fragment, in total");
    check(http_write(&h, body, 1) == FREYA_ERR_ARG,
          "not one byte more than announced");
    rc = http_response(&h);
    check(rc == 200 && h.length == -1, "a 1xx response is skipped");
    check(sent_len > 600 && memcmp(sent + sent_len - 4 - 600, "\r\n\r\n", 4) == 0 &&
          strstr(sent, "Content-Length: 600\r\n") != NULL,
          "the 600 bytes follow the head whole");
    rc = read_all(&h, body, sizeof body);
    check(rc == 13 && strcmp(body, "until the end") == 0,
          "a body with neither length nor chunks runs to the close");
    http_close(&h);

    /* A coding that only ends in "chunked" is not chunked. */
    serve("HTTP/1.1 200 OK\r\nTransfer-Encoding: notchunked\r\n\r\n5\r\n");
    http_init(&h, &api);
    http_open(&h, "GET", "http://10.0.0.1/");
    check(http_response(&h) == 200 && read_all(&h, body, sizeof body) == 3,
          "only the word chunked makes a body chunked");
    http_close(&h);

    /* HEAD: a length and no body. */
    serve("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    http_init(&h, &api);
    rc = http_open(&h, "HEAD", "http://10.0.0.1/");
    check(rc == 0 && http_response(&h) == 200 && h.length == 1234 &&
          http_read(&h, body, 10) == 0, "a HEAD response has no body");
    http_close(&h);

    /* A POST with no body still says how long it is. */
    serve("HTTP/1.1 204 No Content\r\n\r\n");
    http_init(&h, &api);
    rc = http_open(&h, "POST", "http://10.0.0.1/");
    check(rc == 0 && http_response(&h) == 204 && http_read(&h, body, 10) == 0,
          "a 204 has no body");
    sent[sent_len] = '\0';
    check(strstr(sent, "Content-Length: 0\r\n\r\n") != NULL,
          "an empty POST carries Content-Length: 0");
    http_close(&h);

    /* Failures. */
    serve("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
    http_init(&h, &api);
    http_open(&h, "GET", "http://10.0.0.1/");
    http_response(&h);
    rc = read_all(&h, body, sizeof body);
    check(rc == FREYA_ERR_IO, "a body cut short of its length is an error");
    http_close(&h);

    serve("SSH-2.0-OpenSSH\r\n\r\n");
    http_init(&h, &api);
    http_open(&h, "GET", "http://10.0.0.1/");
    check(http_response(&h) == FREYA_ERR_IO, "a reply that is not HTTP is an error");
    http_close(&h);

    http_init(&h, &api);
    check(http_open(&h, "GET", "ftp://x/") == FREYA_ERR_ARG &&
          http_open(&h, "GET", "http://") == FREYA_ERR_ARG &&
          http_open(&h, "GET", "http://a b/") == FREYA_ERR_ARG &&
          http_open(&h, "GET", "http://x:0/") == FREYA_ERR_ARG &&
          http_open(&h, "GET", "http://x:70000/") == FREYA_ERR_ARG &&
          http_open(&h, "GET", "http://x:8a/") == FREYA_ERR_ARG &&
          http_open(&h, "GET", "http://u@x/") == FREYA_ERR_ARG &&
          http_open(&h, "GET", "http://x/a\nb") == FREYA_ERR_ARG &&
          http_open(&h, "G T", "http://x/") == FREYA_ERR_ARG,
          "bad URLs and methods are refused before connecting");
    check(sockets_open == 0, "and leave no socket behind");

    serve("");
    check(http_open(&h, "GET", "http://nowhere.test/") == FREYA_ERR_IO &&
          sockets_open == 0, "a name that does not resolve fails and closes");

    serve("");
    http_init(&h, &api);
    http_open(&h, "GET", "http://10.0.0.1/");
    recv_stall = 1;
    now = 0;
    rc = http_response(&h);
    check(rc == FREYA_ERR_TIMEOUT && now >= FREYA_HTTP_TIMEOUT_MS,
          "a server that says nothing times out");
    http_close(&h);
    h.cancel = on_cancel;
    cancel_now = 0;
    rc = http_open(&h, "GET", "http://10.0.0.1/");
    cancel_now = 1;
    rc = rc ? rc : http_response(&h);
    check(rc == FREYA_ERR_IO, "cancel stops the wait");
    http_close(&h);
    recv_stall = 0;
    check(sockets_open == 0, "every socket was given back");

    {
        freya_api_t old = api;
        old.size = (uint32_t)__builtin_offsetof(freya_api_t, net_resolve);
        serve("");
        http_init(&h, &old);
        check(http_open(&h, "GET", "http://name.test/") == FREYA_ERR_UNSUPPORTED &&
              sockets_open == 0,
              "a kernel without net_resolve cannot reach an http:// name");
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
