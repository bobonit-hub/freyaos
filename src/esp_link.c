#include "freya.h"
#include "esp_link.h"

_Static_assert(sizeof(esp_frame_t) == ESP_FRAME_SIZE,
               "ESP frame must be exactly one SPI transaction");

static uint32_t crc32_bytes(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFUL;

    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
    }
    return ~crc;
}

uint32_t esp_frame_crc(const esp_frame_t *frame)
{
    esp_frame_t copy;

    memcpy(&copy, frame, sizeof copy);
    copy.crc32 = 0;
    return crc32_bytes((const uint8_t *)&copy, sizeof copy);
}

int esp_frame_encode(esp_frame_t *frame, uint16_t opcode,
                     uint32_t sequence, int status,
                     const void *payload, uint16_t length)
{
    if (!frame || length > ESP_FRAME_PAYLOAD || (length && !payload))
        return FREYA_ERR_ARG;
    memset(frame, 0, sizeof *frame);
    frame->magic = ESP_FRAME_MAGIC;
    frame->version = ESP_FRAME_VERSION;
    frame->opcode = opcode;
    frame->sequence = sequence;
    frame->length = length;
    frame->status = (int16_t)status;
    if (length) memcpy(frame->payload, payload, length);
    frame->crc32 = esp_frame_crc(frame);
    return 0;
}

int esp_frame_valid(const esp_frame_t *frame)
{
    if (!frame || frame->magic != ESP_FRAME_MAGIC ||
        frame->version != ESP_FRAME_VERSION ||
        frame->length > ESP_FRAME_PAYLOAD)
        return 0;
    return frame->crc32 == esp_frame_crc(frame);
}

#if BOARD_ESP_LINK && !defined(FREYA_HOST)

#define LINK_TIMEOUT_MS  250U
#define LINK_RETRIES     2

static esp_frame_t s_tx __attribute__((aligned(4)));
static esp_frame_t s_rx __attribute__((aligned(4)));
static esp_frame_t s_reply __attribute__((aligned(4)));
static uint32_t s_sequence;
static uint32_t s_started;
static uint8_t s_open;
static uint8_t s_active;
static uint8_t s_waiting;
static uint8_t s_ready;
static uint8_t s_reply_ready;
static uint8_t s_retries;

static void ready_irq(int source, void *arg)
{
    (void)source;
    (void)arg;
    s_ready = 1;
}

static void spi_configure(void)
{
    uint32_t br = 3; /* PCLK1 / 16: 3 MHz F411, 2.625 MHz F405 */

    board_spi_mux(SPI2, FREYA_PB(13), FREYA_PB(14), FREYA_PB(15),
                  BOARD_ESP_SPI_AF);
    SPI2->CR1 = 0;
#if BOARD_SPI_HAS_I2S
    SPI2->I2SCFGR = 0;
#endif
    SPI2->CR2 = 0;
    SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                (br << SPI_CR1_BR_SHIFT);
    SPI2->CR1 |= SPI_CR1_SPE;
}

static int transfer_start(void)
{
    memset(&s_rx, 0, sizeof s_rx);
    s_started = sys_ticks();
    s_active = 1;
    return spi_dma_start(&s_tx, &s_rx, sizeof s_tx);
}

static void recover(void)
{
    spi_dma_cancel();
    SPI2->CR1 &= ~SPI_CR1_SPE;
    spi_configure();
}

int esp_link_open(void)
{
    spi_info_t info;
    int rc;

    if (s_open) return 0;
    if (spi_info(0, &info) == 0 && info.open) return FREYA_ERR_BUSY;
    if (i2c_owns_pin(BOARD_ESP_READY) || w1_owns_pin(BOARD_ESP_READY) ||
        pwm_pin_busy(BOARD_ESP_READY) || gpio_irq_owns_pin(BOARD_ESP_READY))
        return FREYA_ERR_BUSY;

    rc = gpio_pin_mode(BOARD_ESP_CS, FREYA_PIN_OUT);
    if (rc) return rc;
    gpio_pin_write(BOARD_ESP_CS, 1);
    rc = gpio_pin_mode(BOARD_ESP_READY, FREYA_PIN_IN_PULLDOWN);
    if (rc) return rc;
    rc = gpio_irq_attach(BOARD_ESP_READY, FREYA_EDGE_RISING, ready_irq, NULL);
    if (rc) return rc;

    spi_configure();
    s_sequence = 0;
    s_active = s_waiting = s_ready = s_reply_ready = s_retries = 0;
    s_open = 1;
    return 0;
}

void esp_link_close(void)
{
    if (!s_open) return;
    spi_dma_cancel();
    gpio_irq_detach(BOARD_ESP_READY);
    SPI2->CR1 &= ~SPI_CR1_SPE;
    board_pin_mode(board_gpio_port(1), 10, FREYA_PIN_IN);
    board_pin_mode(board_gpio_port(1), 12, FREYA_PIN_IN);
    board_pin_mode(board_gpio_port(1), 13, FREYA_PIN_IN);
    board_pin_mode(board_gpio_port(1), 14, FREYA_PIN_IN);
    board_pin_mode(board_gpio_port(1), 15, FREYA_PIN_IN);
    s_open = s_active = s_waiting = s_ready = s_reply_ready = 0;
}

int esp_link_is_open(void) { return s_open; }

int esp_link_owns_pin(int pin)
{
    if (!s_open) return 0;
    return pin == BOARD_ESP_READY || pin == BOARD_ESP_CS ||
           pin == FREYA_PB(13) || pin == FREYA_PB(14) || pin == FREYA_PB(15);
}

int esp_link_submit(uint16_t opcode, const void *payload, uint16_t length)
{
    int rc;

    if (!s_open) return FREYA_ERR_IO;
    if (s_active || s_waiting || s_reply_ready) return FREYA_ERR_AGAIN;
    rc = esp_frame_encode(&s_tx, opcode, ++s_sequence, 0, payload, length);
    if (rc) return rc;
    /* READY may still describe the receive slot consumed by this request;
     * only a new rising edge after it completes announces the reply. */
    s_ready = 0;
    s_retries = 0;
    rc = transfer_start();
    return rc;
}

int esp_link_poll(void)
{
    int done;

    if (!s_open) return FREYA_ERR_UNSUPPORTED;
    if (s_active) {
        done = spi_dma_done();
        if (done < 0) {
            recover();
            s_active = 0;
            return done;
        }
        if (done > 0) {
            s_active = 0;
            if (esp_frame_valid(&s_rx) && s_rx.sequence == s_tx.sequence) {
                memcpy(&s_reply, &s_rx, sizeof s_reply);
                s_reply_ready = 1;
                s_waiting = 0;
                return 1;
            }
            /* The request transaction normally clocks out an empty frame.
             * The C6 raises READY after it has prepared the response. */
            if (s_tx.opcode != ESP_OP_FETCH) {
                s_waiting = 1;
                s_started = sys_ticks();
                return 0;
            }
            /* READY may have announced an event after the C6 had already
             * queued its empty receive slot.  This FETCH consumes that
             * slot; the C6 now queues the event and raises READY again. */
            if (s_rx.magic == 0) {
                s_waiting = 1;
                s_started = sys_ticks();
                return 0;
            }
            if (++s_retries <= LINK_RETRIES) {
                recover();
                return transfer_start();
            }
            return FREYA_ERR_IO;
        }
        if ((uint32_t)(sys_ticks() - s_started) >= LINK_TIMEOUT_MS) {
            if (++s_retries <= LINK_RETRIES) {
                recover();
                return transfer_start();
            }
            recover();
            s_active = 0;
            return FREYA_ERR_TIMEOUT;
        }
        return 0;
    }

    /* READY with no request means an asynchronous event is queued. */
    if (s_ready && !s_reply_ready) {
        s_ready = 0;
        if (!s_waiting) ++s_sequence;
        esp_frame_encode(&s_tx, ESP_OP_FETCH, s_sequence, 0, NULL, 0);
        s_retries = 0;
        return transfer_start();
    }
    if (s_waiting && (uint32_t)(sys_ticks() - s_started) >= LINK_TIMEOUT_MS) {
        s_waiting = 0;
        recover();
        return FREYA_ERR_TIMEOUT;
    }
    return s_reply_ready ? 1 : 0;
}

int esp_link_response_ready(uint16_t opcode)
{
    return s_reply_ready && s_reply.opcode == opcode;
}

int esp_link_response(uint16_t opcode, void *payload, uint16_t *length)
{
    uint16_t n;
    int status;

    if (!s_reply_ready || s_reply.opcode != opcode) return FREYA_ERR_AGAIN;
    n = s_reply.length;
    if (length) {
        if (payload && *length < n) return FREYA_ERR_ARG;
        *length = n;
    } else if (n && payload) {
        return FREYA_ERR_ARG;
    }
    if (payload && n) memcpy(payload, s_reply.payload, n);
    status = s_reply.status;
    s_reply_ready = 0;
    return status;
}

#else

int esp_link_open(void) { return FREYA_ERR_UNSUPPORTED; }
void esp_link_close(void) { }
int esp_link_is_open(void) { return 0; }
int esp_link_owns_pin(int pin) { (void)pin; return 0; }
int esp_link_submit(uint16_t opcode, const void *p, uint16_t n)
{ (void)opcode; (void)p; (void)n; return FREYA_ERR_UNSUPPORTED; }
int esp_link_poll(void) { return FREYA_ERR_UNSUPPORTED; }
int esp_link_response_ready(uint16_t opcode)
{ (void)opcode; return 0; }
int esp_link_response(uint16_t opcode, void *p, uint16_t *n)
{ (void)opcode; (void)p; (void)n; return FREYA_ERR_UNSUPPORTED; }

#endif
