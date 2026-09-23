/*
 * edit - a terminal text editor for Freya.
 *
 * Draws on the serial console with ANSI / VT100 sequences and reads
 * single keypresses through the service table.  One file occupies a
 * fixed buffer in the program region; the gap sits at the cursor, so
 * typing does not slide the whole tail.  Keys are listed in
 * samples/edit/README.md.  Ctrl-C still belongs to Freya: it stops the
 * program and does not write the buffer.
 *
 *     run edit.bin
 *     run edit.bin notes.txt
 *     runflash notes.txt       (after install / PROGRAM=edit)
 */
#include "freya_api.h"

#define SCR_COLS    80
#define SCR_ROWS    24
#define TEXT_ROW0   2
#define TEXT_ROWS   (SCR_ROWS - 2)
#define TAB_W       4
#define PATH_MAX    128
#define NAME_MAX    63          /* one path component, what FAT stores */
#define NAME_PFX    8           /* columns used by " write: "          */
#define NAME_ROOM   (SCR_COLS - NAME_PFX - 1)
#define ASK_COL     22          /* one past " discard changes? y/n"    */
#define ESC_MS      40u
#define CRLF_MS     5u

/*
 * The text is .bss inside the program region.  On the Blue Pill that
 * region is 8 KiB and also holds this program's code, so the buffer is
 * what remains after the editor.  On the Black Pill the region is
 * 56 KiB.  A file that does not fit is refused whole; nothing is cut
 * off and shown as if it were complete.
 */
#if defined(FREYA_BOARD_BLUEPILL)
/* 3072 leaves a little of the 8 KiB region past the code and this buffer. */
#define BUF_CAP     3072
#else
#define BUF_CAP     (32 * 1024)
#endif

enum {
    MODE_EDIT = 0,
    MODE_ASK,
    MODE_NAME
};

/* open/read/write failures use these values; they are not in the ABI. */
enum {
    FS_NOFS   = -2,
    FS_NOENT  = -3,
    FS_NOSPC  = -5,
    FS_INVAL  = -6,
    FS_NOTDIR = -7,
    FS_ISDIR  = -8,
    FS_RDONLY = -11
};

static const freya_api_t *g;

static char     s_buf[BUF_CAP];
static int      s_gap;          /* cursor, and the first byte of the gap */
static int      s_gap_end;      /* first byte of the text after the gap  */
static int      s_pref;         /* display column kept across up/down    */
static int      s_top;          /* first visible line                     */
static int      s_left;         /* first visible display column           */
static int      s_mode;
static int      s_changed;
static int      s_quit;
static int      s_discard;
static int      s_pending;      /* key taken early, or -1 */
static char     s_path[PATH_MAX];
static char     s_prompt[PATH_MAX];
static int      s_plen;
static char     s_msg[80];

static int text_len(void)
{
    return s_gap + (BUF_CAP - s_gap_end);
}

static char buf_at(int i)
{
    if (i < s_gap)
        return s_buf[i];
    return s_buf[i + (s_gap_end - s_gap)];
}

static void move_gap(int pos)
{
    int len = text_len();

    if (pos < 0)
        pos = 0;
    if (pos > len)
        pos = len;
    while (s_gap < pos) {
        s_buf[s_gap] = s_buf[s_gap_end];
        s_gap++;
        s_gap_end++;
    }
    while (s_gap > pos) {
        s_gap_end--;
        s_gap--;
        s_buf[s_gap_end] = s_buf[s_gap];
    }
}

static void set_msg(const char *s)
{
    int i = 0;
    int max = SCR_COLS - 2;

    if (max > (int)sizeof(s_msg) - 1)
        max = (int)sizeof(s_msg) - 1;
    while (s[i] && i < max) {
        s_msg[i] = s[i];
        i++;
    }
    s_msg[i] = '\0';
}

static void set_pair(const char *a, const char *b)
{
    int i = 0;
    int max = (int)sizeof(s_msg) - 1;

    while (*a && i < max)
        s_msg[i++] = *a++;
    while (*b && i < max)
        s_msg[i++] = *b++;
    s_msg[i] = '\0';
}

static void set_wrote(int n)
{
    char rev[10];
    int r = 0;
    int i = 0;
    unsigned u = (unsigned)n;
    const char *tail = " bytes";

    if (u == 0)
        rev[r++] = '0';
    while (u && r < (int)sizeof(rev)) {
        rev[r++] = (char)('0' + (u % 10u));
        u /= 10u;
    }
    set_msg("wrote ");
    while (s_msg[i])
        i++;
    while (r > 0 && i < (int)sizeof(s_msg) - 1)
        s_msg[i++] = rev[--r];
    while (*tail && i < (int)sizeof(s_msg) - 1)
        s_msg[i++] = *tail++;
    s_msg[i] = '\0';
}

static const char *err_name(int rc)
{
    if (rc == FS_NOFS)   return "no card";
    if (rc == FS_NOENT)  return "no such file";
    if (rc == FS_NOSPC)  return "no space";
    if (rc == FS_INVAL)  return "bad path";
    if (rc == FS_NOTDIR) return "not a directory";
    if (rc == FS_ISDIR)  return "is a directory";
    if (rc == FS_RDONLY) return "read only";
    return "failed";
}

static int path_ok(const char *s)
{
    int n = 0;
    int comp = 0;

    if (!s || !s[0])
        return 0;
    while (s[n]) {
        unsigned char ch = (unsigned char)s[n];

        if (ch < 0x20 || ch > 0x7E)
            return 0;
        if (ch == '/') {
            comp = 0;
        } else {
            comp++;
            if (comp > NAME_MAX)
                return 0;
        }
        n++;
        if (n >= PATH_MAX)
            return 0;
    }
    if (s[n - 1] == '/')
        return 0;
    return 1;
}

static void set_path(const char *s)
{
    int i = 0;

    while (s[i] && i < PATH_MAX - 1) {
        s_path[i] = s[i];
        i++;
    }
    s_path[i] = '\0';
}

static int span_at(int col, unsigned char ch)
{
    if (ch == '\t')
        return TAB_W - (col & (TAB_W - 1));
    if (ch < 0x20 || ch == 0x7F)
        return 2;
    return 1;
}

static void locate(int pos, int *line, int *col)
{
    int i, ln = 0, c = 0;
    int len = text_len();

    if (pos > len)
        pos = len;
    for (i = 0; i < pos; i++) {
        unsigned char ch = (unsigned char)buf_at(i);

        if (ch == '\n') {
            ln++;
            c = 0;
        } else {
            c += span_at(c, ch);
        }
    }
    *line = ln;
    *col = c;
}

static int last_line(void)
{
    int i, n = 0;
    int len = text_len();

    for (i = 0; i < len; i++) {
        if ((unsigned char)buf_at(i) == '\n')
            n++;
    }
    return n;
}

static int line_begin(int line)
{
    int i, ln = 0;
    int len = text_len();

    if (line <= 0)
        return 0;
    for (i = 0; i < len; i++) {
        if ((unsigned char)buf_at(i) == '\n') {
            ln++;
            if (ln == line)
                return i + 1;
        }
    }
    return len;
}

/* Byte on this line whose display column reaches 'want'. */
static int seek_col(int at, int want)
{
    int col = 0;
    int len = text_len();

    while (at < len) {
        unsigned char ch = (unsigned char)buf_at(at);
        int sp;

        if (ch == '\n')
            break;
        sp = span_at(col, ch);
        if (col + sp > want)
            break;
        col += sp;
        at++;
    }
    return at;
}

static void sync_pref(void)
{
    int ln, col;

    locate(s_gap, &ln, &col);
    s_pref = col;
    (void)ln;
}

static void reveal(void)
{
    int ln, col, last;

    locate(s_gap, &ln, &col);
    last = last_line();
    if (s_top < 0)
        s_top = 0;
    if (s_top > last)
        s_top = last;
    if (ln < s_top)
        s_top = ln;
    if (ln >= s_top + TEXT_ROWS)
        s_top = ln - TEXT_ROWS + 1;
    if (s_left < 0)
        s_left = 0;
    if (col < s_left)
        s_left = col;
    if (col >= s_left + SCR_COLS)
        s_left = col - SCR_COLS + 1;
}

static int insert_byte(char ch)
{
    if (s_gap == s_gap_end) {
        set_msg("buffer full");
        return -1;
    }
    s_buf[s_gap++] = ch;
    s_changed = 1;
    return 0;
}

static void backspace(void)
{
    if (s_gap == 0)
        return;
    s_gap--;
    s_changed = 1;
    sync_pref();
}

static void del_fwd(void)
{
    if (s_gap >= text_len())
        return;
    s_gap_end++;
    s_changed = 1;
    sync_pref();
}

static void kill_eol(void)
{
    int len = text_len();
    int n = 0;

    if (s_gap >= len)
        return;
    while (s_gap + n < len && (unsigned char)buf_at(s_gap + n) != '\n')
        n++;
    if (n == 0)
        n = 1;
    s_gap_end += n;
    s_changed = 1;
    sync_pref();
}

static void kill_bol(void)
{
    int n = 0;

    while (s_gap - n > 0 && (unsigned char)buf_at(s_gap - n - 1) != '\n')
        n++;
    if (n == 0)
        return;
    s_gap -= n;
    s_changed = 1;
    sync_pref();
}

static void move_h(int dir)
{
    move_gap(s_gap + dir);
    sync_pref();
}

static void vmove(int dir)
{
    int ln, col, dest, last;

    locate(s_gap, &ln, &col);
    (void)col;
    last = last_line();
    dest = ln + dir;
    if (dest < 0)
        dest = 0;
    if (dest > last)
        dest = last;
    move_gap(seek_col(line_begin(dest), s_pref));
}

static void page(int dir)
{
    int ln, col, last, dest, row;

    locate(s_gap, &ln, &col);
    (void)col;
    last = last_line();
    row = ln - s_top;
    if (row < 0)
        row = 0;
    if (row >= TEXT_ROWS)
        row = TEXT_ROWS - 1;
    dest = ln + dir * TEXT_ROWS;
    if (dest < 0)
        dest = 0;
    if (dest > last)
        dest = last;
    move_gap(seek_col(line_begin(dest), s_pref));
    s_top = dest - row;
    if (s_top < 0)
        s_top = 0;
}

static void line_edge(int end)
{
    int ln, col, at, len;

    locate(s_gap, &ln, &col);
    (void)col;
    at = line_begin(ln);
    if (!end) {
        move_gap(at);
        s_pref = 0;
        return;
    }
    len = text_len();
    while (at < len && (unsigned char)buf_at(at) != '\n')
        at++;
    move_gap(at);
    sync_pref();
}

static void go(int row, int col)
{
    g->printf("\x1b[%d;%dH", row, col);
}

static int put_lim(const char *s, int room)
{
    int n = 0;

    if (room <= 0)
        return 0;
    while (s[n] && n < room) {
        g->putc(s[n]);
        n++;
    }
    return n;
}

static void emit_span(unsigned char ch, int col)
{
    int sp = span_at(col, ch);
    int k;

    for (k = 0; k < sp; k++) {
        int dc = col + k;
        char out;

        if (dc < s_left || dc >= s_left + SCR_COLS)
            continue;
        if (ch == '\t')
            out = ' ';
        else if (ch >= 0x20 && ch < 0x7F)
            out = (char)ch;
        else if (ch < 0x20 || ch == 0x7F)
            out = (k == 0) ? '^' : ((ch == 0x7F) ? '?' : (char)(ch + '@'));
        else
            out = '?';
        g->putc(out);
    }
}

/* Paints the line at 'at'.  *more is set when another line follows. */
static int paint_line(int at, int *more)
{
    int col = 0;
    int len = text_len();

    *more = 0;
    while (at < len) {
        unsigned char ch = (unsigned char)buf_at(at);

        if (ch == '\n') {
            at++;
            *more = 1;
            break;
        }
        emit_span(ch, col);
        col += span_at(col, ch);
        at++;
    }
    return at;
}

static void draw_title(void)
{
    int room = SCR_COLS - 1;
    int mark = s_changed ? 2 : 0;

    go(1, 1);
    g->puts("\x1b[7m");
    room -= put_lim(" edit ", room);
    if (room < mark)
        mark = room;
    if (s_path[0])
        room -= put_lim(s_path, room - mark);
    else
        room -= put_lim("(new)", room - mark);
    if (s_changed)
        put_lim(" *", room);
    g->puts("\x1b[K\x1b[0m");
}

static void draw_text(void)
{
    int at = line_begin(s_top);
    int more = 1;
    int n;

    for (n = 0; n < TEXT_ROWS; n++) {
        go(TEXT_ROW0 + n, 1);
        if (more)
            at = paint_line(at, &more);
        g->puts("\x1b[K");
    }
}

static void draw_status(void)
{
    int ln, col;

    go(SCR_ROWS, 1);
    g->puts("\x1b[7m");
    if (s_mode == MODE_ASK) {
        g->puts(" discard changes? y/n");
    } else if (s_mode == MODE_NAME) {
        int start = 0;
        int i;

        g->puts(" write: ");
        if (s_plen > NAME_ROOM)
            start = s_plen - NAME_ROOM;
        for (i = start; i < s_plen; i++)
            g->putc(s_prompt[i]);
    } else if (s_msg[0]) {
        g->putc(' ');
        put_lim(s_msg, SCR_COLS - 2);
    } else {
        locate(s_gap, &ln, &col);
        g->printf(" ^O write  ^X quit   %d,%d  %d/%d",
                  ln + 1, col + 1, text_len(), BUF_CAP);
    }
    g->puts("\x1b[K\x1b[0m");
}

static void place_cursor(void)
{
    int ln, col, row, ccol, vis;

    if (s_mode == MODE_NAME) {
        vis = s_plen;
        if (vis > NAME_ROOM)
            vis = NAME_ROOM;
        go(SCR_ROWS, 1 + NAME_PFX + vis);
        return;
    }
    if (s_mode == MODE_ASK) {
        go(SCR_ROWS, ASK_COL);
        return;
    }
    locate(s_gap, &ln, &col);
    row = TEXT_ROW0 + ln - s_top;
    ccol = 1 + col - s_left;
    if (row < TEXT_ROW0)
        row = TEXT_ROW0;
    if (row >= TEXT_ROW0 + TEXT_ROWS)
        row = TEXT_ROW0 + TEXT_ROWS - 1;
    if (ccol < 1)
        ccol = 1;
    if (ccol > SCR_COLS)
        ccol = SCR_COLS;
    go(row, ccol);
}

static void draw(void)
{
    g->puts("\x1b[?25l");
    draw_title();
    draw_text();
    draw_status();
    place_cursor();
    g->puts("\x1b[?25h");
}

static int write_span(int fd, const char *p, int n)
{
    int off = 0;

    while (off < n) {
        int w = g->write(fd, p + off, n - off);

        if (w < 0) {
            set_pair("write: ", err_name(w));
            return -1;
        }
        if (w == 0) {
            set_msg("write failed");
            return -1;
        }
        off += w;
    }
    return 0;
}

static int save_to(const char *path)
{
    int fd;

    fd = g->open(path, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
    if (fd < 0) {
        set_pair("write: ", err_name(fd));
        return -1;
    }
    if (s_gap > 0 && write_span(fd, s_buf, s_gap) != 0) {
        g->close(fd);
        return -1;
    }
    if (s_gap_end < BUF_CAP &&
        write_span(fd, s_buf + s_gap_end, BUF_CAP - s_gap_end) != 0) {
        g->close(fd);
        return -1;
    }
    g->close(fd);
    s_changed = 0;
    set_wrote(text_len());
    return 0;
}

/* 0 loaded (a missing file is a new buffer), 1 does not fit, <0 filesystem. */
static int load_file(void)
{
    int fd;
    unsigned char chunk[64];

    fd = g->open(s_path, FREYA_O_RDONLY);
    if (fd == FS_NOENT) {
        set_msg("new file");
        return 0;
    }
    if (fd < 0)
        return fd;

    for (;;) {
        int r = g->read(fd, chunk, (int)sizeof(chunk));
        int i;

        if (r < 0) {
            g->close(fd);
            return r;
        }
        if (r == 0)
            break;
        for (i = 0; i < r; i++) {
            if (chunk[i] == '\r')
                continue;
            if (s_gap == s_gap_end) {
                g->close(fd);
                return 1;
            }
            s_buf[s_gap++] = (char)chunk[i];
        }
    }
    g->close(fd);
    move_gap(0);
    return 0;
}

static void begin_write(void)
{
    int i = 0;

    while (s_path[i] && i < PATH_MAX - 1) {
        s_prompt[i] = s_path[i];
        i++;
    }
    s_prompt[i] = '\0';
    s_plen = i;
    s_mode = MODE_NAME;
}

static void finish_name(void)
{
    int i;

    if (s_plen == 0) {
        s_mode = MODE_EDIT;
        return;
    }
    s_prompt[s_plen] = '\0';
    if (!path_ok(s_prompt)) {
        s_mode = MODE_EDIT;
        set_msg("bad path");
        return;
    }
    if (save_to(s_prompt) != 0) {
        s_mode = MODE_EDIT;
        return;
    }
    for (i = 0; i <= s_plen; i++)
        s_path[i] = s_prompt[i];
    s_mode = MODE_EDIT;
}

static int prompt_comp(void)
{
    int i = s_plen;

    while (i > 0 && s_prompt[i - 1] != '/')
        i--;
    return s_plen - i;
}

static int read_csi(int *arg, int *final);

static void on_ask(int c)
{
    if (c == 'y' || c == 'Y') {
        s_discard = 1;
        s_quit = 1;
        return;
    }
    if (c == 'n' || c == 'N' || c == 0x18) {
        s_mode = MODE_EDIT;
        return;
    }
    if (c == 0x1B) {
        int arg, final;

        /* Bare Escape is "no".  An arrow sequence must be eaten, or
         * its tail is typed into the buffer once the question is gone. */
        if (read_csi(&arg, &final) != 0)
            s_mode = MODE_EDIT;
    }
}

static void on_name(int c)
{
    int comp;

    if (c == '\r' || c == '\n' || c == 0x0F) {
        finish_name();
        return;
    }
    if (c == 0x18) {
        s_mode = MODE_EDIT;
        return;
    }
    if (c == 0x1B) {
        int arg, final;

        /* A bare Escape cancels.  An arrow sequence is eaten. */
        if (read_csi(&arg, &final) != 0)
            s_mode = MODE_EDIT;
        return;
    }
    if (c == 0x15) {
        s_plen = 0;
        s_prompt[0] = '\0';
        return;
    }
    if (c == 0x08 || c == 0x7F) {
        if (s_plen > 0) {
            s_plen--;
            s_prompt[s_plen] = '\0';
        }
        return;
    }
    if (c < 0x20 || c > 0x7E)
        return;
    if (s_plen >= PATH_MAX - 1)
        return;
    comp = prompt_comp();
    if (c != '/' && comp >= NAME_MAX)
        return;
    if (c == '/' && s_plen > 0 && s_prompt[s_plen - 1] == '/')
        return;
    s_prompt[s_plen++] = (char)c;
    s_prompt[s_plen] = '\0';
}

static int read_csi(int *arg, int *final)
{
    int c, n = 0, acc = 0, froze = 0;

    c = g->getc_timeout(ESC_MS);
    if (c < 0)
        return -1;
    if (c == 'O') {
        c = g->getc_timeout(ESC_MS);
        if (c < 0)
            return -1;
        *arg = 0;
        *final = c;
        return 0;
    }
    if (c != '[')
        return -1;

    for (;;) {
        c = g->getc_timeout(ESC_MS);
        if (c < 0)
            return -1;
        if (!froze && c >= '0' && c <= '9') {
            acc = acc * 10 + (c - '0');
            if (acc > 1000)
                acc = 1000;
        } else if (c == ';') {
            froze = 1;
        } else if (froze && c >= '0' && c <= '9') {
            /* A later parameter is a modifier.  The key is the first. */
        } else if (c >= 0x40 && c <= 0x7E) {
            *arg = acc;
            *final = c;
            return 0;
        } else {
            return -1;
        }
        if (++n > 12)
            return -1;
    }
}

static void on_csi(int arg, int final)
{
    if (final == 'A')
        vmove(-1);
    else if (final == 'B')
        vmove(1);
    else if (final == 'C')
        move_h(1);
    else if (final == 'D')
        move_h(-1);
    else if (final == 'H')
        line_edge(0);
    else if (final == 'F')
        line_edge(1);
    else if (final == '~') {
        if (arg == 1 || arg == 7)
            line_edge(0);
        else if (arg == 3)
            del_fwd();
        else if (arg == 4 || arg == 8)
            line_edge(1);
        else if (arg == 5)
            page(-1);
        else if (arg == 6)
            page(1);
    }
}

static void ask_quit(void)
{
    if (s_changed)
        s_mode = MODE_ASK;
    else
        s_quit = 1;
}

static void on_edit(int c)
{
    if (c == 0x1B) {
        int arg = 0, final = 0;

        if (read_csi(&arg, &final) == 0)
            on_csi(arg, final);
        return;
    }
    if (c == 0x0F) {
        begin_write();
        return;
    }
    if (c == 0x18) {
        ask_quit();
        return;
    }
    if (c == 0x0C) {
        g->puts("\x1b[2J");
        return;
    }
    if (c == 0x01) {
        line_edge(0);
        return;
    }
    if (c == 0x05) {
        line_edge(1);
        return;
    }
    if (c == 0x04) {
        del_fwd();
        return;
    }
    if (c == 0x0B) {
        kill_eol();
        return;
    }
    if (c == 0x15) {
        kill_bol();
        return;
    }
    if (c == 0x08 || c == 0x7F) {
        backspace();
        return;
    }
    if (c == '\r' || c == '\n') {
        if (insert_byte('\n') == 0)
            sync_pref();
        return;
    }
    if (c == '\t' || (c >= 0x20 && c <= 0x7E)) {
        if (insert_byte((char)c) == 0)
            sync_pref();
    }
}

/* Enter is CR, LF, or CR+LF.  A LF sitting next to a CR is part of
 * that Enter.  It is taken before the key is acted on, because writing
 * the file can outlast the pair and that LF must not come back as a
 * second newline.  A lone LF is a newline of its own. */
static void dispatch(int c)
{
    if (c < 0)
        return;
    if (c == '\r') {
        int n = g->getc_timeout(CRLF_MS);

        if (n >= 0 && n != '\n')
            s_pending = n;
        c = '\n';
    }

    if (s_mode == MODE_ASK)
        on_ask(c);
    else if (s_mode == MODE_NAME)
        on_name(c);
    else
        on_edit(c);
}

static int take_key(void)
{
    int c;

    if (s_pending >= 0) {
        c = s_pending;
        s_pending = -1;
        return c;
    }
    return g->getc();
}

static void restore_term(void)
{
    g->puts("\x1b[0m\x1b[?25h\x1b[2J\x1b[H");
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int rc;

    g = api;
    s_gap = 0;
    s_gap_end = BUF_CAP;
    s_pending = -1;

    if (argc > 2) {
        api->puts("edit: usage: edit [file]\r\n");
        return FREYA_EXIT_USAGE;
    }
    if (argc == 2) {
        if (!argv[1] || !path_ok(argv[1])) {
            api->puts("edit: path must be at most 127 characters, "
                      "each name at most 63\r\n");
            return FREYA_EXIT_USAGE;
        }
        set_path(argv[1]);
        rc = load_file();
        if (rc > 0) {
            api->printf("edit: %s does not fit the %d byte buffer\r\n",
                        s_path, BUF_CAP);
            return FREYA_EXIT_FAIL;
        }
        if (rc < 0) {
            api->printf("edit: %s: %s\r\n", s_path, err_name(rc));
            return FREYA_EXIT_FAIL;
        }
    }

    api->puts("\x1b[?25l\x1b[2J\x1b[H");
    reveal();
    draw();

    while (!s_quit && !g->should_stop()) {
        int c = take_key();
        int first = 1;

        if (c < 0)
            break;
        for (;;) {
            if (first) {
                s_msg[0] = '\0';
                first = 0;
            }
            dispatch(c);
            if (s_quit || g->should_stop())
                break;
            if (s_pending < 0 && !g->kbhit())
                break;
            c = take_key();
            if (c < 0)
                break;
        }
        if (s_quit || g->should_stop())
            break;
        reveal();
        draw();
        g->yield();
    }

    restore_term();
    if (s_discard) {
        g->puts("edit: discarded changes\r\n");
    } else if (g->should_stop()) {
        if (s_changed)
            g->puts("edit: changes not written\r\n");
    } else if (s_path[0]) {
        g->printf("edit: %s, %d bytes\r\n", s_path, text_len());
    } else {
        g->printf("edit: (new), %d bytes\r\n", text_len());
    }
    return 0;
}
