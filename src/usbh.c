/*
 * Freya - USB host on the Synopsys OTG core (USB=1).
 *
 * The STM32F411, STM32F405, STM32U585 and STM32H723 all carry the same
 * DesignWare core, as OTG_FS or as OTG_HS on its embedded full speed PHY.
 * The board brings up its clock, its pins and its PHY supply in
 * board_usb_init(); everything below is the core's own registers.
 *
 * The core is used in slave mode, without DMA and without interrupts: a
 * transfer is driven one packet at a time from thread mode.  Channel 0
 * carries every OUT packet and channel 1 every IN packet.  Each packet
 * is started, waited for, and its channel halted again, so a NAK is
 * simply the same packet sent again until the deadline.  Only full speed
 * is supported, which is what every stick falls back to on this port.
 *
 * The core does not switch VBUS on these boards.  The stick needs 5 V
 * from somewhere else; see docs/usb.md.
 */
#include "freya.h"

#ifndef FREYA_USB
#error "src/usbh.c is compiled only with USB=1"
#endif

#define OTG(off)        (*(volatile uint32_t *)(BOARD_USB_OTG_BASE + (off)))

#define GOTGCTL         OTG(0x000)
#define GAHBCFG         OTG(0x008)
#define GUSBCFG         OTG(0x00C)
#define GRSTCTL         OTG(0x010)
#define GINTSTS         OTG(0x014)
#define GINTMSK         OTG(0x018)
#define GRXSTSP         OTG(0x020)
#define GRXFSIZ         OTG(0x024)
#define GNPTXFSIZ       OTG(0x028)
#define GNPTXSTS        OTG(0x02C)
#define GCCFG           OTG(0x038)
#define HPTXFSIZ        OTG(0x100)
#define HCFG            OTG(0x400)
#define HFIR            OTG(0x404)
#define HFNUM           OTG(0x408)
#define HPTXSTS         OTG(0x410)
#define HAINT           OTG(0x414)
#define HAINTMSK        OTG(0x418)
#define HPRT            OTG(0x440)
#define HCCHAR(n)       OTG(0x500 + 0x20 * (n))
#define HCINT(n)        OTG(0x508 + 0x20 * (n))
#define HCINTMSK(n)     OTG(0x50C + 0x20 * (n))
#define HCTSIZ(n)       OTG(0x510 + 0x20 * (n))
#define PCGCCTL         OTG(0xE00)
#define FIFO(n)         OTG(0x1000 * ((n) + 1))

#define GUSBCFG_PHYSEL  (1UL << 6)
#define GUSBCFG_FHMOD   (1UL << 29)
#define GUSBCFG_FDMOD   (1UL << 30)

#define GRSTCTL_CSRST   (1UL << 0)
#define GRSTCTL_RXFFLSH (1UL << 4)
#define GRSTCTL_TXFFLSH (1UL << 5)
#define GRSTCTL_TXFALL  (0x10UL << 6)
#define GRSTCTL_AHBIDL  (1UL << 31)

#define GAHBCFG_GINT    (1UL << 0)

#define GINTSTS_CMOD    (1UL << 0)
#define GINTSTS_SOF     (1UL << 3)
#define GINTSTS_RXFLVL  (1UL << 4)
#define GINTSTS_HCINT   (1UL << 25)
#define GINTSTS_DISCINT (1UL << 29)

/* GCCFG.  The F4's core (BOARD_USB_OTG_V1) has NOVBUSSENS in bit 21; the
 * newer one has VBDEN there, the opposite sense.  Host mode wants VBUS
 * sensing off either way: nothing on these boards wires VBUS to it. */
#define GCCFG_PWRDWN    (1UL << 16)
#define GCCFG_NOVBUSSENS (1UL << 21)

#define HCFG_FSLSPCS_48 (1UL << 0)
#define HCFG_FSLSS      (1UL << 2)

#define HPRT_PCSTS      (1UL << 0)
#define HPRT_PCDET      (1UL << 1)
#define HPRT_PENA       (1UL << 2)
#define HPRT_PENCHNG    (1UL << 3)
#define HPRT_POCCHNG    (1UL << 5)
#define HPRT_PRST       (1UL << 8)
#define HPRT_PPWR       (1UL << 12)
#define HPRT_PSPD(v)    (((v) >> 17) & 3U)
#define HPRT_W1C        (HPRT_PCDET | HPRT_PENA | HPRT_PENCHNG | HPRT_POCCHNG)

#define HCCHAR_MPS(n)   ((uint32_t)(n) & 0x7FFUL)
#define HCCHAR_EP(n)    (((uint32_t)(n) & 0xFUL) << 11)
#define HCCHAR_IN       (1UL << 15)
#define HCCHAR_CTRL     (0UL << 18)
#define HCCHAR_ISO      (1UL << 18)
#define HCCHAR_BULK     (2UL << 18)
#define HCCHAR_MC1      (1UL << 20)
#define HCCHAR_DAD(a)   (((uint32_t)(a) & 0x7FUL) << 22)
#define HCCHAR_ODDFRM   (1UL << 29)
#define HCCHAR_CHDIS    (1UL << 30)
#define HCCHAR_CHENA    (1UL << 31)

#define HCINT_XFRC      (1UL << 0)
#define HCINT_CHH       (1UL << 1)
#define HCINT_AHBERR    (1UL << 2)
#define HCINT_STALL     (1UL << 3)
#define HCINT_NAK       (1UL << 4)
#define HCINT_TXERR     (1UL << 7)
#define HCINT_BBERR     (1UL << 8)
#define HCINT_FRMOR     (1UL << 9)
#define HCINT_DTERR     (1UL << 10)
#define HCINT_ERRORS    (HCINT_AHBERR | HCINT_TXERR | HCINT_BBERR | \
                         HCINT_FRMOR | HCINT_DTERR)

#define TSIZ(len, pkts, pid) \
    (((uint32_t)(len) & 0x7FFFFUL) | ((uint32_t)(pkts) << 19) | \
     ((uint32_t)(pid) << 29))

#define PID_DATA0       0
#define PID_DATA1       2
#define PID_SETUP       3

#define RXSTS_CH(s)     ((s) & 0xFU)
#define RXSTS_BCNT(s)   (((s) >> 4) & 0x7FFU)
#define RXSTS_PKT(s)    (((s) >> 17) & 0xFU)
#define RXSTS_IN_DATA   2

#define CH_OUT          0
#define CH_IN           1
#define CH_ISO_OUT      2       /* a headset's speaker (AUDIO=1)          */
#define CH_ISO_IN       3       /* and its microphone                     */

/* FIFO RAM in 32-bit words.  The OTG_FS cores have 320; the H723's
 * OTG_HS has 1024 and this uses the same part of it.  The periodic FIFO
 * holds a headset's speaker packet, 196 bytes at 48 kHz stereo, with
 * room for the next; the receive FIFO its microphone packet. */
#define FIFO_RX_WORDS   128
#define FIFO_NPTX_WORDS 64
#define FIFO_PTX_WORDS  128

#define PACKET_MS       50      /* one transaction, ACK or NAK or error   */
#define HALT_MS         5
#define TRIES           3       /* transaction errors in a row            */

enum { PKT_OK, PKT_NAK, PKT_STALL, PKT_ERR, PKT_TIMEOUT, PKT_GONE };

static uint8_t s_open;
static volatile uint8_t s_iso_on;       /* the interrupt owns the core   */

static uint32_t hprt(void)
{
    return HPRT & ~HPRT_W1C;            /* never clear a flag by accident */
}

int usbh_connected(void)
{
    return s_open && (HPRT & HPRT_PCSTS) && (HPRT & HPRT_PENA);
}

static int wait_clear(volatile uint32_t *reg, uint32_t bits, uint32_t ms)
{
    uint32_t start = sys_ticks();

    while (*reg & bits)
        if ((uint32_t)(sys_ticks() - start) > ms) return -1;
    return 0;
}

static int core_reset(void)
{
    uint32_t start = sys_ticks();

    while (!(GRSTCTL & GRSTCTL_AHBIDL))
        if ((uint32_t)(sys_ticks() - start) > 100) return -1;
    GRSTCTL |= GRSTCTL_CSRST;
    if (wait_clear(&GRSTCTL, GRSTCTL_CSRST, 100) != 0) return -1;
    sys_delay_ms(1);                    /* three PHY clocks, and then some */
    return 0;
}

static void flush_fifos(void)
{
    GRSTCTL = GRSTCTL_TXFFLSH | GRSTCTL_TXFALL;
    (void)wait_clear(&GRSTCTL, GRSTCTL_TXFFLSH, 10);
    GRSTCTL = GRSTCTL_RXFFLSH;
    (void)wait_clear(&GRSTCTL, GRSTCTL_RXFFLSH, 10);
}

void usbh_close(void)
{
#ifdef FREYA_AUDIO
    usbh_iso_stop();
#endif
    if (s_open) {
        HPRT = hprt() & ~HPRT_PPWR;
        GUSBCFG &= ~GUSBCFG_FHMOD;
        GCCFG &= ~GCCFG_PWRDWN;
        board_usb_off();
    }
    s_open = 0;
}

static int core_up(void)
{
    uint32_t start;

    if (board_usb_init() != 0) return USBH_UNSUP;

    GAHBCFG = 0;                        /* polled: no interrupt, no DMA    */
    GUSBCFG |= GUSBCFG_PHYSEL;          /* the full speed PHY              */
    if (core_reset() != 0) return USBH_ERR;
#ifdef BOARD_USB_OTG_V1
    GCCFG = GCCFG_PWRDWN | GCCFG_NOVBUSSENS;
#else
    GCCFG = GCCFG_PWRDWN;               /* VBDEN off                       */
#endif

    GUSBCFG = (GUSBCFG & ~(GUSBCFG_FHMOD | GUSBCFG_FDMOD)) | GUSBCFG_FHMOD;
    start = sys_ticks();
    while (!(GINTSTS & GINTSTS_CMOD))
        if ((uint32_t)(sys_ticks() - start) > 100) return USBH_ERR;
    sys_delay_ms(25);                   /* the mode change settles         */

    PCGCCTL = 0;
    HCFG = (HCFG & ~3UL) | HCFG_FSLSPCS_48 | HCFG_FSLSS;
    HFIR = 48000;

    GRXFSIZ   = FIFO_RX_WORDS;
    GNPTXFSIZ = ((uint32_t)FIFO_NPTX_WORDS << 16) | FIFO_RX_WORDS;
    HPTXFSIZ  = ((uint32_t)FIFO_PTX_WORDS << 16) |
                (FIFO_RX_WORDS + FIFO_NPTX_WORDS);
    flush_fifos();

    for (int ch = 0; ch < 2; ch++) {
        HCINT(ch) = 0xFFFFFFFFUL;
        HCINTMSK(ch) = 0x7FFUL;
    }
    GINTMSK = 0;
    GINTSTS = 0xFFFFFFFFUL;

    HPRT = hprt() | HPRT_PPWR;
    return USBH_OK;
}

int usbh_open(uint32_t wait_ms)
{
    uint32_t start;
    int rc;

    usbh_close();
    rc = core_up();
    if (rc != USBH_OK) {
        board_usb_off();
        return rc;
    }
    s_open = 1;

    start = sys_ticks();
    while (!(HPRT & HPRT_PCSTS)) {
        if ((uint32_t)(sys_ticks() - start) > wait_ms) return USBH_NODEV;
    }
    sys_delay_ms(100);                  /* connect debounce, USB 2.0 7.1.7.3 */
    if (!(HPRT & HPRT_PCSTS)) return USBH_NODEV;

    HPRT = hprt() | HPRT_PRST;
    sys_delay_ms(20);
    HPRT = hprt() & ~HPRT_PRST;
    sys_delay_ms(20);                   /* reset recovery                  */
    HPRT = hprt() | HPRT_PCDET | HPRT_PENCHNG;   /* acknowledge, keep PENA */

    if (!(HPRT & HPRT_PENA)) return USBH_ERR;
    if (HPRT_PSPD(HPRT) != 1) return USBH_UNSUP;   /* full speed only */
    return USBH_OK;
}

/* ------------------------------------------------------- one packet */
static void fifo_write(int ch, const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4 && i + k < len; k++)
            w |= (uint32_t)p[i + k] << (8 * k);
        FIFO(ch) = w;
    }
}

/* Pops one RX FIFO entry if there is one.  IN data for channel 'ch' goes
 * to 'buf' (at most 'room' bytes); anything else is read and dropped. */
static void rx_drain(int ch, uint8_t *buf, uint32_t room, uint32_t *got)
{
    uint32_t st, n;

    if (!(GINTSTS & GINTSTS_RXFLVL)) return;
    st = GRXSTSP;
    n = RXSTS_BCNT(st);
    if (RXSTS_PKT(st) != RXSTS_IN_DATA || n == 0) return;
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = FIFO(0);
        for (uint32_t k = 0; k < 4 && i + k < n; k++) {
            if ((int)RXSTS_CH(st) == ch && buf && i + k < room)
                buf[i + k] = (uint8_t)(w >> (8 * k));
        }
    }
    if ((int)RXSTS_CH(st) == ch && got) *got = n;
}

static void halt(int ch)
{
    uint32_t start = sys_ticks();

    if (HCCHAR(ch) & HCCHAR_CHENA) {
        HCCHAR(ch) |= HCCHAR_CHDIS;
        if ((GNPTXSTS & (0xFFUL << 16)) == 0)
            HCCHAR(ch) &= ~HCCHAR_CHENA;
        HCCHAR(ch) |= HCCHAR_CHENA;
        while (!(HCINT(ch) & HCINT_CHH) && (HCCHAR(ch) & HCCHAR_CHENA)) {
            rx_drain(ch, NULL, 0, NULL);
            if ((uint32_t)(sys_ticks() - start) > HALT_MS) break;
        }
    }
    rx_drain(ch, NULL, 0, NULL);
    HCINT(ch) = 0xFFFFFFFFUL;
}

/*
 * One transaction on one channel: an OUT or SETUP packet of 'len' bytes
 * from 'buf', or an IN packet of up to 'mps' bytes into 'buf', whose
 * length lands in *got.
 */
static int packet(uint32_t hcchar, int pid, uint8_t *buf, uint32_t len,
                  uint32_t *got)
{
    int in = (hcchar & HCCHAR_IN) != 0;
    int ch = in ? CH_IN : CH_OUT;
    uint32_t mps = hcchar & 0x7FFUL;
    uint32_t start, hcint;
    int rc = PKT_TIMEOUT;

    if (!(HPRT & HPRT_PCSTS)) return PKT_GONE;
    if (got) *got = 0;
    HCINT(ch) = 0xFFFFFFFFUL;
    HCTSIZ(ch) = TSIZ(in ? mps : len, 1, pid);
    HCCHAR(ch) = (hcchar | HCCHAR_MC1 | HCCHAR_CHENA) & ~HCCHAR_CHDIS;

    start = sys_ticks();
    if (!in && len) {
        uint32_t words = (len + 3) / 4;
        while ((GNPTXSTS & 0xFFFFUL) < words) {
            if ((uint32_t)(sys_ticks() - start) > PACKET_MS) {
                halt(ch);
                return PKT_TIMEOUT;
            }
        }
        fifo_write(ch, buf, len);
    }

    for (;;) {
        rx_drain(ch, in ? buf : NULL, mps, got);
        hcint = HCINT(ch);
        if (hcint & HCINT_XFRC)   { rc = PKT_OK;    break; }
        if (hcint & HCINT_STALL)  { rc = PKT_STALL; break; }
        if (hcint & HCINT_NAK)    { rc = PKT_NAK;   break; }
        if (hcint & HCINT_ERRORS) { rc = PKT_ERR;   break; }
        if (!(HPRT & HPRT_PCSTS)) { rc = PKT_GONE;  break; }
        if ((uint32_t)(sys_ticks() - start) > PACKET_MS) break;
    }
    halt(ch);
    return rc;
}

/*
 * A run of packets in one direction, the data toggle carried in *toggle.
 * IN stops at 'len' bytes or at a short packet.  A NAK is retried until
 * 'wait_ms' has passed; three errors in a row end it.
 */
static int transfer(uint32_t hcchar, uint8_t *toggle, uint8_t *buf,
                    uint32_t len, uint32_t *done, uint32_t wait_ms)
{
    int in = (hcchar & HCCHAR_IN) != 0;
    uint32_t mps = hcchar & 0x7FFUL;
    uint32_t off = 0, start = sys_ticks();
    uint8_t tmp[64];
    int errors = 0;

    if (done) *done = 0;
    if (mps == 0 || mps > sizeof(tmp)) return USBH_UNSUP;
    do {
        uint32_t n = MIN(mps, len - off), got = 0;
        int rc;

        rc = packet(hcchar, *toggle ? PID_DATA1 : PID_DATA0,
                    in ? tmp : (buf ? buf + off : NULL), n, &got);
        if (rc == PKT_NAK) {
            if ((uint32_t)(sys_ticks() - start) > wait_ms) return USBH_TIMEOUT;
            continue;
        }
        if (rc == PKT_ERR || rc == PKT_TIMEOUT) {
            if (++errors >= TRIES)
                return rc == PKT_ERR ? USBH_ERR : USBH_TIMEOUT;
            continue;
        }
        if (rc == PKT_STALL) return USBH_STALL;
        if (rc == PKT_GONE) return USBH_GONE;

        errors = 0;
        start = sys_ticks();
        *toggle ^= 1;
        if (in) {
            got = MIN(got, len - off);
            if (got) memcpy(buf + off, tmp, got);
            off += got;
            if (done) *done = off;
            if (got < mps) break;               /* short packet: the end */
        } else {
            off += n;
            if (done) *done = off;
        }
    } while (off < len);
    return USBH_OK;
}

int usbh_control(uint8_t addr, uint8_t mps0, const uint8_t setup[8],
                 void *data, uint16_t *len)
{
    uint32_t base = HCCHAR_MPS(mps0) | HCCHAR_EP(0) | HCCHAR_CTRL |
                    HCCHAR_DAD(addr);
    uint16_t want = (uint16_t)(setup[6] | (setup[7] << 8));
    int in = (setup[0] & 0x80) != 0;
    uint8_t toggle = 1;
    uint32_t done = 0;
    int rc = PKT_ERR;

    if (!s_open) return USBH_NODEV;
    if (s_iso_on) return USBH_ERR;      /* streaming: the interrupt's    */
    for (int i = 0; i < TRIES && rc != PKT_OK; i++) {
        rc = packet(base, PID_SETUP, (uint8_t *)setup, 8, NULL);
        if (rc == PKT_GONE) return USBH_GONE;
    }
    if (rc != PKT_OK) return USBH_ERR;

    if (len && *len < want) want = *len;
    if (want) {
        rc = transfer(base | (in ? HCCHAR_IN : 0), &toggle, data, want,
                      &done, 1000);
        if (rc != USBH_OK) return rc;
    }
    if (len) *len = (uint16_t)done;

    toggle = 1;                         /* status: DATA1, other direction */
    return transfer(base | ((in && want) ? 0 : HCCHAR_IN), &toggle,
                    NULL, 0, NULL, 1000);
}

int usbh_bulk(uint8_t addr, uint8_t ep, uint16_t mps, uint8_t *toggle,
              void *buf, uint32_t len, uint32_t *done, uint32_t wait_ms)
{
    uint32_t hcchar = HCCHAR_MPS(mps) | HCCHAR_EP(ep & 0x0F) | HCCHAR_BULK |
                      HCCHAR_DAD(addr) | ((ep & 0x80) ? HCCHAR_IN : 0);

    if (!s_open) return USBH_NODEV;
    if (s_iso_on) return USBH_ERR;
    return transfer(hcchar, toggle, buf, len, done, wait_ms);
}

#ifdef FREYA_AUDIO
/* ------------------------------------------------------- isochronous */
/*
 * A headset's two streams, from the core's interrupt.  Once streaming
 * starts, nothing else touches the core: control and bulk transfers are
 * refused until usbh_iso_stop().  At each start of frame the speaker's
 * next packet is queued on channel 2 and the microphone's IN on channel
 * 3, both for the frame after this one (ODDFRM).  The microphone's data
 * comes through the receive FIFO.  A channel still busy two frames on is
 * halted and its packet counted lost.
 */
#define ISO_IN_MAX      384     /* the largest microphone packet taken    */
#define ISO_OUT_MAX     400

static uint8_t  s_out_ep, s_in_ep;
static uint16_t s_out_mps, s_in_mps;
static uint8_t  s_out_age, s_in_age;
static uint8_t  s_in_pkt[ISO_IN_MAX];
static uint32_t s_out_pkt[ISO_OUT_MAX / 4];

static void iso_halt(int ch)
{
    if (HCCHAR(ch) & HCCHAR_CHENA)
        HCCHAR(ch) |= HCCHAR_CHDIS | HCCHAR_CHENA;
}

static void iso_rx(void)
{
    uint32_t st = GRXSTSP, n = RXSTS_BCNT(st), words = (n + 3) / 4;
    int take = RXSTS_PKT(st) == RXSTS_IN_DATA && (int)RXSTS_CH(st) == CH_ISO_IN;

    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = FIFO(0);
        if (take && i * 4 + 4 <= ISO_IN_MAX)
            memcpy(s_in_pkt + i * 4, &w, 4);
    }
    if (take && n) audio_frame_in(s_in_pkt, (int)MIN(n, (uint32_t)ISO_IN_MAX));
}

static void iso_ch(int ch)
{
    uint32_t h = HCINT(ch);

    HCINT(ch) = h;
    if (h & HCINT_ERRORS) {
        audio_frame_error();
        iso_halt(ch);
    } else if (h & HCINT_XFRC) {
        iso_halt(ch);       /* slave mode may leave it enabled: free it now */
    }
}

static uint32_t iso_char(uint8_t ep, uint16_t mps)
{
    uint32_t odd = (HFNUM & 1U) ? 0 : HCCHAR_ODDFRM;    /* the next frame */

    return HCCHAR_MPS(mps) | HCCHAR_EP(ep & 0x0F) | HCCHAR_ISO |
           HCCHAR_MC1 | HCCHAR_DAD(g_usbdev.addr) | odd |
           ((ep & 0x80) ? HCCHAR_IN : 0);
}

static void iso_frame(void)
{
    if (s_out_ep) {
        if (HCCHAR(CH_ISO_OUT) & HCCHAR_CHENA) {
            if (++s_out_age > 2) { iso_halt(CH_ISO_OUT); audio_frame_error(); }
        } else {
            int n = audio_frame_out((uint8_t *)s_out_pkt, MIN(s_out_mps, ISO_OUT_MAX));
            uint32_t words = ((uint32_t)n + 3) / 4;

            s_out_age = 0;
            if ((HPTXSTS & 0xFFFFUL) < words || !(HPTXSTS & (0xFFUL << 16))) {
                audio_frame_error();            /* no room: drop the frame */
            } else {
                HCTSIZ(CH_ISO_OUT) = TSIZ(n, 1, PID_DATA0);
                HCCHAR(CH_ISO_OUT) = iso_char(s_out_ep, s_out_mps) | HCCHAR_CHENA;
                for (uint32_t i = 0; i < words; i++)
                    FIFO(CH_ISO_OUT) = s_out_pkt[i];
            }
        }
    } else {
        (void)audio_frame_out(NULL, 0);         /* keeps the frame count */
    }

    if (s_in_ep) {
        if (HCCHAR(CH_ISO_IN) & HCCHAR_CHENA) {
            if (++s_in_age > 2) { iso_halt(CH_ISO_IN); audio_frame_error(); }
        } else {
            s_in_age = 0;
            HCTSIZ(CH_ISO_IN) = TSIZ(s_in_mps, 1, PID_DATA0);
            HCCHAR(CH_ISO_IN) = iso_char(s_in_ep, s_in_mps) | HCCHAR_CHENA;
        }
    }
}

static void iso_off(void)
{
    GAHBCFG &= ~GAHBCFG_GINT;
    GINTMSK = 0;
    nvic_disable(BOARD_USB_IRQn);
    s_iso_on = 0;
}

void BOARD_USB_IRQ_HANDLER(void)
{
    uint32_t st = GINTSTS & GINTMSK;

    if (st & GINTSTS_DISCINT) {
        GINTSTS = GINTSTS_DISCINT;
        iso_off();
        audio_gone();
        return;
    }
    while (GINTSTS & GINTSTS_RXFLVL)
        iso_rx();
    if (st & GINTSTS_HCINT) {
        uint32_t ha = HAINT;
        if (ha & (1UL << CH_ISO_OUT)) iso_ch(CH_ISO_OUT);
        if (ha & (1UL << CH_ISO_IN)) iso_ch(CH_ISO_IN);
    }
    if (st & GINTSTS_SOF) {
        GINTSTS = GINTSTS_SOF;
        iso_frame();
    }
}

int usbh_iso_start(uint8_t out_ep, uint16_t out_mps, uint8_t in_ep,
                   uint16_t in_mps)
{
    if (!s_open || !(HPRT & HPRT_PENA)) return USBH_NODEV;
    if (in_ep && in_mps > ISO_IN_MAX) return USBH_UNSUP;
    if (out_ep && out_mps > ISO_OUT_MAX) out_mps = ISO_OUT_MAX;

    s_out_ep = out_ep; s_out_mps = out_mps;
    s_in_ep = in_ep;   s_in_mps = in_mps;
    s_out_age = s_in_age = 0;
    for (int ch = CH_ISO_OUT; ch <= CH_ISO_IN; ch++) {
        HCINT(ch) = 0xFFFFFFFFUL;
        HCINTMSK(ch) = HCINT_XFRC | HCINT_CHH | HCINT_ERRORS;
    }
    HAINTMSK = (1UL << CH_ISO_OUT) | (1UL << CH_ISO_IN);
    GINTSTS = 0xFFFFFFFFUL;
    GINTMSK = GINTSTS_SOF | GINTSTS_RXFLVL | GINTSTS_HCINT | GINTSTS_DISCINT;

    s_iso_on = 1;
    nvic_set_priority(BOARD_USB_IRQn, IRQ_PRIO_USB);
    nvic_enable(BOARD_USB_IRQn);
    GAHBCFG |= GAHBCFG_GINT;
    return USBH_OK;
}

void usbh_iso_stop(void)
{
    uint32_t start;

    if (!s_iso_on) return;
    iso_off();
    for (int ch = CH_ISO_OUT; ch <= CH_ISO_IN; ch++) {
        iso_halt(ch);
        start = sys_ticks();
        while ((HCCHAR(ch) & HCCHAR_CHENA) &&
               (uint32_t)(sys_ticks() - start) <= HALT_MS)
            rx_drain(ch, NULL, 0, NULL);
        HCINT(ch) = 0xFFFFFFFFUL;
    }
    HAINTMSK = 0;
    flush_fifos();
    s_out_ep = s_in_ep = 0;
}
#endif /* FREYA_AUDIO */
