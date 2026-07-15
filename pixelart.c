/*
 * Terminal Space Invaders
 * Controls: a/d or left/right arrows to move, space to shoot, q to quit.
 * No external dependencies - raw termios + ANSI escapes only.
 * Requires a 256-color terminal for the btop-style panel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>

#define W 60
#define H 24
#define FIELD_TOP 2
#define FIELD_BOTTOM (H - 2)
#define ROWS 5
#define COLS 10
#define SPACING_X 5
#define START_X 6
#define MAX_EBULLETS 6
#define TICK_USEC 50000

#define SHIELD_COUNT 4
#define SHIELD_W 5
#define SHIELD_H 3

#define BORDER_COL "\x1b[38;5;51m"
#define RESET "\x1b[0m"

static const char SHIELD_SHAPE[SHIELD_H][SHIELD_W + 1] = {
    " ### ",
    "#####",
    "## ##",
};

typedef enum {
    COL_NONE = 0,
    COL_INVADER0, COL_INVADER1, COL_INVADER2, COL_INVADER3, COL_INVADER4,
    COL_PLAYER, COL_PBULLET, COL_EBULLET,
    COL_SHIELD_OK, COL_SHIELD_DMG, COL_SHIELD_CRIT,
    COL_LABEL, COL_SCORE_VAL, COL_LIVES_VAL,
    COL_COUNT
} ColorCode;

static const char *PALETTE[COL_COUNT] = {
    [COL_NONE]       = "",
    [COL_INVADER0]   = "\x1b[38;5;45m",
    [COL_INVADER1]   = "\x1b[38;5;51m",
    [COL_INVADER2]   = "\x1b[38;5;46m",
    [COL_INVADER3]   = "\x1b[38;5;226m",
    [COL_INVADER4]   = "\x1b[38;5;203m",
    [COL_PLAYER]     = "\x1b[1;38;5;46m",
    [COL_PBULLET]    = "\x1b[38;5;226m",
    [COL_EBULLET]    = "\x1b[1;38;5;196m",
    [COL_SHIELD_OK]  = "\x1b[38;5;46m",
    [COL_SHIELD_DMG] = "\x1b[38;5;226m",
    [COL_SHIELD_CRIT]= "\x1b[38;5;196m",
    [COL_LABEL]      = "\x1b[38;5;250m",
    [COL_SCORE_VAL]  = "\x1b[1;38;5;46m",
    [COL_LIVES_VAL]  = "\x1b[1;38;5;196m",
};

static const ColorCode INVADER_ROW_COLOR[ROWS] = {
    COL_INVADER0, COL_INVADER1, COL_INVADER2, COL_INVADER3, COL_INVADER4
};

/* Glyph codes >= 200 are multi-byte UTF-8 block-graphics; anything below is
 * used as a literal ASCII character. This lets half-block "pixel" sprites
 * (invaders, player ship) pack 2 vertical sub-pixels into a single terminal
 * cell via the upper/lower/full block characters. */
#define GLYPH_FULL    200
#define GLYPH_UPPER   201
#define GLYPH_LOWER   202
#define GLYPH_PBULLET 203
#define GLYPH_EBULLET 204

static const char *SPECIAL_GLYPH[5] = {
    "\xe2\x96\x88", /* full block  █ */
    "\xe2\x96\x80", /* upper half  ▀ */
    "\xe2\x96\x84", /* lower half  ▄ */
    "\xe2\x96\xb2", /* up triangle ▲ (player bullet) */
    "\xe2\x96\xbc", /* down triangle ▼ (enemy bullet) */
};

/* 3-wide x 2-(sub)row pixel sprites, packed into one terminal row via
 * half-block characters. '#' = lit pixel, '.' = empty. */
#define SPRITE_W 3
static const char *INVADER_TOP = "#.#";
static const char *INVADER_BOT = "###";
static const char *SHIP_TOP    = ".#.";
static const char *SHIP_BOT    = "###";

static struct termios orig_termios;
static int raw_mode_active = 0;

static void restore_terminal(void) {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\x1b[?25h" RESET "\n");
        fflush(stdout);
        raw_mode_active = 0;
    }
}

static void handle_signal(int sig) {
    (void)sig;
    restore_terminal();
    _exit(0);
}

static void enable_raw_mode(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = 1;

    printf("\x1b[?25l\x1b[2J");
    fflush(stdout);
}

static int esc_state = 0;

static int poll_input(void) {
    unsigned char c;
    int result = 0;
    ssize_t n;
    while ((n = read(STDIN_FILENO, &c, 1)) > 0) {
        if (esc_state == 0) {
            if (c == 27) { esc_state = 1; continue; }
            if (c == 'q' || c == 'Q') result = 'q';
            else if (c == 'a' || c == 'A') result = 'a';
            else if (c == 'd' || c == 'D') result = 'd';
            else if (c == ' ') result = ' ';
        } else if (esc_state == 1) {
            esc_state = (c == '[') ? 2 : 0;
        } else if (esc_state == 2) {
            if (c == 'C') result = 'd';
            else if (c == 'D') result = 'a';
            esc_state = 0;
        }
    }
    return result;
}

typedef struct {
    int alive[ROWS][COLS];
    int dx, dy;
    int dir;
    int alive_count;
} Invaders;

typedef struct {
    int x, y, active;
} Bullet;

typedef struct {
    int x0, y0;
    int alive[SHIELD_H][SHIELD_W];
} Shield;

static unsigned char grid[H][W];
static unsigned char colorgrid[H][W];

static void plot(int x, int y, int glyph, ColorCode col) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    grid[y][x] = (unsigned char)glyph;
    colorgrid[y][x] = (unsigned char)col;
}

static int plot_str(int x, int y, const char *s, ColorCode col) {
    for (; *s; s++, x++) plot(x, y, (unsigned char)*s, col);
    return x;
}

/* Draws a SPRITE_W-wide sprite (as pat_top/pat_bot half-block pixel rows)
 * centered on (cx, cy), using one terminal cell's upper/lower half per
 * column. */
static void draw_sprite(int cx, int cy, const char *pat_top, const char *pat_bot, ColorCode col) {
    int half = SPRITE_W / 2;
    for (int i = 0; i < SPRITE_W; i++) {
        int top_lit = pat_top[i] != '.';
        int bot_lit = pat_bot[i] != '.';
        int x = cx - half + i;
        if (top_lit && bot_lit) plot(x, cy, GLYPH_FULL, col);
        else if (top_lit) plot(x, cy, GLYPH_UPPER, col);
        else if (bot_lit) plot(x, cy, GLYPH_LOWER, col);
    }
}

static void invaders_init(Invaders *inv) {
    memset(inv, 0, sizeof(*inv));
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            inv->alive[r][c] = 1;
    inv->dx = 0;
    inv->dy = 0;
    inv->dir = 1;
    inv->alive_count = ROWS * COLS;
}

static int invader_pos(int r, int c, Invaders *inv, int *x, int *y) {
    if (!inv->alive[r][c]) return 0;
    *x = START_X + c * SPACING_X + inv->dx;
    *y = FIELD_TOP + r + inv->dy;
    return 1;
}

static void invaders_step(Invaders *inv) {
    int min_c = -1, max_c = -1;
    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r < ROWS; r++) {
            if (inv->alive[r][c]) { if (min_c < 0) min_c = c; max_c = c; break; }
        }
    }
    if (min_c < 0) return;

    int new_dx = inv->dx + inv->dir;
    int min_x = START_X + min_c * SPACING_X + new_dx;
    int max_x = START_X + max_c * SPACING_X + new_dx;
    if (min_x < 1 || max_x > W - 2) {
        inv->dir = -inv->dir;
        inv->dy += 1;
    } else {
        inv->dx = new_dx;
    }
}

static int bottom_alive_row(Invaders *inv, int c) {
    for (int r = ROWS - 1; r >= 0; r--)
        if (inv->alive[r][c]) return r;
    return -1;
}

static int shield_max_cells(void) {
    int n = 0;
    for (int r = 0; r < SHIELD_H; r++)
        for (int c = 0; c < SHIELD_W; c++)
            if (SHIELD_SHAPE[r][c] != ' ') n++;
    return n;
}

static void shields_init(Shield shields[SHIELD_COUNT], int player_y) {
    int gap = (W - SHIELD_COUNT * SHIELD_W) / (SHIELD_COUNT + 1);
    int shield_y = player_y - 5;
    for (int s = 0; s < SHIELD_COUNT; s++) {
        shields[s].x0 = gap + s * (SHIELD_W + gap);
        shields[s].y0 = shield_y;
        for (int r = 0; r < SHIELD_H; r++)
            for (int c = 0; c < SHIELD_W; c++)
                shields[s].alive[r][c] = (SHIELD_SHAPE[r][c] != ' ');
    }
}

/* If (x,y) hits a live shield block, destroy it and return 1. */
static int shield_hit(Shield shields[SHIELD_COUNT], int x, int y) {
    for (int s = 0; s < SHIELD_COUNT; s++) {
        int lx = x - shields[s].x0;
        int ly = y - shields[s].y0;
        if (lx < 0 || lx >= SHIELD_W || ly < 0 || ly >= SHIELD_H) continue;
        if (shields[s].alive[ly][lx]) {
            shields[s].alive[ly][lx] = 0;
            return 1;
        }
    }
    return 0;
}

static void shields_draw(Shield shields[SHIELD_COUNT], int max_cells) {
    for (int s = 0; s < SHIELD_COUNT; s++) {
        int remaining = 0;
        for (int r = 0; r < SHIELD_H; r++)
            for (int c = 0; c < SHIELD_W; c++)
                if (shields[s].alive[r][c]) remaining++;
        if (remaining == 0) continue;

        double frac = (double)remaining / max_cells;
        ColorCode col = (frac > 0.66) ? COL_SHIELD_OK
                       : (frac > 0.33) ? COL_SHIELD_DMG
                       : COL_SHIELD_CRIT;

        for (int r = 0; r < SHIELD_H; r++)
            for (int c = 0; c < SHIELD_W; c++)
                if (shields[s].alive[r][c])
                    plot(shields[s].x0 + c, shields[s].y0 + r, GLYPH_FULL, col);
    }
}

/* Renders the frame into a single buffer and writes it in one call, wrapped
 * in a btop-style rounded, colored border with the title baked into the
 * top edge. */
static void render(int score, int lives) {
    static char outbuf[65536];
    int p = 0;

    p += sprintf(outbuf + p, "\x1b[H");

    const char *title = " SPACE INVADERS ";
    int used = 1 + (int)strlen(title);
    int dashes = W - used;
    if (dashes < 0) dashes = 0;
    p += sprintf(outbuf + p, BORDER_COL "\xe2\x95\xad\xe2\x94\x80" "\x1b[1;38;5;231m%s" BORDER_COL, title);
    for (int i = 0; i < dashes; i++) p += sprintf(outbuf + p, "\xe2\x94\x80");
    p += sprintf(outbuf + p, "\xe2\x95\xae" RESET "\n");

    for (int y = 0; y < H; y++) {
        if (y == 1) {
            p += sprintf(outbuf + p, BORDER_COL "\xe2\x94\x9c");
            for (int i = 0; i < W; i++) p += sprintf(outbuf + p, "\xe2\x94\x80");
            p += sprintf(outbuf + p, "\xe2\x94\xa4" RESET "\n");
            continue;
        }
        p += sprintf(outbuf + p, BORDER_COL "\xe2\x94\x82" RESET);
        int last_col = -1;
        for (int x = 0; x < W; x++) {
            int c = colorgrid[y][x];
            if (c != last_col) {
                p += sprintf(outbuf + p, "%s", PALETTE[c]);
                last_col = c;
            }
            unsigned char g = grid[y][x];
            if (g >= GLYPH_FULL) p += sprintf(outbuf + p, "%s", SPECIAL_GLYPH[g - GLYPH_FULL]);
            else outbuf[p++] = (char)g;
        }
        p += sprintf(outbuf + p, RESET BORDER_COL "\xe2\x94\x82" RESET "\n");
    }

    p += sprintf(outbuf + p, BORDER_COL "\xe2\x95\xb0");
    for (int i = 0; i < W; i++) p += sprintf(outbuf + p, "\xe2\x94\x80");
    p += sprintf(outbuf + p, "\xe2\x95\xaf" RESET "\n");

    (void)score;
    (void)lives;
    write(STDOUT_FILENO, outbuf, (size_t)p);
}

int main(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col < W + 2 || ws.ws_row < H + 2) {
            fprintf(stderr,
                "Terminal too small: need at least %dx%d, have %dx%d.\n"
                "Resize your terminal and try again.\n",
                W + 2, H + 2, ws.ws_col, ws.ws_row);
            return 1;
        }
    }

    srand((unsigned)time(NULL));
    enable_raw_mode();

    Invaders inv;
    invaders_init(&inv);

    int px = W / 2;
    int py = FIELD_BOTTOM;
    int lives = 3;
    int score = 0;

    Shield shields[SHIELD_COUNT];
    shields_init(shields, py);
    int shield_max_cell_count = shield_max_cells();

    Bullet pbullet = {0, 0, 0};
    Bullet ebullets[MAX_EBULLETS];
    memset(ebullets, 0, sizeof(ebullets));

    int tick = 0;
    int running = 1;
    int won = 0;
    int quit = 0;

    while (running) {
        int key = poll_input();
        if (key == 'q') { running = 0; quit = 1; break; }
        if (key == 'a') { px--; if (px < 1) px = 1; }
        if (key == 'd') { px++; if (px > W - 2) px = W - 2; }
        if (key == ' ' && !pbullet.active) {
            pbullet.x = px;
            pbullet.y = py - 1;
            pbullet.active = 1;
        }

        int interval = 4 + inv.alive_count / 5;
        if (interval < 3) interval = 3;
        if (tick % interval == 0) invaders_step(&inv);

        if (pbullet.active) {
            pbullet.y--;
            if (pbullet.y < FIELD_TOP) pbullet.active = 0;
            else if (shield_hit(shields, pbullet.x, pbullet.y)) pbullet.active = 0;
        }

        if (pbullet.active) {
            for (int r = 0; r < ROWS && pbullet.active; r++) {
                for (int c = 0; c < COLS; c++) {
                    int ix, iy;
                    if (!invader_pos(r, c, &inv, &ix, &iy)) continue;
                    if (ix == pbullet.x && iy == pbullet.y) {
                        inv.alive[r][c] = 0;
                        inv.alive_count--;
                        pbullet.active = 0;
                        score += 10;
                        break;
                    }
                }
            }
        }

        if (tick % 8 == 0 && rand() % 100 < 15) {
            int cols_alive[COLS], n = 0;
            for (int c = 0; c < COLS; c++)
                if (bottom_alive_row(&inv, c) >= 0) cols_alive[n++] = c;
            if (n > 0) {
                int c = cols_alive[rand() % n];
                int r = bottom_alive_row(&inv, c);
                for (int i = 0; i < MAX_EBULLETS; i++) {
                    if (!ebullets[i].active) {
                        int ix = 0, iy = 0;
                        if (invader_pos(r, c, &inv, &ix, &iy)) {
                            ebullets[i].x = ix;
                            ebullets[i].y = iy + 1;
                            ebullets[i].active = 1;
                        }
                        break;
                    }
                }
            }
        }

        for (int i = 0; i < MAX_EBULLETS; i++) {
            if (!ebullets[i].active) continue;
            ebullets[i].y++;
            if (ebullets[i].y > FIELD_BOTTOM) { ebullets[i].active = 0; continue; }
            if (shield_hit(shields, ebullets[i].x, ebullets[i].y)) { ebullets[i].active = 0; continue; }
            if (ebullets[i].x == px && ebullets[i].y == py) {
                ebullets[i].active = 0;
                lives--;
            }
        }

        if (inv.alive_count == 0) { running = 0; won = 1; break; }
        if (FIELD_TOP + (ROWS - 1) + inv.dy >= py) { running = 0; won = 0; break; }
        if (lives <= 0) { running = 0; won = 0; break; }

        memset(grid, ' ', sizeof(grid));
        memset(colorgrid, COL_NONE, sizeof(colorgrid));

        shields_draw(shields, shield_max_cell_count);
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                int ix, iy;
                if (invader_pos(r, c, &inv, &ix, &iy))
                    draw_sprite(ix, iy, INVADER_TOP, INVADER_BOT, INVADER_ROW_COLOR[r]);
            }
        if (pbullet.active) plot(pbullet.x, pbullet.y, GLYPH_PBULLET, COL_PBULLET);
        for (int i = 0; i < MAX_EBULLETS; i++)
            if (ebullets[i].active) plot(ebullets[i].x, ebullets[i].y, GLYPH_EBULLET, COL_EBULLET);
        draw_sprite(px, py, SHIP_TOP, SHIP_BOT, COL_PLAYER);

        char numbuf[16];
        int x = 2;
        x = plot_str(x, 0, "Score: ", COL_LABEL);
        snprintf(numbuf, sizeof(numbuf), "%d", score);
        x = plot_str(x, 0, numbuf, COL_SCORE_VAL);
        x = plot_str(x + 3, 0, "Lives: ", COL_LABEL);
        snprintf(numbuf, sizeof(numbuf), "%d", lives);
        plot_str(x, 0, numbuf, COL_LIVES_VAL);

        render(score, lives);

        tick++;
        usleep(TICK_USEC);
    }

    printf("\x1b[H\x1b[2J");
    if (quit) printf(RESET "Bye! Final score: %d\n", score);
    else if (won) printf("\x1b[1;38;5;46mYOU WIN! Final score: %d" RESET "\n", score);
    else printf("\x1b[1;38;5;196mGAME OVER. Final score: %d" RESET "\n", score);
    fflush(stdout);

    return 0;
}
