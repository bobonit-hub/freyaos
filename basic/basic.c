/* basic.c - a BASIC in the manner of BASIC-11.
 *
 * One translation unit: the arithmetic is included so that a Freya
 * program is one file.  There is no libc; everything below is written
 * against the system calls in bas.h.  The numbers are the C float
 * of fpnat.c, the single precision the FPU of a Cortex-M4F computes.
 *
 * Memory: the host hands bas_main() a heap.  It is cut into the
 * program text, an arena for arrays, and a flat pool for string data.
 * Program lines are stored tokenized:
 *     [len] [line lo] [line hi] tokens... [0]
 * where a keyword is one byte 0x80+index, a numeric constant is 0xff
 * followed by the bytes of the number (parsing one costs more than a
 * hundred multiplications), and everything else is the source text,
 * upper-cased outside quotes.  Line numbers after GOTO and the like
 * stay text.  Variables are a letter and
 * an optional digit, with % for integers and $ for strings, so each of
 * the three kinds has a fixed table of 26*11 slots.
 *
 * Strings live in the pool as [owner] [capacity] data..., where owner
 * is the address of the descriptor {offset, length} that holds them.
 * When the pool is full it is compacted: an entry is alive when its
 * owner still points at it.  Intermediate strings go to a small
 * scratch area that is reset before every statement, so compaction
 * never has to know about them.
 */
#include "bas.h"

#include "fpnat.h"
#include "fpnat.c"

#ifndef BAS_BANNER
#define BAS_BANNER "BASIC-11 for Freya"
#endif

/* ------------------------------------------------------------------ */
/* Small utilities                                                    */

static void mem_copy(void *d, const void *s, uint32_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;

    while (n--) *dp++ = *sp++;
}

static void mem_move(void *d, const void *s, uint32_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;

    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else if (dp > sp) {
        dp += n;
        sp += n;
        while (n--) *--dp = *--sp;
    }
}

static void mem_set(void *d, int c, uint32_t n)
{
    uint8_t *dp = d;

    while (n--) *dp++ = (uint8_t)c;
}

static int is_upper(int c)
{
    return c >= 'A' && c <= 'Z';
}

static int to_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* ------------------------------------------------------------------ */
/* Tokens                                                             */

enum {
    T_LET = 0x80, T_PRINT, T_INPUT, T_LINPUT, T_IF, T_THEN, T_ELSE,
    T_FOR, T_TO, T_STEP, T_NEXT, T_GOTO, T_GOSUB, T_RETURN, T_ON,
    T_DIM, T_READ, T_DATA, T_RESTORE, T_DEF, T_FNEND, T_REM, T_STOP, T_END,
    T_RANDOMIZE, T_SLEEP, T_OPEN, T_CLOSE, T_AS, T_FILE, T_OUTPUT,
    T_DO, T_LOOP, T_UNTIL, T_WHILE, T_TIMER, T_KEY, T_OFF,
    T_RUN, T_RUNNH, T_LIST, T_LISTNH, T_NEW, T_SCR, T_OLD, T_SAVE,
    T_REPLACE, T_UNSAVE, T_BYE, T_CLEAR, T_CONT, T_LENGTH, T_DEL,
    T_AND, T_OR, T_NOT,
    T_ABS, T_ATN, T_COS, T_EXP, T_INT, T_LOG10, T_LOG, T_PI, T_RND,
    T_SGN, T_SIN, T_SQR, T_TAN, T_TIME, T_LEN, T_ASC, T_CHRS, T_POS,
    T_SEGS, T_STRS, T_VAL, T_TRMS, T_LEFTS, T_RIGHTS, T_MIDS,
    T_DATES, T_TIMES, T_INKEYS, T_PIN, T_PWM, T_ADC, T_FN, T_TAB,
    T_LAST,
    T_NUM = 0xff                 /* followed by the bytes of a number */
};

/* The bytes of a number token, after the T_NUM byte: the number as it
 * is in memory, eight bytes of D format or four of a float. */
#define NUMLEN ((int)sizeof(fpac_t))

/* Same order as the enum. */
static const char *const keywords[] = {
    "LET", "PRINT", "INPUT", "LINPUT", "IF", "THEN", "ELSE",
    "FOR", "TO", "STEP", "NEXT", "GOTO", "GOSUB", "RETURN", "ON",
    "DIM", "READ", "DATA", "RESTORE", "DEF", "FNEND", "REM", "STOP", "END",
    "RANDOMIZE", "SLEEP", "OPEN", "CLOSE", "AS", "FILE", "OUTPUT",
    "DO", "LOOP", "UNTIL", "WHILE", "TIMER", "KEY", "OFF",
    "RUN", "RUNNH", "LIST", "LISTNH", "NEW", "SCR", "OLD", "SAVE",
    "REPLACE", "UNSAVE", "BYE", "CLEAR", "CONT", "LENGTH", "DEL",
    "AND", "OR", "NOT",
    "ABS", "ATN", "COS", "EXP", "INT", "LOG10", "LOG", "PI", "RND",
    "SGN", "SIN", "SQR", "TAN", "TIME", "LEN", "ASC", "CHR$", "POS",
    "SEG$", "STR$", "VAL", "TRM$", "LEFT$", "RIGHT$", "MID$",
    "DATE$", "TIME$", "INKEY$", "PIN", "PWM", "ADC", "FN", "TAB",
};

/* ------------------------------------------------------------------ */
/* Errors                                                             */

enum {
    E_SYNTAX = 1, E_LINE, E_NUMBER, E_DIVZERO, E_OVERFLOW, E_SUBSCRIPT,
    E_STRLEN, E_MEMORY, E_NEXT, E_RETURN, E_DATA, E_ARG, E_FILE, E_EOF,
    E_FUNC, E_TYPE, E_NEST, E_REDIM, E_CHANNEL, E_CONT, E_LONGLINE, E_DEF,
    E_CLOCK, E_PIN, E_BUSY, E_IO, E_LOOP, E_DO, E_KEYS, E_KEY,
    E_LAST,
    /* Not an error and not printed: the program ended, or Ctrl-C
     * stopped it, while a DEF ... FNEND body was running, and the C
     * frames of the expression that called it have to go. */
    E_HALT
};

static const char *const messages[] = {
    "",
    "Syntax error", "Undefined line number", "Illegal number",
    "Division by zero", "Overflow", "Subscript out of range",
    "String too long", "Out of memory", "NEXT without FOR",
    "RETURN without GOSUB", "Out of data", "Illegal argument",
    "Bad file", "End of file", "Undefined function", "Type mismatch",
    "Too many nested loops", "Redimensioned array", "Bad channel",
    "Cannot continue", "Line too long", "DEF without FNEND",
    "No clock", "Bad pin", "Pin in use", "Device error",
    "LOOP without DO", "DO without LOOP", "Too many keys", "Undefined key",
};

static jmp_buf err_jb;

static void error(int code)
{
    longjmp(err_jb, code);
}

void fp_fault(int code)
{
    if (code == FP_ERR_DIVZERO) error(E_DIVZERO);
    if (code == FP_ERR_DOMAIN || code == FP_ERR_RANGE) error(E_ARG);
    error(E_OVERFLOW);
}

/* ------------------------------------------------------------------ */
/* Memory layout                                                      */

#define NVARS      (26 * 11)
#define MAXLINE    255           /* characters typed on one line */
#define TMP_BYTES  2048          /* scratch for intermediate strings */
#define MAXSTR     255
#define NFOR       16
#define NDO        16
#define NGOSUB     32
#define NKEY       8             /* ON KEY definitions in one program */
#define DEBOUNCE_MS 20u          /* a key has to hold still this long */
#define NFNDEF     26           /* DEF FNx definitions in one program */
#define NFNARG     4            /* parameters of one */
#define NFN        6            /* function calls inside one another */
#define NCHAN      8             /* channel 0 is the terminal */
#define CHBUF      128
#define WIDTH      72
#define ZONE       14

/* uptr, from bas.h, is an address as an integer: 32 bits on the VM and
 * the board, whatever the host has when the sources are built natively
 * for the tests. */
typedef struct {
    uptr off;                    /* address of the data, 0 when empty */
    uptr len;
} sdesc_t;

#define POOL_HDR (2 * sizeof(uptr))    /* owner, capacity */

typedef struct {
    int32_t nd, d1, d2, pad;     /* dimensions; elements follow */
} arr_t;

typedef struct {
    int str;                     /* 0 number, 1 string */
    fpac_t n;
    const uint8_t *s;
    uint32_t len;
} val_t;

typedef struct {
    int kind;                    /* 0 number, 1 integer, 2 string */
    void *p;                     /* fpac_t * or sdesc_t * */
} lval_t;

typedef struct {
    fpac_t *var;
    fpac_t limit, step;
    const uint8_t *tp;           /* the statement after FOR */
    const uint8_t *line;
} for_t;

/* A GOSUB frame.  src is 0 for a GOSUB the program made, or the event
 * source, 1 + EV_*, whose handler this is; sleep says the RETURN goes
 * back into a SLEEP that the event interrupted. */
typedef struct {
    const uint8_t *tp, *line;
    uint32_t deadline;           /* of that SLEEP */
    uint8_t src, sleep;
} gosub_t;

/* A DO loop: where the body starts, and the LOOP that is its end,
 * which is the loop's identity, as the variable is a FOR's. */
typedef struct {
    const uint8_t *tp, *line;    /* the DO statement */
    const uint8_t *loop, *loopline;
} do_t;

/* An event source: the timer, or a key, which is a pin with a button
 * on it.  A source with a handler and enabled is polled between
 * statements; when it has fired it is pending until its handler can
 * be called, which is not while that handler is running. */
typedef struct {
    uint32_t line;               /* the handler, 0 when there is none */
    uint8_t enabled, pending, busy;
    /* the timer */
    uint32_t period, due;
    /* a key */
    uint8_t pin, active;         /* the level that is a press */
    uint8_t stable, last;        /* what it is, and the newest sample */
    uint32_t since;              /* when the newest sample first read so */
} event_t;

#define EV_TIMER   0
#define EV_KEY     1             /* EV_KEY + k is key k */

/* A DEF, found in the program text, which the definition points into:
 *
 *     DEF FNMAX(A,B) = ...        one statement, multi = 0
 *     DEF FNMAX(A,B)              the statements up to FNEND, multi = 1
 */
typedef struct {
    const uint8_t *name;         /* the characters after FN */
    const uint8_t *hdr;          /* the parameter list, or the '=' */
    const uint8_t *body;         /* the statements, when multi */
    const uint8_t *line;         /* the record the DEF is on */
    const uint8_t *end;          /* just past the FNEND that ends it */
    const uint8_t *endline;      /* the record that one is on */
    uint8_t len;                 /* characters in the name */
    uint8_t str;                 /* the name ends in $ */
    uint8_t strparm;             /* one of the parameters is a string */
    uint8_t multi;
} fndef_t;

/* One call of a DEF ... FNEND in progress.  An assignment to the
 * function's own name inside the body sets val; buf is MAXSTR bytes of
 * the caller's scratch area, held by a function whose name ends in $,
 * because the body's own scratch is emptied before every statement. */
typedef struct {
    const fndef_t *def;
    val_t val;
    uint8_t *buf;
} fncall_t;

typedef struct {
    int fd;                      /* -1 when closed */
    int mode;                    /* 0 input, 1 output */
    int col;
    int pos, len;                /* buffer window */
    int eof;
    uint8_t buf[CHBUF];
} chan_t;

static uint8_t *prog_lo, *prog_hi, *prog_end;
static uint8_t *arena_lo, *arena_hi, *arena_top;
static uint8_t *pool_lo, *pool_hi, *pool_top;
static uint8_t *tmp_base, *tmp_lo, *tmp_hi, *tmp_top;

static fpac_t nvars[NVARS], ivars[NVARS];
static sdesc_t svars[NVARS];
static arr_t *arrays[3][NVARS];

static fndef_t fndefs[NFNDEF];
static int nfndef;               /* definitions in the table */
static int nfnpool;              /* how many of them can allocate a string */
static int defs_valid;           /* the table matches the program text */
static fncall_t fnstk[NFN];
static int fn_depth;

static for_t forstk[NFOR];
static int nfor;
static do_t dostk[NDO];
static int ndo;
static gosub_t gosubstk[NGOSUB];
static int ngosub;
static event_t events[1 + NKEY];  /* the timer, then the keys */
static int nkeys;
static const uint8_t *stmt_tp;   /* the statement being executed */
static int sleep_resume;         /* the next statement is a SLEEP to go on with */
static int handler_done;         /* a handler has just returned */
static uint32_t sleep_deadline;  /* of the SLEEP running, or to go on with */
static chan_t chans[NCHAN];
static int cur_out;              /* channel PRINT writes to */

static const uint8_t *tp;        /* the token being executed */
static const uint8_t *cur_line;  /* its line record, or imm_buf */
static const uint8_t *data_line, *data_tp;
static const uint8_t *cont_tp, *cont_line;
static int running;              /* executing a stored program */
static int jumped;               /* tp now starts another statement */
static int batch;                /* no banner, no prompts, exit at EOF */
static uint32_t rnd_seed = 12345;
static int stmt_count;

static uint8_t imm_buf[MAXLINE + 8];
static char in_buf[MAXLINE + 1];

/* ------------------------------------------------------------------ */
/* Output                                                             */

static void chan_flush(chan_t *c)
{
    if (c->mode == 1 && c->len > 0) {
        sys_write(c->fd, c->buf, c->len);
        c->len = 0;
    }
}

static void out_ch(int ch)
{
    chan_t *c = &chans[cur_out];

    if (cur_out == 0) {
        sys_putc(ch);
    } else {
        if (c->fd < 0 || c->mode != 1) error(E_CHANNEL);
        c->buf[c->len++] = (uint8_t)ch;
        if (c->len == CHBUF) chan_flush(c);
    }
    if (ch == '\n') c->col = 0;
    else if (ch >= ' ') c->col++;
}

static void out_str(const char *s)
{
    while (*s) out_ch(*s++);
}

static void out_mem(const uint8_t *s, uint32_t n)
{
    while (n--) out_ch(*s++);
}

static void out_int(int32_t v)
{
    char buf[12];
    int n = 0;
    uint32_t u;

    if (v < 0) {
        out_ch('-');
        u = (uint32_t)-v;
    } else
        u = (uint32_t)v;
    do {
        buf[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n) out_ch(buf[--n]);
}

static void console(void)
{
    cur_out = 0;
}

/* ------------------------------------------------------------------ */
/* The string pool                                                    */

/* Pool entries and arena blocks hold pointers, so they are aligned to
 * one: 4 bytes on the VM, whatever the host needs natively. */
#define ALIGN ((uint32_t)sizeof(uptr))

static uint32_t round_up(uint32_t n)
{
    return (n + ALIGN - 1) & ~(ALIGN - 1);
}

static uptr rd_ptr(const uint8_t *p)
{
    return *(const uptr *)p;
}

static void wr_ptr(uint8_t *p, uptr v)
{
    *(uptr *)p = v;
}

/* Slide the live entries down over the dead ones. */
static void pool_compact(void)
{
    uint8_t *p = pool_lo, *q = pool_lo;
    uptr owner, cap;
    sdesc_t *d;

    while (p < pool_top) {
        owner = rd_ptr(p);
        cap = rd_ptr(p + sizeof(uptr));
        d = (sdesc_t *)owner;
        if (d->off == (uptr)(p + POOL_HDR)) {
            if (q != p) {
                mem_move(q, p, (uint32_t)(POOL_HDR + cap));
                d->off = (uptr)(q + POOL_HDR);
            }
            q += POOL_HDR + cap;
        }
        p += POOL_HDR + cap;
    }
    pool_top = q;
}

static uint8_t *tmp_alloc(uint32_t n)
{
    uint8_t *p = tmp_top;

    if (n > (uint32_t)(tmp_hi - tmp_top)) error(E_MEMORY);
    tmp_top += n;
    return p;
}

/* Give the descriptor a copy of the bytes. */
static void str_store(sdesc_t *d, const uint8_t *s, uint32_t len)
{
    uint32_t need, cap;
    uint8_t *e;

    if (len > MAXSTR) error(E_STRLEN);
    if (len == 0) {
        d->off = 0;
        d->len = 0;
        return;
    }
    if (d->off) {
        cap = (uint32_t)rd_ptr((uint8_t *)d->off - sizeof(uptr));
        if (cap >= len) {
            mem_move((uint8_t *)d->off, s, len);
            d->len = len;
            return;
        }
    }
    /* The source may live in the pool, which compaction moves. */
    if (s >= pool_lo && s < pool_hi) {
        e = tmp_alloc(len);
        mem_copy(e, s, len);
        s = e;
    }
    need = (uint32_t)POOL_HDR + round_up(len);
    if (need > (uint32_t)(pool_hi - pool_top)) {
        d->off = 0;              /* the old entry is garbage now */
        pool_compact();
        if (need > (uint32_t)(pool_hi - pool_top)) error(E_MEMORY);
    }
    e = pool_top;
    pool_top += need;
    wr_ptr(e, (uptr)d);
    wr_ptr(e + sizeof(uptr), round_up(len));
    mem_copy(e + POOL_HDR, s, len);
    d->off = (uptr)(e + POOL_HDR);
    d->len = len;
}

static void str_val(val_t *v, const uint8_t *s, uint32_t len)
{
    v->str = 1;
    v->s = s;
    v->len = len;
}

static void num_val(val_t *v, const fpac_t *n)
{
    v->str = 0;
    v->n = *n;
}

static void clear_vars(void)
{
    int k, i;

    for (i = 0; i < NVARS; i++) {
        fp_zero(&nvars[i]);
        fp_zero(&ivars[i]);
        svars[i].off = 0;
        svars[i].len = 0;
    }
    for (k = 0; k < 3; k++)
        for (i = 0; i < NVARS; i++) arrays[k][i] = NULL;
    arena_top = arena_lo;
    pool_top = pool_lo;
    nfor = 0;
    ndo = 0;
    ngosub = 0;
    fn_depth = 0;
    data_line = NULL;
}

/* No events: what RUN and NEW start with. */
static void events_reset(void)
{
    mem_set(events, 0, sizeof events);
    nkeys = 0;
    sleep_resume = 0;
    handler_done = 0;
}

/* The handlers are not running any more: after an error, or ^C. */
static void events_unwind(void)
{
    int i;

    for (i = 0; i < 1 + NKEY; i++) events[i].busy = 0;
    sleep_resume = 0;
    handler_done = 0;
}

/* ------------------------------------------------------------------ */
/* Channels                                                           */

static void chan_close(int n)
{
    chan_t *c = &chans[n];

    if (c->fd >= 0) {
        chan_flush(c);
        sys_close(c->fd);
    }
    c->fd = -1;
    c->pos = 0;
    c->len = 0;
    c->eof = 0;
}

static void close_all(void)
{
    int i;

    for (i = 1; i < NCHAN; i++) chan_close(i);
    console();
}

static void chan_open(int n, const char *path, int mode)
{
    chan_t *c = &chans[n];

    chan_close(n);
    c->fd = sys_open(path, mode);
    if (c->fd < 0) error(E_FILE);
    c->mode = mode;
    c->col = 0;
}

static int chan_getc(chan_t *c)
{
    if (c->pos >= c->len) {
        if (c->eof) return -1;
        c->len = sys_read(c->fd, c->buf, CHBUF);
        c->pos = 0;
        if (c->len <= 0) {
            c->len = 0;
            c->eof = 1;
            return -1;
        }
    }
    return c->buf[c->pos++];
}

/* A line without its newline; -1 at end of file. */
static int chan_getline(int n, char *buf, int max)
{
    chan_t *c = &chans[n];
    int len = 0, ch;

    if (c->fd < 0 || c->mode != 0) error(E_CHANNEL);
    ch = chan_getc(c);
    if (ch < 0) return -1;
    while (ch >= 0 && ch != '\n') {
        if (ch != '\r' && len < max - 1) buf[len++] = (char)ch;
        ch = chan_getc(c);
    }
    buf[len] = 0;
    return len;
}

/* Read a line from channel n; on channel 0 the terminal. */
static int read_line(int n, char *buf, int max)
{
    if (n == 0) return sys_readline(buf, max);
    return chan_getline(n, buf, max);
}

/* ------------------------------------------------------------------ */
/* Program storage                                                    */

static uint32_t line_no(const uint8_t *rec)
{
    return rec[1] | ((uint32_t)rec[2] << 8);
}

static const uint8_t *next_rec(const uint8_t *rec)
{
    return rec + rec[0];
}

/* The first record whose number is >= n. */
static uint8_t *find_line(uint32_t n)
{
    uint8_t *p = prog_lo;

    while (p < prog_end && line_no(p) < n) p += p[0];
    return p;
}

static void store_line(uint32_t n, const uint8_t *tok, uint32_t len)
{
    uint8_t *p = find_line(n);
    uint32_t old = 0, need;

    defs_valid = 0;
    if (p < prog_end && line_no(p) == n) old = p[0];
    need = len ? len + 4 : 0;
    if (need > old && need - old > (uint32_t)(prog_hi - prog_end)) error(E_MEMORY);
    if (need != old) {
        mem_move(p + need, p + old, (uint32_t)(prog_end - (p + old)));
        /* not prog_end += need - old: that difference is unsigned, and
         * a pointer of more than 32 bits does not wrap with it */
        if (need > old) prog_end += need - old;
        else prog_end -= old - need;
    }
    if (need) {
        p[0] = (uint8_t)need;
        p[1] = (uint8_t)n;
        p[2] = (uint8_t)(n >> 8);
        mem_copy(p + 3, tok, len);
        p[3 + len] = 0;
    }
}

static void new_program(void)
{
    prog_end = prog_lo;
    defs_valid = 0;
    clear_vars();
    events_reset();
    cont_line = NULL;
}

/* ------------------------------------------------------------------ */
/* Tokenizing                                                         */

/* The longest keyword starting at s, or 0. */
static int match_keyword(const char *s, int *len)
{
    int i, j, best = 0, bestlen = 0;
    const char *k;

    for (i = 0; i < T_LAST - 0x80; i++) {
        k = keywords[i];
        for (j = 0; k[j] && to_upper(s[j]) == k[j]; j++)
            ;
        if (!k[j] && j > bestlen) {
            best = 0x80 + i;
            bestlen = j;
        }
    }
    *len = bestlen;
    return best;
}

/* Copy the rest of a line as it is, but keep it 7-bit so that the
 * scanners never mistake a byte for a token. */
static int copy_raw(const char *s, uint8_t *d, int n)
{
    while (*s) {
        if (n >= MAXLINE - 1) error(E_LONGLINE);
        d[n++] = (uint8_t)(*s & 0x80 ? '?' : *s);
        s++;
    }
    return n;
}

/* Tokenize one typed line.  Returns the token length; *lineno is the
 * line number, or -1 when there was none. */
static int is_line_kw(int t)
{
    return t == T_GOTO || t == T_GOSUB || t == T_THEN || t == T_ELSE ||
           t == T_LIST || t == T_LISTNH || t == T_DEL || t == T_RUN ||
           t == T_RUNNH;
}

/* A number in a token is its bytes as they are in memory; a token is
 * not aligned, so they are copied a byte at a time. */
static void put_num(uint8_t *d, const fpac_t *n)
{
    mem_copy(d, n, (uint32_t)NUMLEN);
}

static void get_num(const uint8_t *d, fpac_t *n)
{
    mem_copy(n, d, (uint32_t)NUMLEN);
}

static int tokenize(const char *s, uint8_t *d, int32_t *lineno)
{
    int n = 0, len, t, linectx = 0;
    int32_t ln = -1;
    fpac_t num;

    while (*s == ' ' || *s == '\t') s++;
    if (is_digit(*s)) {
        ln = 0;
        while (is_digit(*s)) {
            ln = ln * 10 + (*s++ - '0');
            if (ln > 65535) error(E_LINE);
        }
        while (*s == ' ') s++;
    }
    *lineno = ln;
    while (*s) {
        if (n >= MAXLINE - 1) error(E_LONGLINE);
        if (*s == '"') {
            d[n++] = (uint8_t)*s++;
            while (*s && *s != '"') {
                d[n++] = (uint8_t)(*s & 0x80 ? '?' : *s);
                s++;
                if (n >= MAXLINE - 1) error(E_LONGLINE);
            }
            if (*s) d[n++] = (uint8_t)*s++;
            continue;
        }
        if (*s == '!') {
            d[n++] = T_REM;
            return copy_raw(s + 1, d, n);
        }
        if (is_digit(*s) || (*s == '.' && is_digit(s[1]))) {
            if (linectx) {
                /* a line number: keep the digits */
                while (is_digit(*s)) d[n++] = (uint8_t)*s++;
                continue;
            }
            len = fp_parse(s, &num);
            if (len == 0) error(E_NUMBER);
            if (n + 1 + NUMLEN >= MAXLINE - 1) error(E_LONGLINE);
            d[n++] = T_NUM;
            put_num(d + n, &num);
            n += NUMLEN;
            s += len;
            continue;
        }
        if (is_upper(to_upper(*s))) {
            t = match_keyword(s, &len);
            if (t) {
                d[n++] = (uint8_t)t;
                s += len;
                if (t == T_REM || t == T_DATA)
                    return copy_raw(s, d, n);
                if (t == T_FN) {
                    /* The name of a function is text, and not scanned
                     * for keywords: FNTOTAL is a name, not FN and TO.
                     * It ends at the first character that cannot be
                     * part of one, so IF FNA THEN still sees THEN. */
                    while (*s == ' ') {
                        if (n >= MAXLINE - 1) error(E_LONGLINE);
                        d[n++] = (uint8_t)*s++;
                    }
                    while (is_digit(*s) || is_upper(to_upper(*s))) {
                        if (n >= MAXLINE - 1) error(E_LONGLINE);
                        d[n++] = (uint8_t)to_upper(*s++);
                    }
                    if (*s == '$') {
                        if (n >= MAXLINE - 1) error(E_LONGLINE);
                        d[n++] = (uint8_t)*s++;
                    }
                }
                linectx = is_line_kw(t);
                continue;
            }
            linectx = 0;
            d[n++] = (uint8_t)to_upper(*s++);
            continue;
        }
        if (*s != ' ' && *s != ',' && *s != '-') linectx = 0;
        if (*s == '\t') {
            d[n++] = ' ';
            s++;
            continue;
        }
        d[n++] = (uint8_t)(*s & 0x80 ? '?' : *s);
        s++;
    }
    return n;
}

/* Print a line record as source. */
static void list_line(const uint8_t *rec)
{
    const uint8_t *p = rec + 3;
    int quoted = 0, raw = 0, len;
    fpac_t num;
    char buf[32];

    out_int((int32_t)line_no(rec));
    out_ch(' ');
    while (*p) {
        if (raw || quoted) {
            out_ch(*p);
            if (*p == '"') quoted = 0;
        } else if (*p == T_NUM) {
            get_num(p + 1, &num);
            len = fp_format(&num, buf);
            out_mem((const uint8_t *)buf, (uint32_t)len);
            p += NUMLEN;
        } else if (*p >= 0x80) {
            out_str(keywords[*p - 0x80]);
            if (*p == T_REM || *p == T_DATA) raw = 1;
        } else {
            out_ch(*p);
            if (*p == '"') quoted = 1;
        }
        p++;
    }
    out_ch('\n');
}

/* ------------------------------------------------------------------ */
/* Scanning helpers                                                   */

static void skip_sp(void)
{
    while (*tp == ' ') tp++;
}

static int peek(void)
{
    skip_sp();
    return *tp;
}

static int accept(int c)
{
    if (peek() == c) {
        tp++;
        return 1;
    }
    return 0;
}

static void expect(int c)
{
    if (!accept(c)) error(E_SYNTAX);
}

static int at_end(void)
{
    int c = peek();

    return c == 0 || c == '\\' || c == ':' || c == T_ELSE || c == T_REM;
}

/* Skip the rest of the statement, honouring quotes.  A comment ends
 * the line, whatever is in it. */
static void skip_stmt(void)
{
    while (*tp && *tp != '\\' && *tp != ':' && *tp != T_ELSE) {
        if (*tp == '"') {
            tp++;
            while (*tp && *tp != '"') tp++;
            if (!*tp) return;
        } else if (*tp == T_NUM)
            tp += NUMLEN;
        else if (*tp == T_REM) {
            while (*tp) tp++;
            return;
        }
        tp++;
    }
}

static void skip_line(void)
{
    while (*tp) tp++;
}

static uint32_t parse_lineno(void)
{
    uint32_t n = 0;

    if (!is_digit(peek())) error(E_SYNTAX);
    while (is_digit(*tp)) {
        n = n * 10 + (*tp++ - '0');
        if (n > 65535) error(E_LINE);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Variables                                                          */

static void *arena_alloc(uint32_t n)
{
    uint8_t *p = arena_top;

    n = round_up(n);
    if (n > (uint32_t)(arena_hi - arena_top)) error(E_MEMORY);
    arena_top += n;
    mem_set(p, 0, n);
    return p;
}

static arr_t *make_array(int kind, int idx, int32_t d1, int32_t d2)
{
    arr_t *a;
    uint32_t count;

    if (arrays[kind][idx]) error(E_REDIM);
    if (d1 < 0 || d2 < -1 || d1 > 32767 || d2 > 32767) error(E_SUBSCRIPT);
    count = (uint32_t)(d1 + 1) * (d2 >= 0 ? (uint32_t)(d2 + 1) : 1u);
    if (count > 1000000) error(E_MEMORY);
    a = arena_alloc(sizeof *a + count * (kind == 2 ? sizeof(sdesc_t) : sizeof(fpac_t)));
    a->nd = d2 >= 0 ? 2 : 1;
    a->d1 = d1;
    a->d2 = d2;
    arrays[kind][idx] = a;
    return a;
}

static void eval(val_t *v);

static int32_t eval_int(void)
{
    val_t v;

    eval(&v);
    if (v.str) error(E_TYPE);
    return fp_to_int(&v.n);
}

static void eval_num(fpac_t *n)
{
    val_t v;

    eval(&v);
    if (v.str) error(E_TYPE);
    *n = v.n;
}

static void eval_str(val_t *v)
{
    eval(v);
    if (!v->str) error(E_TYPE);
}

/* A variable name at tp: letter, optional digit, optional % or $.
 * Returns 0 if there is none. */
static int parse_name(int *idx, int *kind)
{
    int c = peek();

    if (!is_upper(c)) return 0;
    *idx = (c - 'A') * 11;
    tp++;
    if (is_digit(*tp)) *idx += 1 + (*tp++ - '0');
    *kind = 0;
    if (*tp == '%') {
        *kind = 1;
        tp++;
    } else if (*tp == '$') {
        *kind = 2;
        tp++;
    }
    return 1;
}

/* The variable or array element at tp. */
static void parse_lval(lval_t *lv)
{
    int idx, kind;
    int32_t i, j = -1;
    arr_t *a;
    uint32_t e;

    if (!parse_name(&idx, &kind)) error(E_SYNTAX);
    lv->kind = kind;
    if (!accept('(')) {
        if (kind == 0) lv->p = &nvars[idx];
        else if (kind == 1) lv->p = &ivars[idx];
        else lv->p = &svars[idx];
        return;
    }
    i = eval_int();
    if (accept(',')) j = eval_int();
    expect(')');
    a = arrays[kind][idx];
    if (!a) a = make_array(kind, idx, 10, j >= 0 ? 10 : -1);
    if ((a->nd == 2) != (j >= 0)) error(E_SUBSCRIPT);
    if (i < 0 || i > a->d1 || j > a->d2) error(E_SUBSCRIPT);
    e = (uint32_t)i;
    if (a->nd == 2) e = e * (uint32_t)(a->d2 + 1) + (uint32_t)j;
    lv->p = (uint8_t *)(a + 1) + e * (kind == 2 ? sizeof(sdesc_t) : sizeof(fpac_t));
}

static void assign(lval_t *lv, val_t *v)
{
    if (lv->kind == 2) {
        if (!v->str) error(E_TYPE);
        str_store(lv->p, v->s, v->len);
    } else {
        if (v->str) error(E_TYPE);
        if (lv->kind == 1) fp_trunc(&v->n);
        *(fpac_t *)lv->p = v->n;
    }
}

static void load_var(lval_t *lv, val_t *v)
{
    sdesc_t *d;

    if (lv->kind == 2) {
        d = lv->p;
        str_val(v, (const uint8_t *)d->off, d->len);
    } else
        num_val(v, lv->p);
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */

/* Keep a string value that has to outlive a nested evaluation.  An
 * expression allocates only from the scratch area, with two
 * exceptions: binding a string parameter of a DEF, and the statements
 * of a DEF ... FNEND body.  Either can compact the pool, which a
 * value still held in the expression around the call would not
 * survive; the scratch area is never compacted.  nfnpool is 0 for a
 * program with no such definition, and then nothing is copied. */
static void pin_str(val_t *v)
{
    uint8_t *p;

    if (!nfnpool || !v->str || v->len == 0) return;
    if (v->s < pool_lo || v->s >= pool_hi) return;
    p = tmp_alloc(v->len);
    mem_copy(p, v->s, v->len);
    v->s = p;
}

static void fp_from_bool(fpac_t *d, int b)
{
    fp_from_int(d, b ? -1 : 0);
}

static int str_cmp(const val_t *a, const val_t *b)
{
    uint32_t n = a->len < b->len ? a->len : b->len, i;

    for (i = 0; i < n; i++)
        if (a->s[i] != b->s[i]) return a->s[i] < b->s[i] ? -1 : 1;
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

static int32_t to_int(const val_t *v)
{
    if (v->str) error(E_TYPE);
    return fp_to_int(&v->n);
}

/* A string of len bytes in the scratch area. */
static uint8_t *tmp_str(val_t *v, uint32_t len)
{
    uint8_t *p;

    if (len > MAXSTR) error(E_STRLEN);
    p = tmp_alloc(len);
    str_val(v, p, len);
    return p;
}

static void parse_number_at(val_t *v)
{
    int n;

    v->str = 0;
    if (*tp == T_NUM) {
        get_num(tp + 1, &v->n);
        tp += 1 + NUMLEN;
        return;
    }
    n = fp_parse((const char *)tp, &v->n);
    if (n == 0) error(E_NUMBER);
    tp += n;
}

static void fn_call(val_t *v);

static void sub_string(val_t *v, const val_t *s, int32_t from, int32_t to)
{
    /* characters from..to, 1-based inclusive, clipped */
    if (from < 1) from = 1;
    if (to > (int32_t)s->len) to = (int32_t)s->len;
    if (to < from) {
        str_val(v, s->s, 0);
        return;
    }
    str_val(v, s->s + from - 1, (uint32_t)(to - from + 1));
}

/* The last n digits of v, zero filled and with no terminator. */
static void put_digits(char *d, int v, int n)
{
    while (n-- > 0) {
        d[n] = (char)('0' + v % 10);
        v /= 10;
    }
}

/* ------------------------------------------------------------------ */
/* Pins: PIN, PWM and ADC, the shell's pin, pwm and adc as functions   */

/* The string is the word, in either case; the word is upper case. */
static int str_is(const val_t *v, const char *w)
{
    uint32_t i;

    for (i = 0; i < v->len && w[i]; i++)
        if (to_upper(v->s[i]) != w[i]) return 0;
    return i == v->len && !w[i];
}

/* "PB0", "pb0" and "B0" are the same pin, as they are at the shell's
 * prompt.  Only the shape is checked here; the host says whether the
 * board has that pin and whether a program may have it. */
static int pin_name(const val_t *v)
{
    const uint8_t *s = v->s;
    uint32_t n = v->len;
    int port, num = 0;

    if (n && to_upper(*s) == 'P') {
        s++;
        n--;
    }
    if (n < 2 || n > 3) error(E_PIN);
    port = to_upper(*s++) - 'A';
    if (port < 0 || port > 15) error(E_PIN);
    while (--n) {
        if (!is_digit(*s)) error(E_PIN);
        num = num * 10 + (*s++ - '0');
    }
    if (num > 15) error(E_PIN);
    return port * 16 + num;
}

/* What a pin call answered: the value, or the error it stands for. */
static int pin_check(int rc)
{
    if (rc >= 0) return rc;
    if (rc == SYS_EPIN) error(E_PIN);
    if (rc == SYS_EBUSY) error(E_BUSY);
    if (rc == SYS_EARG) error(E_ARG);
    error(E_IO);
    return 0;
}

/* The modes of the shell's pin command, in the order of SYS_PIN_*. */
static const char *const pin_modes[] = {
    "IN", "UP", "DOWN", "OUT", "OD", "ANALOG"
};

/* What is done to a pin after its mode: a level, or "TOGGLE". */
static void pin_action(const val_t *a, int *op, int *level)
{
    if (a->str) {
        if (!str_is(a, "TOGGLE")) error(E_ARG);
        *op = 2;
    } else {
        *op = 1;
        *level = !fp_iszero(&a->n);
    }
}

/* PIN(P$) reads a pin.  PIN(P$, M$) sets its mode, PIN(P$, L) makes
 * it a push-pull output at level L, PIN(P$, "TOGGLE") flips it, and
 * PIN(P$, M$, L) does both.  Either way the value is what the pin
 * reads afterwards, which is what it really is. */
static void fn_pin(val_t *v)
{
    val_t a, act;
    fpac_t n;
    int pin, mode = -1, op = 0, level = 0, i;

    eval_str(&a);
    pin = pin_name(&a);
    if (accept(',')) {
        eval(&act);
        if (act.str)
            for (i = 0; i < (int)(sizeof pin_modes / sizeof pin_modes[0]); i++)
                if (str_is(&act, pin_modes[i])) mode = i;
        if (mode >= 0) {
            if (accept(',')) {
                eval(&act);
                pin_action(&act, &op, &level);
            }
        } else
            pin_action(&act, &op, &level);
    }
    expect(')');
    if (mode >= 0) pin_check(sys_pin_mode(pin, mode));
    else if (op) pin_check(sys_pin_mode(pin, SYS_PIN_OUT));
    if (op == 1) pin_check(sys_pin_write(pin, level));
    else if (op == 2) pin_check(sys_pin_toggle(pin));
    fp_from_int(&n, pin_check(sys_pin_read(pin)));
    num_val(v, &n);
}

/* PWM(P$, HZ, D) starts the channel on the pin at HZ hertz with a duty
 * cycle of D percent, which may be fractional, and is HZ; PWM(P$) stops
 * it and is 0.  Nothing else stops a channel: it runs on after END,
 * until PWM(P$) or the end of the interpreter itself. */
static void fn_pwm(val_t *v)
{
    val_t a;
    fpac_t n, m;
    int32_t hz = 0;
    uint32_t duty = 0;
    int pin;

    eval_str(&a);
    pin = pin_name(&a);
    if (accept(',')) {
        hz = eval_int();
        expect(',');
        eval_num(&n);
        if (hz <= 0 || fp_isneg(&n)) error(E_ARG);
        /* percent to ten-thousandths, to the nearest */
        fp_from_int(&m, 100);
        fp_mul(&n, &m);
        fp_from_int(&m, 1);
        fp_ldexp(&m, -1);
        fp_add(&n, &m);
        fp_from_int(&m, (int32_t)SYS_PWM_FULL + 1);
        if (fp_cmp(&n, &m) >= 0) error(E_ARG);
        duty = (uint32_t)fp_to_int(&n);
    }
    expect(')');
    pin_check(sys_pwm(pin, (uint32_t)hz, duty));
    fp_from_int(&n, hz);
    num_val(v, &n);
}

/* ADC(S$): one raw 12-bit conversion, 0 to 4095, from a pin, or from
 * "TEMP" or "VREF", the chip's own sources. */
static void fn_adc(val_t *v)
{
    val_t a;
    fpac_t n;
    int source;

    eval_str(&a);
    if (str_is(&a, "TEMP")) source = SYS_ADC_TEMP;
    else if (str_is(&a, "VREF")) source = SYS_ADC_VREF;
    else source = pin_name(&a);
    expect(')');
    fp_from_int(&n, pin_check(sys_adc(source)));
    num_val(v, &n);
}

static void function(int t, val_t *v)
{
    val_t a, b;
    fpac_t n, m;
    int32_t i, j;
    uint8_t *p;
    char buf[32];

    tp++;
    if (t == T_PI) {
        num_val(v, &fp_pi);
        return;
    }
    if (t == T_TIME) {
        /* Milliseconds since the board came up.  A BASIC number holds
         * whole milliseconds exactly for the first 2^24 of them, four
         * hours and three quarters; past that the count is still right
         * but it steps in 2 ms, then 4, and so on. */
        fp_from_uint(&n, sys_ticks());
        num_val(v, &n);
        return;
    }
    if (t == T_INKEYS) {
        /* the key typed since the last one was taken, or "" */
        i = sys_inkey();
        if (i < 0) {
            str_val(v, (const uint8_t *)"", 0);
            return;
        }
        p = tmp_str(v, 1);
        p[0] = (uint8_t)i;
        return;
    }
    if (t == T_DATES || t == T_TIMES) {
        int f[6];

        if (sys_clock(f) != 0) error(E_CLOCK);
        if (t == T_DATES) {
            put_digits(buf, f[0], 4);
            buf[4] = '-';
            put_digits(buf + 5, f[1], 2);
            buf[7] = '-';
            put_digits(buf + 8, f[2], 2);
        } else {
            put_digits(buf, f[3], 2);
            buf[2] = ':';
            put_digits(buf + 3, f[4], 2);
            buf[5] = ':';
            put_digits(buf + 6, f[5], 2);
        }
        i = t == T_DATES ? 10 : 8;
        p = tmp_str(v, (uint32_t)i);
        mem_copy(p, buf, (uint32_t)i);
        return;
    }
    if (t == T_RND) {
        /* RND or RND(x); the argument is ignored */
        if (accept('(')) {
            eval(&a);
            expect(')');
        }
        rnd_seed = rnd_seed * 1664525u + 1013904223u;
        fp_from_int(&n, (int32_t)(rnd_seed >> 8));
        fp_ldexp(&n, -24);
        num_val(v, &n);
        return;
    }
    expect('(');
    switch (t) {
    case T_PIN: fn_pin(v); return;
    case T_PWM: fn_pwm(v); return;
    case T_ADC: fn_adc(v); return;
    case T_LEN:
        eval_str(&a);
        expect(')');
        fp_from_int(&n, (int32_t)a.len);
        num_val(v, &n);
        return;
    case T_ASC:
        eval_str(&a);
        expect(')');
        fp_from_int(&n, a.len ? a.s[0] : 0);
        num_val(v, &n);
        return;
    case T_CHRS:
        i = eval_int();
        expect(')');
        p = tmp_str(v, 1);
        p[0] = (uint8_t)i;
        return;
    case T_POS:
        eval_str(&a);
        expect(',');
        pin_str(&a);
        eval_str(&b);
        expect(',');
        pin_str(&a);
        pin_str(&b);
        i = eval_int();
        expect(')');
        if (i < 1) i = 1;
        j = 0;
        for (; (uint32_t)i + b.len <= a.len + 1; i++) {
            uint32_t k;
            for (k = 0; k < b.len && a.s[i - 1 + k] == b.s[k]; k++)
                ;
            if (k == b.len) {
                j = i;
                break;
            }
        }
        fp_from_int(&n, j);
        num_val(v, &n);
        return;
    case T_SEGS:
        eval_str(&a);
        expect(',');
        pin_str(&a);
        i = eval_int();
        expect(',');
        j = eval_int();
        expect(')');
        sub_string(v, &a, i, j);
        return;
    case T_LEFTS:
        eval_str(&a);
        expect(',');
        pin_str(&a);
        i = eval_int();
        expect(')');
        sub_string(v, &a, 1, i);
        return;
    case T_RIGHTS:
        /* as in BASIC-11: from character i to the end */
        eval_str(&a);
        expect(',');
        pin_str(&a);
        i = eval_int();
        expect(')');
        sub_string(v, &a, i, (int32_t)a.len);
        return;
    case T_MIDS:
        eval_str(&a);
        expect(',');
        pin_str(&a);
        i = eval_int();
        expect(',');
        j = eval_int();
        expect(')');
        if (j < 0) j = 0;
        sub_string(v, &a, i, i + j - 1);
        return;
    case T_STRS:
        eval_num(&n);
        expect(')');
        i = fp_format(&n, buf);
        p = tmp_str(v, (uint32_t)i);
        mem_copy(p, buf, (uint32_t)i);
        return;
    case T_VAL:
        eval_str(&a);
        expect(')');
        p = tmp_alloc(a.len + 1);
        mem_copy(p, a.s, a.len);
        p[a.len] = 0;
        i = 0;
        while (p[i] == ' ') i++;
        j = 0;
        if (p[i] == '-' || p[i] == '+') j = p[i++] == '-';
        if (fp_parse((const char *)p + i, &n) == 0) fp_zero(&n);
        if (j) fp_neg(&n);
        num_val(v, &n);
        return;
    case T_TRMS:
        eval_str(&a);
        expect(')');
        while (a.len && a.s[a.len - 1] == ' ') a.len--;
        *v = a;
        return;
    default:
        break;
    }
    /* the numeric functions of one argument */
    eval_num(&n);
    expect(')');
    switch (t) {
    case T_ABS: fp_abs(&n); break;
    case T_ATN: fp_atan(&n); break;
    case T_COS: fp_cos(&n); break;
    case T_EXP: fp_exp(&n); break;
    case T_INT: fp_floor(&n); break;
    case T_LOG: fp_log(&n); break;
    case T_LOG10:
        fp_log(&n);
        fp_from_int(&m, 10);
        fp_log(&m);
        fp_div(&n, &m);
        break;
    case T_SGN: fp_from_int(&n, fp_iszero(&n) ? 0 : fp_isneg(&n) ? -1 : 1); break;
    case T_SIN: fp_sin(&n); break;
    case T_SQR: fp_sqrt(&n); break;
    case T_TAN:
        m = n;
        fp_sin(&n);
        fp_cos(&m);
        fp_div(&n, &m);
        break;
    default:
        error(E_SYNTAX);
    }
    num_val(v, &n);
}

static void primary(val_t *v)
{
    int c = peek();
    lval_t lv;
    const uint8_t *s;

    if (c == T_NUM || is_digit(c) || c == '.') {
        parse_number_at(v);
        return;
    }
    if (c == '"') {
        s = ++tp;
        while (*tp && *tp != '"') tp++;
        str_val(v, s, (uint32_t)(tp - s));
        if (*tp) tp++;
        return;
    }
    if (c == '(') {
        tp++;
        eval(v);
        expect(')');
        return;
    }
    if (c == T_FN) {
        fn_call(v);
        return;
    }
    if ((c >= T_ABS && c <= T_TIME) || (c >= T_LEN && c <= T_ADC)) {
        function(c, v);
        return;
    }
    if (is_upper(c)) {
        parse_lval(&lv);
        load_var(&lv, v);
        return;
    }
    error(E_SYNTAX);
}

static void unary(val_t *v);

static void power(val_t *v)
{
    val_t r;

    primary(v);
    if (peek() == '^') {
        tp++;
        if (v->str) error(E_TYPE);
        unary(&r);
        if (r.str) error(E_TYPE);
        fp_pow(&v->n, &r.n);
    }
}

static void unary(val_t *v)
{
    int c = peek();

    if (c == '-' || c == '+') {
        tp++;
        unary(v);
        if (v->str) error(E_TYPE);
        if (c == '-') fp_neg(&v->n);
        return;
    }
    power(v);
}

static void term(val_t *v)
{
    val_t r;
    int c;

    unary(v);
    for (;;) {
        c = peek();
        if (c != '*' && c != '/') return;
        tp++;
        if (v->str) error(E_TYPE);
        unary(&r);
        if (r.str) error(E_TYPE);
        if (c == '*') fp_mul(&v->n, &r.n);
        else fp_div(&v->n, &r.n);
    }
}

static void sum(val_t *v)
{
    val_t r;
    int c;
    uint8_t *p;

    term(v);
    for (;;) {
        c = peek();
        if (c != '+' && c != '-') return;
        tp++;
        pin_str(v);
        term(&r);
        if (v->str != r.str) error(E_TYPE);
        if (v->str) {
            if (c != '+') error(E_TYPE);
            p = tmp_alloc(v->len + r.len);
            if (v->len + r.len > MAXSTR) error(E_STRLEN);
            mem_copy(p, v->s, v->len);
            mem_copy(p + v->len, r.s, r.len);
            v->s = p;
            v->len += r.len;
        } else if (c == '+')
            fp_add(&v->n, &r.n);
        else
            fp_sub(&v->n, &r.n);
    }
}

static void relation(val_t *v)
{
    val_t r;
    int c, op, res, t;

    sum(v);
    c = peek();
    if (c != '=' && c != '<' && c != '>') return;
    tp++;
    op = c;
    if (c == '<' && *tp == '>') { op = 'n'; tp++; }
    else if (c == '<' && *tp == '=') { op = 'l'; tp++; }
    else if (c == '>' && *tp == '=') { op = 'g'; tp++; }
    else if (c == '=' && *tp == '=') tp++;
    else if (c == '=' && *tp == '<') { op = 'l'; tp++; }
    else if (c == '=' && *tp == '>') { op = 'g'; tp++; }
    else if (c == '>' && *tp == '<') { op = 'n'; tp++; }
    pin_str(v);
    sum(&r);
    if (v->str != r.str) error(E_TYPE);
    t = v->str ? str_cmp(v, &r) : fp_cmp(&v->n, &r.n);
    switch (op) {
    case '=': res = t == 0; break;
    case 'n': res = t != 0; break;
    case '<': res = t < 0; break;
    case '>': res = t > 0; break;
    case 'l': res = t <= 0; break;
    default: res = t >= 0; break;
    }
    fp_from_bool(&v->n, res);
    v->str = 0;
}

static void negation(val_t *v)
{
    if (peek() == T_NOT) {
        tp++;
        negation(v);
        fp_from_int(&v->n, ~to_int(v));
        return;
    }
    relation(v);
}

static void conjunction(val_t *v)
{
    val_t r;

    negation(v);
    while (peek() == T_AND) {
        tp++;
        negation(&r);
        fp_from_int(&v->n, to_int(v) & to_int(&r));
    }
}

static void eval(val_t *v)
{
    val_t r;

    conjunction(v);
    while (peek() == T_OR) {
        tp++;
        conjunction(&r);
        fp_from_int(&v->n, to_int(v) | to_int(&r));
    }
}

/* ------------------------------------------------------------------ */
/* User functions: DEF FNx(p, ...) = expr, and DEF FNx(p, ...) ... FNEND */

/* run_body() is the statement loop of a DEF ... FNEND; it is below the
 * statements it runs. */
static void run_body(const fndef_t *d);

static int is_name_ch(int c)
{
    return is_upper(c) || is_digit(c);
}

/* The name after an FN token, which the tokenizer left as text. */
static int fn_name(const uint8_t **name)
{
    const uint8_t *p;

    skip_sp();
    p = tp;
    while (is_name_ch(*tp)) tp++;
    if (*tp == '$') tp++;
    *name = p;
    return (int)(tp - p);
}

/* p is just after a DEF token.  Reads the header into d, all but the
 * record it is on; returns 0 when it is not one. */
static int def_header(const uint8_t *p, fndef_t *d)
{
    int depth;

    while (*p == ' ') p++;
    if (*p != T_FN) return 0;
    for (p++; *p == ' '; p++)
        ;
    d->name = p;
    while (is_name_ch(*p)) p++;
    if (*p == '$') p++;
    d->len = (uint8_t)(p - d->name);
    if (d->len == 0) return 0;
    d->str = p[-1] == '$';
    d->strparm = 0;
    d->hdr = p;
    while (*p == ' ') p++;
    if (*p == '(') {
        for (depth = 1, p++; *p && depth; p++) {
            if (*p == T_NUM) p += NUMLEN;
            else if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == '$') d->strparm = 1;
        }
        if (depth) return 0;
        while (*p == ' ') p++;
    }
    /* what follows the header tells the two forms apart */
    d->multi = *p != '=';
    d->body = p;
    d->end = NULL;               /* scan_defs() looks for the FNEND */
    d->endline = NULL;
    return 1;
}

/* Find every DEF in the program.  The definitions point into the
 * program text, so the table is thrown away whenever a line is
 * entered or deleted. */
static void scan_defs(void)
{
    const uint8_t *rec, *p;
    fndef_t *open = NULL;        /* the DEF ... FNEND being scanned */
    fndef_t d;

    nfndef = 0;
    nfnpool = 0;
    for (rec = prog_lo; rec < prog_end; rec = next_rec(rec)) {
        int cond = 0;            /* a THEN or an ELSE has opened a clause */
        for (p = rec + 3; *p; p++) {
            if (*p == '"') {
                for (p++; *p && *p != '"'; p++)
                    ;
                if (!*p) break;
            } else if (*p == T_NUM)
                p += NUMLEN;
            else if (*p == T_REM || *p == T_DATA)
                break;
            else if (*p == T_THEN || *p == T_ELSE)
                cond = 1;
            else if (*p == T_FNEND && open && !cond) {
                /* A body leaves early by reaching an FNEND of its own
                 * in the THEN or the ELSE part of an IF, so the block
                 * ends at the first one that is not conditional.  Any
                 * after that belong to no DEF and are a syntax error
                 * when the program reaches them. */
                open->end = p + 1;
                open->endline = rec;
                open = NULL;
            } else if (*p == T_DEF && def_header(p + 1, &d)) {
                if (nfndef == NFNDEF) error(E_MEMORY);
                d.line = rec;
                fndefs[nfndef++] = d;
                if (d.multi || d.strparm) nfnpool++;
                open = d.multi ? &fndefs[nfndef - 1] : NULL;
                p = d.body - 1;      /* the loop steps on to the body */
            }
        }
    }
    defs_valid = 1;              /* only a whole table is worth keeping */
}

static void ensure_defs(void)
{
    if (!defs_valid) scan_defs();
}

static const fndef_t *find_def(const uint8_t *name, int len)
{
    int i, j;

    ensure_defs();
    for (i = 0; i < nfndef; i++) {
        if (fndefs[i].len != len) continue;
        for (j = 0; j < len && fndefs[i].name[j] == name[j]; j++)
            ;
        if (j == len) return &fndefs[i];
    }
    return NULL;
}

static void fn_call(val_t *v)
{
    const uint8_t *name, *save_tp, *save_line;
    uint8_t *save_tmp_lo, *p;
    const fndef_t *d;
    lval_t params[NFNARG], plv;
    val_t args[NFNARG], old[NFNARG];
    int n = 0, i, len, lvl, save_run, save_jumped, save_nfor, save_ngosub;
    int save_out, save_ndo;

    tp++;                                /* past FN */
    len = fn_name(&name);
    if (len == 0) error(E_SYNTAX);
    d = find_def(name, len);
    if (!d) error(E_FUNC);
    /* the arguments, in the caller's context */
    if (accept('(')) {
        do {
            if (n == NFNARG) error(E_SYNTAX);
            eval(&args[n++]);
        } while (accept(','));
        expect(')');
    }
    if (fn_depth == NFN) error(E_NEST);
    /* Binding a string parameter writes to the pool, so no argument
     * may still be pointing there when the next one is bound. */
    for (i = 0; i < n; i++) pin_str(&args[i]);
    save_tp = tp;
    tp = d->hdr;
    if (accept('(')) {
        for (i = 0; i < n; i++) {
            parse_lval(&plv);
            params[i] = plv;
            /* the parameters are the caller's variables, put back
             * below, so their values have to leave the pool too */
            load_var(&plv, &old[i]);
            pin_str(&old[i]);
            assign(&plv, &args[i]);
            if (i + 1 < n) expect(',');
        }
        expect(')');
    } else if (n)
        error(E_SYNTAX);
    lvl = fn_depth;
    if (!d->multi) {
        expect('=');
        fnstk[lvl].def = NULL;
        fn_depth = lvl + 1;
        eval(v);
        fn_depth = lvl;
        if (v->str && v->len) {
            p = tmp_alloc(v->len);
            mem_copy(p, v->s, v->len);
            v->s = p;
        }
    } else {
        /* The value of the function starts at 0 or "", and the body
         * sets it by assigning to the function's own name. */
        fnstk[lvl].def = d;
        fnstk[lvl].buf = d->str ? tmp_alloc(MAXSTR) : NULL;
        fnstk[lvl].val.str = d->str;
        fnstk[lvl].val.s = fnstk[lvl].buf;
        fnstk[lvl].val.len = 0;
        fp_zero(&fnstk[lvl].val.n);
        save_line = cur_line;
        save_run = running;
        save_jumped = jumped;
        save_nfor = nfor;
        save_ndo = ndo;
        save_ngosub = ngosub;
        save_out = cur_out;
        save_tmp_lo = tmp_lo;
        /* the body's own PRINT goes to the terminal, not to the
         * channel of a PRINT #n the call is an item of */
        cur_out = 0;
        /* The body's statements empty the scratch area before each of
         * them; give it one of its own above what the caller holds.
         * tmp_top stays where the body left it, so that the value the
         * call returns is not handed back over free space. */
        tmp_lo = tmp_top;
        fn_depth = lvl + 1;
        run_body(d);
        fn_depth = lvl;
        tmp_lo = save_tmp_lo;
        nfor = save_nfor;                /* a jump may have left loops open */
        ndo = save_ndo;
        ngosub = save_ngosub;
        cur_out = save_out;
        jumped = save_jumped;
        running = save_run;
        cur_line = save_line;
        *v = fnstk[lvl].val;
    }
    for (i = 0; i < n; i++) assign(&params[i], &old[i]);
    tp = save_tp;
}

/* FNx = expr inside its own DEF ... FNEND: the value of the call. */
static void st_fnset(void)               /* the FN token has been read */
{
    const uint8_t *name;
    const fndef_t *d;
    val_t v;
    int len, i, lvl = fn_depth - 1;

    len = fn_name(&name);
    if (lvl < 0 || !fnstk[lvl].def) error(E_FUNC);
    d = fnstk[lvl].def;                  /* the innermost body's own name */
    if (len != d->len) error(E_FUNC);
    for (i = 0; i < len; i++)
        if (name[i] != d->name[i]) error(E_FUNC);
    expect('=');
    eval(&v);                            /* may call this function again */
    if (v.str != (int)d->str) error(E_TYPE);
    if (d->str) {
        if (v.len > MAXSTR) error(E_STRLEN);
        mem_copy(fnstk[lvl].buf, v.s, v.len);
        fnstk[lvl].val.len = v.len;
    } else
        fnstk[lvl].val.n = v.n;
}

/* ------------------------------------------------------------------ */
/* Printing                                                           */

static void print_num(const fpac_t *n)
{
    char buf[32];
    int len;

    /* fp_format writes the minus sign itself */
    if (!fp_isneg(n)) out_ch(' ');
    len = fp_format(n, buf);
    out_mem((const uint8_t *)buf, (uint32_t)len);
    out_ch(' ');
}

static void print_val(const val_t *v)
{
    if (v->str) out_mem(v->s, v->len);
    else print_num(&v->n);
}

/* #n, at the start of PRINT, INPUT, LINPUT. */
static int parse_channel(void)
{
    int32_t n;

    if (!accept('#')) return 0;
    n = eval_int();
    if (n < 0 || n >= NCHAN) error(E_CHANNEL);
    if (n && chans[n].fd < 0) error(E_CHANNEL);
    return (int)n;
}

static void st_print(void)
{
    val_t v;
    int nl = 1, c, ch;
    chan_t *out;
    int32_t col;

    ch = parse_channel();
    if (ch) {
        if (!accept(',')) accept(';');
        cur_out = ch;
    }
    out = &chans[cur_out];
    while (!at_end()) {
        c = peek();
        nl = 1;
        if (c == ',') {
            tp++;
            if (out->col >= WIDTH - ZONE - 2) out_ch('\n');
            else do out_ch(' '); while (out->col % ZONE);
            nl = 0;
        } else if (c == ';') {
            tp++;
            nl = 0;
        } else if (c == T_TAB) {
            tp++;
            expect('(');
            col = eval_int();
            expect(')');
            if (col > out->col && col < 512)
                while (out->col < col) out_ch(' ');
            nl = 0;
        } else {
            eval(&v);
            print_val(&v);
        }
    }
    if (nl) out_ch('\n');
    console();
}

/* ------------------------------------------------------------------ */
/* INPUT                                                              */

/* The next comma-separated item of an input line.  Returns 0 at the
 * end of the line. */
static int input_item(const char **sp, val_t *v)
{
    const char *s = *sp, *start;
    int neg = 0, n;
    uint32_t len;

    while (*s == ' ') s++;
    if (!*s) return 0;
    if (*s == '"') {
        start = ++s;
        while (*s && *s != '"') s++;
        str_val(v, (const uint8_t *)start, (uint32_t)(s - start));
        if (*s) s++;
        while (*s == ' ') s++;
        if (*s == ',') s++;
        *sp = s;
        return 1;
    }
    start = s;
    while (*s && *s != ',') s++;
    len = (uint32_t)(s - start);
    while (len && start[len - 1] == ' ') len--;
    str_val(v, (const uint8_t *)start, len);
    if (*s == ',') s++;
    *sp = s;
    /* also try it as a number; the caller picks */
    s = start;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    n = fp_parse(s, &v->n);
    v->str = !(n > 0 && (uint32_t)(s + n - start) == len) ? 1 : 2;
    if (neg) fp_neg(&v->n);
    return 1;
}

static void st_input(int whole_line)
{
    int ch, first = 1, r, more;
    lval_t lv;
    val_t v, prompt;
    const char *s;

    ch = parse_channel();
    if (ch) {
        if (!accept(',')) accept(';');
    }
    prompt.str = 0;
    if (peek() == '"') {
        eval_str(&prompt);
        if (!accept(';')) accept(',');
    }
    s = "";
    while (!at_end()) {
        parse_lval(&lv);
        for (;;) {
            while (*s == ' ') s++;
            if (whole_line || !*s) {
                /* fetch a line when the last one is used up */
                if (ch == 0) {
                    if (first && prompt.str) out_mem(prompt.s, prompt.len);
                    else out_str("? ");
                }
                first = 0;
                r = read_line(ch, in_buf, sizeof in_buf);
                if (r < 0) error(E_EOF);
                s = in_buf;
                if (whole_line) {
                    if (lv.kind != 2) error(E_TYPE);
                    str_val(&v, (const uint8_t *)s, (uint32_t)r);
                    assign(&lv, &v);
                    s = "";
                    break;
                }
            }
            more = input_item(&s, &v);
            if (!more) {
                if (lv.kind != 2) continue;       /* ask again */
                str_val(&v, (const uint8_t *)s, 0);
                assign(&lv, &v);
                break;
            }
            if (lv.kind == 2) {
                v.str = 1;
                assign(&lv, &v);
                break;
            }
            if (v.str != 2) {
                if (ch) error(E_NUMBER);
                console();
                out_str("?Bad input, try again\n");
                s = "";
                continue;
            }
            v.str = 0;
            assign(&lv, &v);
            break;
        }
        if (!accept(',')) break;
    }
    if (!at_end()) error(E_SYNTAX);
}

/* ------------------------------------------------------------------ */
/* READ / DATA                                                        */

static void restore_data(void)
{
    data_line = prog_lo;
    data_tp = NULL;
}

/* Position data_tp on the next DATA item, or fail. */
static void next_data(void)
{
    const uint8_t *p;

    if (!data_line) restore_data();
    for (;;) {
        if (data_tp) {
            while (*data_tp == ' ') data_tp++;
            if (*data_tp == ',') {
                data_tp++;
                while (*data_tp == ' ') data_tp++;
                return;
            }
            if (*data_tp) return;      /* first item of the statement */
            data_tp = NULL;
            data_line = next_rec(data_line);
        }
        while (data_line < prog_end) {
            for (p = data_line + 3; *p; p++) {
                if (*p == '"') {
                    for (p++; *p && *p != '"'; p++)
                        ;
                    if (!*p) break;
                } else if (*p == T_NUM)
                    p += NUMLEN;
                else if (*p == T_REM)
                    break;
                else if (*p == T_DATA) {
                    data_tp = p + 1;
                    goto found;
                }
            }
            data_line = next_rec(data_line);
        }
        error(E_DATA);
    found:
        while (*data_tp == ' ') data_tp++;
        if (*data_tp) return;
        data_tp = NULL;
        data_line = next_rec(data_line);
    }
}

static void st_read(void)
{
    lval_t lv;
    val_t v;
    const uint8_t *start;
    uint32_t len;
    int neg;

    do {
        parse_lval(&lv);
        next_data();
        /* an item ends at a comma or the end of the line */
        if (*data_tp == '"') {
            start = ++data_tp;
            while (*data_tp && *data_tp != '"') data_tp++;
            len = (uint32_t)(data_tp - start);
            if (*data_tp) data_tp++;
        } else {
            start = data_tp;
            while (*data_tp && *data_tp != ',') data_tp++;
            len = (uint32_t)(data_tp - start);
            while (len && start[len - 1] == ' ') len--;
        }
        while (*data_tp == ' ') data_tp++;
        if (lv.kind == 2) {
            str_val(&v, start, len);
        } else {
            const uint8_t *s = start;
            int n;
            neg = 0;
            if (*s == '-' || *s == '+') neg = *s++ == '-';
            n = fp_parse((const char *)s, &v.n);
            if (n == 0 || (uint32_t)(s + n - start) != len) error(E_NUMBER);
            if (neg) fp_neg(&v.n);
            v.str = 0;
        }
        assign(&lv, &v);
    } while (accept(','));
}

/* ------------------------------------------------------------------ */
/* Control flow                                                       */

static void goto_line(uint32_t n)
{
    uint8_t *rec = find_line(n);

    if (rec >= prog_end || line_no(rec) != n) error(E_LINE);
    cur_line = rec;
    tp = rec + 3;
    running = 1;
    jumped = 1;
}

/* Skip forward from tp to just past the NEXT that matches a FOR whose
 * body is not to be executed at all. */
static void skip_loop(void)
{
    int depth = 0;
    const uint8_t *p = tp;
    const uint8_t *rec = cur_line;
    int idx, kind;

    for (;;) {
        while (*p) {
            if (*p == '"') {
                for (p++; *p && *p != '"'; p++)
                    ;
                if (!*p) break;
            } else if (*p == T_NUM)
                p += NUMLEN;
            else if (*p == T_REM || *p == T_DATA)
                break;
            else if (*p == T_FOR)
                depth++;
            else if (*p == T_NEXT) {
                if (depth == 0) {
                    tp = p + 1;
                    cur_line = rec;
                    parse_name(&idx, &kind);
                    return;
                }
                depth--;
            }
            p++;
        }
        if (rec == imm_buf || next_rec(rec) >= prog_end) error(E_NEXT);
        rec = next_rec(rec);
        p = rec + 3;
    }
}

static void st_for(void)
{
    lval_t lv;
    val_t v;
    for_t *f;
    int cmp;

    parse_lval(&lv);
    if (lv.kind == 2) error(E_TYPE);
    expect('=');
    eval(&v);
    assign(&lv, &v);
    if (peek() != T_TO) error(E_SYNTAX);
    tp++;
    if (nfor == NFOR) error(E_NEST);
    f = &forstk[nfor];
    eval_num(&f->limit);
    if (peek() == T_STEP) {
        tp++;
        eval_num(&f->step);
    } else
        fp_from_int(&f->step, 1);
    f->var = lv.p;
    f->tp = tp;
    f->line = cur_line;
    cmp = fp_cmp(f->var, &f->limit);
    if (fp_isneg(&f->step) ? cmp < 0 : cmp > 0) {
        skip_loop();
        return;
    }
    nfor++;
}

static void st_next(void)
{
    lval_t lv;
    for_t *f;
    int cmp;

    do {
        parse_lval(&lv);
        while (nfor > 0 && forstk[nfor - 1].var != lv.p) nfor--;
        if (nfor == 0) error(E_NEXT);
        f = &forstk[nfor - 1];
        fp_add(f->var, &f->step);
        if (lv.kind == 1) fp_trunc(f->var);
        cmp = fp_cmp(f->var, &f->limit);
        if (fp_isneg(&f->step) ? cmp >= 0 : cmp <= 0) {
            tp = f->tp;
            cur_line = f->line;
            return;
        }
        nfor--;
    } while (accept(','));
}

/* Remember where to come back to: the statement boundary at ret. */
static void push_gosub(const uint8_t *ret, int src, int sleep)
{
    if (ngosub == NGOSUB) error(E_NEST);
    gosubstk[ngosub].tp = ret;
    gosubstk[ngosub].line = cur_line;
    gosubstk[ngosub].src = (uint8_t)src;
    gosubstk[ngosub].sleep = (uint8_t)sleep;
    gosubstk[ngosub].deadline = sleep_deadline;
    ngosub++;
}

static void st_gosub(void)
{
    uint32_t n = parse_lineno();

    push_gosub(tp, 0, 0);
    goto_line(n);
}

static void st_return(void)
{
    gosub_t *f;

    if (ngosub == 0) error(E_RETURN);
    f = &gosubstk[--ngosub];
    tp = f->tp;
    cur_line = f->line;
    if (f->src) {
        events[f->src - 1].busy = 0;
        handler_done = 1;
    }
    if (f->sleep) {
        sleep_resume = 1;
        sleep_deadline = f->deadline;
    }
    if (cur_line == imm_buf) running = 0;
}

/* ------------------------------------------------------------------ */
/* Events: ON TIMER(ms) GOSUB n, ON KEY(P$) GOSUB n                    */

/* The key on a pin, or -1. */
static int find_key(int pin)
{
    int k;

    for (k = 0; k < nkeys; k++)
        if (events[EV_KEY + k].pin == pin) return k;
    return -1;
}

/* KEY(P$) at tp, the KEY token read: the pin, and the level a press
 * is, which is low with a pull-up unless a second argument says 1. */
static int parse_key(int *active)
{
    val_t a;
    int pin;

    expect('(');
    eval_str(&a);
    pin = pin_name(&a);
    *active = 0;
    if (accept(',')) *active = eval_int() != 0;
    expect(')');
    return pin;
}

/* ON TIMER(ms) GOSUB line, ON KEY(P$ [, level]) GOSUB line: the TIMER
 * or KEY token is at tp. */
static void st_on_event(int t)
{
    event_t *e;
    const uint8_t *rec;
    int32_t ms = 0;
    int pin, active, k;
    uint32_t line;

    tp++;
    if (t == T_TIMER) {
        expect('(');
        ms = eval_int();
        expect(')');
        if (ms <= 0) error(E_ARG);
        e = &events[EV_TIMER];
    } else {
        pin = parse_key(&active);
        k = find_key(pin);
        if (k < 0) {
            if (nkeys == NKEY) error(E_KEYS);
            k = nkeys++;
            mem_set(&events[EV_KEY + k], 0, sizeof events[0]);
        }
        e = &events[EV_KEY + k];
        e->pin = (uint8_t)pin;
        e->active = (uint8_t)active;
    }
    if (peek() != T_GOSUB) error(E_SYNTAX);
    tp++;
    line = parse_lineno();
    rec = find_line(line);
    if (rec >= prog_end || line_no(rec) != line) error(E_LINE);
    e->line = line;
    if (t == T_TIMER) {
        e->period = (uint32_t)ms;
        e->due = sys_ticks() + e->period;
    }
}

/* Take a key's level as what it is now, so that only a change counts. */
static void key_settle(event_t *e)
{
    int r = sys_pin_read(e->pin);

    e->stable = e->last = (uint8_t)(r < 0 ? !e->active : r);
    e->since = sys_ticks();
}

/* TIMER ON, TIMER OFF: the TIMER token has been read. */
static void st_timer(void)
{
    event_t *e = &events[EV_TIMER];
    int t = peek();

    if (t != T_ON && t != T_OFF) error(E_SYNTAX);
    tp++;
    if (!e->line) error(E_ARG);
    e->enabled = t == T_ON;
    e->pending = 0;
    e->due = sys_ticks() + e->period;
}

/* KEY(P$) ON, KEY(P$) OFF: the KEY token has been read.  ON puts the
 * pull the press works against on the pin. */
static void st_key(void)
{
    event_t *e;
    int pin, active, k, t;

    pin = parse_key(&active);
    t = peek();
    if (t != T_ON && t != T_OFF) error(E_SYNTAX);
    tp++;
    k = find_key(pin);
    if (k < 0) error(E_KEY);
    e = &events[EV_KEY + k];
    e->pending = 0;
    if (t == T_OFF) {
        e->enabled = 0;
        return;
    }
    pin_check(sys_pin_mode(pin, e->active ? SYS_PIN_IN_PULLDOWN : SYS_PIN_IN_PULLUP));
    key_settle(e);
    e->enabled = 1;
}

/* Call the handler of source i, to come back to ret. */
static void dispatch(int i, const uint8_t *ret, int sleep)
{
    event_t *e = &events[i];

    push_gosub(ret, 1 + i, sleep);
    e->pending = 0;
    e->busy = 1;
    goto_line(e->line);
}

/* Between two statements of a running program: sample the sources,
 * and call the handler of one that has fired, coming back to ret.
 * Returns 1 when a handler was called.  A key counts as pressed when
 * it has read the pressed level for DEBOUNCE_MS without a break, so
 * the bounce of a switch is one press.  Right after a handler has
 * returned nothing is called, so that the program gets a statement
 * in between: a handler slower than its period would otherwise be
 * called again the moment it returned, for ever. */
static int poll_events(const uint8_t *ret, int sleep)
{
    event_t *e;
    uint32_t now;
    int i, r;

    if (!running || cur_line == imm_buf || handler_done) return 0;
    now = sys_ticks();
    e = &events[EV_TIMER];
    if (e->enabled && (int32_t)(now - e->due) >= 0) {
        e->pending = 1;
        e->due += e->period;
        if ((int32_t)(now - e->due) >= 0) e->due = now + e->period;
    }
    for (i = 0; i < nkeys; i++) {
        e = &events[EV_KEY + i];
        if (!e->enabled) continue;
        r = sys_pin_read(e->pin);
        if (r < 0) continue;
        if (r != e->last) {
            e->last = (uint8_t)r;
            e->since = now;
        } else if (r != e->stable && now - e->since >= DEBOUNCE_MS) {
            e->stable = (uint8_t)r;
            if (r == e->active) e->pending = 1;
        }
    }
    for (i = 0; i < 1 + nkeys; i++) {
        e = &events[i];
        if (e->pending && e->enabled && !e->busy) {
            dispatch(i, ret, sleep);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DO ... LOOP                                                        */

/* The LOOP that closes the DO at tp, or an error.  It is found once,
 * when the loop is entered, and is what the loop is known by. */
static const uint8_t *find_loop(const uint8_t **line)
{
    int depth = 0;
    const uint8_t *p = tp;
    const uint8_t *rec = cur_line;

    for (;;) {
        while (*p) {
            if (*p == '"') {
                for (p++; *p && *p != '"'; p++)
                    ;
                if (!*p) break;
            } else if (*p == T_NUM)
                p += NUMLEN;
            else if (*p == T_REM || *p == T_DATA)
                break;
            else if (*p == T_DO)
                depth++;
            else if (*p == T_LOOP) {
                if (depth == 0) {
                    *line = rec;
                    return p;
                }
                depth--;
            }
            p++;
        }
        if (rec == imm_buf || next_rec(rec) >= prog_end) error(E_DO);
        rec = next_rec(rec);
        p = rec + 3;
    }
}

/* WHILE cond or UNTIL cond at tp, if either: 1 to go on, 0 to stop,
 * and -1 when there is no condition. */
static int loop_cond(void)
{
    val_t v;
    int t = peek(), go;

    if (t != T_WHILE && t != T_UNTIL) return -1;
    tp++;
    eval(&v);
    go = v.str ? v.len != 0 : !fp_iszero(&v.n);
    return t == T_WHILE ? go : !go;
}

/* DO [WHILE cond | UNTIL cond].  Every iteration comes back here, so
 * that the condition is asked again; the frame on top of the stack
 * is this loop's when it is that return, or a jump to the DO. */
static void st_do(void)
{
    do_t *d = NULL;
    int go, i;

    /* a loop left by a jump and entered again drops what was inside it */
    for (i = ndo - 1; i >= 0 && !d; i--)
        if (dostk[i].tp == stmt_tp) {
            d = &dostk[i];
            ndo = i + 1;
        }
    if (!d) {
        if (ndo == NDO) error(E_NEST);
        d = &dostk[ndo];
        d->loop = find_loop(&d->loopline);
        d->tp = stmt_tp;
        d->line = cur_line;
        ndo++;
    }
    go = loop_cond();
    if (go == 0) {
        /* past the LOOP and its condition */
        ndo--;
        tp = d->loop + 1;
        cur_line = d->loopline;
        skip_stmt();
    }
}

/* LOOP [WHILE cond | UNTIL cond] */
static void st_loop(void)
{
    const uint8_t *me = tp - 1;          /* the LOOP token */
    do_t *d;
    int go;

    while (ndo > 0 && dostk[ndo - 1].loop != me) ndo--;
    if (ndo == 0) error(E_LOOP);
    d = &dostk[ndo - 1];
    go = loop_cond();
    if (go == 0) {
        ndo--;
        return;
    }
    tp = d->tp;                          /* the DO, which asks its own */
    cur_line = d->line;
    jumped = 1;
}

static void st_on(void)
{
    int32_t n, i = 1;
    int t = peek();
    uint32_t line;

    if (t == T_TIMER || t == T_KEY) {
        st_on_event(t);
        return;
    }
    n = eval_int();
    t = peek();
    if (t != T_GOTO && t != T_GOSUB) error(E_SYNTAX);
    tp++;
    for (;;) {
        line = parse_lineno();
        if (i == n) {
            if (t == T_GOSUB) {
                skip_stmt();
                push_gosub(tp, 0, 0);
            }
            goto_line(line);
            return;
        }
        i++;
        if (!accept(',')) break;
    }
    if (n < 1 || n >= i) error(E_ARG);
}

/* ------------------------------------------------------------------ */
/* Other statements                                                   */

static void st_let(void)
{
    lval_t lv;
    val_t v;

    parse_lval(&lv);
    expect('=');
    eval(&v);
    assign(&lv, &v);
}

static void st_dim(void)
{
    int idx, kind;
    int32_t d1, d2;

    do {
        if (!parse_name(&idx, &kind)) error(E_SYNTAX);
        expect('(');
        d1 = eval_int();
        d2 = -1;
        if (accept(',')) d2 = eval_int();
        expect(')');
        make_array(kind, idx, d1, d2);
    } while (accept(','));
}

/* A file name in quotes, copied out as a C string. */
static char *parse_filename(void)
{
    val_t v;
    char *p;

    eval_str(&v);
    p = (char *)tmp_alloc(v.len + 1);
    mem_copy(p, v.s, v.len);
    p[v.len] = 0;
    return p;
}

/* OPEN "name" FOR INPUT|OUTPUT AS FILE #n */
static void st_open(void)
{
    char *name = parse_filename();
    int mode = 0, n;

    if (peek() != T_FOR) error(E_SYNTAX);
    tp++;
    if (peek() == T_INPUT) mode = 0;
    else if (peek() == T_OUTPUT) mode = 1;
    else error(E_SYNTAX);
    tp++;
    if (peek() != T_AS) error(E_SYNTAX);
    tp++;
    if (peek() == T_FILE) tp++;
    accept('#');
    n = eval_int();
    if (n < 1 || n >= NCHAN) error(E_CHANNEL);
    chan_open(n, name, mode);
}

static void st_close(void)
{
    int n;

    if (at_end()) {
        close_all();
        return;
    }
    do {
        accept('#');
        n = eval_int();
        if (n < 1 || n >= NCHAN) error(E_CHANNEL);
        chan_close(n);
    } while (accept(','));
}

/* IF END #n THEN ... */
static int if_end(void)
{
    int n;

    tp++;
    accept('#');
    n = eval_int();
    if (n < 1 || n >= NCHAN || chans[n].fd < 0) error(E_CHANNEL);
    if (chans[n].pos < chans[n].len) return 0;
    if (chans[n].eof) return 1;
    return chan_getc(&chans[n]) < 0 ? 1 : (chans[n].pos--, 0);
}

/* IF cond THEN stmt|line [ELSE stmt|line]; IF cond GOTO line */
static void st_if(void)
{
    val_t v;
    int taken;

    if (peek() == T_END) {
        taken = if_end();
    } else {
        eval(&v);
        taken = v.str ? v.len != 0 : !fp_iszero(&v.n);
    }
    if (peek() == T_GOTO) {
        tp++;
        if (taken) {
            goto_line(parse_lineno());
            return;
        }
    } else {
        if (peek() != T_THEN) error(E_SYNTAX);
        tp++;
        if (taken) {
            if (is_digit(peek())) goto_line(parse_lineno());
            return;              /* the statement follows */
        }
    }
    /* not taken: find ELSE on this line, else finish the line */
    for (;;) {
        skip_stmt();
        if (*tp == T_ELSE) {
            tp++;
            if (is_digit(peek())) goto_line(parse_lineno());
            return;
        }
        if (*tp == 0) return;
        tp++;
    }
}

/* DEF FNx(...) = ... is not executed.  Neither is a DEF ... FNEND:
 * reaching it skips the whole block, so that a definition may sit
 * among the lines that call it without a GOTO around it. */
static void st_def(void)
{
    fndef_t d;
    int i;

    if (!def_header(tp, &d)) error(E_SYNTAX);
    if (!d.multi) {
        skip_stmt();
        return;
    }
    if (cur_line == imm_buf) {
        /* a definition has to be in the program to be called */
        skip_line();
        return;
    }
    ensure_defs();
    /* the name is a place in the program text, so it says which DEF
     * this is even where two of them share a name */
    for (i = 0; i < nfndef; i++)
        if (fndefs[i].name == d.name) {
            if (!fndefs[i].end) error(E_DEF);
            tp = fndefs[i].end;
            cur_line = fndefs[i].endline;
            return;
        }
    error(E_DEF);
}

static void st_randomize(void)
{
    rnd_seed = sys_ticks() * 2654435761u + 1;
}


/* ------------------------------------------------------------------ */
/* Commands                                                           */

static void ready(void)
{
    if (!batch) out_str("\nREADY\n\n");
}

static void run_program(void)
{
    clear_vars();
    events_reset();
    close_all();
    if (prog_end == prog_lo) {
        running = 0;
        return;
    }
    if (at_end()) {
        cur_line = prog_lo;
        tp = cur_line + 3;
        running = 1;
    } else
        goto_line(parse_lineno());
}

static void list_program(void)
{
    uint32_t from = 0, to = 65535;
    const uint8_t *rec;

    if (is_digit(peek())) {
        from = parse_lineno();
        to = from;
        if (accept('-')) {
            to = 65535;
            if (is_digit(peek())) to = parse_lineno();
        }
    }
    for (rec = prog_lo; rec < prog_end; rec = next_rec(rec)) {
        if (line_no(rec) >= from && line_no(rec) <= to) list_line(rec);
        if (sys_break()) break;
    }
}

static void delete_lines(void)
{
    uint32_t from, to;
    uint8_t *a, *b;

    from = parse_lineno();
    to = from;
    if (accept('-')) {
        to = 65535;
        if (is_digit(peek())) to = parse_lineno();
    }
    a = find_line(from);
    b = find_line(to + 1);
    mem_move(a, b, (uint32_t)(prog_end - b));
    prog_end -= b - a;
    defs_valid = 0;
    cont_line = NULL;
}

static void save_program(int replace)
{
    char *name = parse_filename();
    const uint8_t *rec;

    (void)replace;
    chan_open(NCHAN - 1, name, 1);
    cur_out = NCHAN - 1;
    for (rec = prog_lo; rec < prog_end; rec = next_rec(rec)) list_line(rec);
    chan_close(NCHAN - 1);
    console();
}

static void enter_line(const char *text);

static void old_program(void)
{
    char *name = parse_filename();
    static char line[MAXLINE + 1];

    chan_open(NCHAN - 1, name, 0);
    new_program();
    while (chan_getline(NCHAN - 1, line, sizeof line) >= 0) enter_line(line);
    chan_close(NCHAN - 1);
}

static void show_length(void)
{
    out_int((int32_t)(prog_end - prog_lo));
    out_str(" bytes of program, ");
    out_int((int32_t)(prog_hi - prog_end));
    out_str(" free\n");
}

/* ------------------------------------------------------------------ */
/* The statement loop                                                 */

static void stop_message(const char *what)
{
    console();
    if (chans[0].col) out_ch('\n');
    out_str(what);
    if (running && cur_line != imm_buf) {
        out_str(" at line ");
        out_int((int32_t)line_no(cur_line));
    }
    out_ch('\n');
}

/* SLEEP milliseconds.  The wait is broken into pieces so that Ctrl-C
 * stops the program during a long one, as it does between statements;
 * a piece is short enough that the console does not feel stuck and
 * long enough that the polling costs nothing. */
#define SLEEP_SLICE 20u

/* An event during the wait calls its handler, whose RETURN comes back
 * to this statement with resume set, and the wait goes on to the
 * deadline it had. */
static void st_sleep(int resume)
{
    fpac_t n;
    int32_t ms;
    uint32_t now, left, slice, deadline;

    eval_num(&n);
    ms = fp_to_int(&n);
    if (ms < 0) error(E_ARG);
    now = sys_ticks();
    deadline = resume ? sleep_deadline : now + (uint32_t)ms;
    while ((int32_t)(deadline - now) > 0) {
        left = deadline - now;
        slice = left < SLEEP_SLICE ? left : SLEEP_SLICE;
        sys_sleep(slice);
        if (sys_break()) {
            cont_tp = tp;
            cont_line = cur_line;
            stop_message("STOP");
            running = 0;
            skip_line();
            return;
        }
        sleep_deadline = deadline;
        if (poll_events(stmt_tp, 1)) return;
        now = sys_ticks();
    }
}

/* Execute one statement at tp. */
static void statement(void)
{
    int t = peek(), resume = sleep_resume;

    sleep_resume = 0;
    stmt_tp = tp;
    tmp_top = tmp_lo;
    switch (t) {
    case 0:
    case '\\':
    case ':':
        return;
    case T_ELSE:
        /* the THEN part was executed: the rest of the line is not */
        skip_line();
        return;
    }
    handler_done = 0;                   /* this statement is the one in between */
    if (t >= 0x80) tp++;
    switch (t) {
    case T_LET: st_let(); break;
    case T_PRINT: st_print(); break;
    case T_INPUT: st_input(0); break;
    case T_LINPUT: st_input(1); break;
    case T_IF:
        st_if();
        jumped = 1;
        return;
    case T_FOR: st_for(); break;
    case T_NEXT: st_next(); break;
    case T_GOTO: goto_line(parse_lineno()); return;
    case T_GOSUB: st_gosub(); return;
    case T_RETURN:
        st_return();
        jumped = 1;
        return;
    case T_ON: st_on(); return;
    case T_DIM: st_dim(); break;
    case T_READ: st_read(); break;
    case T_DATA:
    case T_REM:
        skip_line();
        return;
    case T_RESTORE: restore_data(); break;
    case T_DEF: st_def(); break;
    case T_FN: st_fnset(); break;
    case T_FNEND:                       /* reached only by a jump into a body */
        error(E_SYNTAX);
        break;
    case T_STOP:
        cont_tp = tp;
        cont_line = cur_line;
        stop_message("STOP");
        running = 0;
        skip_line();
        return;
    case T_END:
        running = 0;
        cont_line = NULL;
        close_all();
        skip_line();
        return;
    case T_RANDOMIZE: st_randomize(); break;
    case T_SLEEP:
        st_sleep(resume);
        if (jumped) return;             /* an event took over */
        break;
    case T_DO: st_do(); break;
    case T_LOOP:
        st_loop();
        if (jumped) return;
        break;
    case T_TIMER: st_timer(); break;
    case T_KEY: st_key(); break;
    case T_OPEN: st_open(); break;
    case T_CLOSE: st_close(); break;
    case T_RUN:
    case T_RUNNH:
        run_program();
        jumped = 1;
        return;
    case T_LIST:
    case T_LISTNH:
        list_program();
        break;
    case T_NEW:
    case T_SCR:
        new_program();
        break;
    case T_OLD:
        old_program();
        break;
    case T_SAVE: save_program(0); break;
    case T_REPLACE: save_program(1); break;
    case T_UNSAVE:
        if (sys_unlink(parse_filename()) < 0) error(E_FILE);
        break;
    case T_BYE:
        close_all();
        sys_exit(0);
        break;
    case T_CLEAR: clear_vars(); break;
    case T_CONT:
        if (!cont_line) error(E_CONT);
        tp = cont_tp;
        cur_line = cont_line;
        cont_line = NULL;
        running = 1;
        jumped = 1;
        return;
    case T_LENGTH: show_length(); break;
    case T_DEL: delete_lines(); break;
    default:
        if (is_upper(t)) {
            st_let();
            break;
        }
        error(E_SYNTAX);
    }
    if (!at_end()) error(E_SYNTAX);
}

/* The statements of a DEF ... FNEND, as a loop of its own: fn_call()
 * is in the middle of an expression, so the body cannot be run by the
 * loop below and returned from.  It ends at the FNEND.  Anything else
 * that would end it -- an error, Ctrl-C, END, the last line of the
 * program -- has to unwind the expression too, and leaves through
 * longjmp() rather than a return. */
static void run_body(const fndef_t *d)
{
    int c;

    tp = d->body;
    cur_line = d->line;
    running = 1;
    jumped = 0;
    for (;;) {
        if (peek() == T_FNEND) return;
        statement();
        if ((++stmt_count & 63) == 0 && sys_break()) {
            stop_message("STOP");
            running = 0;
        }
        if (!running) {
            cont_line = NULL;            /* CONT cannot re-enter a body */
            error(E_HALT);
        }
        if (poll_events(tp, sleep_resume)) sleep_resume = 0;
        if (jumped) {
            jumped = 0;
            continue;
        }
        c = peek();
        if (c == '\\' || c == ':') {
            tp++;
            continue;
        }
        if (c == T_ELSE || c == T_REM) {
            skip_line();
            c = 0;
        }
        if (c != 0) error(E_SYNTAX);
        if (cur_line == imm_buf || next_rec(cur_line) >= prog_end) error(E_DEF);
        cur_line = next_rec(cur_line);
        tp = cur_line + 3;
    }
}

/* Run from tp until the program ends or, in immediate mode, until the
 * end of the typed line. */
static void execute(void)
{
    int c;

    ensure_defs();
    jumped = 0;
    for (;;) {
        statement();
        /* ^C between statements; CONT picks up at tp, which is at the
         * separator, the end of the line or the target of a jump */
        if (running && (++stmt_count & 63) == 0 && sys_break()) {
            cont_tp = tp;
            cont_line = cur_line;
            stop_message("STOP");
            running = 0;
            return;
        }
        /* An event's handler is called here, between statements, and
         * its RETURN comes back to tp: a separator, the end of the line
         * or the target of a jump, all places a statement may start.
         * When that is a SLEEP an earlier event interrupted, the new
         * frame carries the resumption on. */
        if (poll_events(tp, sleep_resume)) sleep_resume = 0;
        if (jumped) {
            jumped = 0;
            continue;
        }
        c = peek();
        if (c == '\\' || c == ':') {
            tp++;
            continue;
        }
        if (c == T_ELSE || c == T_REM) {     /* or a trailing ! comment */
            skip_line();
            c = 0;
        }
        if (c != 0) error(E_SYNTAX);
        /* end of the line */
        if (!running) return;
        if (cur_line == imm_buf) {
            running = 0;
            return;
        }
        cur_line = next_rec(cur_line);
        if (cur_line >= prog_end) {
            running = 0;
            close_all();
            return;
        }
        tp = cur_line + 3;
    }
}

/* A typed line: store it, or execute it. */
static void enter_line(const char *text)
{
    uint8_t tok[MAXLINE + 4];
    int32_t n;
    int len;

    len = tokenize(text, tok, &n);
    if (n >= 0) {
        if (n == 0) error(E_LINE);
        store_line((uint32_t)n, tok, (uint32_t)len);
        cont_line = NULL;
        return;
    }
    if (len == 0) return;
    imm_buf[0] = (uint8_t)(len + 4);
    imm_buf[1] = 0;
    imm_buf[2] = 0;
    mem_copy(imm_buf + 3, tok, (uint32_t)len);
    imm_buf[3 + len] = 0;
    cur_line = imm_buf;
    tp = imm_buf + 3;
    running = 0;
    execute();
}

/* The interpreter: heap_size bytes at heap are its memory; flags & 1
 * is batch mode.  Returns only through sys_exit(). */
int bas_main(uint8_t *heap, uint32_t heap_size, uint32_t flags)
{
    uint32_t size;
    int code, i, r;

    batch = flags & 1;
    fp_init();
    for (i = 0; i < NCHAN; i++) chans[i].fd = -1;
    chans[0].fd = 0;
    chans[0].mode = 1;
    if (heap_size < 8192 + TMP_BYTES) {
        out_str("?Not enough memory\n");
        sys_exit(1);
    }
    tmp_base = tmp_lo = heap;
    tmp_hi = tmp_lo + TMP_BYTES;
    size = (heap_size - TMP_BYTES) & ~(ALIGN - 1);
    prog_lo = tmp_hi;
    prog_hi = prog_lo + (size * 2 / 5 & ~(ALIGN - 1));
    arena_lo = prog_hi;
    arena_hi = arena_lo + (size / 4 & ~(ALIGN - 1));
    pool_lo = arena_hi;
    pool_hi = pool_lo + ((size - (uint32_t)(pool_lo - prog_lo)) & ~(ALIGN - 1));
    new_program();
    console();
    if (!batch) {
        out_str(BAS_BANNER "\n");
        show_length();
    }
    for (;;) {
        code = setjmp(err_jb);
        if (code) {
            console();
            close_all();
            /* E_HALT is the end of a program that stopped inside a
             * DEF ... FNEND, and has said so already */
            if (code != E_HALT) {
                if (chans[0].col) out_ch('\n');
                out_ch('?');
                out_str(messages[code]);
                if (running && cur_line != imm_buf) {
                    out_str(" at line ");
                    out_int((int32_t)line_no(cur_line));
                }
                out_ch('\n');
            }
            nfor = 0;
            ndo = 0;
            ngosub = 0;
            fn_depth = 0;
            events_unwind();
            tmp_lo = tmp_base;
            running = 0;
            cont_line = NULL;
            if (code != E_HALT && batch) sys_exit(1);
        }
        ready();
        for (;;) {
            r = sys_readline(in_buf, sizeof in_buf);
            if (r < 0) {
                out_str(batch ? "" : "\n");
                sys_exit(0);
            }
            enter_line(in_buf);
            if (!batch && (in_buf[0] < '0' || in_buf[0] > '9')) break;
        }
    }
    return 0;
}
