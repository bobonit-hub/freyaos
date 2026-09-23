/*
 * Freya - putting 8080 code into the Altair's memory from the card.
 *
 * Two formats: a raw memory image, which lands at an address the caller
 * names, and Intel HEX, which carries its own.  A file whose name ends
 * in .hex or .ihx is read as HEX, anything else as an image.  Loading
 * writes through mem_poke(), so a PROM image goes into its socket the
 * same way a program goes into RAM.
 *
 * 'save' is the other direction: a range of memory, as a raw image.
 * 'upload' is the same load with the console as the file: an XMODEM
 * stream, 128-byte packets from the static buffer below and 1K packets
 * from a block borrowed off the heap for the transfer.
 */
#define LOAD_BUF  256

static uint8_t s_io_buf[LOAD_BUF];

static int ends_with(const char *s, const char *tail)
{
    int n = 0, m = 0;

    while (s[n])
        n++;
    while (tail[m])
        m++;
    if (m > n)
        return 0;
    for (int i = 0; i < m; i++) {
        char a = s[n - m + i], b = tail[i];

        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

static int is_hex_name(const char *path)
{
    return ends_with(path, ".hex") || ends_with(path, ".ihx");
}

/* Returns the number of bytes loaded, or -1 if the file cannot be read.
 * An image that runs past FFFFh is cut off there. */
static int32_t load_bin(const char *path, uint16_t at)
{
    int fd = g->open(path, FREYA_O_RDONLY);
    uint32_t addr = at;
    int n;

    if (fd < 0)
        return -1;
    while (addr <= 0xFFFFu && (n = g->read(fd, s_io_buf, LOAD_BUF)) > 0) {
        for (int i = 0; i < n && addr <= 0xFFFFu; i++)
            mem_poke((uint16_t)addr++, s_io_buf[i]);
    }
    g->close(fd);
    return (int32_t)(addr - at);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/*
 * One Intel HEX record, without its colon.  Returns the record type, or
 * -1 for a line that is not a record or whose checksum is wrong.
 */
static int hex_record(const char *s, int len, int32_t *loaded)
{
    uint8_t rec[4 + 255 + 1];
    int n = 0;

    if (len < 10 || (len & 1))
        return -1;
    for (int i = 0; i + 1 < len && n < (int)sizeof rec; i += 2) {
        int hi = hexval(s[i]), lo = hexval(s[i + 1]);

        if (hi < 0 || lo < 0)
            return -1;
        rec[n++] = (uint8_t)(hi << 4 | lo);
    }
    if (n != rec[0] + 5)
        return -1;
    {
        uint8_t sum = 0;

        for (int i = 0; i < n; i++)
            sum = (uint8_t)(sum + rec[i]);
        if (sum)
            return -1;
    }
    if (rec[3] == 0) {
        uint16_t a = (uint16_t)(rec[1] << 8 | rec[2]);

        for (int i = 0; i < rec[0]; i++)
            mem_poke((uint16_t)(a + i), rec[4 + i]);
        *loaded += rec[0];
    }
    return rec[3];
}

/* Returns the number of data bytes loaded, -1 if the file cannot be
 * read, or -2 - the line number of the first bad record. */
static int32_t load_hex(const char *path)
{
    int fd = g->open(path, FREYA_O_RDONLY);
    char line[4 + 2 * (4 + 255 + 1)];
    int len = 0, in_rec = 0, lineno = 1, n;
    int32_t loaded = 0;

    if (fd < 0)
        return -1;
    while ((n = g->read(fd, s_io_buf, LOAD_BUF)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)s_io_buf[i];

            if (c == ':') {
                in_rec = 1;
                len = 0;
            } else if (c == '\n' || c == '\r') {
                if (in_rec) {
                    int t = hex_record(line, len, &loaded);

                    if (t < 0) {
                        g->close(fd);
                        return -2 - lineno;
                    }
                    if (t == 1) {
                        g->close(fd);
                        return loaded;
                    }
                }
                in_rec = 0;
                if (c == '\n')
                    lineno++;
            } else if (in_rec && len < (int)sizeof line) {
                line[len++] = c;
            }
        }
    }
    if (in_rec && hex_record(line, len, &loaded) < 0)
        loaded = -2 - lineno;
    g->close(fd);
    return loaded;
}

static int32_t load_file(const char *path, uint16_t at)
{
    return is_hex_name(path) ? load_hex(path) : load_bin(path, at);
}

/*
 * The bootstrap loader, done here instead of toggled in.
 *
 * An Altair tape starts with a leader of one repeated byte, then the
 * tape's own checksum loader, which arrives last byte first.  The loader
 * MITS printed for keying in holds the load address in HL, with L both
 * the length of the checksum loader and the leader byte: every byte read
 * that equals L is skipped, anything else goes to --HL, and when L
 * reaches 0 it jumps to H:00.  This does exactly that, quirks included -
 * a checksum loader byte that happens to equal L is skipped too, which
 * the tapes are made to allow for - and leaves the rest of the tape in
 * the reader for the checksum loader to read through the terminal port.
 *
 * Which BASIC is on the tape decides HL.
 */
static const struct {
    const char *name;
    uint16_t    hl;
} k_boot[] = {
    { "4k32", 0x0FAE },             /* 4K BASIC 3.2                  */
    { "4k40", 0x0FC2 },             /* 4K BASIC 4.0                  */
    { "8k",   0x1FC2 },             /* 8K BASIC 3.2 / 4.0            */
    { "ext",  0x3FC2 },             /* Extended BASIC 4.0 / 4.1      */
    { "disk", 0x7EC2 },             /* Extended Disk BASIC 5.x       */
};

static int boot_lookup(const char *name, uint16_t *hl)
{
    for (unsigned i = 0; i < sizeof k_boot / sizeof k_boot[0]; i++) {
        if (streq(name, k_boot[i].name)) {
            *hl = k_boot[i].hl;
            return 0;
        }
    }
    return -1;
}

/* Returns where the checksum loader starts, or -1 if the tape ran out
 * first. */
static int32_t tape_boot(uint16_t hl)
{
    uint8_t h = (uint8_t)(hl >> 8), l = (uint8_t)hl;

    while (l) {
        uint8_t b;

        if (!tape_ready())
            return -1;
        b = tape_read();
        if (b == l)
            continue;
        l--;
        mem_poke((uint16_t)(h << 8 | l), b);
    }
    s_tape_tty = 1;
    return (int32_t)h << 8;
}

/* Returns the bytes written, or -1. */
static int32_t save_bin(const char *path, uint16_t at, uint32_t len)
{
    int fd = g->open(path, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    uint32_t done = 0;

    if (fd < 0)
        return -1;
    if (len > 0x10000u - at)
        len = 0x10000u - at;
    while (done < len) {
        int n = len - done > LOAD_BUF ? LOAD_BUF : (int)(len - done);

        for (int i = 0; i < n; i++)
            s_io_buf[i] = mem_rd((uint16_t)(at + done + (uint32_t)i));
        if (g->write(fd, s_io_buf, n) != n) {
            g->close(fd);
            return -1;
        }
        done += (uint32_t)n;
    }
    g->close(fd);
    return (int32_t)done;
}

/*
 * XMODEM / XMODEM-1K / checksum, the same stream 'download' receives
 * onto the card.  The last packet is padded with SUB because the
 * protocol has no length; those bytes are held back and dropped unless
 * the caller asks for the raw stream.  A 1K packet is 1024 bytes and
 * the static buffer is 256, so that packet borrows its buffer from the
 * heap and a board with no room left says so instead of overflowing.
 *
 * Returns the number of bytes handed to the sink, or -2 on a timeout,
 * -3 when the sender cancels, -4 after too many bad packets, -5 on a
 * broken sequence, -6 when the sink refuses a byte, -7 when a 1K
 * packet arrives and the heap cannot hold it.
 */
#define XM_SOH  0x01
#define XM_STX  0x02
#define XM_EOT  0x04
#define XM_ACK  0x06
#define XM_NAK  0x15
#define XM_CAN  0x18
#define XM_SUB  0x1A

static int s_hex_line;              /* bad uploaded HEX record, else 0 */

typedef int (*xm_sink_fn)(const uint8_t *p, int n, void *ctx);

static uint16_t crc16_xmodem(const uint8_t *p, int len)
{
    uint16_t crc = 0;

    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

static void xm_cancel(void)
{
    for (int i = 0; i < 8; i++)
        g->putc((char)XM_CAN);
    for (int i = 0; i < 8; i++)
        g->putc('\b');
}

/* Drains whatever the sender still has in flight. */
static void xm_flush(void)
{
    while (g->getc_timeout(300) >= 0) { }
}

static int xm_emit_subs(xm_sink_fn sink, void *ctx, int n)
{
    uint8_t b = XM_SUB;

    while (n--) {
        if (sink(&b, 1, ctx))
            return -1;
    }
    return 0;
}

/* Hands packet bytes to the sink.  A run of SUB at the end of a packet
 * is remembered in *held: the next real byte means it was data, and
 * end of stream means it was padding. */
static int xm_feed(xm_sink_fn sink, void *ctx, const uint8_t *pkt, int len,
                   int strip, int *held, int32_t *total)
{
    int i = 0;

    while (i < len) {
        int start, run;

        if (strip && pkt[i] == XM_SUB) {
            run = 0;
            while (i < len && pkt[i] == XM_SUB) {
                run++;
                i++;
            }
            if (i == len) {
                *held += run;
                break;
            }
            if (xm_emit_subs(sink, ctx, *held + run))
                return -1;
            *total += *held + run;
            *held = 0;
            continue;
        }
        if (*held) {
            if (xm_emit_subs(sink, ctx, *held))
                return -1;
            *total += *held;
            *held = 0;
        }
        start = i;
        while (i < len && !(strip && pkt[i] == XM_SUB))
            i++;
        if (sink(pkt + start, i - start, ctx))
            return -1;
        *total += i - start;
    }
    return 0;
}

static int32_t xmodem_receive(xm_sink_fn sink, void *ctx, int strip)
{
    uint8_t *big = NULL;
    uint8_t expect = 1;
    int crc_mode = 1, handshakes = 0, retries = 0, held = 0;
    int32_t total = 0;
    int rc = 0;

    while (rc == 0) {
        int c = g->getc_timeout(1000);

        if (c < 0) {
            if (++handshakes > 60) {
                rc = -2;
                break;
            }
            if (crc_mode && handshakes <= 20)
                g->putc('C');
            else {
                crc_mode = 0;
                g->putc((char)XM_NAK);
            }
            continue;
        }
        if (c == XM_CAN) {
            c = g->getc_timeout(1000);
            if (c == XM_CAN)
                rc = -3;
            continue;
        }
        if (c == XM_EOT) {
            if (!strip && held) {
                if (xm_emit_subs(sink, ctx, held))
                    rc = -6;
                else
                    total += held;
            }
            if (rc == 0)
                g->putc((char)XM_ACK);
            break;
        }
        if (c != XM_SOH && c != XM_STX)
            continue;

        {
            uint8_t *pkt = s_io_buf;
            int len = 128;
            int blk, nblk, bad = 0, short_read = 0;
            int b1 = 0, b2 = 0;
            int need;

            if (c == XM_STX) {
                if (!big) {
                    big = g->malloc(1024);
                    if (!big) {
                        rc = -7;
                        break;
                    }
                }
                pkt = big;
                len = 1024;
            }
            need = len + (crc_mode ? 2 : 1);
            blk = g->getc_timeout(1000);
            nblk = g->getc_timeout(1000);
            if (blk < 0 || nblk < 0 || ((blk + nblk) & 0xFF) != 0xFF)
                bad = 1;
            for (int i = 0; i < need; i++) {
                int d = g->getc_timeout(1000);

                if (d < 0) {
                    bad = 1;
                    short_read = 1;
                    break;
                }
                if (i < len)
                    pkt[i] = (uint8_t)d;
                else if (i == len)
                    b1 = d;
                else
                    b2 = d;
            }
            if (!bad) {
                if (crc_mode) {
                    uint16_t want = (uint16_t)((b1 << 8) | b2);

                    if (crc16_xmodem(pkt, len) != want)
                        bad = 1;
                } else {
                    uint8_t sum = 0;

                    for (int i = 0; i < len; i++)
                        sum = (uint8_t)(sum + pkt[i]);
                    if (sum != (uint8_t)b1)
                        bad = 1;
                }
            }
            if (bad) {
                if (++retries > 10) {
                    rc = -4;
                    break;
                }
                if (short_read)
                    xm_flush();
                g->putc((char)XM_NAK);
                continue;
            }
            if ((uint8_t)blk == (uint8_t)(expect - 1)) {
                g->putc((char)XM_ACK);
                continue;
            }
            if ((uint8_t)blk != expect) {
                rc = -5;
                break;
            }
            if (xm_feed(sink, ctx, pkt, len, strip, &held, &total)) {
                rc = -6;
                break;
            }
            expect++;
            retries = 0;
            handshakes = 0;
            g->putc((char)XM_ACK);
        }
    }

    if (big)
        g->free(big);
    if (rc != 0)
        xm_cancel();
    xm_flush();
    return rc != 0 ? rc : total;
}

struct bin_up {
    uint32_t addr;
    int32_t  n;
};

static int bin_sink(const uint8_t *p, int n, void *ctx)
{
    struct bin_up *b = ctx;

    for (int i = 0; i < n && b->addr <= 0xFFFFu; i++, b->addr++) {
        mem_poke((uint16_t)b->addr, p[i]);
        b->n++;
    }
    return 0;
}

/* Returns the bytes stored, cut off at FFFFh, or an xmodem_receive()
 * error.  strip drops the SUB padding; raw keeps it. */
static int32_t upload_bin(uint16_t at, int strip)
{
    struct bin_up b;

    b.addr = at;
    b.n = 0;
    {
        int32_t rc = xmodem_receive(bin_sink, &b, strip);

        return rc < 0 ? rc : b.n;
    }
}

struct hex_up {
    char    line[4 + 2 * (4 + 255 + 1)];
    int     len;
    int     in_rec;
    int     lineno;
    int32_t loaded;
    int     eof;
};

static int hex_byte(struct hex_up *h, char c)
{
    if (h->eof)
        return 0;
    if (c == ':') {
        h->in_rec = 1;
        h->len = 0;
    } else if (c == '\n' || c == '\r') {
        if (h->in_rec) {
            int t = hex_record(h->line, h->len, &h->loaded);

            if (t < 0) {
                s_hex_line = h->lineno;
                return -1;
            }
            if (t == 1)
                h->eof = 1;
        }
        h->in_rec = 0;
        if (c == '\n')
            h->lineno++;
    } else if (h->in_rec && h->len < (int)sizeof h->line) {
        h->line[h->len++] = c;
    }
    return 0;
}

static int hex_sink(const uint8_t *p, int n, void *ctx)
{
    struct hex_up *h = ctx;

    for (int i = 0; i < n; i++) {
        if (hex_byte(h, (char)p[i]))
            return -1;
    }
    return 0;
}

/* Returns the data bytes loaded, an xmodem_receive() error, or -6 with
 * s_hex_line set to the bad record. */
static int32_t upload_hex(void)
{
    struct hex_up h;
    int32_t rc;

    s_hex_line = 0;
    h.len = 0;
    h.in_rec = 0;
    h.lineno = 1;
    h.loaded = 0;
    h.eof = 0;
    rc = xmodem_receive(hex_sink, &h, 1);
    if (rc < 0)
        return rc;
    if (h.in_rec && !h.eof && hex_record(h.line, h.len, &h.loaded) < 0) {
        s_hex_line = h.lineno;
        return -6;
    }
    return h.loaded;
}
