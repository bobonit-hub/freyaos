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
