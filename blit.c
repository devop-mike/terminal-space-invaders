/*
 * Terminal Space Invaders - "blit" version
 * Controls: a/d or left/right arrows to move, space to shoot, q to quit.
 * No external dependencies - raw termios + POSIX only. Renders the play
 * field as a real bitmap image, blitted into the terminal via the Sixel
 * graphics protocol. Requires a Sixel-capable terminal (xterm with Sixel
 * enabled, mlterm, foot, wezterm, Windows Terminal w/ VT passthrough, ...).
 * Game logic/grid is identical to ver1/ver2 - only the renderer differs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <time.h>

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

/* ---- pixel canvas / palette ------------------------------------------- */

#define CELL_PX_W 6
#define CELL_PX_H 12
#define FIELD_ROWS (H - FIELD_TOP)
#define PX_W (W * CELL_PX_W)
#define PX_H (FIELD_ROWS * CELL_PX_H)

enum {
    PAL_BORDER = 1,
    PAL_INV0, PAL_INV1, PAL_INV2, PAL_INV3, PAL_INV4,
    PAL_PLAYER, PAL_PBULLET, PAL_EBULLET,
    PAL_SHIELD_OK, PAL_SHIELD_DMG, PAL_SHIELD_CRIT,
    PAL_COUNT
};

static const unsigned char PALETTE_RGB[PAL_COUNT][3] = {
    [PAL_BORDER]      = {0, 255, 255},
    [PAL_INV0]        = {0, 215, 255},
    [PAL_INV1]        = {0, 255, 255},
    [PAL_INV2]        = {0, 255, 0},
    [PAL_INV3]        = {255, 255, 0},
    [PAL_INV4]        = {255, 95, 95},
    [PAL_PLAYER]      = {0, 255, 0},
    [PAL_PBULLET]     = {255, 255, 0},
    [PAL_EBULLET]     = {255, 0, 0},
    [PAL_SHIELD_OK]   = {0, 255, 0},
    [PAL_SHIELD_DMG]  = {255, 255, 0},
    [PAL_SHIELD_CRIT] = {255, 0, 0},
};

static const int INVADER_ROW_PAL[ROWS] = {
    PAL_INV0, PAL_INV1, PAL_INV2, PAL_INV3, PAL_INV4
};

static unsigned char fb[PX_H][PX_W];

static void fb_rect(int x0, int y0, int w, int h, int pal) {
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PX_W) x1 = PX_W;
    if (y1 > PX_H) y1 = PX_H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            fb[y][x] = (unsigned char)pal;
}

static void fb_border(int pal) {
    fb_rect(0, 0, PX_W, 2, pal);
    fb_rect(0, PX_H - 2, PX_W, 2, pal);
    fb_rect(0, 0, 2, PX_H, pal);
    fb_rect(PX_W - 2, 0, 2, PX_H, pal);
}

/* Draws a bw x bh '0'/'1' bitmap, scaled up, centered on the pixel-space
 * point (cx, cy). */
static void fb_sprite(int cx, int cy, const char *const *rows, int bw, int bh, int scale, int pal) {
    int x0 = cx - (bw * scale) / 2;
    int y0 = cy - (bh * scale) / 2;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
            if (rows[by][bx] == '1')
                fb_rect(x0 + bx * scale, y0 + by * scale, scale, scale, pal);
}

static const char *INVADER_BITMAP[4] = {
    "0011111100",
    "0111111110",
    "1111111111",
    "0010101000",
};
static const char *SHIP_BITMAP[4] = {
    "0000110000",
    "0000110000",
    "0011111100",
    "1111111111",
};

/* Grid coordinate (game logic space) -> pixel-canvas center point. */
static void cell_center(int gx, int gy, int *px, int *py) {
    *px = gx * CELL_PX_W + CELL_PX_W / 2;
    *py = (gy - FIELD_TOP) * CELL_PX_H + CELL_PX_H / 2;
}

/* Encodes fb[][] as a Sixel image into out, returns bytes written. */
static int sixel_encode(char *out) {
    int p = 0;
    p += sprintf(out + p, "\x1bP0;1;0q");
    for (int i = 1; i < PAL_COUNT; i++) {
        int r = PALETTE_RGB[i][0] * 100 / 255;
        int g = PALETTE_RGB[i][1] * 100 / 255;
        int b = PALETTE_RGB[i][2] * 100 / 255;
        p += sprintf(out + p, "#%d;2;%d;%d;%d", i, r, g, b);
    }
    static unsigned char colmask[PAL_COUNT][PX_W];
    for (int y0 = 0; y0 < PX_H; y0 += 6) {
        memset(colmask, 0, sizeof(colmask));
        for (int yy = 0; yy < 6; yy++) {
            int y = y0 + yy;
            for (int x = 0; x < PX_W; x++) {
                int pal = fb[y][x];
                if (pal) colmask[pal][x] |= (unsigned char)(1 << yy);
            }
        }
        for (int reg = 1; reg < PAL_COUNT; reg++) {
            int any = 0;
            for (int x = 0; x < PX_W; x++) if (colmask[reg][x]) { any = 1; break; }
            if (!any) continue;
            p += sprintf(out + p, "#%d", reg);
            for (int x = 0; x < PX_W; x++) out[p++] = (char)(63 + colmask[reg][x]);
            p += sprintf(out + p, "$");
        }
        p += sprintf(out + p, "-");
    }
    p += sprintf(out + p, "\x1b\\");
    return p;
}

/* ---- terminal / input (identical to ver1/ver2) ------------------------ */

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

/* ---- game logic (identical to ver1/ver2) ------------------------------ */

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
        int pal = (frac > 0.66) ? PAL_SHIELD_OK
                : (frac > 0.33) ? PAL_SHIELD_DMG
                : PAL_SHIELD_CRIT;

        for (int r = 0; r < SHIELD_H; r++)
            for (int c = 0; c < SHIELD_W; c++)
                if (shields[s].alive[r][c]) {
                    int gx = shields[s].x0 + c, gy = shields[s].y0 + r;
                    int cx, cy;
                    cell_center(gx, gy, &cx, &cy);
                    fb_rect(cx - CELL_PX_W / 2 + 1, cy - CELL_PX_H / 2 + 1,
                            CELL_PX_W - 2, CELL_PX_H - 2, pal);
                }
    }
}

/* ---- render ------------------------------------------------------------ */

static char outbuf[400000];

static void render(int score, int lives) {
    int p = 0;
    p += sprintf(outbuf + p, "\x1b[H");

    const char *title = " SPACE INVADERS ";
    int used = 1 + (int)strlen(title);
    int dashes = W - used;
    if (dashes < 0) dashes = 0;
    p += sprintf(outbuf + p, BORDER_COL "\xe2\x95\xad\xe2\x94\x80" "\x1b[1;38;5;231m%s" BORDER_COL, title);
    for (int i = 0; i < dashes; i++) p += sprintf(outbuf + p, "\xe2\x94\x80");
    p += sprintf(outbuf + p, "\xe2\x95\xae" RESET "\n");

    p += sprintf(outbuf + p,
        BORDER_COL "\xe2\x94\x82" RESET "  \x1b[38;5;250mScore: \x1b[1;38;5;46m%-5d\x1b[38;5;250mLives: \x1b[1;38;5;196m%-3d" RESET,
        score, lives);
    int status_len = 2 + 7 + 5 + 7 + 3;
    for (int i = status_len; i < W; i++) p += sprintf(outbuf + p, " ");
    p += sprintf(outbuf + p, BORDER_COL "\xe2\x94\x82" RESET "\n");

    p += sprintf(outbuf + p, BORDER_COL "\xe2\x94\x9c");
    for (int i = 0; i < W; i++) p += sprintf(outbuf + p, "\xe2\x94\x80");
    p += sprintf(outbuf + p, "\xe2\x94\xa4" RESET "\n");

    p += sixel_encode(outbuf + p);
    p += sprintf(outbuf + p, "\n");

    p += sprintf(outbuf + p, BORDER_COL "\xe2\x95\xb0");
    for (int i = 0; i < W; i++) p += sprintf(outbuf + p, "\xe2\x94\x80");
    p += sprintf(outbuf + p, "\xe2\x95\xaf" RESET "\n");

    write(STDOUT_FILENO, outbuf, (size_t)p);
}

int main(void) {
    fprintf(stderr,
        "Space Invaders (blit version) needs a Sixel-capable terminal\n"
        "(xterm -ti vt340, mlterm, foot, wezterm, or similar).\n"
        "If you just see garbage text below, your terminal doesn't support Sixel.\n");

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

        memset(fb, 0, sizeof(fb));
        fb_border(PAL_BORDER);

        shields_draw(shields, shield_max_cell_count);
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                int ix, iy;
                if (invader_pos(r, c, &inv, &ix, &iy)) {
                    int cx, cy;
                    cell_center(ix, iy, &cx, &cy);
                    fb_sprite(cx, cy, INVADER_BITMAP, 10, 4, 2, INVADER_ROW_PAL[r]);
                }
            }
        if (pbullet.active) {
            int cx, cy;
            cell_center(pbullet.x, pbullet.y, &cx, &cy);
            fb_rect(cx - 2, cy - 5, 4, 10, PAL_PBULLET);
        }
        for (int i = 0; i < MAX_EBULLETS; i++) {
            if (!ebullets[i].active) continue;
            int cx, cy;
            cell_center(ebullets[i].x, ebullets[i].y, &cx, &cy);
            fb_rect(cx - 2, cy - 3, 4, 6, PAL_EBULLET);
        }
        {
            int cx, cy;
            cell_center(px, py, &cx, &cy);
            fb_sprite(cx, cy, SHIP_BITMAP, 10, 4, 2, PAL_PLAYER);
        }

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
