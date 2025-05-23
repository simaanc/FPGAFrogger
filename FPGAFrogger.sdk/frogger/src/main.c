#include "xparameters.h"
#include "xgpio.h"
#include "xil_types.h"
#include "sleep.h"            // busy-wait helpers (no usleep per pixel)
#include "PmodKYPD.h"
#include <stdint.h>
#include <stdlib.h>

#include "sprites.h"          // frog & log sprites (16×16)
#include "background.h"       // background tile-map
#undef  NUM_SPRITES           // silence duplicate warning

/* framebuffer geometry */
#define FB_W 224
#define FB_H 256
#define TILE_W SPR_W          /* 16 */
#define TILE_H SPR_H

/* GPIO peripheral IDs */
#define WE_DEVICE_ID     XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID   XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID    XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID  XPAR_AXI_GPIO_VSYNC_DEVICE_ID
#define GPIO_CH          1     /* all AXI-GPIO blocks use channel-1 */

/* keypad (PMOD KYPD) */
#define KYPD_GPIO_ID     XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE         "0FED789C456B123A"

/* green timer bar (bottom of screen) */
#define BAR_W          118
#define BAR_H          8
#define BAR_Y0         (FB_H - BAR_H)
#define BAR_X1         (FB_W - 32 - 1)
#define BAR_X0         (BAR_X1 - BAR_W + 1)
#define BAR_COLOR      0x6
#define BAR_FRAMES     (30 * 60)
#define FRAMES_PER_COL (BAR_FRAMES / BAR_W)

/* River and road boundaries */
#define RIVER_TOP    32
#define RIVER_BOTTOM 112
#define ROAD_TOP     144
#define ROAD_BOTTOM  224

/* moving object type (logs, cars, frog) */
struct Obj {
    int x, y;      /* current position */
    int px, py;    /* previous position */
    int idx;       /* sprite index */
    int dx;        /* velocity (for logs/cars) */
};

/* peripherals */
static XGpio    gpio_we, gpio_addr, gpio_dat, gpio_vsync;
static PmodKYPD keypad;

/* global game objects and state */
static struct Obj log0, log1, log2, car0, car1, turtle0, turtle1, frog;
/* lengths of each log in tiles */
static int log0_len = 4;
static int log1_len = 6;
static int log2_len = 3;
static int turtle_dive_timer = 0;
static int turtle_submerged = 0;
/* lily-pad targets */
static struct {
    int x;
    int filled;
} targets[5] = {
    { 8, 0 }, { 56, 0 }, { 104, 0 }, { 152, 0 }, { 200, 0 }
};
static int lives     = 3;
static int score     = 0;
static int game_over = 0;
static int log_dx    = 0;

/* tight pixel write (back buffer) */
static inline void draw_pixel(int x, int y, uint8_t c4) {
    uint16_t idx = y * FB_W + x;
    XGpio_DiscreteWrite(&gpio_addr, GPIO_CH, idx);
    XGpio_DiscreteWrite(&gpio_dat,  GPIO_CH, c4 & 0xF);
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 1);
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 0);
}

/* background palette index at (x,y) */
static inline uint8_t bg_pixel(int x, int y) {
    int tx = x / TILE_W, ty = y / TILE_H;
    int ox = x & (TILE_W-1), oy = y & (TILE_H-1);
    uint8_t tid = bg_tilemap[ty][tx];
    return background[tid][oy * TILE_W + ox];
}

/* restore one 16×16 block from background */
static void restore_block(int x, int y) {
    for (int dy = 0; dy < TILE_H; dy++)
        for (int dx = 0; dx < TILE_W; dx++)
            draw_pixel(x + dx, y + dy,
                       bg_pixel(x + dx, y + dy));
}

/* draw sprite (transparent index 0) */
static void draw_sprite(int n, int sx, int sy) {
    const uint8_t *spr = sprites[n];
    for (int dy = 0; dy < TILE_H; dy++)
        for (int dx = 0; dx < TILE_W; dx++) {
            uint8_t c = spr[dy * TILE_W + dx];
            if (c) draw_pixel(sx + dx, sy + dy, c);
        }
}

/* draw a variable-length log (sprite 46=left,47=middle,48=right) */
static void draw_log(int x, int y, int len) {
    if (len <= 0) return;
    if (len == 1) {
        draw_sprite(47, x, y);
        return;
    }
    draw_sprite(46, x, y);
    for (int i = 1; i < len - 1; i++)
        draw_sprite(47, x + i * TILE_W, y);
    draw_sprite(48, x + (len - 1) * TILE_W, y);
}

/* restore the background behind a multi-tile log */
static void restore_log(int x, int y, int len) {
    for (int i = 0; i < len; i++)
        restore_block(x + i * TILE_W, y);
}

/* VSYNC high → low edge (end of blank) */
static void wait_vsync_edge(void) {
    while (!(XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1));
    while  (XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1);
}

/* green bar helpers */
static int bar_cols, bar_frame;
static inline void bar_set_column(int x, uint8_t col4) {
    for (int y = 0; y < BAR_H; y++)
        draw_pixel(x, BAR_Y0 + y, col4);
}
static void bar_init(void) {
    bar_cols  = BAR_W;
    bar_frame = 0;
    for (int x = BAR_X0; x <= BAR_X1; x++)
        bar_set_column(x, BAR_COLOR);
}
static void bar_tick(void) {
    if (bar_cols == 0) return;
    if (++bar_frame >= FRAMES_PER_COL) {
        bar_frame = 0;
        int x = BAR_X1 - bar_cols + 1;   // shrink left→right
        bar_set_column(x, bg_pixel(x, BAR_Y0));
        --bar_cols;
    }
}

/* Check if frog is riding log or turtle */
static int check_frog_on_log(int fx,int fy,
                             int l0x,int l0y,int l0dx,
                             int l1x,int l1y,int l1dx,
                             int l2x,int l2y,int l2dx,
                             int t0x,int t0y,int t0dx,
                             int t1x,int t1y,int t1dx,
                             int turtle_submerged)
{
    if (fy < RIVER_TOP || fy > RIVER_BOTTOM) return 0;

    /* Check logs over their entire width */
    if (fy == l0y
        && fx >= l0x
        && fx <  l0x + log0_len * TILE_W)
        return l0dx;
    if (fy == l1y
        && fx >= l1x
        && fx <  l1x + log1_len * TILE_W)
        return l1dx;
    if (fy == l2y
        && fx >= l2x
        && fx <  l2x + log2_len * TILE_W)
        return l2dx;

    /* Check turtles – only safe when not submerged */
    if (!turtle_submerged) {
        if (fy == t0y
            && fx >= t0x
            && fx <  t0x + TILE_W)
            return t0dx;
        if (fy == t1y
            && fx >= t1x
            && fx <  t1x + TILE_W)
            return t1dx;
    }

    return -999;  /* drowning */
}
    

/* Check for collisions with cars */
static int check_collision(int fx,int fy,int cx,int cy) {
    return (fy == cy && abs(fx - cx) <= TILE_W/2);
}

/* init GPIO & keypad */
static void init_io(void) {
    XGpio_Initialize(&gpio_we,   WE_DEVICE_ID);
    XGpio_Initialize(&gpio_addr, ADDR_DEVICE_ID);
    XGpio_Initialize(&gpio_dat,  DAT_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_we,   GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_addr, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_dat,  GPIO_CH, 0);
    XGpio_Initialize(&gpio_vsync, VSYNC_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_vsync, GPIO_CH, 0xFFFFFFFF);
    KYPD_begin(&keypad, KYPD_GPIO_ID);
    KYPD_loadKeyTable(&keypad, (u8*)KEYTABLE);
}

/* Reset frog & timer bar */
static void reset_frog(struct Obj *f) {
    f->x  = (FB_W - TILE_W) / 2;
    f->y  = FB_H - 2 * TILE_H;
    f->px = f->x;  f->py = f->y;
    bar_init();
}

/* Clear entire screen to background */
static void clear_screen(void) {
    for (int ty = 0; ty < FB_H / TILE_H; ty++) {
        for (int tx = 0; tx < FB_W / TILE_W; tx++) {
            uint8_t tid = bg_tilemap[ty][tx];
            for (int dy = 0; dy < TILE_H; dy++) {
                for (int dx = 0; dx < TILE_W; dx++) {
                    int x = tx * TILE_W + dx;
                    int y = ty * TILE_H + dy;
                    draw_pixel(x, y, background[tid][dy * TILE_W + dx]);
                }
            }
        }
    }
}

/* Initialize or restart the game */
static void init_game(void) {
    clear_screen();

    log0 = (struct Obj){   0,  48,   0, 48, 48,  1 };
    log1 = (struct Obj){ 128,  80, 128, 80, 48,  2 };
    log2 = (struct Obj){ 128,  96, 128, 96, 48,  1 };
    car0 = (struct Obj){ 128, 208, 128,208,  4, -2 };
    car1 = (struct Obj){  64, 176,  64,176,  8,  2 };
    turtle0=(struct Obj){32, 64, 32,64, 22, 1},            /* Turtles - slower speed */
    turtle1=(struct Obj){160, 112, 160,112, 23, -2},        /* Different turtle sprite */

    frog.idx = 2;
    frog.dx  = 0;
    reset_frog(&frog);
    log_dx = 0;
    turtle_dive_timer = 0;
    turtle_submerged = 0;

    /* draw all 5 targets at y=32, sprite 29 */
    for (int i = 0; i < 5; i++)
        draw_sprite(29, targets[i].x, 32);

    /* draw variable-length logs */
    draw_log(log0.x, log0.y, log0_len);
    draw_log(log1.x, log1.y, log1_len);
    draw_log(log2.x, log2.y, log2_len);

    /* draw turtles */
    draw_sprite(turtle0.idx, turtle0.x, turtle0.y);
    draw_sprite(turtle1.idx, turtle1.x, turtle1.y);

    /* draw cars & frog */
    draw_sprite(car0.idx, car0.x, car0.y);
    draw_sprite(car1.idx, car1.x, car1.y);
    draw_sprite(frog.idx, frog.x, frog.y);


    /* draw lives (sprite 2) at bottom-left */
    for (int i = 0; i < lives; i++)
        draw_sprite(2, 8 + i*20, 242);

    /* draw score icon */
    draw_sprite(27, 8, 8);
}

int main(void) {
    init_io();
    init_game();

    uint16_t ks, st;
    uint8_t  key, last = 0;

    while (1) {
        wait_vsync_edge();

        if (game_over) {
            draw_sprite(49,  60, 100);
            draw_sprite(50,  80, 100);
            draw_sprite(51, 100, 100);
            draw_sprite(52, 120, 100);
            draw_sprite(53,  140, 100);
            draw_sprite(50,  160, 100);

            ks = KYPD_getKeyStates(&keypad);
            st = KYPD_getKeyPressed(&keypad, ks, &key);
            if (st == KYPD_SINGLE_KEY && key == '5') {
                game_over = 0;
                lives     = 3;
                score     = 0;
                init_game();
            }
            continue;
        }

        /* erase previous logs */
        restore_log(log0.px, log0.py, log0_len);
        restore_log(log1.px, log1.py, log1_len);
        restore_log(log2.px, log2.py, log2_len);
        /* erase other objects */
        restore_block(car0.px, car0.py);
        restore_block(car1.px, car1.py);
        restore_block(frog.px, frog.py);

        /* erase turtles */
        restore_block(turtle0.px, turtle0.py);
        restore_block(turtle1.px, turtle1.py);

        /* move logs & cars */
        log0.px = log0.x; log1.px = log1.x; log2.px = log2.x;
        turtle0.px = turtle0.x; turtle1.px = turtle1.x;
        car0.px = car0.x; car1.px = car1.x;
        log0.x += log0.dx; if (log0.x > FB_W)  log0.x = -TILE_W;
                          if (log0.x < -TILE_W) log0.x = FB_W;
        log1.x += log1.dx; if (log1.x > FB_W)  log1.x = -TILE_W;
                          if (log1.x < -TILE_W) log1.x = FB_W;
        log2.x += log2.dx; if (log2.x > FB_W)  log2.x = -TILE_W;
                          if (log2.x < -TILE_W) log2.x = FB_W;
        car0.x += car0.dx; if (car0.x > FB_W)  car0.x = -TILE_W;
                          if (car0.x < -TILE_W) car0.x = FB_W;
        car1.x += car1.dx; if (car1.x > FB_W)  car1.x = -TILE_W;
                          if (car1.x < -TILE_W) car1.x = FB_W;
        turtle0.x += turtle0.dx; if (turtle0.x > FB_W)  turtle0.x = -TILE_W;
                                if (turtle0.x < -TILE_W) turtle0.x = FB_W;
        turtle1.x += turtle1.dx; if (turtle1.x > FB_W)  turtle1.x = -TILE_W;
                                if (turtle1.x < -TILE_W) turtle1.x = FB_W;
        
        /* Turtle diving logic - dive every 3-4 seconds */
        turtle_dive_timer++;
        if (turtle_dive_timer > 30) {
        	turtle0.idx = 25;
        	turtle1.idx = 25;
        }
        if (turtle_dive_timer > 60) {  /* 3 seconds at 60fps */
            turtle_submerged = !turtle_submerged;
            turtle_dive_timer = 0;
            
            /* Change turtle sprites when diving/surfacing */
            if (turtle_submerged) {
                turtle0.idx = 26;  /* Submerged turtle sprite */
                turtle1.idx = 26;  /* Submerged turtle sprite */
            } else {
                turtle0.idx = 22;   /* Surface turtle sprite */
                turtle1.idx = 23;  /* Surface turtle sprite */
            }
        }

        /* carry frog on log */
        if (log_dx) {
            frog.px = frog.x;
            frog.x += log_dx;
            if (frog.x < 0 || frog.x > FB_W - TILE_W) {
                if (--lives <= 0) { game_over = 1; continue; }
                init_game(); continue;
            }
        }

        /* input/move frog */
        ks = KYPD_getKeyStates(&keypad);
        st = KYPD_getKeyPressed(&keypad, ks, &key);
        if (st == KYPD_SINGLE_KEY && key != last) {
            frog.px = frog.x; frog.py = frog.y;
            int moved = 0;
            switch (key) {
                case '2': case '8':
                    frog.y -= TILE_H; moved = 1;
                    if (frog.y < frog.py) score += 10;
                    break;
                case '5':
                    frog.y += TILE_H; moved = 1; break;
                case '4':
                    frog.x -= TILE_W; moved = 1; break;
                case '6':
                    frog.x += TILE_W; moved = 1; break;
            }
            if (frog.x < 0) frog.x = 0;
            if (frog.x > FB_W - TILE_W) frog.x = FB_W - TILE_W;
            if (frog.y < 0) frog.y = 0;
            if (frog.y > FB_H - TILE_H) frog.y = FB_H - TILE_H;
            last = key;
            if (moved) { log_dx = 0; }
        }
        else if (st != KYPD_SINGLE_KEY) {
            last = 0;
        }

        /* reached lily pad? */
        if (frog.y < RIVER_TOP) {
            for (int i = 0; i < 5; i++) {
                if (!targets[i].filled &&
                    abs(frog.x - targets[i].x) < TILE_W)
                {
                    targets[i].filled = 1;
                    score += 100;
                    int all = 1;
                    for (int j = 0; j < 5; j++)
                        if (!targets[j].filled) { all = 0; break; }
                    if (all) game_over = 1;
                    init_game();
                }
            }
        }

        /* logs or drown */
        log_dx = check_frog_on_log(
            frog.x, frog.y,
            log0.x, log0.y, log0.dx,
            log1.x, log1.y, log1.dx,
            log2.x, log2.y, log2.dx,
            turtle0.x, turtle0.y, turtle0.dx,
            turtle1.x, turtle1.y, turtle1.dx,
            turtle_submerged
        );
        if (log_dx == -999) {
            draw_sprite(15, frog.x, frog.y);
            wait_vsync_edge();
            sleep(1);
            if (--lives <= 0) game_over = 1;
            else              init_game();
            continue;
        }

        /* collision with cars */
        if (check_collision(frog.x,frog.y,car0.x,car0.y) ||
            check_collision(frog.x,frog.y,car1.x,car1.y)) {
            draw_sprite(15, frog.x, frog.y);
            wait_vsync_edge();
            sleep(1);
            if (--lives <= 0) game_over = 1;
            else              init_game();
            continue;
        }

        /* redraw targets */
        for (int i = 0; i < 5; i++)
            draw_sprite(29, targets[i].x, 32);

        /* redraw variable-length logs */
        draw_log(log0.x, log0.y, log0_len);
        draw_log(log1.x, log1.y, log1_len);
        draw_log(log2.x, log2.y, log2_len);

        /* redraw cars & frog */
        draw_sprite(car0.idx, car0.x, car0.y);
        draw_sprite(car1.idx, car1.x, car1.y);
        draw_sprite(frog.idx, frog.x, frog.y);
        draw_sprite(turtle0.idx, turtle0.x, turtle0.y);
        draw_sprite(turtle1.idx, turtle1.x, turtle1.y);

        /* redraw lives at bottom-left */
        for (int i = 0; i < lives; i++)
            draw_sprite(2, 8 + i*20, 242);

        /* redraw score icon */
        draw_sprite(27, 8, 8);

        /* timer tick */
        bar_tick();
        if (bar_cols == 0) {
            if (--lives <= 0) game_over = 1;
            else              init_game();
        }
        
    }

    return 0;
}
