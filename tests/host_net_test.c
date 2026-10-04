#include <stdio.h>
#include <string.h>
#include "freya.h"
#include "esp_link.h"

app_state_t g_app;
static int open_link, submitted, ready;
static uint16_t last_op, last_len;
static uint8_t last_data[ESP_FRAME_PAYLOAD];
static int next_socket;
static int tls_attempts;
static int tls_failed;        /* the mock C6 has forgotten the slot */
static int web_reads;
static int web_post;          /* the mock C6 holds a POST, not a GET */
static uint32_t ticks;

int app_in_handler(void) { return 0; }
int app_should_stop(void) { return 0; }
uint32_t sys_ticks(void) { return ticks++; }
void thread_yield(void) { ticks++; }

int esp_link_open(void) { open_link = 1; return 0; }
void esp_link_close(void) { open_link = 0; }
int esp_link_is_open(void) { return open_link; }
int esp_link_submit(uint16_t op, const void *p, uint16_t n)
{
    if (submitted) return FREYA_ERR_AGAIN;
    last_op = op; last_len = n;
    if (n) memcpy(last_data, p, n);
    submitted = 1; ready = 0;
    return 0;
}
int esp_link_poll(void) { if (submitted) ready = 1; return ready; }
int esp_link_response_ready(uint16_t op) { return ready && op == last_op; }
int esp_link_response(uint16_t op, void *p, uint16_t *n)
{
    int status = 0;

    if (!ready || op != last_op) return FREYA_ERR_AGAIN;
    if (op == ESP_OP_WIFI_STATUS) {
        freya_wifi_status_t st;
        memset(&st, 0, sizeof st);
        st.state = FREYA_WIFI_CONNECTED;
        st.ip = 0xC0A80102;
        memcpy(st.ssid, "lab", 4);
        if (!n || *n < sizeof st) return FREYA_ERR_ARG;
        memcpy(p, &st, sizeof st); *n = sizeof st;
    } else if (op == ESP_OP_SOCKET) {
        status = next_socket++;
        if (n) *n = 0;
    } else if (op == ESP_OP_SEND) {
        status = last_len - 2;
        if (n) *n = 0;
    } else if (op == ESP_OP_SYSLOG) {
        status = last_len - 6;
        if (n) *n = 0;
    } else if (op == ESP_OP_TLS_CONNECT) {
        status = tls_attempts++ ? 0 : FREYA_ERR_AGAIN;
        if (n) *n = 0;
    } else if (op == ESP_OP_RESOLVE) {
        uint8_t *a = p;
        if (strcmp((char *)last_data, "nowhere.test") == 0) {
            status = FREYA_ERR_IO;
            *n = 0;
        } else {
            /* 93.184.215.14, low byte first on the link */
            a[0] = 14; a[1] = 215; a[2] = 184; a[3] = 93;
            *n = 4;
        }
    } else if (op == ESP_OP_CLOSE && tls_failed) {
        status = FREYA_ERR_ARG;
        if (n) *n = 0;
    } else if (op == ESP_OP_WEB && last_data[0] == 0) {
        /* The request: method, path, query, and for a POST the length. */
        uint8_t *r = p;
        const char *path = web_post ? "/firmware" : "/index.html";
        const char *query = web_post ? "t=0123456789ab&sum=1c2" : "";
        int pl = (int)strlen(path), ql = (int)strlen(query);

        r[0] = (uint8_t)(web_post ? FREYA_WEB_POST : FREYA_WEB_GET);
        r[1] = (uint8_t)pl;
        r[2] = (uint8_t)ql;
        memcpy(r + 3, path, (size_t)pl);
        memcpy(r + 3 + pl, query, (size_t)ql);
        *n = (uint16_t)(3 + pl + ql);
        if (web_post) {
            r[*n] = 7; r[*n + 1] = 0; r[*n + 2] = 0; r[*n + 3] = 0;
            *n = (uint16_t)(*n + 4);
        }
    } else if (op == ESP_OP_WEB && last_data[0] == 4) {
        /* The body, 7 bytes, in two pieces and then the end. */
        int want = last_data[1] | (last_data[2] << 8);
        const char *body = "payload";
        int off = web_reads == 0 ? 0 : 4;
        int piece = web_reads == 0 ? 4 : 3;

        if (!web_post || web_reads >= 2) piece = 0;
        if (piece > want) piece = want;
        memcpy(p, body + off, (size_t)piece);
        *n = (uint16_t)piece;
        if (web_post) web_reads++;
    } else if (n) {
        *n = 0;
    }
    submitted = ready = 0;
    return status;
}

int uart_term_pending(void) { return 0; }
int uart_term_peek(uint8_t *dst, int max) { (void)dst; (void)max; return 0; }
void uart_term_drop(int n) { (void)n; }
void uart_term_disconnected(void) { }
void uart_term_rx_push(uint8_t c) { (void)c; }
void syslog_flush(void) { }
int app_password_enabled(void) { return 0; }
void app_password_read(uint8_t *out) { if (out) memset(out, 0, 8); }

#include "../src/net.c"

static int checks, fails;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("  FAIL  %s\n", what); }
    else printf("  ok    %s\n", what);
}

#define FINISH(expr, out) do {                 \
    (out) = (expr);                            \
    check((out) == FREYA_ERR_AGAIN,            \
          "operation starts nonblocking");     \
    net_poll(1);                               \
    (out) = (expr);                            \
} while (0)

int main(void)
{
    freya_wifi_status_t st;
    uint8_t data[FREYA_NET_PAYLOAD_MAX];
    int rc, s;

    check(wifi_on() == FREYA_ERR_AGAIN, "wifi on starts asynchronously");
    net_poll(1);
    check(wifi_on() == 0, "wifi on completes after polling");

    FINISH(wifi_status(&st), rc);
    check(rc == 0 && st.state == FREYA_WIFI_CONNECTED &&
          st.ip == 0xC0A80102 && strcmp(st.ssid, "lab") == 0,
          "DHCP/status payload decodes");

    FINISH(net_socket(FREYA_AF_INET, FREYA_SOCK_STREAM, FREYA_IPPROTO_TCP), s);
    check(s == 0, "the first bounded socket is allocated");
    FINISH(net_tls_connect(s, "voice.example", 443), rc);
    check(rc == FREYA_ERR_AGAIN,
          "TLS reports the remote nonblocking handshake state");
    FINISH(net_tls_connect(s, "voice.example", 443), rc);
    check(rc == 0 && last_op == ESP_OP_TLS_CONNECT,
          "a stream socket starts a TLS 1.3 hostname connection");
    check(net_tls_connect(s, "", 443) == FREYA_ERR_ARG,
          "TLS requires a certificate hostname");
    memset(data, 0x5a, sizeof data);
    FINISH(net_send(s, data, sizeof data), rc);
    check(rc == (int)sizeof data, "a maximum socket fragment is sent");
    check(net_send(s, data, sizeof data + 1) == FREYA_ERR_ARG,
          "socket fragments are explicitly bounded");

    {
        uint32_t addr = 0;
        FINISH(net_resolve("example.test", &addr), rc);
        check(rc == 0 && last_op == ESP_OP_RESOLVE &&
              strcmp((char *)last_data, "example.test") == 0 &&
              addr == 0x5DB8D70EU,
              "a host name resolves to the address order of freya_net_addr_t");
        FINISH(net_resolve("nowhere.test", &addr), rc);
        check(rc == FREYA_ERR_IO, "an unknown host name is an error");
        check(net_resolve("", &addr) == FREYA_ERR_ARG &&
              net_resolve("example.test", NULL) == FREYA_ERR_ARG,
              "resolve needs a name and somewhere to put the address");
    }
    {
        int t;
        FINISH(net_socket(FREYA_AF_INET, FREYA_SOCK_STREAM, FREYA_IPPROTO_TCP), t);
        tls_failed = 1;
        FINISH(net_close(t), rc);
        tls_failed = 0;
        check(t == 1 && rc == 0 && !s_socket[t].used,
              "a slot the C6 dropped after a TLS failure is freed by close");
    }

    {
        freya_web_req_t req;
        uint32_t left = 99;
        char body[8];

        FINISH(web_take(&req), rc);
        check(rc == 0 && req.method == FREYA_WEB_GET &&
              strcmp(req.path, "/index.html") == 0 && req.query[0] == '\0',
              "a GET request decodes without a body length");
        FINISH(web_read(body, sizeof body, &left), rc);
        check(rc == 0 && left == 0, "a GET has no body to read");

        web_post = 1;
        FINISH(web_take(&req), rc);
        check(rc == 0 && req.method == FREYA_WEB_POST &&
              strcmp(req.path, "/firmware") == 0 &&
              strcmp(req.query, "t=0123456789ab&sum=1c2") == 0,
              "a POST request decodes with its path and query");
        check(web_read(body, 0, &left) == FREYA_ERR_ARG,
              "web_read needs room for at least one byte");
        memset(body, 0, sizeof body);
        FINISH(web_read(body, sizeof body, &left), rc);
        check(rc == 4 && memcmp(body, "payl", 4) == 0 && left == 3 &&
              last_data[0] == 4 && last_len == 3 &&
              (last_data[1] | (last_data[2] << 8)) == (int)sizeof body,
              "web_read asks the C6 for a bounded piece and counts down");
        FINISH(web_read(body, sizeof body, &left), rc);
        check(rc == 3 && memcmp(body, "oad", 3) == 0 && left == 0,
              "the rest of the body follows");
        FINISH(web_read(body, sizeof body, NULL), rc);
        check(rc == 0, "the end of the body is a zero-length read");
        web_post = 0;
    }

#if BOARD_ESP_LINK
    {
        static const uint8_t req[] = { 0x14, 0x01, 0xA8, 0xC0, 0x02, 0x02,
                                       '<', '1', '4', '>', 'h', 'i' };

        rc = net_syslog_send(req, sizeof req);
        check(rc == 6 && last_op == ESP_OP_SYSLOG && last_len == sizeof req &&
              memcmp(last_data, req, sizeof req) == 0,
              "a syslog datagram is one waited-for request");
        check(wifi_status(&st) == FREYA_ERR_AGAIN,
              "a program request goes out");
        check(net_syslog_send(req, sizeof req) == FREYA_ERR_AGAIN,
              "syslog waits while a program request is in flight");
        net_poll(1);
        check(wifi_status(&st) == 0, "the program request still completes");
    }
#endif

    g_app.running = 1;
    FINISH(net_socket(FREYA_AF_INET, FREYA_SOCK_DGRAM, FREYA_IPPROTO_UDP), rc);
    check(rc == 1, "an app-owned UDP socket is allocated");
    check(net_tls_connect(rc, "voice.example", 443) == FREYA_ERR_ARG,
          "TLS cannot upgrade a UDP socket");
    net_release();
    check(esp_link_is_open(), "loader cleanup keeps a console-owned transport");
    check(net_send(1, data, 1) == FREYA_ERR_ARG,
          "loader cleanup releases app-owned sockets");

    g_app.running = 0;
    FINISH(wifi_off(), rc);
    check(rc == 0 && !esp_link_is_open(), "wifi off releases SPI2");
    g_app.running = 1;
    check(wifi_on() == FREYA_ERR_AGAIN, "an app can open the transport");
    net_release();
    check(!esp_link_is_open(), "loader cleanup closes an app-owned transport");
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
