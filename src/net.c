#include "freya.h"
#include "esp_link.h"

#if BOARD_ESP_LINK
typedef struct {
    int16_t remote;
    uint8_t used;
    uint8_t from_app;
    uint8_t type;
} net_socket_state_t;

static net_socket_state_t s_socket[FREYA_NET_SOCKETS];
static uint16_t s_rpc_op;
static uint8_t s_transport_from_app;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int rpc(uint16_t op, const void *request, uint16_t request_len,
               void *response, uint16_t *response_len)
{
    int rc;

    if (!esp_link_is_open()) return FREYA_ERR_UNSUPPORTED;
    if (s_rpc_op) {
        if (s_rpc_op != op) return FREYA_ERR_AGAIN;
        if (!esp_link_response_ready(op)) return FREYA_ERR_AGAIN;
        rc = esp_link_response(op, response, response_len);
        /* FREYA_ERR_AGAIN may be the remote operation's result, not an
         * indication that the response frame is still pending. */
        s_rpc_op = 0;
        return rc;
    }
    rc = esp_link_submit(op, request, request_len);
    if (rc == 0) {
        s_rpc_op = op;
        return FREYA_ERR_AGAIN;
    }
    return rc;
}

static int noarg(uint16_t op)
{
    uint16_t n = 0;
    return rpc(op, NULL, 0, NULL, &n);
}

int wifi_on(void)
{
    int rc;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!esp_link_is_open()) {
        rc = esp_link_open();
        if (rc) return rc;
        s_transport_from_app = (uint8_t)(g_app.running ? 1 : 0);
    }
    return noarg(ESP_OP_WIFI_ON);
}

int wifi_off(void)
{
    int rc;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!esp_link_is_open()) return 0;
    rc = noarg(ESP_OP_WIFI_OFF);
    if (rc == 0) {
        memset(s_socket, 0, sizeof s_socket);
        esp_link_close();
        s_rpc_op = 0;
        s_transport_from_app = 0;
    }
    return rc;
}

int wifi_credentials(const char *ssid, const char *password)
{
    uint8_t b[FREYA_WIFI_SSID_MAX + FREYA_WIFI_PASS_MAX + 2];
    uint32_t sn, pn;
    uint16_t n = 0;

    if (!ssid || !password) return FREYA_ERR_ARG;
    sn = strlen(ssid);
    pn = strlen(password);
    if (!sn || sn > FREYA_WIFI_SSID_MAX || pn > FREYA_WIFI_PASS_MAX)
        return FREYA_ERR_ARG;
    memcpy(b, ssid, sn + 1);
    memcpy(b + sn + 1, password, pn + 1);
    return rpc(ESP_OP_WIFI_CREDENTIALS, b, (uint16_t)(sn + pn + 2), NULL, &n);
}

int wifi_connect(void) { return noarg(ESP_OP_WIFI_CONNECT); }
int wifi_disconnect(void) { return noarg(ESP_OP_WIFI_DISCONNECT); }

int wifi_status(freya_wifi_status_t *status)
{
    uint16_t n = sizeof *status;
    int rc;

    if (!status) return FREYA_ERR_ARG;
    rc = rpc(ESP_OP_WIFI_STATUS, NULL, 0, status, &n);
    if (rc == 0 && n != sizeof *status) return FREYA_ERR_IO;
    return rc;
}

int wifi_scan_start(void) { return noarg(ESP_OP_WIFI_SCAN_START); }

int wifi_scan_next(freya_wifi_scan_t *entry)
{
    uint16_t n = sizeof *entry;
    int rc;

    if (!entry) return FREYA_ERR_ARG;
    rc = rpc(ESP_OP_WIFI_SCAN_NEXT, NULL, 0, entry, &n);
    if (rc == 0 && n != sizeof *entry) return FREYA_ERR_IO;
    return rc;
}

int ping_start(const char *host, uint32_t timeout_ms)
{
    uint8_t b[260];
    uint32_t n;
    uint16_t z = 0;

    if (!host || !*host || !timeout_ms) return FREYA_ERR_ARG;
    n = strlen(host);
    if (n > 253) return FREYA_ERR_ARG;
    put32(b, timeout_ms);
    memcpy(b + 4, host, n + 1);
    return rpc(ESP_OP_PING_START, b, (uint16_t)(n + 5), NULL, &z);
}

int ping_result(freya_ping_result_t *result)
{
    uint16_t n = sizeof *result;
    int rc;

    if (!result) return FREYA_ERR_ARG;
    rc = rpc(ESP_OP_PING_RESULT, NULL, 0, result, &n);
    if (rc == 0 && n != sizeof *result) return FREYA_ERR_IO;
    return rc;
}

static int local_socket(int socket)
{
    if (socket < 0 || socket >= FREYA_NET_SOCKETS || !s_socket[socket].used)
        return -1;
    return socket;
}

static int free_socket(void)
{
    for (int i = 0; i < FREYA_NET_SOCKETS; i++)
        if (!s_socket[i].used) return i;
    return -1;
}

int net_socket(int domain, int type, int protocol)
{
    uint8_t b[6];
    uint16_t n = 0;
    int slot, rc;

    if (domain != FREYA_AF_INET ||
        (type != FREYA_SOCK_STREAM && type != FREYA_SOCK_DGRAM) ||
        (protocol != 0 && protocol != FREYA_IPPROTO_TCP &&
         protocol != FREYA_IPPROTO_UDP))
        return FREYA_ERR_ARG;
    slot = free_socket();
    if (slot < 0) return FREYA_ERR_BUSY;
    put16(b, (uint16_t)domain);
    put16(b + 2, (uint16_t)type);
    put16(b + 4, (uint16_t)protocol);
    rc = rpc(ESP_OP_SOCKET, b, sizeof b, NULL, &n);
    if (rc >= 0) {
        s_socket[slot].used = 1;
        s_socket[slot].remote = (int16_t)rc;
        s_socket[slot].from_app = (uint8_t)(g_app.running ? 1 : 0);
        s_socket[slot].type = (uint8_t)type;
        return slot;
    }
    return rc;
}

static uint16_t socket_request(uint8_t *b, int socket)
{
    put16(b, (uint16_t)s_socket[socket].remote);
    return 2;
}

int net_close(int socket)
{
    uint8_t b[2];
    uint16_t n = 0;
    int rc;

    if (local_socket(socket) < 0) return FREYA_ERR_ARG;
    socket_request(b, socket);
    rc = rpc(ESP_OP_CLOSE, b, sizeof b, NULL, &n);
    if (rc == 0) memset(&s_socket[socket], 0, sizeof s_socket[socket]);
    return rc;
}

static uint16_t add_addr(uint8_t *b, int socket, const freya_net_addr_t *addr)
{
    socket_request(b, socket);
    put32(b + 2, addr->addr);
    put16(b + 6, addr->port);
    return 8;
}

static int address_call(uint16_t op, int socket, const freya_net_addr_t *addr)
{
    uint8_t b[8];
    uint16_t n = 0;

    if (local_socket(socket) < 0 || !addr) return FREYA_ERR_ARG;
    add_addr(b, socket, addr);
    return rpc(op, b, sizeof b, NULL, &n);
}

int net_connect(int socket, const freya_net_addr_t *addr)
{ return address_call(ESP_OP_CONNECT, socket, addr); }

int net_tls_connect(int socket, const char *hostname, uint16_t port)
{
    uint8_t b[2 + 2 + 254];
    uint32_t host_len;
    uint16_t n = 0;

    if (local_socket(socket) < 0 ||
        s_socket[socket].type != FREYA_SOCK_STREAM ||
        !hostname || !*hostname || !port)
        return FREYA_ERR_ARG;
    host_len = strlen(hostname);
    if (host_len > 253) return FREYA_ERR_ARG;
    socket_request(b, socket);
    put16(b + 2, port);
    memcpy(b + 4, hostname, host_len + 1);
    return rpc(ESP_OP_TLS_CONNECT, b, (uint16_t)(host_len + 5), NULL, &n);
}

int net_bind(int socket, const freya_net_addr_t *addr)
{ return address_call(ESP_OP_BIND, socket, addr); }

int net_listen(int socket, int backlog)
{
    uint8_t b[4];
    uint16_t n = 0;

    if (local_socket(socket) < 0 || backlog < 1 || backlog > FREYA_NET_SOCKETS)
        return FREYA_ERR_ARG;
    socket_request(b, socket);
    put16(b + 2, (uint16_t)backlog);
    return rpc(ESP_OP_LISTEN, b, sizeof b, NULL, &n);
}

int net_accept(int socket, freya_net_addr_t *peer)
{
    uint8_t b[2];
    uint16_t n = peer ? sizeof *peer : 0;
    int slot, rc;

    if (local_socket(socket) < 0) return FREYA_ERR_ARG;
    slot = free_socket();
    if (slot < 0) return FREYA_ERR_BUSY;
    socket_request(b, socket);
    rc = rpc(ESP_OP_ACCEPT, b, sizeof b, peer, &n);
    if (rc >= 0) {
        s_socket[slot].used = 1;
        s_socket[slot].remote = (int16_t)rc;
        s_socket[slot].from_app = (uint8_t)(g_app.running ? 1 : 0);
        s_socket[slot].type = FREYA_SOCK_STREAM;
        return slot;
    }
    return rc;
}

static int send_call(uint16_t op, int socket, const void *buf, int len,
                     const freya_net_addr_t *to)
{
    uint8_t b[8 + FREYA_NET_PAYLOAD_MAX];
    uint16_t h, n = 0;

    if (local_socket(socket) < 0 || len < 0 || len > FREYA_NET_PAYLOAD_MAX ||
        (len && !buf))
        return FREYA_ERR_ARG;
    h = to ? add_addr(b, socket, to) : socket_request(b, socket);
    if (len) memcpy(b + h, buf, (uint32_t)len);
    return rpc(op, b, (uint16_t)(h + len), NULL, &n);
}

int net_send(int socket, const void *buf, int len)
{ return send_call(ESP_OP_SEND, socket, buf, len, NULL); }
int net_sendto(int socket, const void *buf, int len, const freya_net_addr_t *to)
{
    if (!to) return FREYA_ERR_ARG;
    return send_call(ESP_OP_SENDTO, socket, buf, len, to);
}

static int recv_call(uint16_t op, int socket, void *buf, int len,
                     freya_net_addr_t *from)
{
    uint8_t req[4];
    uint8_t response[sizeof(freya_net_addr_t) + FREYA_NET_PAYLOAD_MAX];
    uint16_t n = sizeof response;
    int rc, data_off = from ? (int)sizeof(*from) : 0;

    if (local_socket(socket) < 0 || !buf || len < 0 ||
        len > FREYA_NET_PAYLOAD_MAX) return FREYA_ERR_ARG;
    socket_request(req, socket);
    put16(req + 2, (uint16_t)len);
    rc = rpc(op, req, sizeof req, response, &n);
    if (rc < 0) return rc;
    if (rc > len || n != (uint16_t)(data_off + rc)) return FREYA_ERR_IO;
    if (from) memcpy(from, response, sizeof *from);
    if (rc) memcpy(buf, response + data_off, (uint32_t)rc);
    return rc;
}

int net_recv(int socket, void *buf, int len)
{ return recv_call(ESP_OP_RECV, socket, buf, len, NULL); }
int net_recvfrom(int socket, void *buf, int len, freya_net_addr_t *from)
{
    if (!from) return FREYA_ERR_ARG;
    return recv_call(ESP_OP_RECVFROM, socket, buf, len, from);
}

int net_poll(uint32_t timeout_ms)
{
    uint32_t start = sys_ticks();
    int rc;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    do {
        rc = esp_link_poll();
        if (rc != 0) {
            if (rc < 0) s_rpc_op = 0;
            if (rc > 0 && !s_rpc_op) {
                uint16_t n = 0;
                int event = esp_link_response(ESP_OP_EVENT, NULL, &n);
                if (event != FREYA_ERR_AGAIN) return 1;
            }
            return rc;
        }
        if (app_should_stop()) return FREYA_ERR_IO;
        if (!timeout_ms) break;
        thread_yield();
#ifndef FREYA_HOST
        __wfi();
#endif
    } while ((uint32_t)(sys_ticks() - start) < timeout_ms);
    return 0;
}

void net_release(void)
{
    int have_app_socket = 0;

    for (int i = 0; i < FREYA_NET_SOCKETS; i++) {
        if (s_socket[i].used && s_socket[i].from_app) {
            memset(&s_socket[i], 0, sizeof s_socket[i]);
            have_app_socket = 1;
        }
    }
    if (s_transport_from_app) {
        esp_link_close();
        memset(s_socket, 0, sizeof s_socket);
        s_transport_from_app = 0;
    } else if (have_app_socket) {
        /* Socket resources are bounded locally.  The C6 also drops stale
         * descriptors when their generation is no longer referenced. */
        s_rpc_op = 0;
    }
}

int net_http_start(uint8_t flags, const char *url, const char *user_agent,
                   const char *basic, const char *data)
{
    uint8_t b[ESP_FRAME_PAYLOAD];
    const char *field[4] = { url, user_agent, basic, data };
    uint16_t used = 1, n = 0;

    if (!url || !*url) return FREYA_ERR_ARG;
    b[0] = flags;
    for (int i = 0; i < 4; i++) {
        const char *s = field[i] ? field[i] : "";
        uint32_t length = strlen(s) + 1;
        if (length > ESP_FRAME_PAYLOAD - used) return FREYA_ERR_ARG;
        memcpy(b + used, s, length);
        used = (uint16_t)(used + length);
    }
    return rpc(ESP_OP_HTTP_START, b, used, NULL, &n);
}

int net_http_info(freya_http_info_t *info)
{
    uint16_t n = sizeof *info;
    int rc;

    if (!info) return FREYA_ERR_ARG;
    rc = rpc(ESP_OP_HTTP_INFO, NULL, 0, info, &n);
    if (rc == 0 && n != sizeof *info) return FREYA_ERR_IO;
    return rc;
}

int net_http_read(void *buf, int len)
{
    uint8_t request[2];
    uint16_t n;
    int rc;

    if (!buf || len < 1 || len > FREYA_NET_PAYLOAD_MAX) return FREYA_ERR_ARG;
    put16(request, (uint16_t)len);
    n = (uint16_t)len;
    rc = rpc(ESP_OP_HTTP_READ, request, sizeof request, buf, &n);
    if (rc >= 0 && (rc != n || rc > len)) return FREYA_ERR_IO;
    return rc;
}

int net_http_close(void)
{
    uint16_t n = 0;
    return rpc(ESP_OP_HTTP_CLOSE, NULL, 0, NULL, &n);
}

#define TERM_CHUNK 240

void term_pump(void)
{
    uint8_t req[11 + TERM_CHUNK];
    uint8_t resp[3 + TERM_CHUNK];
    static uint32_t next_ms;
    static uint8_t state;
    static uint8_t busy;
    uint16_t nresp;
    int ntx, nrx, i, rc;
    uint32_t now;

    if (busy || !esp_link_is_open() || s_rpc_op) return;
    now = sys_ticks();
    if (!uart_term_pending() && (int32_t)(now - next_ms) < 0) return;

    memset(req, 0, 11);
#ifdef FREYA_APP_FLASH_ADDR
    if (app_password_enabled()) {
        req[0] = 1;
        app_password_read(req + 1);
    }
#endif
    ntx = uart_term_peek(req + 11, TERM_CHUNK);
    req[9] = (uint8_t)ntx;
    req[10] = (uint8_t)(ntx >> 8);

    busy = 1;
    rc = esp_link_submit(ESP_OP_TERM, req, (uint16_t)(11 + ntx));
    memset(req + 1, 0, 8);
    if (rc) {
        busy = 0;
        next_ms = sys_ticks() + 20;
        return;
    }
    for (;;) {
        rc = esp_link_poll();
        if (rc != 0) break;
#ifndef FREYA_HOST
        __wfi();
#endif
    }
    if (rc < 0) {
        busy = 0;
        next_ms = sys_ticks() + 20;
        return;
    }
    nresp = sizeof resp;
    rc = esp_link_response(ESP_OP_TERM, resp, &nresp);
    busy = 0;
    if (rc < 0) {
        next_ms = sys_ticks() + 20;
        return;
    }
    uart_term_drop(ntx);
    if (nresp < 3) {
        next_ms = sys_ticks() + 20;
        return;
    }
    state = resp[0];
    nrx = (int)resp[1] | ((int)resp[2] << 8);
    if (nrx < 0 || nrx > TERM_CHUNK || (uint16_t)(3 + nrx) > nresp) {
        next_ms = sys_ticks() + 20;
        return;
    }
    for (i = 0; i < nrx; i++)
        uart_rx_push(resp[3 + i]);
    next_ms = sys_ticks() + (state >= 2 ? 20U : 500U);
}

int web_take(freya_web_req_t *req)
{
    uint8_t cmd = 0;
    uint8_t resp[ESP_FRAME_PAYLOAD];
    uint16_t n = sizeof resp;
    int rc, path_len, query_len;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!req) return FREYA_ERR_ARG;
    if (!esp_link_is_open()) return FREYA_ERR_UNSUPPORTED;
    rc = rpc(ESP_OP_WEB, &cmd, 1, resp, &n);
    if (rc != 0) return rc;
    if (n < 3) return FREYA_ERR_IO;
    path_len = resp[1];
    query_len = resp[2];
    if ((resp[0] != FREYA_WEB_GET && resp[0] != FREYA_WEB_HEAD) ||
        path_len > FREYA_WEB_PATH || query_len > FREYA_WEB_QUERY ||
        (uint16_t)(3 + path_len + query_len) > n)
        return FREYA_ERR_IO;
    memset(req, 0, sizeof *req);
    req->method = resp[0];
    memcpy(req->path, resp + 3, (size_t)path_len);
    memcpy(req->query, resp + 3 + path_len, (size_t)query_len);
    return 0;
}

int web_begin(int status, const char *type, uint32_t length)
{
    uint8_t b[8 + FREYA_WEB_TYPE];
    uint16_t n = 0;
    int tlen;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!esp_link_is_open()) return FREYA_ERR_UNSUPPORTED;
    if (status < 100 || status > 599 || !type) return FREYA_ERR_ARG;
    tlen = (int)strlen(type);
    if (tlen < 1 || tlen > FREYA_WEB_TYPE) return FREYA_ERR_ARG;
    b[0] = 1;
    put16(b + 1, (uint16_t)status);
    put32(b + 3, length);
    b[7] = (uint8_t)tlen;
    memcpy(b + 8, type, (size_t)tlen);
    return rpc(ESP_OP_WEB, b, (uint16_t)(8 + tlen), NULL, &n);
}

int web_body(const void *data, int len)
{
    uint8_t b[3 + FREYA_WEB_CHUNK];
    uint16_t n = 0;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!esp_link_is_open()) return FREYA_ERR_UNSUPPORTED;
    if (!data || len < 1 || len > FREYA_WEB_CHUNK) return FREYA_ERR_ARG;
    b[0] = 2;
    put16(b + 1, (uint16_t)len);
    memcpy(b + 3, data, (size_t)len);
    return rpc(ESP_OP_WEB, b, (uint16_t)(3 + len), NULL, &n);
}

int web_end(void)
{
    uint8_t cmd = 3;
    uint16_t n = 0;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!esp_link_is_open()) return FREYA_ERR_UNSUPPORTED;
    return rpc(ESP_OP_WEB, &cmd, 1, NULL, &n);
}

#else

int net_unsupported(void) { return FREYA_ERR_UNSUPPORTED; }
__asm__(
    ".global wifi_on, wifi_off, wifi_credentials, wifi_connect\n"
    ".global wifi_disconnect, wifi_status, wifi_scan_start, wifi_scan_next\n"
    ".global ping_start, ping_result, net_socket, net_close, net_connect\n"
    ".global net_tls_connect, net_bind, net_listen, net_accept, net_send, net_recv\n"
    ".global net_sendto, net_recvfrom, net_poll, net_release\n"
    ".global net_http_start, net_http_info, net_http_read, net_http_close\n"
    ".global web_take, web_begin, web_body, web_end\n"
    ".thumb_set wifi_on, net_unsupported\n"
    ".thumb_set wifi_off, net_unsupported\n"
    ".thumb_set wifi_credentials, net_unsupported\n"
    ".thumb_set wifi_connect, net_unsupported\n"
    ".thumb_set wifi_disconnect, net_unsupported\n"
    ".thumb_set wifi_status, net_unsupported\n"
    ".thumb_set wifi_scan_start, net_unsupported\n"
    ".thumb_set wifi_scan_next, net_unsupported\n"
    ".thumb_set ping_start, net_unsupported\n"
    ".thumb_set ping_result, net_unsupported\n"
    ".thumb_set net_socket, net_unsupported\n"
    ".thumb_set net_close, net_unsupported\n"
    ".thumb_set net_connect, net_unsupported\n"
    ".thumb_set net_tls_connect, net_unsupported\n"
    ".thumb_set net_bind, net_unsupported\n"
    ".thumb_set net_listen, net_unsupported\n"
    ".thumb_set net_accept, net_unsupported\n"
    ".thumb_set net_send, net_unsupported\n"
    ".thumb_set net_recv, net_unsupported\n"
    ".thumb_set net_sendto, net_unsupported\n"
    ".thumb_set net_recvfrom, net_unsupported\n"
    ".thumb_set net_poll, net_unsupported\n"
    ".thumb_set net_release, net_unsupported\n"
    ".thumb_set net_http_start, net_unsupported\n"
    ".thumb_set net_http_info, net_unsupported\n"
    ".thumb_set net_http_read, net_unsupported\n"
    ".thumb_set net_http_close, net_unsupported\n"
    ".thumb_set web_take, net_unsupported\n"
    ".thumb_set web_begin, net_unsupported\n"
    ".thumb_set web_body, net_unsupported\n"
    ".thumb_set web_end, net_unsupported\n");

#endif
