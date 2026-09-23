/*
 * Freya - the terminal on the end of the Altair's serial port.
 *
 * The Freya console stands in for a teletype.  With a raw console the
 * kernel leaves Ctrl-C alone, so BASIC gets it to break a program with,
 * and Ctrl-] is the one key the emulator keeps for itself: it opens the
 * menu that stands in for a front panel.  A kernel without raw mode
 * still works, only Ctrl-C then stops the emulator instead.
 *
 * Altair software was written for uppercase terminals, and its line
 * editor rubs out with DEL, so by default lowercase is folded to
 * uppercase and Backspace is sent as DEL.  Output loses bit 7, which
 * some programs set as mark parity.
 */
#define KEY_MENU    0x1D            /* Ctrl-] */
#define KEY_BS      0x08
#define KEY_DEL     0x7F

static int     s_pending = -1;      /* a key read ahead by a status poll */
static uint8_t s_menu_req;
static uint8_t s_raw;
static uint8_t s_upper = 1;
static uint8_t s_bs_del = 1;
static uint8_t s_tape_tty;          /* the tape is in the teletype's reader */

#define AHEAD 32                    /* power of two */
static uint8_t s_ahead[AHEAD];      /* typed while the tape was reading */
static uint8_t s_ahead_head, s_ahead_tail;

static int  tape_ready(void);
static uint8_t tape_read(void);
static void tape_detach(void);

static void term_open(void)
{
    s_pending = -1;
    s_menu_req = 0;
    s_ahead_head = s_ahead_tail = 0;
    s_raw = 0;
    if (FREYA_API_HAS(g, console_raw)) {
        g->console_raw(1);
        s_raw = 1;
    }
}

static void term_close(void)
{
    if (s_raw)
        g->console_raw(0);
    s_raw = 0;
}

/*
 * A tape in the teletype's reader is read through the terminal port, the
 * way a paper tape went into an Altair.  The reader runs until the tape
 * ends and the port is then the keyboard's again.  Keys typed meanwhile
 * are kept for after it - a tape ends in a trailer the program is
 * already reading as input, and an answer typed then must not be lost -
 * except Ctrl-], which opens the menu as always.
 */
static int tape_tty_ready(void)
{
    while (g->kbhit()) {
        int c = g->getc_timeout(0);

        if (c == KEY_MENU)
            s_menu_req = 1;
        else if (c >= 0 && (uint8_t)(s_ahead_head - s_ahead_tail) < AHEAD)
            s_ahead[s_ahead_head++ & (AHEAD - 1)] = (uint8_t)c;
    }
    if (tape_ready()) {
        s_pending = tape_read();
        return 1;
    }
    s_tape_tty = 0;
    tape_detach();
    return 0;
}

static int key_in(void)
{
    if (s_ahead_head != s_ahead_tail)
        return s_ahead[s_ahead_tail++ & (AHEAD - 1)];
    if (!g->kbhit())
        return -1;
    return g->getc_timeout(0);
}

/* Whether a key is waiting for the 8080.  Ctrl-] is taken here and only
 * raises the menu request. */
static int term_ready(void)
{
    int c;

    if (s_pending >= 0)
        return 1;
    if (s_tape_tty && tape_tty_ready())
        return 1;
    c = key_in();
    if (c < 0)
        return 0;
    if (c == KEY_MENU) {
        s_menu_req = 1;
        return 0;
    }
    if (s_upper && c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    if (s_bs_del && c == KEY_BS)
        c = KEY_DEL;
    s_pending = c & 0x7F;
    return 1;
}

/* The key term_ready() found, or 0 if a program reads with none there,
 * which is what a 6850 hands back from an empty receive register. */
static uint8_t term_read(void)
{
    uint8_t c;

    if (!term_ready())
        return 0;
    c = (uint8_t)s_pending;
    s_pending = -1;
    return c;
}

static void term_write(uint8_t c)
{
    g->putc((char)(c & 0x7F));
}
