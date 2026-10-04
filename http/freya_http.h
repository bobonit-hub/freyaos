/*
 * Freya - the HTTP client library.
 *
 * HTTP/1.1 over the network calls of freya_api.h: http:// on a TCP
 * socket, https:// on a TLS 1.3 socket that the ESP32-C6 terminates and
 * verifies.  One request per connection ("Connection: close").  The
 * request body and the response body are streamed in pieces, so neither
 * has a size limit here; a chunked response is decoded.  The library
 * keeps no state of its own: everything is in the freya_http_t the
 * caller provides, about 800 bytes.
 *
 * The kernel's curl command is built on it, and a program links
 * build/<board>/http/libfreya_http.a (every board with the network
 * link).  See docs/http.md.
 *
 * Every call waits: it polls the link with net_poll() until the
 * operation is done.  A call returns a FREYA_ERR_* code (negative) on
 * failure: FREYA_ERR_ARG for a bad URL, header or call order,
 * FREYA_ERR_TIMEOUT when nothing moved for timeout_ms, FREYA_ERR_IO for
 * a connection that failed, closed early or sent something that is not
 * HTTP, or that was cancelled, and FREYA_ERR_UNSUPPORTED for a board
 * without the network or an http:// host name on a kernel that cannot
 * look names up.
 *
 *     freya_http_t h;
 *     http_init(&h, api);
 *     if (http_open(&h, "GET", "https://example.com/") == 0 &&
 *         http_response(&h) == 200)
 *         while ((n = http_read(&h, buf, sizeof buf)) > 0)
 *             ...
 *     http_close(&h);
 */
#ifndef FREYA_HTTP_H
#define FREYA_HTTP_H

#include <stdint.h>
#include "freya_api.h"

#define FREYA_HTTP_TIMEOUT_MS   30000U  /* default timeout_ms              */
#define FREYA_HTTP_LINE         256     /* a response head line, with NUL;
                                         * a longer one is cut short      */

typedef struct freya_http {
    /* Set by http_init(); the caller may change them before http_open(). */
    const freya_api_t *api;
    uint32_t timeout_ms;                /* longest wait without progress  */
    int    (*cancel)(void *ctx);        /* non-zero stops the call, or NULL */
    void   (*header)(void *ctx, const char *line); /* each response head
                                         * line, the status line first, or
                                         * NULL                           */
    void    *ctx;                       /* passed to cancel and header    */

    /* The response, filled in by http_response(). */
    int      status;                    /* 200, 404, ...                  */
    int32_t  length;                    /* Content-Length, or -1          */

    /* The library's own. */
    int      socket;
    uint8_t  state;
    uint8_t  mode;
    uint8_t  flags;
    uint8_t  spare;
    uint32_t left;
    uint16_t pos;
    uint16_t len;
    uint8_t  io[FREYA_NET_PAYLOAD_MAX];
    char     line[FREYA_HTTP_LINE];
} freya_http_t;

/* Clears h and sets api, the default timeout and no callbacks. */
void http_init(freya_http_t *h, const freya_api_t *api);

/* Connects to the URL's host and starts the request: the request line,
 * Host and Connection.  method is "GET", "POST", "PUT", "DELETE",
 * "HEAD" or any other token.  The URL is http:// or https://, a host
 * name or an IPv4 address, an optional port, and a path with an
 * optional query; a fragment is dropped.  The C6 must be associated
 * (wifi("connect")). */
int  http_open(freya_http_t *h, const char *method, const char *url);

/* Adds a request header.  Neither part may hold CR or LF. */
int  http_header(freya_http_t *h, const char *name, const char *value);

/* Adds "Authorization: Basic" for "user:password". */
int  http_basic_auth(freya_http_t *h, const char *user_password);

/* Ends the request head.  When body is not NULL it is sent, len bytes.
 * When body is NULL and len is not 0, len bytes follow with
 * http_write().  Content-Length is sent when len is not 0, and for a
 * POST, PUT or PATCH.  http_response() ends the head itself when the
 * request has no body. */
int  http_send(freya_http_t *h, const void *body, uint32_t len);

/* Sends the next part of the body announced by http_send().  It may not
 * go past that length. */
int  http_write(freya_http_t *h, const void *data, uint32_t len);

/* Reads the response head, skipping any 1xx response, and returns the
 * status code, or a FREYA_ERR_*.  status and length are set. */
int  http_response(freya_http_t *h);

/* Reads up to len bytes of the response body: the count, 0 at its end,
 * or a FREYA_ERR_*.  A chunked body comes back decoded. */
int  http_read(freya_http_t *h, void *buf, int len);

/* Closes the connection.  Safe to call at any point, and twice. */
void http_close(freya_http_t *h);

#endif /* FREYA_HTTP_H */
