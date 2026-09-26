#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "term_cert.h"
#include "term_server.h"

#define TERM_PORT 8022
#define TERM_RING 1024
#define TERM_CHUNK 240
#define TERM_PASS 8
#define TERM_DOWN 0
#define TERM_LISTEN 1
#define TERM_LOGIN 2
#define TERM_OPEN 3
#define TERM_ARG (-3)
#define TERM_AGAIN (-8)

static const char *TAG = "freya-term";

typedef struct {
    uint8_t b[TERM_RING];
    uint16_t h, t;
} ring_t;

static SemaphoreHandle_t term_mu;
static ring_t term_rx; /* client -> STM32 */
static ring_t term_tx; /* STM32 -> client */
static uint8_t term_pass[TERM_PASS];
static bool term_pass_on;
static volatile bool term_net_up;
static volatile uint8_t term_state;

static int ring_push(ring_t *r, const uint8_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        uint16_t next = (uint16_t)((r->h + 1) & (TERM_RING - 1));
        if (next == r->t) break;
        r->b[r->h] = p[i];
        r->h = next;
    }
    return i;
}

static int ring_pop(ring_t *r, uint8_t *p, int n)
{
    int i = 0;
    while (i < n && r->t != r->h) {
        p[i++] = r->b[r->t];
        r->t = (uint16_t)((r->t + 1) & (TERM_RING - 1));
    }
    return i;
}

static bool lock(void)
{
    return term_mu && xSemaphoreTake(term_mu, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(term_mu);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    term_net_up = true;
}

static void on_disc(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    term_net_up = false;
}

int term_server_handle(const uint8_t *data, uint16_t length,
                       uint8_t *reply, uint16_t *reply_length)
{
    uint16_t ntx, nrx;

    *reply_length = 0;
    if (length < 11 || !data) return TERM_ARG;
    ntx = (uint16_t)data[9] | (uint16_t)((uint16_t)data[10] << 8);
    if (ntx > TERM_CHUNK || (uint16_t)(11 + ntx) != length) return TERM_ARG;
    if (!lock()) return TERM_AGAIN;
    if (data[0]) {
        memcpy(term_pass, data + 1, TERM_PASS);
        term_pass_on = true;
    } else {
        memset(term_pass, 0, sizeof term_pass);
        term_pass_on = false;
    }
    if (ntx) ring_push(&term_tx, data + 11, ntx);
    nrx = (uint16_t)ring_pop(&term_rx, reply + 3, TERM_CHUNK);
    unlock();
    reply[0] = term_net_up ? term_state : TERM_DOWN;
    reply[1] = (uint8_t)nrx;
    reply[2] = (uint8_t)(nrx >> 8);
    *reply_length = (uint16_t)(3 + nrx);
    return 0;
}

void term_server_down(void)
{
    term_net_up = false;
}

static int open_listen(void)
{
    struct sockaddr_in addr;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TERM_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    ESP_LOGI(TAG, "TLS terminal listening on %d", TERM_PORT);
    return fd;
}

static int tls_send_all(esp_tls_t *tls, const char *text)
{
    size_t left = strlen(text);
    const char *p = text;
    TickType_t start = xTaskGetTickCount();

    while (left) {
        ssize_t n = esp_tls_conn_write(tls, p, left);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(10000)) return -1;
            vTaskDelay(1);
            continue;
        }
        if (n <= 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static int read_line(esp_tls_t *tls, char *out, int max)
{
    int n = 0;
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(20000)) {
        char c;
        ssize_t r;

        if (!term_net_up) return -1;
        r = esp_tls_conn_read(tls, &c, 1);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
            continue;
        }
        if (r <= 0) return -1;
        if (c == '\n') {
            out[n] = '\0';
            return n;
        }
        if (c == '\r') continue;
        if (n + 1 >= max) return -1;
        out[n++] = c;
    }
    return -1;
}

static bool password_ok(const char *got, int n)
{
    uint8_t expect[TERM_PASS];
    uint8_t diff;
    bool on;
    int i;

    if (!lock()) return false;
    memcpy(expect, term_pass, TERM_PASS);
    on = term_pass_on;
    unlock();
    diff = (uint8_t)(n ^ TERM_PASS);
    for (i = 0; i < TERM_PASS; i++) {
        uint8_t c = (n > i) ? (uint8_t)got[i] : 0;
        diff |= (uint8_t)(expect[i] ^ c);
    }
    memset(expect, 0, sizeof expect);
    return on && diff == 0;
}

static int login(esp_tls_t *tls)
{
    char line[32];
    int n;

    term_state = TERM_LOGIN;
    if (tls_send_all(tls, "username: ") != 0) return -1;
    n = read_line(tls, line, (int)sizeof line);
    if (n < 0 || strcmp(line, "admin") != 0) {
        tls_send_all(tls, "denied\r\n");
        return -1;
    }
    if (tls_send_all(tls, "password: ") != 0) return -1;
    n = read_line(tls, line, (int)sizeof line);
    if (!password_ok(line, n)) {
        memset(line, 0, sizeof line);
        tls_send_all(tls, term_pass_on ? "denied\r\n" : "password not set\r\n");
        return -1;
    }
    memset(line, 0, sizeof line);
    if (tls_send_all(tls, "\r\n") != 0) return -1;
    if (lock()) {
        uint8_t cr = '\r';
        ring_push(&term_rx, &cr, 1);
        unlock();
    }
    term_state = TERM_OPEN;
    ESP_LOGI(TAG, "terminal session open");
    return 0;
}

static void drop_session(esp_tls_t **tls, int *client)
{
    if (*tls) {
        esp_tls_server_session_delete(*tls);
        *tls = NULL;
    }
    if (*client >= 0) {
        close(*client);
        *client = -1;
    }
    if (lock()) {
        term_rx.h = term_rx.t = 0;
        term_tx.h = term_tx.t = 0;
        unlock();
    }
    term_state = term_net_up ? TERM_LISTEN : TERM_DOWN;
}

static bool bridge(esp_tls_t *tls, int client)
{
    uint8_t buf[TERM_CHUNK];
    int n;
    ssize_t r;

    (void)client;
    r = esp_tls_conn_read(tls, buf, sizeof buf);
    if (r > 0) {
        if (lock()) {
            ring_push(&term_rx, buf, (int)r);
            unlock();
        }
    } else if (r != ESP_TLS_ERR_SSL_WANT_READ && r != ESP_TLS_ERR_SSL_WANT_WRITE) {
        return false;
    }
    n = 0;
    if (lock()) {
        n = ring_pop(&term_tx, buf, TERM_CHUNK);
        unlock();
    }
    if (n > 0) {
        int off = 0;
        while (off < n) {
            ssize_t w = esp_tls_conn_write(tls, buf + off, (size_t)(n - off));
            if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(1);
                continue;
            }
            if (w <= 0) return false;
            off += (int)w;
        }
    }
    return true;
}

static void term_task(void *arg)
{
    int listen_fd = -1;
    int client = -1;
    esp_tls_t *tls = NULL;
    esp_tls_cfg_server_t cfg;

    (void)arg;
    memset(&cfg, 0, sizeof cfg);
    cfg.servercert_buf = (const unsigned char *)TERM_CERT_PEM;
    cfg.servercert_bytes = (unsigned int)strlen(TERM_CERT_PEM) + 1;
    cfg.serverkey_buf = (const unsigned char *)TERM_KEY_PEM;
    cfg.serverkey_bytes = (unsigned int)strlen(TERM_KEY_PEM) + 1;
    cfg.tls_handshake_timeout_ms = 10000;

    for (;;) {
        if (!term_net_up) {
            drop_session(&tls, &client);
            if (listen_fd >= 0) {
                close(listen_fd);
                listen_fd = -1;
            }
            term_state = TERM_DOWN;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (listen_fd < 0) {
            listen_fd = open_listen();
            if (listen_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            term_state = TERM_LISTEN;
        }
        if (client < 0) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof peer;
            int one = 1;

            client = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
            if (client < 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) & ~O_NONBLOCK);
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            tls = esp_tls_init();
            if (!tls || esp_tls_server_session_create(&cfg, client, tls) != 0) {
                ESP_LOGW(TAG, "TLS handshake failed");
                if (tls) esp_tls_server_session_delete(tls);
                tls = NULL;
                close(client);
                client = -1;
                continue;
            }
            fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
            if (login(tls) != 0) {
                drop_session(&tls, &client);
                continue;
            }
        }
        if (!bridge(tls, client)) {
            ESP_LOGI(TAG, "terminal session closed");
            drop_session(&tls, &client);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void term_server_start(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *netif;

    term_mu = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               WIFI_EVENT_STA_DISCONNECTED,
                                               on_disc, NULL));
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr)
        term_net_up = true;
    xTaskCreate(term_task, "freya_term", 16384, NULL, 5, NULL);
}
