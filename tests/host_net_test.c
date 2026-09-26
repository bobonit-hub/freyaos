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
static int http_reads;
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
    } else if (op == ESP_OP_TLS_CONNECT) {
        status = tls_attempts++ ? 0 : FREYA_ERR_AGAIN;
        if (n) *n = 0;
    } else if (op == ESP_OP_HTTP_INFO) {
        freya_http_info_t info;
        memset(&info, 0, sizeof info);
        info.body_length = 2;
        info.status = 200;
        if (!n || *n < sizeof info) return FREYA_ERR_ARG;
        memcpy(p, &info, sizeof info);
        *n = sizeof info;
    } else if (op == ESP_OP_HTTP_READ) {
        if (http_reads++ == 0) {
            memcpy(p, "ok", 2);
            *n = 2;
            status = 2;
        } else {
            *n = 0;
        }
    } else if (n) {
        *n = 0;
    }
    submitted = ready = 0;
    return status;
}

int uart_term_pending(void) { return 0; }
int uart_term_peek(uint8_t *dst, int max) { (void)dst; (void)max; return 0; }
void uart_term_drop(int n) { (void)n; }
void uart_rx_push(uint8_t c) { (void)c; }
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

    FINISH(net_http_start(HTTP_FLAG_COMPRESSED | HTTP_FLAG_INSECURE,
                          "https://example.test/", "agent", "u:p", "a=1"), rc);
    check(rc == 0 && last_op == ESP_OP_HTTP_START &&
          last_data[0] == (HTTP_FLAG_COMPRESSED | HTTP_FLAG_INSECURE) &&
          strcmp((char *)last_data + 1, "https://example.test/") == 0,
          "curl options and URL are packed into a bounded RPC");
    {
        freya_http_info_t info;
        FINISH(net_http_info(&info), rc);
        check(rc == 0 && info.status == 200 && info.body_length == 2,
              "curl response metadata decodes");
    }
    FINISH(net_http_read(data, sizeof data), rc);
    check(rc == 2 && memcmp(data, "ok", 2) == 0,
          "curl response body streams back to Freya");
    FINISH(net_http_close(), rc);
    check(rc == 0, "curl response resources close");

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
