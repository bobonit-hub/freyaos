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
#include "mbedtls/ssl.h"
#include "term_cert.h"
#include "web_server.h"

#define WEB_PORT 443
#define WEB_RING 8192
#define WEB_PATH 96
#define WEB_QUERY 31
#define WEB_TYPE 40
#define WEB_ARG (-3)
#define WEB_IO (-7)
#define WEB_AGAIN (-8)

enum {
    PH_IDLE = 0,
    PH_REQ,
    PH_TAKEN,
    PH_BODY,
    PH_DONE
};

static const char *TAG = "freya-web";

static SemaphoreHandle_t web_mu;
static volatile bool web_net_up;
static volatile int web_phase;
static int web_method;
static int web_status;
static uint32_t web_length;
static uint32_t web_sent;
static char web_path[WEB_PATH + 1];
static char web_query[WEB_QUERY + 1];
static char web_type[WEB_TYPE + 1];
static uint8_t web_ring[WEB_RING];
static uint16_t web_head, web_tail;

static bool lock(void)
{
    return web_mu && xSemaphoreTake(web_mu, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(web_mu);
}

static void ring_clear(void)
{
    web_head = web_tail = 0;
}

static int ring_free(void)
{
    int used = (int)((web_head - web_tail) & (WEB_RING - 1));
    return (WEB_RING - 1) - used;
}

static int ring_push_all(const uint8_t *p, int n)
{
    int i;

    if (n > ring_free()) return -1;
    for (i = 0; i < n; i++) {
        web_ring[web_head] = p[i];
        web_head = (uint16_t)((web_head + 1) & (WEB_RING - 1));
    }
    return n;
}

static int ring_pop(uint8_t *p, int n)
{
    int i = 0;

    while (i < n && web_tail != web_head) {
        p[i++] = web_ring[web_tail];
        web_tail = (uint16_t)((web_tail + 1) & (WEB_RING - 1));
    }
    return i;
}

static void job_reset(void)
{
    web_phase = PH_IDLE;
    web_sent = 0;
    web_length = 0;
    ring_clear();
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    web_net_up = true;
}

static void on_disc(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    web_net_up = false;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode in place. A decoded slash or NUL would hide a segment. */
static int decode_path(char *s)
{
    char *w = s;

    while (*s) {
        if (*s == '%') {
            int hi = hex_nibble((unsigned char)s[1]);
            int lo = s[1] ? hex_nibble((unsigned char)s[2]) : -1;
            int c;

            if (hi < 0 || lo < 0) return -1;
            c = (hi << 4) | lo;
            if (c == 0 || c == '/' || c == '\\') return -1;
            *w++ = (char)c;
            s += 3;
            continue;
        }
        if (*s == '\\') return -1;
        *w++ = *s++;
    }
    *w = '\0';
    return 0;
}

static int query_ok(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
            c == '~' || c == '=' || c == '&' || c == '%' || c == '+')
            continue;
        return 0;
    }
    return 1;
}

static int path_ok(const char *s)
{
    int i = 0;

    if (s[0] != '/') return 0;
    while (s[i]) {
        int start, n;
        unsigned char c;

        if (s[i] != '/') return 0;
        i++;
        start = i;
        while (s[i] && s[i] != '/') {
            c = (unsigned char)s[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == '~'))
                return 0;
            i++;
        }
        n = i - start;
        if (n == 0)
            return s[i] == '\0';
        if ((n == 1 && s[start] == '.') ||
            (n == 2 && s[start] == '.' && s[start + 1] == '.'))
            return 0;
    }
    return 1;
}

static int parse_request(char *buf, int *method, char *path, char *query)
{
    char *line_end, *sp, *ver, *qmark;

    line_end = strstr(buf, "\r\n");
    if (!line_end) return -1;
    *line_end = '\0';
    if (strncmp(buf, "GET ", 4) == 0) *method = 1;
    else if (strncmp(buf, "HEAD ", 5) == 0) *method = 2;
    else return -2;
    sp = strchr(buf, ' ');
    ver = strrchr(buf, ' ');
    if (!sp || !ver || ver == sp) return -1;
    if (strncmp(ver + 1, "HTTP/1.", 7) != 0) return -1;
    *ver = '\0';
    if (sp[1] != '/') return -1;
    qmark = strchr(sp + 1, '?');
    if (qmark) {
        *qmark = '\0';
        if (strlen(qmark + 1) > WEB_QUERY || !query_ok(qmark + 1)) return -1;
        memcpy(query, qmark + 1, strlen(qmark + 1) + 1);
    } else {
        query[0] = '\0';
    }
    if (decode_path(sp + 1) != 0) return -1;
    if (strlen(sp + 1) > WEB_PATH || !path_ok(sp + 1)) return -1;
    memcpy(path, sp + 1, strlen(sp + 1) + 1);
    return 0;
}

int web_server_handle(const uint8_t *data, uint16_t length,
                      uint8_t *reply, uint16_t *reply_length)
{
    uint8_t cmd;

    *reply_length = 0;
    if (!data || length < 1) return WEB_ARG;
    cmd = data[0];
    if (!lock()) return WEB_AGAIN;

    if (cmd == 0) {
        int plen, qlen;

        if (web_phase != PH_REQ) {
            unlock();
            return WEB_AGAIN;
        }
        plen = (int)strlen(web_path);
        qlen = (int)strlen(web_query);
        reply[0] = (uint8_t)web_method;
        reply[1] = (uint8_t)plen;
        reply[2] = (uint8_t)qlen;
        memcpy(reply + 3, web_path, (size_t)plen);
        memcpy(reply + 3 + plen, web_query, (size_t)qlen);
        *reply_length = (uint16_t)(3 + plen + qlen);
        web_phase = PH_TAKEN;
        unlock();
        return 0;
    }

    if (cmd == 1) {
        int status, tlen;
        uint32_t body_len;

        if (length < 8 || web_phase != PH_TAKEN) {
            unlock();
            return web_phase == PH_TAKEN ? WEB_ARG : WEB_IO;
        }
        status = (int)data[1] | ((int)data[2] << 8);
        body_len = (uint32_t)data[3] | ((uint32_t)data[4] << 8) |
                   ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 24);
        tlen = data[7];
        if (status < 100 || status > 599 || tlen < 1 || tlen > WEB_TYPE ||
            (uint16_t)(8 + tlen) != length) {
            unlock();
            return WEB_ARG;
        }
        web_status = status;
        web_length = body_len;
        web_sent = 0;
        memcpy(web_type, data + 8, (size_t)tlen);
        web_type[tlen] = '\0';
        ring_clear();
        web_phase = PH_BODY;
        unlock();
        return 0;
    }

    if (cmd == 2) {
        int n;

        if (length < 3 || web_phase != PH_BODY) {
            unlock();
            return web_phase == PH_BODY ? WEB_ARG : WEB_IO;
        }
        n = (int)data[1] | ((int)data[2] << 8);
        if (n < 1 || (uint16_t)(3 + n) != length ||
            web_sent + (uint32_t)n > web_length) {
            unlock();
            return WEB_ARG;
        }
        if (ring_push_all(data + 3, n) < 0) {
            unlock();
            return WEB_AGAIN;
        }
        web_sent += (uint32_t)n;
        unlock();
        return 0;
    }

    if (cmd == 3) {
        int head = web_method == 2;

        /* HEAD carries the real length and no body. */
        if (web_phase != PH_BODY ||
            (head && web_sent != 0) ||
            (!head && web_sent != web_length)) {
            unlock();
            return WEB_IO;
        }
        web_phase = PH_DONE;
        unlock();
        return 0;
    }

    unlock();
    return WEB_ARG;
}

void web_server_down(void)
{
    web_net_up = false;
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
    addr.sin_port = htons(WEB_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    ESP_LOGI(TAG, "TLS web server listening on %d", WEB_PORT);
    return fd;
}

static int tls_write_all(esp_tls_t *tls, const void *data, int len)
{
    const uint8_t *p = data;
    TickType_t start = xTaskGetTickCount();

    while (len > 0) {
        ssize_t n;

        if (!web_net_up) return -1;
        n = esp_tls_conn_write(tls, p, (size_t)len);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(10000)) return -1;
            vTaskDelay(1);
            continue;
        }
        if (n <= 0) return -1;
        p += n;
        len -= (int)n;
        start = xTaskGetTickCount();
    }
    return 0;
}

static const char *reason_for(int status)
{
    if (status == 200) return "OK";
    if (status == 400) return "Bad Request";
    if (status == 403) return "Forbidden";
    if (status == 404) return "Not Found";
    if (status == 405) return "Method Not Allowed";
    if (status == 500) return "Error";
    if (status == 503) return "Unavailable";
    return "Error";
}

static int reply_fixed(esp_tls_t *tls, int status, const char *type,
                       const char *body)
{
    char hdr[240];
    int blen = (int)strlen(body);
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "Server: Freya\r\n"
                     "\r\n",
                     status, reason_for(status), type, blen);

    if (n < 0 || n >= (int)sizeof hdr) return -1;
    if (tls_write_all(tls, hdr, n) != 0) return -1;
    if (blen && tls_write_all(tls, body, blen) != 0) return -1;
    return 0;
}

static int read_headers(esp_tls_t *tls, char *buf, int max)
{
    int n = 0;
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(10000)) {
        ssize_t r;

        if (!web_net_up) return -1;
        r = esp_tls_conn_read(tls, buf + n, (size_t)(max - 1 - n));
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
            continue;
        }
        if (r <= 0) return -1;
        n += (int)r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n")) return n;
        if (n >= max - 1) return -1;
    }
    return -1;
}

static void drop_client(esp_tls_t **tls, int *client)
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
        job_reset();
        unlock();
    }
}

static int serve(esp_tls_t *tls, int method)
{
    int status, tlen, head_only;
    uint32_t length, got;
    char type[WEB_TYPE + 1];
    char hdr[240];
    TickType_t start;
    uint8_t chunk[480];

    start = xTaskGetTickCount();
    status = 0;
    length = 0;
    type[0] = '\0';
    for (;;) {
        int phase;

        if (!web_net_up) return -1;
        if (!lock()) {
            vTaskDelay(1);
            continue;
        }
        phase = web_phase;
        if (phase == PH_BODY) {
            status = web_status;
            length = web_length;
            memcpy(type, web_type, sizeof type);
            unlock();
            break;
        }
        unlock();
        if (phase == PH_IDLE) return -1;
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(15000)) {
            if (lock()) {
                if (web_phase == PH_BODY) {
                    status = web_status;
                    length = web_length;
                    memcpy(type, web_type, sizeof type);
                    unlock();
                    break;
                }
                job_reset();
                unlock();
            }
            reply_fixed(tls, 503, "text/plain", "file service down\n");
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    tlen = snprintf(hdr, sizeof hdr,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n"
                    "Server: Freya\r\n"
                    "\r\n",
                    status, reason_for(status), type, (unsigned)length);
    if (tlen < 0 || tlen >= (int)sizeof hdr ||
        tls_write_all(tls, hdr, tlen) != 0)
        return -1;

    head_only = method == 2;
    got = 0;
    start = xTaskGetTickCount();
    while (!head_only && got < length) {
        int n = 0;
        int phase = PH_BODY;

        if (lock()) {
            n = ring_pop(chunk, (int)sizeof chunk);
            phase = web_phase;
            unlock();
        }
        if (n > 0) {
            if (tls_write_all(tls, chunk, n) != 0) return -1;
            got += (uint32_t)n;
            start = xTaskGetTickCount();
            continue;
        }
        if (phase == PH_IDLE || !web_net_up) return -1;
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(15000)) return -1;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    start = xTaskGetTickCount();
    for (;;) {
        int phase;

        if (!lock()) {
            vTaskDelay(1);
            continue;
        }
        phase = web_phase;
        unlock();
        if (phase == PH_DONE) return 0;
        if (phase == PH_IDLE || !web_net_up) return -1;
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(15000)) return -1;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void web_task(void *arg)
{
    int listen_fd = -1;
    int client = -1;
    esp_tls_t *tls = NULL;
    esp_tls_cfg_server_t cfg;
    char req[1024];

    (void)arg;
    memset(&cfg, 0, sizeof cfg);
    cfg.servercert_buf = (const unsigned char *)TERM_CERT_PEM;
    cfg.servercert_bytes = (unsigned int)strlen(TERM_CERT_PEM) + 1;
    cfg.serverkey_buf = (const unsigned char *)TERM_KEY_PEM;
    cfg.serverkey_bytes = (unsigned int)strlen(TERM_KEY_PEM) + 1;
    cfg.tls_handshake_timeout_ms = 10000;

    for (;;) {
        int method, parsed, n;
        char path[WEB_PATH + 1];
        char query[WEB_QUERY + 1];
        void *ssl;

        if (!web_net_up) {
            drop_client(&tls, &client);
            if (listen_fd >= 0) {
                close(listen_fd);
                listen_fd = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (listen_fd < 0) {
            listen_fd = open_listen();
            if (listen_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
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
            ssl = esp_tls_get_ssl_context(tls);
            if (!ssl ||
                mbedtls_ssl_get_version_number((mbedtls_ssl_context *)ssl) !=
                    MBEDTLS_SSL_VERSION_TLS1_3) {
                ESP_LOGW(TAG, "refusing non-TLS1.3");
                drop_client(&tls, &client);
                continue;
            }
            fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
        }

        n = read_headers(tls, req, (int)sizeof req);
        if (n < 0) {
            drop_client(&tls, &client);
            continue;
        }
        parsed = parse_request(req, &method, path, query);
        if (parsed == -2) {
            reply_fixed(tls, 405, "text/plain", "method not allowed\n");
            drop_client(&tls, &client);
            continue;
        }
        if (parsed != 0) {
            reply_fixed(tls, 400, "text/plain", "bad request\n");
            drop_client(&tls, &client);
            continue;
        }
        if (!lock()) {
            drop_client(&tls, &client);
            continue;
        }
        memcpy(web_path, path, sizeof web_path);
        memcpy(web_query, query, sizeof web_query);
        web_method = method;
        web_sent = 0;
        ring_clear();
        web_phase = PH_REQ;
        unlock();
        ESP_LOGI(TAG, "%s %s", method == 2 ? "HEAD" : "GET", path);
        if (serve(tls, method) != 0)
            ESP_LOGW(TAG, "response failed for %s", path);
        drop_client(&tls, &client);
    }
}

void web_server_start(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *netif;

    web_mu = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               WIFI_EVENT_STA_DISCONNECTED,
                                               on_disc, NULL));
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr)
        web_net_up = true;
    xTaskCreate(web_task, "freya_web", 16384, NULL, 5, NULL);
}
