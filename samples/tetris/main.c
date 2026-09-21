/*
 * tetris - console Tetris for Freya.
 *
 * Draws a 10x20 well on the serial terminal (ANSI / VT100) and reads
 * single keypresses through the service table.  Keys are hardcoded; see
 * samples/tetris/README.md.  Ctrl-C still belongs to Freya and stops the
 * program.
 *
 *     run tetris.bin
 *     runflash                 (Blue Pill, after install / PROGRAM=tetris)
 */
#include "freya_api.h"

#define WELL_W     10
#define WELL_H     20
#define N_PIECES   7
#define N_KICKS    5

#define COL_WELL   3
#define ROW_WELL   3

static const freya_api_t *g;

/* Seven tetrominoes, four rotations, packed as four rows of four bits.
 * Row 0 is the high nibble of the high byte. */
static const uint16_t k_shape[N_PIECES][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 }, /* I */
    { 0x0660, 0x0660, 0x0660, 0x0660 }, /* O */
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 }, /* T */
    { 0x06C0, 0x8C40, 0x6C00, 0x4C80 }, /* S */
    { 0x0C60, 0x4C80, 0xC600, 0x2640 }, /* Z */
    { 0x08E0, 0x6440, 0x0E20, 0x44C0 }, /* J */
    { 0x02E0, 0x4460, 0x0E80, 0xC440 }, /* L */
};

static const int8_t k_kick[N_KICKS][2] = {
    { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { -2, 0 },
};

/* ANSI foreground: empty, I, O, T, S, Z, J, L */
static const char *const k_color[8] = {
    "0", "96", "93", "95", "92", "91", "94", "33",
};

static uint8_t  s_well[WELL_H][WELL_W];
static uint8_t  s_cur, s_rot, s_next;
static int8_t   s_px, s_py;
static uint8_t  s_bag[N_PIECES];
static uint8_t  s_bag_n;
static uint32_t s_rng;
static uint32_t s_score, s_lines, s_level;
static uint32_t s_fall_at;
static uint8_t  s_paused, s_over, s_quit, s_dirty;
static uint8_t  s_started;

static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static void shuffle_bag(void)
{
    int i, j;
    uint8_t t;

    for (i = 0; i < N_PIECES; i++)
        s_bag[i] = (uint8_t)i;
    for (i = N_PIECES - 1; i > 0; i--) {
        j = (int)(rnd() % (uint32_t)(i + 1));
        t = s_bag[i];
        s_bag[i] = s_bag[j];
        s_bag[j] = t;
    }
    s_bag_n = N_PIECES;
}

static uint8_t take_piece(void)
{
    if (s_bag_n == 0)
        shuffle_bag();
    return s_bag[--s_bag_n];
}

static int cell_on(uint8_t piece, uint8_t rot, int cx, int cy)
{
    uint16_t bits = k_shape[piece][rot & 3u];
    uint8_t  row  = (uint8_t)((bits >> (12 - 4 * cy)) & 0xFu);
    return (row >> (3 - cx)) & 1;
}

static int fits(uint8_t piece, uint8_t rot, int x, int y)
{
    int cx, cy;

    for (cy = 0; cy < 4; cy++) {
        for (cx = 0; cx < 4; cx++) {
            int wx, wy;

            if (!cell_on(piece, rot, cx, cy))
                continue;
            wx = x + cx;
            wy = y + cy;
            if (wx < 0 || wx >= WELL_W || wy >= WELL_H)
                return 0;
            if (wy >= 0 && s_well[wy][wx])
                return 0;
        }
    }
    return 1;
}

static int ghost_y(void)
{
    int y = s_py;

    while (fits(s_cur, s_rot, s_px, y + 1))
        y++;
    return y;
}

static void stamp(void)
{
    int cx, cy;

    for (cy = 0; cy < 4; cy++) {
        for (cx = 0; cx < 4; cx++) {
            int wx, wy;

            if (!cell_on(s_cur, s_rot, cx, cy))
                continue;
            wx = s_px + cx;
            wy = s_py + cy;
            if (wy >= 0 && wy < WELL_H && wx >= 0 && wx < WELL_W)
                s_well[wy][wx] = (uint8_t)(s_cur + 1u);
        }
    }
}

static uint32_t gravity_ms(void)
{
    uint32_t ms = 800u - s_level * 70u;

    if (ms > 800u)
        ms = 80u;
    if (ms < 80u)
        ms = 80u;
    return ms;
}

static int spawn(void)
{
    s_cur = s_next;
    s_next = take_piece();
    s_rot = 0;
    s_px  = 3;
    s_py  = 0;
    if (!fits(s_cur, s_rot, s_px, s_py)) {
        s_py = -1;
        if (!fits(s_cur, s_rot, s_px, s_py))
            return 0;
    }
    s_fall_at = g->ticks_ms() + gravity_ms();
    return 1;
}

static void clear_lines(void)
{
    int y, x, dest, cleared = 0;

    dest = WELL_H - 1;
    for (y = WELL_H - 1; y >= 0; y--) {
        int full = 1;
        for (x = 0; x < WELL_W; x++) {
            if (!s_well[y][x]) {
                full = 0;
                break;
            }
        }
        if (full) {
            cleared++;
            continue;
        }
        if (dest != y) {
            for (x = 0; x < WELL_W; x++)
                s_well[dest][x] = s_well[y][x];
        }
        dest--;
    }
    while (dest >= 0) {
        for (x = 0; x < WELL_W; x++)
            s_well[dest][x] = 0;
        dest--;
    }

    if (cleared) {
        static const uint32_t k_pts[5] = { 0, 100, 300, 500, 800 };
        s_lines += (uint32_t)cleared;
        s_score += k_pts[cleared] * (s_level + 1u);
        s_level  = s_lines / 10u;
    }
}

static void lock_piece(void)
{
    stamp();
    clear_lines();
    if (!spawn())
        s_over = 1;
    s_dirty = 1;
}

static int try_move(int dx, int dy)
{
    if (!fits(s_cur, s_rot, s_px + dx, s_py + dy))
        return 0;
    s_px = (int8_t)(s_px + dx);
    s_py = (int8_t)(s_py + dy);
    s_dirty = 1;
    return 1;
}

static void try_rotate(void)
{
    uint8_t nrot = (uint8_t)((s_rot + 1u) & 3u);
    int k;

    for (k = 0; k < N_KICKS; k++) {
        int nx = s_px + k_kick[k][0];
        int ny = s_py + k_kick[k][1];
        if (fits(s_cur, nrot, nx, ny)) {
            s_rot = nrot;
            s_px  = (int8_t)nx;
            s_py  = (int8_t)ny;
            s_dirty = 1;
            return;
        }
    }
}

static void hard_drop(void)
{
    int n = 0;

    while (try_move(0, 1))
        n++;
    s_score += (uint32_t)n * 2u;
    lock_piece();
}

static void reset_game(void)
{
    int x, y;

    for (y = 0; y < WELL_H; y++) {
        for (x = 0; x < WELL_W; x++)
            s_well[y][x] = 0;
    }
    s_score  = 0;
    s_lines  = 0;
    s_level  = 0;
    s_paused = 0;
    s_over   = 0;
    s_bag_n  = 0;
    s_rng   ^= g->ticks_ms() + 1u;
    shuffle_bag();
    s_next = take_piece();
    spawn();
    s_dirty  = 1;
    s_started = 1;
}

static void go(int row, int col)
{
    g->printf("\x1b[%d;%dH", row, col);
}

static void paint_cell(uint8_t kind, int ghost)
{
    if (kind == 0) {
        g->puts(ghost ? "\x1b[2;37m::\x1b[0m" : "  ");
        return;
    }
    g->printf("\x1b[%sm[]\x1b[0m", k_color[kind]);
}

static int occupied_by_cur(int x, int y)
{
    int cx = x - s_px;
    int cy = y - s_py;

    if (cx < 0 || cx > 3 || cy < 0 || cy > 3)
        return 0;
    return cell_on(s_cur, s_rot, cx, cy);
}

static void draw_well(void)
{
    int x, y;
    int gy = (!s_over && s_started) ? ghost_y() : s_py;

    for (y = 0; y < WELL_H; y++) {
        go(ROW_WELL + y, COL_WELL);
        g->puts("\x1b[90m|\x1b[0m");
        for (x = 0; x < WELL_W; x++) {
            uint8_t kind = s_well[y][x];
            int ghost = 0;

            if (!s_over && occupied_by_cur(x, y))
                kind = (uint8_t)(s_cur + 1u);
            else if (!kind && !s_over && occupied_by_cur(x, y - (gy - s_py)))
                ghost = 1;
            paint_cell(kind, ghost);
        }
        g->puts("\x1b[90m|\x1b[0m");
    }
    go(ROW_WELL + WELL_H, COL_WELL);
    g->puts("\x1b[90m+");
    for (x = 0; x < WELL_W; x++)
        g->puts("--");
    g->puts("+\x1b[0m");
}

static void draw_next(void)
{
    int cx, cy;
    int row = ROW_WELL + 8;
    int col = COL_WELL + WELL_W * 2 + 6;

    go(row - 1, col);
    g->puts("next");
    for (cy = 0; cy < 4; cy++) {
        go(row + cy, col);
        for (cx = 0; cx < 4; cx++)
            paint_cell(cell_on(s_next, 0, cx, cy) ? (uint8_t)(s_next + 1u) : 0, 0);
    }
}

static void draw_hud(void)
{
    int col = COL_WELL + WELL_W * 2 + 6;

    go(1, COL_WELL);
    g->puts("\x1b[1mTETRIS\x1b[0m");

    go(ROW_WELL, col);
    g->printf("score  %u     ", s_score);
    go(ROW_WELL + 1, col);
    g->printf("lines  %u     ", s_lines);
    go(ROW_WELL + 2, col);
    g->printf("level  %u     ", s_level + 1u);

    draw_next();

    go(ROW_WELL + 14, col);
    g->puts("A / D   move");
    go(ROW_WELL + 15, col);
    g->puts("W       rotate");
    go(ROW_WELL + 16, col);
    g->puts("S       soft drop");
    go(ROW_WELL + 17, col);
    g->puts("SPACE   hard drop");
    go(ROW_WELL + 18, col);
    g->puts("P       pause");
    go(ROW_WELL + 19, col);
    g->puts("R       restart");
    go(ROW_WELL + 20, col);
    g->puts("Q       quit");
    go(ROW_WELL + 21, col);
    g->puts("Ctrl-C  stop");

    go(ROW_WELL + WELL_H + 2, COL_WELL);
    if (s_over)
        g->puts("\x1b[1;91mgame over\x1b[0m  R restart, Q quit        ");
    else if (s_paused)
        g->puts("\x1b[1;93mpaused\x1b[0m     P resume                 ");
    else
        g->puts("                                          ");
}

static void draw(void)
{
    draw_well();
    draw_hud();
    s_dirty = 0;
}

static void drain_escape(void)
{
    int a = g->getc_timeout(20);
    if (a == '[')
        (void)g->getc_timeout(20);
}

static void handle_key(int c)
{
    if (c < 0)
        return;
    if (c == 0x1b) {
        drain_escape();
        return;
    }
    if (c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';

    if (c == 'q') {
        s_quit = 1;
        return;
    }
    if (c == 'r') {
        reset_game();
        return;
    }
    if (c == 'p') {
        if (!s_over) {
            s_paused = (uint8_t)!s_paused;
            if (!s_paused)
                s_fall_at = g->ticks_ms() + gravity_ms();
            s_dirty = 1;
        }
        return;
    }

    if (s_paused || s_over)
        return;

    if (c == 'a') {
        try_move(-1, 0);
    } else if (c == 'd') {
        try_move(1, 0);
    } else if (c == 's') {
        if (try_move(0, 1)) {
            s_score += 1u;
            s_fall_at = g->ticks_ms() + gravity_ms();
        } else {
            lock_piece();
        }
    } else if (c == 'w') {
        try_rotate();
    } else if (c == ' ') {
        hard_drop();
    }
}

static void restore_term(void)
{
    g->puts("\x1b[0m\x1b[?25h\x1b[2J\x1b[H");
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g = api;

    g->puts("\x1b[?25l\x1b[2J\x1b[H");
    reset_game();
    draw();

    while (!s_quit && !g->should_stop()) {
        uint32_t now = g->ticks_ms();
        uint32_t wait = 16;
        int c;

        if (!s_paused && !s_over) {
            if (now >= s_fall_at)
                wait = 0;
            else if (s_fall_at - now < wait)
                wait = s_fall_at - now;
        }

        c = g->getc_timeout(wait);
        if (c >= 0)
            handle_key(c);
        while (g->kbhit()) {
            c = g->getc();
            if (c < 0)
                break;
            handle_key(c);
        }

        now = g->ticks_ms();
        if (!s_paused && !s_over && now >= s_fall_at) {
            if (!try_move(0, 1))
                lock_piece();
            else
                s_fall_at = now + gravity_ms();
        }

        if (s_dirty)
            draw();
        g->yield();
    }

    restore_term();
    g->printf("tetris: score %u, lines %u, level %u\r\n",
              s_score, s_lines, s_level + 1u);
    return 0;
}
