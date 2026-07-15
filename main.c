/*
 * Terminal Space Invaders
 * Controls: a/d or left/right arrows to move, space to shoot, q to quit.
 * No external dependencies - raw termios + ANSI escapes only.
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

static struct termios orig_termios;
static int raw_mode_active = 0;

static void restore_terminal(void) {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\x1b[?25h\x1b[0m\n");
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

static char grid[H][W];

static void grid_set(int x, int y, char ch) {
    if (x >= 0 && x < W && y >= 0 && y < H) grid[y][x] = ch;
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

int main(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col < W || ws.ws_row < H) {
            fprintf(stderr,
                "Terminal too small: need at least %dx%d, have %dx%d.\n"
                "Resize your terminal and try again.\n",
                W, H, ws.ws_col, ws.ws_row);
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
            if (ebullets[i].x == px && ebullets[i].y == py) {
                ebullets[i].active = 0;
                lives--;
            }
        }

        if (inv.alive_count == 0) { running = 0; won = 1; break; }
        if (FIELD_TOP + (ROWS - 1) + inv.dy >= py) { running = 0; won = 0; break; }
        if (lives <= 0) { running = 0; won = 0; break; }

        memset(grid, ' ', sizeof(grid));
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                int ix, iy;
                if (invader_pos(r, c, &inv, &ix, &iy)) grid_set(ix, iy, 'W');
            }
        if (pbullet.active) grid_set(pbullet.x, pbullet.y, '|');
        for (int i = 0; i < MAX_EBULLETS; i++)
            if (ebullets[i].active) grid_set(ebullets[i].x, ebullets[i].y, ':');
        grid_set(px, py, 'A');

        char status[W + 1];
        int len = snprintf(status, sizeof(status), "Score: %d  Lives: %d", score, lives);
        for (int i = 0; i < len && i < W; i++) grid[0][i] = status[i];

        printf("\x1b[H");
        for (int y = 0; y < H; y++) {
            fwrite(grid[y], 1, W, stdout);
            fputc('\n', stdout);
        }
        fflush(stdout);

        tick++;
        usleep(TICK_USEC);
    }

    printf("\x1b[H\x1b[2J");
    if (quit) printf("Bye! Final score: %d\n", score);
    else if (won) printf("YOU WIN! Final score: %d\n", score);
    else printf("GAME OVER. Final score: %d\n", score);
    fflush(stdout);

    return 0;
}
