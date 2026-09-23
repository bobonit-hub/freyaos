/*
 * Freya - the Altair's I/O ports, for the Altair sample.
 *
 *   00h / 01h   88-SIO: status (active low) and data - the terminal
 *   04h / 05h   88-PIO: control/status and data - the tape reader,
 *               the terminal or nothing, as 'pio' says
 *   06h / 07h   88-ACR cassette, SIO compatible - the tape reader
 *   10h / 11h   Turnkey SIO, which is 88-2SIO port A - the terminal
 *   12h / 13h   88-2SIO port B - the tape reader
 *   FFh         sense switches
 *
 * The Turnkey Module's serial port is a 6850 ACIA wired like port A of
 * an 88-2SIO, so software built for a 2SIO finds its terminal there.  An
 * 88-SIO at port 0 answers too, for software built for that board.
 * Any tape port reads the file attached with 'tape', a byte at a time,
 * which is how a loader PROM reads a paper tape or a cassette.
 *
 * The 88-PIO is a pair of 8-bit latches with a handshake flip-flop each.
 * Its status is active high: bit 0 is set when the output device can
 * take a byte, bit 1 when the input device has strobed one in, and no
 * other bit is driven.  A control write sets the two interrupt enables,
 * which have nothing to raise here.  It was most often a parallel paper
 * tape reader such as the OP-80, or a parallel terminal.
 *
 * Any other port reads FFh and ignores writes.
 */
#define ACIA_RDRF   0x01            /* receive data register full    */
#define ACIA_TDRE   0x02            /* transmit data register empty  */
#define SIO_IN_BUSY 0x01            /* 88-SIO: 0 when a byte is in   */
#define SIO_OUT_BUSY 0x80           /* 88-SIO: 0 when it can send    */
#define PIO_OUT_RDY 0x01            /* 88-PIO: the output device is ready */
#define PIO_IN_RDY  0x02            /* 88-PIO: a byte has come in    */

#define TAPE_BUF    256

enum { PIO_OFF, PIO_TAPE, PIO_TERM };
static const char *const k_pio_names[] = { "off", "tape", "term" };

static uint8_t s_switches;
static uint8_t s_pio = PIO_TAPE;     /* what the 88-PIO is wired to   */
static int     s_tape = -1;          /* fd of the attached tape       */
static uint8_t s_tape_buf[TAPE_BUF];
static int     s_tape_len, s_tape_pos;
static uint32_t s_tape_count;

static int tape_ready(void)
{
    if (s_tape < 0)
        return 0;
    if (s_tape_pos < s_tape_len)
        return 1;
    s_tape_len = g->read(s_tape, s_tape_buf, TAPE_BUF);
    s_tape_pos = 0;
    if (s_tape_len <= 0) {
        s_tape_len = 0;
        return 0;
    }
    return 1;
}

static uint8_t tape_read(void)
{
    if (!tape_ready())
        return 0;
    s_tape_count++;
    return s_tape_buf[s_tape_pos++];
}

static void tape_detach(void)
{
    if (s_tape >= 0)
        g->close(s_tape);
    s_tape = -1;
    s_tape_len = s_tape_pos = 0;
    s_tape_count = 0;
    s_tape_tty = 0;
}

static int tape_attach(const char *path)
{
    tape_detach();
    s_tape = g->open(path, FREYA_O_RDONLY);
    return s_tape >= 0 ? 0 : -1;
}

static int pio_lookup(const char *name, uint8_t *out)
{
    for (uint8_t i = 0; i < 3; i++) {
        if (streq(name, k_pio_names[i])) {
            *out = i;
            return 0;
        }
    }
    return -1;
}

static uint8_t pio_status(void)
{
    int in = s_pio == PIO_TERM ? term_ready() : tape_ready();

    return (uint8_t)(PIO_OUT_RDY | (in ? PIO_IN_RDY : 0));
}

static uint8_t io_in(uint8_t port)
{
    switch (port) {
    case 0x00:
        return term_ready() ? 0 : SIO_IN_BUSY;
    case 0x01:
        return term_read();
    case 0x04:
        return s_pio == PIO_OFF ? 0xFF : pio_status();
    case 0x05:
        if (s_pio == PIO_OFF)
            return 0xFF;
        return s_pio == PIO_TERM ? term_read() : tape_read();
    case 0x06:
        return tape_ready() ? 0 : SIO_IN_BUSY;
    case 0x07:
        return tape_read();
    case 0x10:
        return (uint8_t)(ACIA_TDRE | (term_ready() ? ACIA_RDRF : 0));
    case 0x11:
        return term_read();
    case 0x12:
        return (uint8_t)(ACIA_TDRE | (tape_ready() ? ACIA_RDRF : 0));
    case 0x13:
        return tape_read();
    case 0xFF:
        return s_switches;
    default:
        return 0xFF;
    }
}

/* Control writes - a 6850 master reset, word format, baud rate, the
 * PIO's interrupt enables - have nothing to set up on this side, and the
 * tape has no punch, so a PIO wired to the reader drops what it is sent. */
static void io_out(uint8_t port, uint8_t v)
{
    if (port == 0x01 || port == 0x11 || (port == 0x05 && s_pio == PIO_TERM))
        term_write(v);
}
