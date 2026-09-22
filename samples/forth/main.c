/*
 * forth - a Forth machine for Freya.
 *
 * Three pieces: an outer interpreter that reads the console a line at a
 * time, a compiler that lays threaded code into a dictionary, and the
 * virtual machine that runs it.  A word is a 16-bit token - below
 * PRIM_COUNT it is one of the primitives in the switch in do_prim(),
 * above it it is the body offset of a definition in the dictionary,
 * divided by two.
 * Token threading rather than cell threading halves the size of compiled
 * code, which is the whole point on a board whose program region is
 * 8 KiB of a 20 KiB SRAM.
 *
 *     run forth.bin                 interactive
 *     run forth.bin demo.fs         interpret a file first, then the console
 *     runflash                      the flash image, with a bigger dictionary
 *
 * Numbers are 32-bit cells, addresses are real addresses, so @ and ! reach
 * the whole memory map.  Poking a bad one faults, and Freya contains the
 * fault and takes the console back.
 */
#include "freya_api.h"

typedef int32_t  cell_t;
typedef uint32_t ucell_t;

/*
 * Every arena is sized from the program region, because the region is
 * what differs between boards: 8 KiB on the Blue Pill, 56 KiB on the
 * Black Pill.  A flash resident image spends the whole window on data; a
 * RAM image shares it with its own code, so it settles for a dictionary
 * that is small but still enough to define in.
 */
#if FREYA_APP_REGION_SIZE >= 32u * 1024u
#define DICT_SIZE   (FREYA_APP_REGION_SIZE - 14u * 1024u)
#define DS_CELLS    128
#define RS_CELLS    128
#define LINE_SIZE   200
#define PAD_SIZE    128
#elif defined(FREYA_APP_XIP)
#define DICT_SIZE   7168u
#define DS_CELLS    48
#define RS_CELLS    48
#define LINE_SIZE   128
#define PAD_SIZE    80
#else
#error "forth does not fit a program RAM region this small - build the flash image"
#endif

#define NAME_MAX    31
#define LEAVE_MAX   8
#define INCLUDE_MAX 2
#define NO_WORD     0xFFFFu

#define F_IMMEDIATE 0x80u
#define F_HIDDEN    0x40u
#define F_NAMELEN   0x3Fu

/*
 * The primitives, in the one place that defines both their token numbers
 * and their names.  Order carries two things the code relies on: every
 * primitive from OP_PAREN on is immediate, and every one from OP_SEMI on
 * is compile only.  A nameless primitive is one the compiler emits and
 * the user cannot type.
 */
#define PRIM_INTERNAL(X)   \
    X(LIT,      "")        \
    X(BRANCH,   "")        \
    X(ZBRANCH,  "")        \
    X(EXIT,     "")        \
    X(DOVAR,    "")        \
    X(DOSTR,    "")        \
    X(DOTSTR,   "")        \
    X(XDO,      "")        \
    X(XLOOP,    "")        \
    X(XPLOOP,   "")

#define PRIM_PLAIN(X)      \
    X(DUP,      "dup")     \
    X(DROP,     "drop")    \
    X(SWAP,     "swap")    \
    X(OVER,     "over")    \
    X(ROT,      "rot")     \
    X(NROT,     "-rot")    \
    X(NIP,      "nip")     \
    X(TUCK,     "tuck")    \
    X(QDUP,     "?dup")    \
    X(DEPTH,    "depth")   \
    X(TWODUP,   "2dup")    \
    X(TWODROP,  "2drop")   \
    X(TWOSWAP,  "2swap")   \
    X(TWOOVER,  "2over")   \
    X(TOR,      ">r")      \
    X(FROMR,    "r>")      \
    X(RFETCH,   "r@")      \
    X(LOOPI,    "i")       \
    X(LOOPJ,    "j")       \
    X(UNLOOP,   "unloop")  \
    X(ADD,      "+")       \
    X(SUB,      "-")       \
    X(MUL,      "*")       \
    X(DIV,      "/")       \
    X(MOD,      "mod")     \
    X(DIVMOD,   "/mod")    \
    X(NEGATE,   "negate")  \
    X(ABSVAL,   "abs")     \
    X(MINVAL,   "min")     \
    X(MAXVAL,   "max")     \
    X(INC,      "1+")      \
    X(DEC,      "1-")      \
    X(TWOMUL,   "2*")      \
    X(TWODIV,   "2/")      \
    X(LSHIFT,   "lshift")  \
    X(RSHIFT,   "rshift")  \
    X(AND,      "and")     \
    X(OR,       "or")      \
    X(XOR,      "xor")     \
    X(INVERT,   "invert")  \
    X(EQ,       "=")       \
    X(NE,       "<>")      \
    X(LT,       "<")       \
    X(GT,       ">")       \
    X(ULT,      "u<")      \
    X(ZEQ,      "0=")      \
    X(ZLT,      "0<")      \
    X(ZGT,      "0>")      \
    X(TRUEVAL,  "true")    \
    X(FALSEVAL, "false")   \
    X(BL,       "bl")      \
    X(CELLPLUS, "cell+")   \
    X(CELLS,    "cells")   \
    X(FETCH,    "@")       \
    X(STORE,    "!")       \
    X(CFETCH,   "c@")      \
    X(CSTORE,   "c!")      \
    X(PLUSSTORE, "+!")     \
    X(COMMA,    ",")       \
    X(CCOMMA,   "c,")      \
    X(HERE,     "here")    \
    X(ALLOT,    "allot")   \
    X(ALIGN,    "align")   \
    X(UNUSED,   "unused")  \
    X(MOVE,     "move")    \
    X(FILL,     "fill")    \
    X(COLON,    ":")       \
    X(VARIABLE, "variable") \
    X(CONSTANT, "constant") \
    X(CREATE,   "create")  \
    X(TICK,     "'")       \
    X(EXECUTE,  "execute") \
    X(IMMEDIATE, "immediate") \
    X(FORGET,   "forget")  \
    X(RBRACKET, "]")       \
    X(EMIT,     "emit")    \
    X(KEY,      "key")     \
    X(KEYQ,     "key?")    \
    X(DOT,      ".")       \
    X(UDOT,     "u.")      \
    X(DOTS,     ".s")      \
    X(CR,       "cr")      \
    X(SPACE,    "space")   \
    X(SPACES,   "spaces")  \
    X(TYPE,     "type")    \
    X(DUMP,     "dump")    \
    X(PAGE,     "page")    \
    X(WORDS,    "words")   \
    X(BASE,     "base")    \
    X(HEX,      "hex")     \
    X(DECIMAL,  "decimal") \
    X(MS,       "ms")      \
    X(TICKS,    "ticks")   \
    X(LED,      "led")     \
    X(CPUHZ,    "cpuhz")   \
    X(INCLUDE,  "include") \
    X(CHAR,     "char")    \
    X(BYE,      "bye")     \
    X(ABORT,    "abort")

/* immediate, and usable while interpreting as well as while compiling */
#define PRIM_IMM(X)        \
    X(PAREN,    "(")       \
    X(BACKSLASH, "\\")     \
    X(DOTQUOTE, ".\"")     \
    X(SQUOTE,   "s\"")     \
    X(BRCHAR,   "[char]")  \
    X(BRTICK,   "[']")     \
    X(LBRACKET, "[")

/* immediate and compile only */
#define PRIM_COMPILE(X)    \
    X(SEMI,     ";")       \
    X(IF,       "if")      \
    X(ELSE,     "else")    \
    X(THEN,     "then")    \
    X(BEGIN,    "begin")   \
    X(UNTIL,    "until")   \
    X(WHILE,    "while")   \
    X(REPEAT,   "repeat")  \
    X(AGAIN,    "again")   \
    X(DO,       "do")      \
    X(LOOP,     "loop")    \
    X(PLUSLOOP, "+loop")   \
    X(LEAVE,    "leave")   \
    X(RETURN,   "exit")    \
    X(RECURSE,  "recurse") \
    X(LITERAL,  "literal")

#define PRIM_LIST(X) PRIM_INTERNAL(X) PRIM_PLAIN(X) PRIM_IMM(X) PRIM_COMPILE(X)

#define X_ENUM(id, name) OP_##id,
#define X_NAME(id, name) name "\0"

enum { PRIM_LIST(X_ENUM) PRIM_COUNT };

static const char k_names[] = PRIM_LIST(X_NAME);

/* ------------------------------------------------------------- state */

static const freya_api_t *g;

static cell_t   s_ds[DS_CELLS];
static int      s_dsp;
static uint32_t s_rs[RS_CELLS];
static int      s_rsp;

static uint8_t  s_dict[DICT_SIZE] __attribute__((aligned(4)));
static uint16_t s_dp;                   /* first free byte of s_dict     */
static uint16_t s_latest;               /* newest header, or NO_WORD     */
static uint16_t s_defining;             /* header being compiled         */

static const uint16_t *s_ip;            /* the machine's program counter */

static const char *s_src;               /* current input line            */
static int      s_srclen;
static int      s_in;                   /* offset into it                */

static uint16_t s_leave[LEAVE_MAX];
static int      s_leavesp;

static int      s_state;                /* 0 interpreting, 1 compiling   */
static int      s_base;
static int      s_err;
static int      s_bye;
static int      s_depth;                /* include nesting               */
static uint32_t s_fuel;                 /* yields every so many tokens   */

static char     s_line[LINE_SIZE];
static char     s_pad[PAD_SIZE];

static void interpret(const char *line, int len);
static int  include_file(const char *path);

/* ------------------------------------------------------------ errors */

static void fail(const char *msg)
{
    if (!s_err)
        g->printf(" ? %s\r\n", msg);
    s_err = 1;
}

/* ------------------------------------------------------------ stacks */

static void push(cell_t v)
{
    if (s_dsp >= DS_CELLS) {
        fail("stack overflow");
        return;
    }
    s_ds[s_dsp++] = v;
}

static cell_t pop(void)
{
    if (s_dsp <= 0) {
        fail("stack underflow");
        return 0;
    }
    return s_ds[--s_dsp];
}

static void rpush(uint32_t v)
{
    if (s_rsp >= RS_CELLS) {
        fail("return stack overflow");
        return;
    }
    s_rs[s_rsp++] = v;
}

static uint32_t rpop(void)
{
    if (s_rsp <= 0) {
        fail("return stack underflow");
        return 0;
    }
    return s_rs[--s_rsp];
}

/* -------------------------------------------------------- dictionary */

static uint16_t *tok_at(uint16_t off)
{
    return (uint16_t *)(void *)&s_dict[off];
}

static int room(uint32_t n)
{
    if ((uint32_t)s_dp + n > DICT_SIZE) {
        fail("dictionary full");
        return 0;
    }
    return 1;
}

static void align_dp(uint16_t a)
{
    uint32_t v = ((uint32_t)s_dp + a - 1u) & ~(uint32_t)(a - 1u);

    if (v > DICT_SIZE) {
        fail("dictionary full");
        return;
    }
    s_dp = (uint16_t)v;
}

static void comma_tok(uint16_t t)
{
    align_dp(2);
    if (s_err || !room(2))
        return;
    *tok_at(s_dp) = t;
    s_dp = (uint16_t)(s_dp + 2);
}

static void comma_lit(cell_t v)
{
    comma_tok(OP_LIT);
    comma_tok((uint16_t)((ucell_t)v & 0xFFFFu));
    comma_tok((uint16_t)((ucell_t)v >> 16));
}

/* A definition's execution token is its body offset halved, which is why
 * every body starts on an even address. */
static cell_t body_xt(uint16_t body)
{
    return (cell_t)(PRIM_COUNT + body / 2u);
}

static const uint16_t *xt_body(cell_t xt)
{
    return tok_at((uint16_t)(((ucell_t)xt - PRIM_COUNT) * 2u));
}

static uint16_t hdr_body(uint16_t hdr)
{
    uint16_t n = (uint16_t)(s_dict[hdr + 2] & F_NAMELEN);

    return (uint16_t)((hdr + 3u + n + 1u) & ~1u);
}

/* Lays down link, flags and name and leaves s_dp at the body. */
static int create_header(const char *name, int len, int hidden)
{
    int i;

    if (len <= 0) {
        fail("name expected");
        return 0;
    }
    if (len > NAME_MAX)
        len = NAME_MAX;
    align_dp(2);
    if (s_err || !room((uint32_t)(4 + len)))
        return 0;

    s_defining = s_dp;
    *tok_at(s_dp) = s_latest;
    s_dp = (uint16_t)(s_dp + 2);
    s_dict[s_dp++] = (uint8_t)((unsigned)len | (hidden ? F_HIDDEN : 0u));
    for (i = 0; i < len; i++)
        s_dict[s_dp++] = (uint8_t)name[i];
    align_dp(2);
    s_latest = s_defining;
    return 1;
}

static int ci_eq(const char *a, const char *b, int n)
{
    while (n-- > 0) {
        char x = *a++, y = *b++;

        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return 1;
}

static int find_header(const char *w, int len)
{
    uint16_t h = s_latest;

    while (h != NO_WORD) {
        uint8_t meta = s_dict[h + 2];
        int n = meta & F_NAMELEN;

        if (!(meta & F_HIDDEN) && n == len &&
            ci_eq((const char *)&s_dict[h + 3], w, len))
            return (int)h;
        h = *tok_at(h);
    }
    return -1;
}

/* The nameless primitives are the ones the compiler emits; only the rest
 * are words anyone can type. */
static int count_named(void)
{
    const char *p = k_names;
    int idx, n = 0;

    for (idx = 0; idx < PRIM_COUNT; idx++) {
        int len = 0;

        while (p[len])
            len++;
        if (len)
            n++;
        p += len + 1;
    }
    return n;
}

static int find_prim(const char *w, int len)
{
    const char *p = k_names;
    int idx;

    for (idx = 0; idx < PRIM_COUNT; idx++) {
        int n = 0;

        while (p[n])
            n++;
        if (n == len && ci_eq(p, w, len))
            return idx;
        p += n + 1;
    }
    return -1;
}

/* Definitions shadow primitives, as in any Forth. */
static int find(const char *w, int len, int *immediate)
{
    int h = find_header(w, len);
    int t;

    if (h >= 0) {
        *immediate = (s_dict[h + 2] & F_IMMEDIATE) != 0;
        return (int)body_xt(hdr_body((uint16_t)h));
    }
    t = find_prim(w, len);
    if (t >= 0)
        *immediate = t >= OP_PAREN;
    return t;
}

/* ------------------------------------------------------------- input */

/* Returns the length of the text up to the next delimiter, and leaves the
 * input pointer past that delimiter.  A space stands for whitespace. */
static int parse_to(char delim, const char **wp)
{
    int start, len;

    if (delim == ' ')
        while (s_in < s_srclen && (unsigned char)s_src[s_in] <= ' ')
            s_in++;
    start = s_in;
    while (s_in < s_srclen && s_src[s_in] != delim &&
           !(delim == ' ' && (unsigned char)s_src[s_in] <= ' '))
        s_in++;
    *wp = s_src + start;
    len = s_in - start;
    if (s_in < s_srclen)
        s_in++;                 /* step over the delimiter itself */
    return len;
}

static int parse_name(const char **wp)
{
    return parse_to(' ', wp);
}

/* ------------------------------------------------------------ output */

static void type_out(const char *s, int n)
{
    while (n-- > 0)
        g->putc(*s++);
}

/* BASE is a variable a program can store anything into, and the core
 * traps a division by zero, so every use of it goes through here. */
static int cur_base(void)
{
    return (s_base < 2 || s_base > 36) ? 10 : s_base;
}

static void print_u(ucell_t v)
{
    ucell_t base = (ucell_t)cur_base();
    char buf[34];
    int n = 0;

    do {
        int d = (int)(v % base);

        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    while (n > 0)
        g->putc(buf[--n]);
}

static void print_cell(cell_t v)
{
    if (v < 0) {
        g->putc('-');
        print_u((ucell_t)(0u - (ucell_t)v));
    } else {
        print_u((ucell_t)v);
    }
}

static void wrap_out(const char *s, int len, int *col)
{
    if (*col + len + 1 > 72) {
        g->puts("\r\n");
        *col = 0;
    }
    type_out(s, len);
    g->putc(' ');
    *col += len + 1;
}

static void list_words(void)
{
    uint16_t h = s_latest;
    const char *p = k_names;
    int col = 0, n = 0, idx;

    while (h != NO_WORD) {
        int len = s_dict[h + 2] & F_NAMELEN;

        if (!(s_dict[h + 2] & F_HIDDEN)) {
            wrap_out((const char *)&s_dict[h + 3], len, &col);
            n++;
        }
        h = *tok_at(h);
    }
    for (idx = 0; idx < PRIM_COUNT; idx++) {
        int len = 0;

        while (p[len])
            len++;
        if (len) {
            wrap_out(p, len, &col);
            n++;
        }
        p += len + 1;
    }
    g->printf("\r\n%d words, %u bytes free\r\n",
              n, (unsigned)(DICT_SIZE - s_dp));
}

static void dump_mem(const uint8_t *a, cell_t len)
{
    cell_t i;

    for (i = 0; i < len; i += 8) {
        int j, n = (int)(len - i < 8 ? len - i : 8);

        g->printf("%p ", (const void *)(a + i));
        for (j = 0; j < 8; j++) {
            if (j < n) g->printf(" %02x", a[i + j]);
            else       g->puts("   ");
        }
        g->puts("  ");
        for (j = 0; j < n; j++) {
            uint8_t c = a[i + j];

            g->putc((c >= 32 && c < 127) ? (char)c : '.');
        }
        g->puts("\r\n");
        g->yield();
    }
}

/* ------------------------------------------------------------ numbers */

static int digit_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* Accepts the current base, $hex, #decimal, %binary and 'c' characters. */
static int number(const char *w, int len, cell_t *out)
{
    ucell_t v = 0;
    int b = cur_base(), neg = 0, i = 0, digits = 0;

    if (len == 3 && w[0] == '\'' && w[2] == '\'') {
        *out = (cell_t)(uint8_t)w[1];
        return 1;
    }
    if (len > 1) {
        if      (w[0] == '$') { b = 16; i = 1; }
        else if (w[0] == '#') { b = 10; i = 1; }
        else if (w[0] == '%') { b = 2;  i = 1; }
    }
    if (i < len && (w[i] == '-' || w[i] == '+')) {
        neg = w[i] == '-';
        i++;
    }
    for (; i < len; i++) {
        int d = digit_val(w[i]);

        if (d < 0 || d >= b)
            return 0;
        v = v * (ucell_t)b + (ucell_t)d;
        digits++;
    }
    if (!digits)
        return 0;
    *out = neg ? (cell_t)(0u - v) : (cell_t)v;
    return 1;
}

/* ------------------------------------------------- compiler helpers */

static void compile_xt(cell_t xt)
{
    comma_tok((uint16_t)xt);
}

static void compile_string(uint16_t op, const char *s, int len)
{
    int i;

    comma_tok(op);
    comma_tok((uint16_t)len);
    if (s_err || !room((uint32_t)((len + 1) & ~1)))
        return;
    for (i = 0; i < len; i++)
        s_dict[s_dp + i] = (uint8_t)s[i];
    if (len & 1)
        s_dict[s_dp + len] = 0;
    s_dp = (uint16_t)(s_dp + ((len + 1) & ~1));
}

/* Forward branches are compiled with an empty slot and patched later;
 * the slot's own offset lives on the data stack meanwhile. */
static void mark_forward(uint16_t op)
{
    comma_tok(op);
    align_dp(2);
    push((cell_t)s_dp);
    comma_tok(0);
}

static void resolve_forward(cell_t slot)
{
    if (slot < 0 || (ucell_t)slot >= DICT_SIZE) {
        fail("unstructured");
        return;
    }
    *tok_at((uint16_t)slot) = s_dp;
}

static void compile_backward(uint16_t op, cell_t dest)
{
    comma_tok(op);
    comma_tok((uint16_t)dest);
}

/* --------------------------------------------------------- the machine */

static void exec_xt(cell_t xt);

static void do_prim(uint16_t op)
{
    switch (op) {

    /* ---- compiled internals ---- */
    case OP_LIT: {
        ucell_t lo = *s_ip++;
        ucell_t hi = *s_ip++;

        push((cell_t)(lo | (hi << 16)));
        break;
    }
    case OP_BRANCH:
        s_ip = tok_at(*s_ip);
        break;
    case OP_ZBRANCH: {
        uint16_t t = *s_ip++;

        if (pop() == 0)
            s_ip = tok_at(t);
        break;
    }
    case OP_EXIT:
        s_ip = (const uint16_t *)(uintptr_t)rpop();
        break;
    case OP_DOVAR: {
        uintptr_t a = ((uintptr_t)s_ip + 3u) & ~(uintptr_t)3u;

        push((cell_t)a);
        s_ip = (const uint16_t *)(uintptr_t)rpop();
        break;
    }
    case OP_DOSTR: {
        uint16_t n = *s_ip++;

        push((cell_t)(uintptr_t)s_ip);
        push((cell_t)n);
        s_ip += (n + 1) / 2;
        break;
    }
    case OP_DOTSTR: {
        uint16_t n = *s_ip++;

        type_out((const char *)s_ip, n);
        s_ip += (n + 1) / 2;
        break;
    }
    case OP_XDO: {
        cell_t idx = pop(), lim = pop();

        rpush((uint32_t)lim);
        rpush((uint32_t)idx);
        break;
    }
    case OP_XLOOP: {
        uint16_t t = *s_ip++;
        cell_t idx;

        if (s_rsp < 2) {
            fail("loop outside do");
            break;
        }
        idx = (cell_t)s_rs[s_rsp - 1] + 1;
        if (idx == (cell_t)s_rs[s_rsp - 2]) {
            s_rsp -= 2;
        } else {
            s_rs[s_rsp - 1] = (uint32_t)idx;
            s_ip = tok_at(t);
        }
        break;
    }
    case OP_XPLOOP: {
        uint16_t t = *s_ip++;
        cell_t step = pop(), idx, lim;

        if (s_rsp < 2) {
            fail("loop outside do");
            break;
        }
        lim = (cell_t)s_rs[s_rsp - 2];
        idx = (cell_t)s_rs[s_rsp - 1] + step;
        if (step >= 0 ? idx >= lim : idx < lim) {
            s_rsp -= 2;
        } else {
            s_rs[s_rsp - 1] = (uint32_t)idx;
            s_ip = tok_at(t);
        }
        break;
    }

    /* ---- stack ---- */
    case OP_DUP: {
        cell_t a = pop();

        push(a);
        push(a);
        break;
    }
    case OP_DROP:
        (void)pop();
        break;
    case OP_SWAP: {
        cell_t b = pop(), a = pop();

        push(b);
        push(a);
        break;
    }
    case OP_OVER: {
        cell_t b = pop(), a = pop();

        push(a);
        push(b);
        push(a);
        break;
    }
    case OP_ROT: {
        cell_t c = pop(), b = pop(), a = pop();

        push(b);
        push(c);
        push(a);
        break;
    }
    case OP_NROT: {
        cell_t c = pop(), b = pop(), a = pop();

        push(c);
        push(a);
        push(b);
        break;
    }
    case OP_NIP: {
        cell_t b = pop();

        (void)pop();
        push(b);
        break;
    }
    case OP_TUCK: {
        cell_t b = pop(), a = pop();

        push(b);
        push(a);
        push(b);
        break;
    }
    case OP_QDUP:
        if (s_dsp > 0 && s_ds[s_dsp - 1] != 0)
            push(s_ds[s_dsp - 1]);
        break;
    case OP_DEPTH:
        push((cell_t)s_dsp);
        break;
    case OP_TWODUP: {
        cell_t b = pop(), a = pop();

        push(a);
        push(b);
        push(a);
        push(b);
        break;
    }
    case OP_TWODROP:
        (void)pop();
        (void)pop();
        break;
    case OP_TWOSWAP: {
        cell_t d = pop(), c = pop(), b = pop(), a = pop();

        push(c);
        push(d);
        push(a);
        push(b);
        break;
    }
    case OP_TWOOVER: {
        cell_t d = pop(), c = pop(), b = pop(), a = pop();

        push(a);
        push(b);
        push(c);
        push(d);
        push(a);
        push(b);
        break;
    }

    /* ---- return stack ---- */
    case OP_TOR:
        rpush((uint32_t)pop());
        break;
    case OP_FROMR:
        push((cell_t)rpop());
        break;
    case OP_RFETCH:
        push(s_rsp > 0 ? (cell_t)s_rs[s_rsp - 1] : 0);
        break;
    case OP_LOOPI:
        if (s_rsp < 1) fail("i outside do");
        else           push((cell_t)s_rs[s_rsp - 1]);
        break;
    case OP_LOOPJ:
        if (s_rsp < 3) fail("j outside two loops");
        else           push((cell_t)s_rs[s_rsp - 3]);
        break;
    case OP_UNLOOP:
        if (s_rsp < 2) fail("unloop outside do");
        else           s_rsp -= 2;
        break;

    /* ---- arithmetic ---- */
    case OP_ADD: {
        cell_t b = pop();

        push(pop() + b);
        break;
    }
    case OP_SUB: {
        cell_t b = pop();

        push(pop() - b);
        break;
    }
    case OP_MUL: {
        cell_t b = pop();

        push(pop() * b);
        break;
    }
    case OP_DIV: {
        cell_t b = pop(), a = pop();

        if (b == 0) fail("division by zero");
        else        push(a / b);
        break;
    }
    case OP_MOD: {
        cell_t b = pop(), a = pop();

        if (b == 0) fail("division by zero");
        else        push(a % b);
        break;
    }
    case OP_DIVMOD: {
        cell_t b = pop(), a = pop();

        if (b == 0) {
            fail("division by zero");
        } else {
            push(a % b);
            push(a / b);
        }
        break;
    }
    case OP_NEGATE:
        push(-pop());
        break;
    case OP_ABSVAL: {
        cell_t a = pop();

        push(a < 0 ? -a : a);
        break;
    }
    case OP_MINVAL: {
        cell_t b = pop(), a = pop();

        push(a < b ? a : b);
        break;
    }
    case OP_MAXVAL: {
        cell_t b = pop(), a = pop();

        push(a > b ? a : b);
        break;
    }
    case OP_INC:
        push(pop() + 1);
        break;
    case OP_DEC:
        push(pop() - 1);
        break;
    case OP_TWOMUL:
        push((cell_t)((ucell_t)pop() << 1));
        break;
    case OP_TWODIV:
        push(pop() >> 1);
        break;
    case OP_LSHIFT: {
        cell_t n = pop(), a = pop();

        push(n >= 0 && n < 32 ? (cell_t)((ucell_t)a << n) : 0);
        break;
    }
    case OP_RSHIFT: {
        cell_t n = pop(), a = pop();

        push(n >= 0 && n < 32 ? (cell_t)((ucell_t)a >> n) : 0);
        break;
    }
    case OP_AND: {
        cell_t b = pop();

        push(pop() & b);
        break;
    }
    case OP_OR: {
        cell_t b = pop();

        push(pop() | b);
        break;
    }
    case OP_XOR: {
        cell_t b = pop();

        push(pop() ^ b);
        break;
    }
    case OP_INVERT:
        push(~pop());
        break;

    /* ---- comparison, flags are 0 and -1 ---- */
    case OP_EQ: {
        cell_t b = pop();

        push(pop() == b ? -1 : 0);
        break;
    }
    case OP_NE: {
        cell_t b = pop();

        push(pop() != b ? -1 : 0);
        break;
    }
    case OP_LT: {
        cell_t b = pop();

        push(pop() < b ? -1 : 0);
        break;
    }
    case OP_GT: {
        cell_t b = pop();

        push(pop() > b ? -1 : 0);
        break;
    }
    case OP_ULT: {
        ucell_t b = (ucell_t)pop();

        push((ucell_t)pop() < b ? -1 : 0);
        break;
    }
    case OP_ZEQ:
        push(pop() == 0 ? -1 : 0);
        break;
    case OP_ZLT:
        push(pop() < 0 ? -1 : 0);
        break;
    case OP_ZGT:
        push(pop() > 0 ? -1 : 0);
        break;
    case OP_TRUEVAL:
        push(-1);
        break;
    case OP_FALSEVAL:
        push(0);
        break;
    case OP_BL:
        push(' ');
        break;
    case OP_CELLPLUS:
        push(pop() + 4);
        break;
    case OP_CELLS:
        push(pop() * 4);
        break;

    /* ---- memory ---- */
    case OP_FETCH:
        push(*(const cell_t *)(uintptr_t)pop());
        break;
    case OP_STORE: {
        cell_t a = pop(), v = pop();

        *(cell_t *)(uintptr_t)a = v;
        break;
    }
    case OP_CFETCH:
        push((cell_t)*(const uint8_t *)(uintptr_t)pop());
        break;
    case OP_CSTORE: {
        cell_t a = pop(), v = pop();

        *(uint8_t *)(uintptr_t)a = (uint8_t)v;
        break;
    }
    case OP_PLUSSTORE: {
        cell_t a = pop(), v = pop();

        *(cell_t *)(uintptr_t)a += v;
        break;
    }
    case OP_COMMA: {
        cell_t v = pop();

        align_dp(4);
        if (!s_err && room(4)) {
            *(cell_t *)(void *)&s_dict[s_dp] = v;
            s_dp = (uint16_t)(s_dp + 4);
        }
        break;
    }
    case OP_CCOMMA: {
        cell_t v = pop();

        if (room(1))
            s_dict[s_dp++] = (uint8_t)v;
        break;
    }
    case OP_HERE:
        push((cell_t)(uintptr_t)&s_dict[s_dp]);
        break;
    case OP_ALLOT: {
        cell_t n = pop();

        if (n < 0)
            s_dp = (uint16_t)(-n < (cell_t)s_dp ? (cell_t)s_dp + n : 0);
        else if (room((uint32_t)n))
            s_dp = (uint16_t)(s_dp + n);
        break;
    }
    case OP_ALIGN:
        align_dp(4);
        break;
    case OP_UNUSED:
        push((cell_t)(DICT_SIZE - s_dp));
        break;
    case OP_MOVE: {
        cell_t n = pop();
        uint8_t *d = (uint8_t *)(uintptr_t)pop();
        const uint8_t *s = (const uint8_t *)(uintptr_t)pop();

        if (d < s) while (n-- > 0) *d++ = *s++;
        else       while (n-- > 0) d[n] = s[n];
        break;
    }
    case OP_FILL: {
        cell_t c = pop(), n = pop();
        uint8_t *d = (uint8_t *)(uintptr_t)pop();

        while (n-- > 0)
            *d++ = (uint8_t)c;
        break;
    }

    /* ---- defining ---- */
    case OP_COLON: {
        const char *w;
        int len = parse_name(&w);

        if (create_header(w, len, 1))
            s_state = 1;
        break;
    }
    case OP_VARIABLE: {
        const char *w;
        int len = parse_name(&w);

        if (!create_header(w, len, 0))
            break;
        comma_tok(OP_DOVAR);
        align_dp(4);
        if (!s_err && room(4)) {
            *(cell_t *)(void *)&s_dict[s_dp] = 0;
            s_dp = (uint16_t)(s_dp + 4);
        }
        break;
    }
    case OP_CONSTANT: {
        const char *w;
        cell_t v = pop();
        int len = parse_name(&w);

        if (!create_header(w, len, 0))
            break;
        comma_lit(v);
        comma_tok(OP_EXIT);
        break;
    }
    case OP_CREATE: {
        const char *w;
        int len = parse_name(&w);

        if (!create_header(w, len, 0))
            break;
        comma_tok(OP_DOVAR);
        align_dp(4);
        break;
    }
    case OP_TICK: {
        const char *w;
        int len = parse_name(&w), imm, xt;

        xt = find(w, len, &imm);
        if (xt < 0) fail("undefined word");
        else        push((cell_t)xt);
        break;
    }
    case OP_EXECUTE: {
        cell_t xt = pop();

        if (xt < 0 || (xt >= PRIM_COUNT &&
                       ((ucell_t)xt - PRIM_COUNT) * 2u >= s_dp))
            fail("bad execution token");
        else
            exec_xt(xt);
        break;
    }
    case OP_IMMEDIATE:
        if (s_latest == NO_WORD) fail("no definition");
        else                     s_dict[s_latest + 2] |= F_IMMEDIATE;
        break;
    case OP_FORGET: {
        const char *w;
        int len = parse_name(&w);
        int h = find_header(w, len);

        if (h < 0) {
            fail("undefined word");
        } else {
            s_latest = *tok_at((uint16_t)h);
            s_dp = (uint16_t)h;
        }
        break;
    }
    case OP_RBRACKET:
        s_state = 1;
        break;

    /* ---- console ---- */
    case OP_EMIT:
        g->putc((char)pop());
        break;
    case OP_KEY: {
        int c = g->getc();

        if (c < 0) {
            s_bye = 1;
            c = 0;
        }
        push((cell_t)c);
        break;
    }
    case OP_KEYQ:
        push(g->kbhit() ? -1 : 0);
        break;
    case OP_DOT:
        print_cell(pop());
        g->putc(' ');
        break;
    case OP_UDOT:
        print_u((ucell_t)pop());
        g->putc(' ');
        break;
    case OP_DOTS: {
        int i;

        g->printf("<%d> ", s_dsp);
        for (i = 0; i < s_dsp; i++) {
            print_cell(s_ds[i]);
            g->putc(' ');
        }
        g->puts("\r\n");
        break;
    }
    case OP_CR:
        g->puts("\r\n");
        break;
    case OP_SPACE:
        g->putc(' ');
        break;
    case OP_SPACES: {
        cell_t n = pop();

        while (n-- > 0)
            g->putc(' ');
        break;
    }
    case OP_TYPE: {
        cell_t n = pop();
        const char *s = (const char *)(uintptr_t)pop();

        type_out(s, (int)n);
        break;
    }
    case OP_DUMP: {
        cell_t n = pop();

        dump_mem((const uint8_t *)(uintptr_t)pop(), n);
        break;
    }
    case OP_PAGE:
        g->puts("\x1b[2J\x1b[H");
        break;
    case OP_WORDS:
        list_words();
        break;

    /* ---- number base ---- */
    case OP_BASE:
        push((cell_t)(uintptr_t)&s_base);
        break;
    case OP_HEX:
        s_base = 16;
        break;
    case OP_DECIMAL:
        s_base = 10;
        break;

    /* ---- the board ---- */
    case OP_MS:
        g->delay_ms((uint32_t)pop());
        break;
    case OP_TICKS:
        push((cell_t)g->ticks_ms());
        break;
    case OP_LED:
        g->led(pop() != 0);
        break;
    case OP_CPUHZ:
        push((cell_t)g->cpu_hz());
        break;
    case OP_INCLUDE: {
        const char *w;
        int len = parse_name(&w), i;
        char path[64];

        if (len <= 0 || len >= (int)sizeof path) {
            fail("file name expected");
            break;
        }
        for (i = 0; i < len; i++)
            path[i] = w[i];
        path[len] = '\0';
        (void)include_file(path);
        break;
    }
    case OP_CHAR: {
        const char *w;
        int len = parse_name(&w);

        if (len <= 0) fail("character expected");
        else          push((cell_t)(uint8_t)w[0]);
        break;
    }
    case OP_BYE:
        s_bye = 1;
        break;
    case OP_ABORT:
        s_dsp = 0;
        s_err = 1;
        break;

    /* ---- immediate, usable in either state ---- */
    case OP_PAREN: {
        const char *w;

        (void)parse_to(')', &w);
        break;
    }
    case OP_BACKSLASH:
        s_in = s_srclen;
        break;
    case OP_DOTQUOTE: {
        const char *w;
        int len = parse_to('"', &w);

        if (s_state) compile_string(OP_DOTSTR, w, len);
        else         type_out(w, len);
        break;
    }
    case OP_SQUOTE: {
        const char *w;
        int len = parse_to('"', &w);

        if (s_state) {
            compile_string(OP_DOSTR, w, len);
        } else {
            int i;

            if (len > PAD_SIZE)
                len = PAD_SIZE;
            for (i = 0; i < len; i++)
                s_pad[i] = w[i];
            push((cell_t)(uintptr_t)s_pad);
            push((cell_t)len);
        }
        break;
    }
    case OP_BRCHAR: {
        const char *w;
        int len = parse_name(&w);

        if (len <= 0) fail("character expected");
        else          comma_lit((cell_t)(uint8_t)w[0]);
        break;
    }
    case OP_BRTICK: {
        const char *w;
        int len = parse_name(&w), imm = 0, xt;

        xt = find(w, len, &imm);
        if (xt < 0) fail("undefined word");
        else        comma_lit((cell_t)xt);
        break;
    }
    case OP_LBRACKET:
        s_state = 0;
        break;

    /* ---- immediate and compile only ---- */
    case OP_SEMI:
        comma_tok(OP_EXIT);
        if (s_defining != NO_WORD)
            s_dict[s_defining + 2] &= (uint8_t)~F_HIDDEN;
        s_state = 0;
        break;
    case OP_IF:
        mark_forward(OP_ZBRANCH);
        break;
    case OP_ELSE: {
        cell_t slot = pop();

        mark_forward(OP_BRANCH);    /* leaves its own slot for THEN */
        resolve_forward(slot);      /* the IF jumps here, past that branch */
        break;
    }
    case OP_THEN:
        resolve_forward(pop());
        break;
    case OP_BEGIN:
        align_dp(2);
        push((cell_t)s_dp);
        break;
    case OP_UNTIL:
        compile_backward(OP_ZBRANCH, pop());
        break;
    case OP_WHILE:
        mark_forward(OP_ZBRANCH);
        break;
    case OP_REPEAT: {
        cell_t slot = pop(), dest = pop();

        compile_backward(OP_BRANCH, dest);
        resolve_forward(slot);
        break;
    }
    case OP_AGAIN:
        compile_backward(OP_BRANCH, pop());
        break;
    case OP_DO:
        comma_tok(OP_XDO);
        align_dp(2);
        push((cell_t)s_leavesp);
        push((cell_t)s_dp);
        break;
    case OP_LOOP:
    case OP_PLUSLOOP: {
        cell_t dest = pop(), base = pop();

        compile_backward(op == OP_LOOP ? OP_XLOOP : OP_XPLOOP, dest);
        if (base < 0 || base > s_leavesp) {
            fail("unstructured");
            break;
        }
        while (s_leavesp > base)
            *tok_at(s_leave[--s_leavesp]) = s_dp;
        break;
    }
    case OP_LEAVE:
        if (s_leavesp >= LEAVE_MAX) {
            fail("too many leaves");
            break;
        }
        comma_tok(OP_UNLOOP);
        comma_tok(OP_BRANCH);
        align_dp(2);
        s_leave[s_leavesp++] = s_dp;
        comma_tok(0);
        break;
    case OP_RETURN:
        comma_tok(OP_EXIT);
        break;
    case OP_RECURSE:
        if (s_defining == NO_WORD) fail("no definition");
        else comma_tok((uint16_t)body_xt(hdr_body(s_defining)));
        break;
    case OP_LITERAL:
        comma_lit(pop());
        break;

    default:
        fail("bad token");
        break;
    }
}

/*
 * Runs one execution token to completion.  A primitive is just a call; a
 * definition pushes the caller's program counter and runs until the EXIT
 * that pops it back, which is also how a nested call returns.
 */
static void exec_xt(cell_t xt)
{
    const uint16_t *saved = s_ip;
    int base = s_rsp;

    if (xt < PRIM_COUNT) {
        do_prim((uint16_t)xt);
        return;
    }

    rpush((uint32_t)(uintptr_t)s_ip);
    s_ip = xt_body(xt);

    while (s_rsp > base && !s_err && !s_bye) {
        uint16_t t = *s_ip++;

        if (t < PRIM_COUNT) {
            do_prim(t);
        } else {
            rpush((uint32_t)(uintptr_t)s_ip);
            s_ip = xt_body((cell_t)t);
        }
        /* Ctrl-C reaches even 'begin again': yield aborts the program. */
        if ((++s_fuel & 0x3FFu) == 0)
            g->yield();
    }

    s_rsp = base;
    s_ip = saved;
}

/* ------------------------------------------------- outer interpreter */

static void interpret(const char *line, int len)
{
    const char *save_src = s_src;
    int save_len = s_srclen, save_in = s_in;

    s_src = line;
    s_srclen = len;
    s_in = 0;

    while (!s_err && !s_bye) {
        const char *w;
        int wlen = parse_name(&w);
        int imm = 0, xt;
        cell_t n;

        if (wlen == 0)
            break;

        xt = find(w, wlen, &imm);
        if (xt >= 0) {
            if (s_state && !imm)
                compile_xt(xt);
            else if (!s_state && xt >= OP_SEMI && xt < PRIM_COUNT)
                fail("compile only");
            else
                exec_xt(xt);
        } else if (number(w, wlen, &n)) {
            if (s_state) comma_lit(n);
            else         push(n);
        } else {
            g->printf(" ? %.*s\r\n", wlen, w);
            s_err = 1;
        }
    }

    s_src = save_src;
    s_srclen = save_len;
    s_in = save_in;
}

/*
 * A source file is the console by another name: read it a line at a time
 * and hand each line to the same interpreter.
 */
static int include_file(const char *path)
{
    char line[LINE_SIZE];
    char buf[32];
    int fd, n, i, used = 0, lineno = 1;

    if (s_depth >= INCLUDE_MAX) {
        fail("include nested too deep");
        return -1;
    }
    fd = g->open(path, FREYA_O_RDONLY);
    if (fd < 0) {
        g->printf(" ? cannot open %s\r\n", path);
        s_err = 1;
        return -1;
    }
    s_depth++;

    while (!s_err && !s_bye && (n = g->read(fd, buf, (int)sizeof buf)) > 0) {
        for (i = 0; i < n && !s_err && !s_bye; i++) {
            char c = buf[i];

            if (c == '\n' || c == '\r') {
                if (used || c == '\n') {
                    interpret(line, used);
                    used = 0;
                }
                if (c == '\n')
                    lineno++;
            } else if (used < (int)sizeof line) {
                line[used++] = c;
            }
        }
    }
    if (!s_err && !s_bye && used)
        interpret(line, used);

    g->close(fd);
    s_depth--;
    if (s_err)
        g->printf("   in %s line %d\r\n", path, lineno);
    return s_err ? -1 : 0;
}

/* --------------------------------------------------------------- repl */

static void recover(void)
{
    /* A definition interrupted by an error is rolled back rather than
     * left half compiled in a dictionary this small. */
    if (s_state && s_defining != NO_WORD) {
        s_latest = *tok_at(s_defining);
        s_dp = s_defining;
        s_defining = NO_WORD;
        g->puts("   definition abandoned\r\n");
    }
    s_dsp = 0;
    s_rsp = 0;
    s_leavesp = 0;
    s_state = 0;
    s_err = 0;
}

static int read_line(char *buf, int max)
{
    int n = 0;

    for (;;) {
        int c = g->getc();

        if (c < 0)
            return -1;
        if (c == '\r' || c == '\n') {
            g->puts("\r\n");
            return n;
        }
        if (c == 0x08 || c == 0x7F) {
            if (n > 0) {
                n--;
                g->puts("\b \b");
            }
            continue;
        }
        if (c == 0x15) {                    /* Ctrl-U */
            while (n > 0) {
                n--;
                g->puts("\b \b");
            }
            continue;
        }
        if (c < 32 || c > 126)
            continue;
        if (n < max) {
            buf[n++] = (char)c;
            g->putc((char)c);
        }
    }
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int i;

    g = api;
    s_latest = NO_WORD;
    s_defining = NO_WORD;
    s_base = 10;

    g->printf("forth: %d words, %u byte dictionary, 32-bit cells\r\n",
              count_named(), (unsigned)DICT_SIZE);
    g->puts("  'words' lists them, 'bye' or Ctrl-C leaves\r\n");

    for (i = 1; i < argc && !s_bye && !g->should_stop(); i++) {
        g->printf("including %s\r\n", argv[i]);
        if (include_file(argv[i]) < 0)
            recover();
    }

    while (!s_bye && !g->should_stop()) {
        int n;

        g->puts(s_state ? "   ... " : "forth> ");
        n = read_line(s_line, LINE_SIZE);
        if (n < 0)
            break;

        interpret(s_line, n);

        if (s_err)
            recover();
        else if (s_state)
            g->puts("  compiling\r\n");
        else if (s_dsp)
            g->printf("  ok (%d)\r\n", s_dsp);
        else
            g->puts("  ok\r\n");
    }

    g->printf("forth: %u of %u dictionary bytes used\r\n",
              (unsigned)s_dp, (unsigned)DICT_SIZE);
    return 0;
}
