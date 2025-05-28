#include "xparameters.h"
#include "xgpio.h"
#include "xil_types.h"
#include "sleep.h"
#include "PmodKYPD.h"
#include <stdint.h>
#include <stdlib.h>

#include "sprites.h"
#include "background.h"
#undef  NUM_SPRITES

/* framebuffer geometry */
#define FB_W 224
#define FB_H 256
#define TILE_W SPR_W
#define TILE_H SPR_H

/* GPIO peripheral IDs */
#define WE_DEVICE_ID     XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID   XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID    XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID  XPAR_AXI_GPIO_VSYNC_DEVICE_ID
#define GPIO_CH          1

/* keypad (PMOD KYPD) */
#define KYPD_GPIO_ID     XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE         "0FED789C456B123A"

/* green timer bar */
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

/* Dirty rectangle system */
#define MAX_DIRTY_RECTS 32
typedef struct {
    int x, y, w, h;
    uint8_t active;
} DirtyRect;

static DirtyRect dirty_rects[MAX_DIRTY_RECTS];
static int num_dirty_rects = 0;

/* Object count - back to manageable numbers */
#define LOGS_PER_ROW     2
#define TURTLE_CLUSTERS  2
#define TURTLES_PER_CLUSTER 3

/* moving object type */
struct Obj {
    int x, y;
    int px, py;
    int idx;
    int dx;
    int width, height;  /* For dirty rectangle calculation */
};

/* peripherals */
static XGpio    gpio_we, gpio_addr, gpio_dat, gpio_vsync;
static PmodKYPD keypad;

/* game objects */
static struct Obj car0, car1, frog;
static struct Obj log_row0[LOGS_PER_ROW];
static struct Obj log_row1[LOGS_PER_ROW];
static struct Obj log_row2[LOGS_PER_ROW];
static struct Obj turtle_row0[TURTLE_CLUSTERS * TURTLES_PER_CLUSTER];
static struct Obj turtle_row1[TURTLE_CLUSTERS * TURTLES_PER_CLUSTER];

static int log_row0_len = 4;
static int log_row1_len = 6;
static int log_row2_len = 3;
static int turtle_dive_timer = 0;
static int turtle_submerged = 0;

static struct {
    int x;
    int filled;
} targets[5] = {
    { 8, 0 }, { 56, 0 }, { 104, 0 }, { 152, 0 }, { 200, 0 }
};

static int lives = 3;
static int score = 0;
static int game_over = 0;
static int log_dx = 0;
static int bar_cols, bar_frame;

/* Dirty rectangle management */
static void add_dirty_rect(int x, int y, int w, int h) {
    if (num_dirty_rects >= MAX_DIRTY_RECTS) return;
    
    /* Clamp to screen bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    
    if (w <= 0 || h <= 0) return;
    
    dirty_rects[num_dirty_rects].x = x;
    dirty_rects[num_dirty_rects].y = y;
    dirty_rects[num_dirty_rects].w = w;
    dirty_rects[num_dirty_rects].h = h;
    dirty_rects[num_dirty_rects].active = 1;
    num_dirty_rects++;
}

static void clear_dirty_rects(void) {
    num_dirty_rects = 0;
}

/* Mark object's old and new positions as dirty */
static void mark_object_dirty(struct Obj *obj) {
    /* Old position */
    add_dirty_rect(obj->px, obj->py, obj->width, obj->height);
    /* New position */
    add_dirty_rect(obj->x, obj->y, obj->width, obj->height);
}

/* Mark log's old and new positions as dirty */
static void mark_log_dirty(struct Obj *log, int len) {
    int width = len * TILE_W;
    /* Old position */
    add_dirty_rect(log->px, log->py, width, TILE_H);
    /* New position */
    add_dirty_rect(log->x, log->y, width, TILE_H);
}

/* Basic drawing functions */
static inline void draw_pixel(int x, int y, uint8_t c4) {
    if (x >= 0 && x < FB_W && y >= 0 && y < FB_H) {
        uint16_t idx = y * FB_W + x;
        XGpio_DiscreteWrite(&gpio_addr, GPIO_CH, idx);
        XGpio_DiscreteWrite(&gpio_dat,  GPIO_CH, c4 & 0xF);
        XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 1);
        XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 0);
    }
}

static inline uint8_t bg_pixel(int x, int y) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return 0;
    int tx = x / TILE_W, ty = y / TILE_H;
    int ox = x & (TILE_W-1), oy = y & (TILE_H-1);
    uint8_t tid = bg_tilemap[ty][tx];
    return background[tid][oy * TILE_W + ox];
}

static void draw_sprite(int n, int sx, int sy) {
    const uint8_t *spr = sprites[n];
    for (int dy = 0; dy < TILE_H; dy++)
        for (int dx = 0; dx < TILE_W; dx++) {
            uint8_t c = spr[dy * TILE_W + dx];
            if (c) draw_pixel(sx + dx, sy + dy, c);
        }
}

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

/* Restore background in dirty rectangle */
static void restore_dirty_rect(DirtyRect *rect) {
    for (int y = rect->y; y < rect->y + rect->h; y++) {
        for (int x = rect->x; x < rect->x + rect->w; x++) {
            draw_pixel(x, y, bg_pixel(x, y));
        }
    }
}

/* Process all dirty rectangles */
static void update_dirty_regions(void) {
    /* Step 1: Restore background in all dirty areas */
    for (int i = 0; i < num_dirty_rects; i++) {
        if (dirty_rects[i].active) {
            restore_dirty_rect(&dirty_rects[i]);
        }
    }
    
    /* Step 2: Redraw all objects (they'll only draw in dirty areas) */
    /* Draw logs */
    for (int i = 0; i < LOGS_PER_ROW; i++) {
        draw_log(log_row0[i].x, log_row0[i].y, log_row0_len);
        draw_log(log_row1[i].x, log_row1[i].y, log_row1_len);
        draw_log(log_row2[i].x, log_row2[i].y, log_row2_len);
    }
    
    /* Draw turtles - don't draw if fully submerged */
    if (!turtle_submerged || turtle_dive_timer < 30) {
        for (int i = 0; i < TURTLE_CLUSTERS * TURTLES_PER_CLUSTER; i++) {
            draw_sprite(turtle_row0[i].idx, turtle_row0[i].x, turtle_row0[i].y);
            draw_sprite(turtle_row1[i].idx, turtle_row1[i].x, turtle_row1[i].y);
        }
    }
    
    /* Draw cars and frog */
    draw_sprite(car0.idx, car0.x, car0.y);
    draw_sprite(car1.idx, car1.x, car1.y);
    draw_sprite(frog.idx, frog.x, frog.y);
    
    /* Draw static elements if they're in dirty areas */
    for (int i = 0; i < 5; i++)
        draw_sprite(29, targets[i].x, 32);
    
    for (int i = 0; i < lives; i++)
        draw_sprite(2, 8 + i*20, 242);
    
    draw_sprite(27, 8, 8);
}

static void wait_vsync_edge(void) {
    while (!(XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1));
    while  ( XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1);
}

static inline void bar_set_column(int x, uint8_t col4) {
    for (int y = 0; y < BAR_H; y++)
        draw_pixel(x, BAR_Y0 + y, col4);
}

static void bar_init(void) {
    bar_cols = BAR_W;
    bar_frame = 0;
    for (int x = BAR_X0; x <= BAR_X1; x++)
        bar_set_column(x, BAR_COLOR);
}

static void bar_tick(void) {
    if (bar_cols == 0) return;
    if (++bar_frame >= FRAMES_PER_COL) {
        bar_frame = 0;
        int x = BAR_X1 - bar_cols + 1;
        bar_set_column(x, bg_pixel(x, BAR_Y0));
        --bar_cols;
        /* Mark timer bar as dirty */
        add_dirty_rect(x, BAR_Y0, 1, BAR_H);
    }
}

static int check_frog_on_log(int fx, int fy, int turtle_submerged) {
    if (fy < RIVER_TOP || fy > RIVER_BOTTOM) return 0;

    /* Check log rows */
    for (int i = 0; i < LOGS_PER_ROW; i++) {
        if (fy == log_row0[i].y && fx >= log_row0[i].x && fx < log_row0[i].x + log_row0_len * TILE_W)
            return log_row0[i].dx;
        if (fy == log_row1[i].y && fx >= log_row1[i].x && fx < log_row1[i].x + log_row1_len * TILE_W)
            return log_row1[i].dx;
        if (fy == log_row2[i].y && fx >= log_row2[i].x && fx < log_row2[i].x + log_row2_len * TILE_W)
            return log_row2[i].dx;
    }

    /* Check turtles */
    if (!turtle_submerged) {
        for (int i = 0; i < TURTLE_CLUSTERS * TURTLES_PER_CLUSTER; i++) {
            if (fy == turtle_row0[i].y && fx >= turtle_row0[i].x && fx < turtle_row0[i].x + TILE_W)
                return turtle_row0[i].dx;
            if (fy == turtle_row1[i].y && fx >= turtle_row1[i].x && fx < turtle_row1[i].x + TILE_W)
                return turtle_row1[i].dx;
        }
    }

    return -999;
}

static int check_collision(int fx, int fy, int cx, int cy) {
    return (fy == cy && abs(fx - cx) <= TILE_W/2);
}

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

static void reset_frog(struct Obj *f) {
    f->x = (FB_W - TILE_W) / 2;
    f->y = FB_H - 2 * TILE_H;
    f->px = f->x; f->py = f->y;
    f->width = TILE_W; f->height = TILE_H;
    bar_init();
}

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

static void init_log_arrays(void) {
    /* Log row 0: 2 logs */
    for (int i = 0; i < LOGS_PER_ROW; i++) {
        log_row0[i].x = i * 120;
        log_row0[i].y = 48;
        log_row0[i].px = log_row0[i].x;
        log_row0[i].py = log_row0[i].y;
        log_row0[i].dx = 1;
        log_row0[i].idx = 48;
        log_row0[i].width = log_row0_len * TILE_W;
        log_row0[i].height = TILE_H;
    }
    
    /* Log row 1: 2 logs */
    for (int i = 0; i < LOGS_PER_ROW; i++) {
        log_row1[i].x = 60 + i * 120;
        log_row1[i].y = 80;
        log_row1[i].px = log_row1[i].x;
        log_row1[i].py = log_row1[i].y;
        log_row1[i].dx = 2;
        log_row1[i].idx = 48;
        log_row1[i].width = log_row1_len * TILE_W;
        log_row1[i].height = TILE_H;
    }
    
    /* Log row 2: 2 logs */
    for (int i = 0; i < LOGS_PER_ROW; i++) {
        log_row2[i].x = 30 + i * 120;
        log_row2[i].y = 96;
        log_row2[i].px = log_row2[i].x;
        log_row2[i].py = log_row2[i].y;
        log_row2[i].dx = 1;
        log_row2[i].idx = 48;
        log_row2[i].width = log_row2_len * TILE_W;
        log_row2[i].height = TILE_H;
    }
}

static void init_turtle_clusters(void) {
    /* Turtle row 0: 2 clusters */
    for (int cluster = 0; cluster < TURTLE_CLUSTERS; cluster++) {
        for (int turtle = 0; turtle < TURTLES_PER_CLUSTER; turtle++) {
            int index = cluster * TURTLES_PER_CLUSTER + turtle;
            turtle_row0[index].x = cluster * 120 + turtle * TILE_W;
            turtle_row0[index].y = 64;
            turtle_row0[index].px = turtle_row0[index].x;
            turtle_row0[index].py = turtle_row0[index].y;
            turtle_row0[index].dx = 1;
            turtle_row0[index].idx = 22;
            turtle_row0[index].width = TILE_W;
            turtle_row0[index].height = TILE_H;
        }
    }
    
    /* Turtle row 1: 2 clusters */
    for (int cluster = 0; cluster < TURTLE_CLUSTERS; cluster++) {
        for (int turtle = 0; turtle < TURTLES_PER_CLUSTER; turtle++) {
            int index = cluster * TURTLES_PER_CLUSTER + turtle;
            turtle_row1[index].x = 60 + cluster * 120 + turtle * TILE_W;
            turtle_row1[index].y = 112;
            turtle_row1[index].px = turtle_row1[index].x;
            turtle_row1[index].py = turtle_row1[index].y;
            turtle_row1[index].dx = -2;
            turtle_row1[index].idx = 23;
            turtle_row1[index].width = TILE_W;
            turtle_row1[index].height = TILE_H;
        }
    }
}

static void init_game(void) {
    clear_screen();

    car0 = (struct Obj){ 128, 208, 128, 208, 4, -2, TILE_W, TILE_H };
    car1 = (struct Obj){  64, 176,  64, 176, 8,  2, TILE_W, TILE_H };

    frog.idx = 2;
    frog.dx = 0;
    reset_frog(&frog);
    log_dx = 0;
    turtle_dive_timer = 0;
    turtle_submerged = 0;

    init_log_arrays();
    init_turtle_clusters();
    
    /* Reset target states */
    for (int i = 0; i < 5; i++)
        targets[i].filled = 0;

    /* Initial draw of all objects */
    for (int i = 0; i < LOGS_PER_ROW; i++) {
        draw_log(log_row0[i].x, log_row0[i].y, log_row0_len);
        draw_log(log_row1[i].x, log_row1[i].y, log_row1_len);
        draw_log(log_row2[i].x, log_row2[i].y, log_row2_len);
    }

    for (int i = 0; i < TURTLE_CLUSTERS * TURTLES_PER_CLUSTER; i++) {
        draw_sprite(turtle_row0[i].idx, turtle_row0[i].x, turtle_row0[i].y);
        draw_sprite(turtle_row1[i].idx, turtle_row1[i].x, turtle_row1[i].y);
    }

    draw_sprite(car0.idx, car0.x, car0.y);
    draw_sprite(car1.idx, car1.x, car1.y);
    draw_sprite(frog.idx, frog.x, frog.y);
    
    /* Draw static elements */
    for (int i = 0; i < 5; i++)
        draw_sprite(29, targets[i].x, 32);
    for (int i = 0; i < lives; i++)
        draw_sprite(2, 8 + i*20, 242);
    draw_sprite(27, 8, 8);
}

int main(void) {
    init_io();
    init_game();

    uint16_t ks, st;
    uint8_t key, last = 0;

    while (1) {
        wait_vsync_edge();

        if (game_over) {
            draw_sprite(49,  60, 100);
            draw_sprite(50,  80, 100);
            draw_sprite(51, 100, 100);
            draw_sprite(52, 120, 100);
            draw_sprite(53, 140, 100);
            draw_sprite(50, 160, 100);

            ks = KYPD_getKeyStates(&keypad);
            st = KYPD_getKeyPressed(&keypad, ks, &key);
            if (st == KYPD_SINGLE_KEY && key == '5') {
                game_over = 0;
                lives = 3;
                score = 0;
                init_game();
            }
            continue;
        }

        /* Clear dirty rectangles from previous frame */
        clear_dirty_rects();

        /* Move objects and mark dirty areas */
        for (int i = 0; i < LOGS_PER_ROW; i++) {
            /* Log row 0 */
            log_row0[i].px = log_row0[i].x;
            log_row0[i].x += log_row0[i].dx;
            if (log_row0[i].x > FB_W + TILE_W) log_row0[i].x = -(log_row0_len * TILE_W);
            if (log_row0[i].x < -(log_row0_len * TILE_W) - TILE_W) log_row0[i].x = FB_W + TILE_W;
            mark_log_dirty(&log_row0[i], log_row0_len);
            
            /* Log row 1 */
            log_row1[i].px = log_row1[i].x;
            log_row1[i].x += log_row1[i].dx;
            if (log_row1[i].x > FB_W + TILE_W) log_row1[i].x = -(log_row1_len * TILE_W);
            if (log_row1[i].x < -(log_row1_len * TILE_W) - TILE_W) log_row1[i].x = FB_W + TILE_W;
            mark_log_dirty(&log_row1[i], log_row1_len);
            
            /* Log row 2 */
            log_row2[i].px = log_row2[i].x;
            log_row2[i].x += log_row2[i].dx;
            if (log_row2[i].x > FB_W + TILE_W) log_row2[i].x = -(log_row2_len * TILE_W);
            if (log_row2[i].x < -(log_row2_len * TILE_W) - TILE_W) log_row2[i].x = FB_W + TILE_W;
            mark_log_dirty(&log_row2[i], log_row2_len);
        }

        /* Move cars */
        car0.px = car0.x;
        car0.x += car0.dx;
        if (car0.x > FB_W + TILE_W) car0.x = -TILE_W;
        if (car0.x < -2*TILE_W) car0.x = FB_W + TILE_W;
        mark_object_dirty(&car0);
        
        car1.px = car1.x;
        car1.x += car1.dx;
        if (car1.x > FB_W + TILE_W) car1.x = -TILE_W;
        if (car1.x < -2*TILE_W) car1.x = FB_W + TILE_W;
        mark_object_dirty(&car1);

        /* Move turtles */
        for (int i = 0; i < TURTLE_CLUSTERS * TURTLES_PER_CLUSTER; i++) {
            turtle_row0[i].px = turtle_row0[i].x;
            turtle_row0[i].x += turtle_row0[i].dx;
            if (turtle_row0[i].x > FB_W + TILE_W) turtle_row0[i].x = -TILE_W;
            if (turtle_row0[i].x < -2*TILE_W) turtle_row0[i].x = FB_W + TILE_W;
            mark_object_dirty(&turtle_row0[i]);
            
            turtle_row1[i].px = turtle_row1[i].x;
            turtle_row1[i].x += turtle_row1[i].dx;
            if (turtle_row1[i].x > FB_W + TILE_W) turtle_row1[i].x = -TILE_W;
            if (turtle_row1[i].x < -2*TILE_W) turtle_row1[i].x = FB_W + TILE_W;
            mark_object_dirty(&turtle_row1[i]);
        }
        
        /* Turtle diving logic */
        turtle_dive_timer++;
        if (turtle_dive_timer > 30) {
            for (int i = 0; i < TURTLE_CLUSTERS * TURTLES_PER_CLUSTER; i++) {
                turtle_row0[i].idx = 25;
                turtle_row1[i].idx = 25;
                mark_object_dirty(&turtle_row0[i]);  /* Mark dirty for sprite change */
                mark_object_dirty(&turtle_row1[i]);
            }
        }
        if (turtle_dive_timer > 60) {
            turtle_submerged = !turtle_submerged;
            turtle_dive_timer = 0;
            
            if (turtle_submerged) {
                for (int i = 0; i < TURTLE_CLUSTERS * TURTLES_PER_CLUSTER; i++) {
                    turtle_row0[i].idx = 26;
                    turtle_row1[i].idx = 26;
                    mark_object_dirty(&turtle_row0[i]);
                    mark_object_dirty(&turtle_row1[i]);
                }
            } else {
                for (int i = 0; i < TURTLE_CLUSTERS * TURTLES_PER_CLUSTER; i++) {
                    turtle_row0[i].idx = 22;
                    turtle_row1[i].idx = 23;
                    mark_object_dirty(&turtle_row0[i]);
                    mark_object_dirty(&turtle_row1[i]);
                }
            }
        }

        /* Carry frog on log or turtle */
        if (log_dx) {
            frog.px = frog.x;
            frog.x += log_dx;
            mark_object_dirty(&frog);
            if (frog.x < 0 || frog.x > FB_W - TILE_W) {
                if (--lives <= 0) { 
                    game_over = 1; 
                    continue; 
                }
                init_game(); 
                continue;
            }
        }

        /* Input/move frog */
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
            if (moved) { 
                log_dx = 0; 
                mark_object_dirty(&frog);
            }
        }
        else if (st != KYPD_SINGLE_KEY) {
            last = 0;
        }

        /* Check lily pad */
        if (frog.y < RIVER_TOP) {
            for (int i = 0; i < 5; i++) {
                if (!targets[i].filled && abs(frog.x - targets[i].x) < TILE_W) {
                    targets[i].filled = 1;
                    score += 100;
                    /* Mark target area as dirty */
                    add_dirty_rect(targets[i].x, 32, TILE_W, TILE_H);
                    int all = 1;
                    for (int j = 0; j < 5; j++)
                        if (!targets[j].filled) { all = 0; break; }
                    if (all) game_over = 1;
                    init_game();
                }
            }
        }

        /* Check drowning */
        log_dx = check_frog_on_log(frog.x, frog.y, turtle_submerged);
        if (log_dx == -999) {
            draw_sprite(15, frog.x, frog.y);
            wait_vsync_edge();
            sleep(1);
            if (--lives <= 0) {
                game_over = 1;
            } else {
                init_game();
            }
            continue;
        }

        /* Check car collision */
        if (check_collision(frog.x, frog.y, car0.x, car0.y) ||
            check_collision(frog.x, frog.y, car1.x, car1.y)) {
            draw_sprite(15, frog.x, frog.y);
            wait_vsync_edge();
            sleep(1);
            if (--lives <= 0) {
                game_over = 1;
            } else {
                init_game();
            }
            continue;
        }

        /* Timer tick */
        bar_tick();
        if (bar_cols == 0) {
            if (--lives <= 0) {
                game_over = 1;
            } else {
                init_game();
            }
        }

        /* Update only dirty regions */
        update_dirty_regions();
    }

    return 0;
}
