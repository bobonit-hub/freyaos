#include "freya.h"
#include "esp_link.h"

#if BOARD_ESP_LINK && !defined(FREYA_HOST)

static volatile uint8_t s_rx_done;
static volatile uint8_t s_tx_done;
static volatile uint8_t s_error;
static volatile uint8_t s_active;

static void cs_high(void)
{
    GPIO_TypeDef *p = board_gpio_port(FREYA_PIN_PORT(BOARD_ESP_CS));
    p->BSRR = 1UL << FREYA_PIN_NUM(BOARD_ESP_CS);
}

#ifdef BOARD_SPI_FIFO
/* The FIFO SPI takes its DMA enables in CFG1, which can be written only
 * while it is off.  spi_dma_done() and spi_dma_cancel() turn it off. */
static void spi_dma_off(void)
{
    SPI2->CR1 &= ~SPI_CR1_SPE;
    SPI2->CFG1 &= ~(SPI_CFG1_RXDMAEN | SPI_CFG1_TXDMAEN);
}

static void finish_if_done(void) { }
#else
static void spi_dma_off(void)
{
    SPI2->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
}

static void finish_if_done(void)
{
    if (s_active && (s_error || (s_rx_done && s_tx_done))) {
        SPI2->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    }
}
#endif

#if defined(BOARD_ESP_GPDMA)

/*
 * The U5's GPDMA: SPI2 receives on channel 0 and transmits on channel 1,
 * each a single block of bytes with the request line chosen in CTR2.  A
 * channel that finished its block is idle again on its own; one that has
 * to stop early is suspended and then reset.
 */
#define RX_DMA  GPDMA1_Channel0
#define TX_DMA  GPDMA1_Channel1

static void channel_stop(DMA_Channel_TypeDef *ch)
{
    uint32_t spin = 100000;

    if (ch->CCR & DMA_CCR_EN) {
        ch->CCR |= DMA_CCR_SUSP;
        while (!(ch->CSR & DMA_CSR_IDLEF) && --spin) { }
    }
    ch->CCR = DMA_CCR_RESET;
    ch->CFCR = DMA_CFCR_ALL;
}

static void channel_load(DMA_Channel_TypeDef *ch, uint32_t ctr1, uint32_t ctr2,
                         const volatile void *src, volatile void *dst,
                         uint16_t length)
{
    ch->CLLR = 0;
    ch->CTR1 = ctr1;
    ch->CTR2 = ctr2;
    ch->CBR1 = length;
    ch->CSAR = (uint32_t)(uintptr_t)src;
    ch->CDAR = (uint32_t)(uintptr_t)dst;
    ch->CCR  = DMA_CCR_PRIO_HIGH | DMA_CCR_TCIE | DMA_CCR_DTEIE |
               DMA_CCR_ULEIE | DMA_CCR_USEIE;
}

int spi_dma_start(const void *tx, void *rx, uint16_t length)
{
    GPIO_TypeDef *cs;

    if (!tx || !rx || !length || s_active) return FREYA_ERR_ARG;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
    (void)RCC->AHB1ENR;

    channel_stop(RX_DMA);
    channel_stop(TX_DMA);
    channel_load(RX_DMA, DMA_CTR1_DINC, GPDMA1_REQUEST_SPI2_RX,
                 &SPI2->RXDR, rx, length);
    channel_load(TX_DMA, DMA_CTR1_SINC, GPDMA1_REQUEST_SPI2_TX | DMA_CTR2_DREQ,
                 tx, &SPI2->TXDR, length);

    s_rx_done = s_tx_done = s_error = 0;
    s_active = 1;
    nvic_set_priority(GPDMA1_Channel0_IRQn, IRQ_PRIO_HANDLER);
    nvic_set_priority(GPDMA1_Channel1_IRQn, IRQ_PRIO_HANDLER);
    nvic_enable(GPDMA1_Channel0_IRQn);
    nvic_enable(GPDMA1_Channel1_IRQn);

    cs = board_gpio_port(FREYA_PIN_PORT(BOARD_ESP_CS));
    sys_delay_us(2000);
    cs->BSRR = 1UL << (FREYA_PIN_NUM(BOARD_ESP_CS) + 16);

    /* RM0456's order: receive requests first, both channels on, then
     * transmit requests, and only then the SPI. */
    spi_dma_off();
    SPI2->IFCR = SPI_IFCR_ALL;
    SPI2->CFG1 |= SPI_CFG1_RXDMAEN;
    RX_DMA->CCR |= DMA_CCR_EN;
    TX_DMA->CCR |= DMA_CCR_EN;
    SPI2->CFG1 |= SPI_CFG1_TXDMAEN;
    SPI2->CR1 |= SPI_CR1_SPE;
    SPI2->CR1 |= SPI_CR1_CSTART;
    return 0;
}

static void channels_off(void)
{
    channel_stop(RX_DMA);
    channel_stop(TX_DMA);
}

static void channel_event(DMA_Channel_TypeDef *ch, volatile uint8_t *done)
{
    uint32_t f = ch->CSR;

    ch->CFCR = DMA_CFCR_ALL;
    if (f & DMA_CSR_ERRORS) s_error = 1;
    if (f & DMA_CSR_TCF) *done = 1;
    finish_if_done();
}

void GPDMA1_Channel0_IRQHandler(void) { channel_event(RX_DMA, &s_rx_done); }
void GPDMA1_Channel1_IRQHandler(void) { channel_event(TX_DMA, &s_tx_done); }

#elif defined(BOARD_ESP_DMA_CHANNELS)

/*
 * The F103's DMA, which the AT32F403A keeps: SPI2 receives on channel 4
 * and transmits on channel 5, and a channel has its own control and count
 * registers with the flags of all seven in one ISR.
 */
#define RX_CH   4
#define TX_CH   5

int spi_dma_start(const void *tx, void *rx, uint16_t length)
{
    GPIO_TypeDef *cs;

    if (!tx || !rx || !length || s_active) return FREYA_ERR_ARG;
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;
    (void)RCC->AHBENR;

    DMA1_Channel4->CCR = 0;
    DMA1_Channel5->CCR = 0;
    DMA1->IFCR = DMA_IFCR_CALL(RX_CH) | DMA_IFCR_CALL(TX_CH);

    DMA1_Channel4->CPAR = (uint32_t)(uintptr_t)&SPI2->DR;
    DMA1_Channel4->CMAR = (uint32_t)(uintptr_t)rx;
    DMA1_Channel4->CNDTR = length;
    DMA1_Channel4->CCR = DMA_CCR_MINC | DMA_CCR_PL_HIGH |
                         DMA_CCR_TCIE | DMA_CCR_TEIE;

    DMA1_Channel5->CPAR = (uint32_t)(uintptr_t)&SPI2->DR;
    DMA1_Channel5->CMAR = (uint32_t)(uintptr_t)tx;
    DMA1_Channel5->CNDTR = length;
    DMA1_Channel5->CCR = DMA_CCR_DIR_M2P | DMA_CCR_MINC | DMA_CCR_PL_HIGH |
                         DMA_CCR_TCIE | DMA_CCR_TEIE;

    s_rx_done = s_tx_done = s_error = 0;
    s_active = 1;
    nvic_set_priority(DMA1_Channel4_IRQn, IRQ_PRIO_HANDLER);
    nvic_set_priority(DMA1_Channel5_IRQn, IRQ_PRIO_HANDLER);
    nvic_enable(DMA1_Channel4_IRQn);
    nvic_enable(DMA1_Channel5_IRQn);

    cs = board_gpio_port(FREYA_PIN_PORT(BOARD_ESP_CS));
    sys_delay_us(2000);
    cs->BSRR = 1UL << (FREYA_PIN_NUM(BOARD_ESP_CS) + 16);
    SPI2->CR2 |= SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    DMA1_Channel4->CCR |= DMA_CCR_EN;      /* receiver must be ready first */
    DMA1_Channel5->CCR |= DMA_CCR_EN;
    return 0;
}

static void channels_off(void)
{
    DMA1_Channel4->CCR &= ~DMA_CCR_EN;
    DMA1_Channel5->CCR &= ~DMA_CCR_EN;
}

void DMA1_Channel4_IRQHandler(void)
{
    uint32_t f = DMA1->ISR;
    DMA1->IFCR = DMA_IFCR_CALL(RX_CH);
    if (f & DMA_ISR_TEIF(RX_CH)) s_error = 1;
    if (f & DMA_ISR_TCIF(RX_CH)) s_rx_done = 1;
    finish_if_done();
}

void DMA1_Channel5_IRQHandler(void)
{
    uint32_t f = DMA1->ISR;
    DMA1->IFCR = DMA_IFCR_CALL(TX_CH);
    if (f & DMA_ISR_TEIF(TX_CH)) s_error = 1;
    if (f & DMA_ISR_TCIF(TX_CH)) s_tx_done = 1;
    finish_if_done();
}

#else /* the F4's DMA streams */

int spi_dma_start(const void *tx, void *rx, uint16_t length)
{
    GPIO_TypeDef *cs;

    if (!tx || !rx || !length || s_active) return FREYA_ERR_ARG;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    (void)RCC->AHB1ENR;

    DMA1_Stream3->CR &= ~DMA_SxCR_EN;
    DMA1_Stream4->CR &= ~DMA_SxCR_EN;
    while ((DMA1_Stream3->CR | DMA1_Stream4->CR) & DMA_SxCR_EN) { }
    DMA1->LIFCR = DMA_LIFCR_CSTREAM3;
    DMA1->HIFCR = DMA_HIFCR_CSTREAM4;

    DMA1_Stream3->PAR = (uint32_t)(uintptr_t)&SPI2->DR;
    DMA1_Stream3->M0AR = (uint32_t)(uintptr_t)rx;
    DMA1_Stream3->NDTR = length;
    DMA1_Stream3->FCR = 0;
    DMA1_Stream3->CR = DMA_SxCR_MINC | DMA_SxCR_PL_HIGH |
                       DMA_SxCR_TCIE | DMA_SxCR_TEIE;

    DMA1_Stream4->PAR = (uint32_t)(uintptr_t)&SPI2->DR;
    DMA1_Stream4->M0AR = (uint32_t)(uintptr_t)tx;
    DMA1_Stream4->NDTR = length;
    DMA1_Stream4->FCR = 0;
    DMA1_Stream4->CR = DMA_SxCR_DIR_M2P | DMA_SxCR_MINC |
                       DMA_SxCR_PL_HIGH | DMA_SxCR_TCIE | DMA_SxCR_TEIE;

    s_rx_done = s_tx_done = s_error = 0;
    s_active = 1;
    nvic_set_priority(DMA1_Stream3_IRQn, IRQ_PRIO_HANDLER);
    nvic_set_priority(DMA1_Stream4_IRQn, IRQ_PRIO_HANDLER);
    nvic_enable(DMA1_Stream3_IRQn);
    nvic_enable(DMA1_Stream4_IRQn);

    cs = board_gpio_port(FREYA_PIN_PORT(BOARD_ESP_CS));
    sys_delay_us(2000);
    cs->BSRR = 1UL << (FREYA_PIN_NUM(BOARD_ESP_CS) + 16);
    SPI2->CR2 |= SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    DMA1_Stream3->CR |= DMA_SxCR_EN;       /* receiver must be ready first */
    DMA1_Stream4->CR |= DMA_SxCR_EN;
    return 0;
}

static void channels_off(void)
{
    DMA1_Stream3->CR &= ~DMA_SxCR_EN;
    DMA1_Stream4->CR &= ~DMA_SxCR_EN;
    DMA1->LIFCR = DMA_LIFCR_CSTREAM3;
    DMA1->HIFCR = DMA_HIFCR_CSTREAM4;
}

void DMA1_Stream3_IRQHandler(void)
{
    uint32_t f = DMA1->LISR;
    DMA1->LIFCR = DMA_LIFCR_CSTREAM3;
    if (f & DMA_LISR_TEIF3) s_error = 1;
    if (f & DMA_LISR_TCIF3) s_rx_done = 1;
    finish_if_done();
}

void DMA1_Stream4_IRQHandler(void)
{
    uint32_t f = DMA1->HISR;
    DMA1->HIFCR = DMA_HIFCR_CSTREAM4;
    if (f & DMA_HISR_TEIF4) s_error = 1;
    if (f & DMA_HISR_TCIF4) s_tx_done = 1;
    finish_if_done();
}

#endif /* BOARD_ESP_GPDMA, BOARD_ESP_DMA_CHANNELS */

int spi_dma_done(void)
{
    if (s_error) return FREYA_ERR_IO;
    if (!s_active) return 0;
#ifdef BOARD_SPI_FIFO
    if (!s_rx_done || !s_tx_done || !(SPI2->SR & SPI_SR_TXC)) return 0;
    spi_dma_off();
#else
    if (!s_rx_done || !s_tx_done || (SPI2->SR & SPI_SR_BSY)) return 0;
#endif
    cs_high();
    s_active = 0;
    return 1;
}

void spi_dma_cancel(void)
{
    channels_off();
    spi_dma_off();
    cs_high();
    s_active = s_rx_done = s_tx_done = s_error = 0;
}

#else

int spi_dma_start(const void *tx, void *rx, uint16_t length)
{
    (void)tx; (void)rx; (void)length;
    return FREYA_ERR_UNSUPPORTED;
}
int spi_dma_done(void) { return FREYA_ERR_UNSUPPORTED; }
void spi_dma_cancel(void) { }

#endif
