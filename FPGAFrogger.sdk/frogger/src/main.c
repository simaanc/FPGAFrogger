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
#define TILE_W SPR_W          /* 16 */
#define TILE_H SPR_H          /* 16 */

/* GPIO peripheral IDs - ADD the frame_ready GPIO in Vivado! */
#define WE_DEVICE_ID       XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID     XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID      XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID    XPAR_AXI_GPIO_VSYNC_DEVICE_ID
#define FRAME_RDY_DEVICE_ID XPAR_AXI_GPIO_FRAME_RDY_DEVICE_ID  // NEW: Add this in block design
#define GPIO_CH            1

/* keypad (PMOD KYPD) */
#define KYPD_GPIO_ID       XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE           "0FED789C456B123A"

/* green timer bar (bottom of screen) */
#define BAR_W          118
#define BAR_H          8
#define BAR_Y0         (FB_H - BAR_H)
#define BAR_X1         (FB_W - 32 - 1)
#define BAR_X0         (BAR_X1 - BAR_W + 1)
#define BAR_COLOR      0x6
#define BAR_FRAMES     (30 * 60)        // 30 seconds at 60fps
#define FRAMES_PER_COL (BAR_FRAMES / BAR_W)

/* River and road boundaries */
#define RIVER_TOP    32
#define RIVER_BOTTOM 112
#define ROAD_TOP     144
#define ROAD_BOTTOM  224

/* moving object type (logs, cars, frog) */
struct Obj {
    int x, y;      /* current position */
    int px, py;    /* previous position (not used in new approach) */
    int idx;       /* sprite index */
    int dx;        /* velocity (for logs/cars) */
};

/* peripherals */
static XGpio    gpio_we, gpio_addr, gpio_dat, gpio_vsync, gpio_frame_rdy;
static PmodKYPD keypad;

/* global game objects and state */
static struct Obj log0, log1, log2, car0, car1, frog;
/* lengths of each log in tiles */
static int log0_len = 4;
static int log1_len = 6;
static int log2_len = 3;
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

/* timer bar state */
static int bar_cols, bar_frame;

/* Safe pixel write with bounds checking */
static inline void draw_pixel(int x, int y, uint8_t c4) {
    // Bounds check to prevent crashes
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return;

    uint16_t idx = y * FB_W + x;

    // Additional safety check
    if (idx >= FB_W * FB_H) return;

    XGpio_DiscreteWrite(&gpio_addr, GPIO_CH, idx);
    XGpio_DiscreteWrite(&gpio_dat,  GPIO_CH, c4 & 0xF);
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 1);
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 0);
}

/* background palette index at (x,y) */
static inline uint8_t bg_pixel(int x, int y) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return 0;

    int tx = x / TILE_W, ty = y / TILE_H;
    int ox = x & (TILE_W-1), oy = y & (TILE_H-1);
    uint8_t tid = bg_tilemap[ty][tx];
    return background[tid][oy * TILE_W + ox];
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

/* Wait for hardware state machine to clear back buffer and be ready */
static void wait_frame_ready(void) {
    // Wait for frame_ready to go low (state machine working: INIT/CLEAR states)
    while (XGpio_DiscreteRead(&gpio_frame_rdy, GPIO_CH) & 1);

    // Wait for frame_ready to go high (DRAW state: back buffer cleared and ready)
    while (!(XGpio_DiscreteRead(&gpio_frame_rdy, GPIO_CH) & 1));

    // Now we can draw - hardware guarantees a clean, cleared back buffer
}

/* Timer bar functions */
static void bar_draw(void) {
    for (int x = BAR_X0; x <= BAR_X1; x++) {
        uint8_t color = (x - BAR_X0 < bar_cols) ? BAR_COLOR : bg_pixel(x, BAR_Y0);
        for (int y = 0; y < BAR_H; y++) {
            draw_pixel(x, BAR_Y0 + y, color);
        }
    }
}

static void bar_init(void) {
    bar_cols  = BAR_W;
    bar_frame = 0;
}

static void bar_tick(void) {
    if (bar_cols == 0) return;
    if (++bar_frame >= FRAMES_PER_COL) {
        bar_frame = 0;
        --bar_cols;
    }
}

/* Check if frog is riding log */
static int check_frog_on_log(int fx,int fy,
                             int l0x,int l0y,int l0dx,
                             int l1x,int l1y,int l1dx,
                             int l2x,int l2y,int l2dx) {
    if (fy < RIVER_TOP || fy > RIVER_BOTTOM) return 0;
    if (fy == l0y && abs(fx - l0x) < TILE_W) return l0dx;
    if (fy == l1y && abs(fx - l1x) < TILE_W) return l1dx;
    if (fy == l2y && abs(fx - l2x) < TILE_W) return l2dx;
    return -999;  /* drowning */
}

/* Check for collisions with cars */
static int check_collision(int fx,int fy,int cx,int cy) {
    return (fy == cy && abs(fx - cx) <= TILE_W/2);
}

/* init GPIO & keypad */
static void init_io(void) {
    XGpio_Initialize(&gpio_we,        WE_DEVICE_ID);
    XGpio_Initialize(&gpio_addr,      ADDR_DEVICE_ID);
    XGpio_Initialize(&gpio_dat,       DAT_DEVICE_ID);
    XGpio_Initialize(&gpio_vsync,     VSYNC_DEVICE_ID);
    XGpio_Initialize(&gpio_frame_rdy, FRAME_RDY_DEVICE_ID);  // NEW

    XGpio_SetDataDirection(&gpio_we,        GPIO_CH, 0);           // Output
    XGpio_SetDataDirection(&gpio_addr,      GPIO_CH, 0);           // Output
    XGpio_SetDataDirection(&gpio_dat,       GPIO_CH, 0);           // Output
    XGpio_SetDataDirection(&gpio_vsync,     GPIO_CH, 0xFFFFFFFF);  // Input
    XGpio_SetDataDirection(&gpio_frame_rdy, GPIO_CH, 0xFFFFFFFF);  // Input

    KYPD_begin(&keypad, KYPD_GPIO_ID);
    KYPD_loadKeyTable(&keypad, (u8*)KEYTABLE);
}

/* Reset frog to starting position */
static void reset_frog(struct Obj *f) {
    f->x  = (FB_W - TILE_W) / 2;
    f->y  = FB_H - 2 * TILE_H;
    f->px = f->x;  f->py = f->y;
    bar_init();
}

/* Draw complete frame (hardware auto-cleared to black) */
static void draw_complete_frame(void) {
    // Draw background - hardware cleared to black, so only draw non-black tiles
    for (int ty = 0; ty < FB_H / TILE_H; ty++) {
        for (int tx = 0; tx < FB_W / TILE_W; tx++) {
            uint8_t tid = bg_tilemap[ty][tx];
            if (tid != 0) {  // Only draw non-black background tiles
                for (int dy = 0; dy < TILE_H; dy++) {
                    for (int dx = 0; dx < TILE_W; dx++) {
                        int x = tx * TILE_W + dx;
                        int y = ty * TILE_H + dy;
                        uint8_t color = background[tid][dy * TILE_W + dx];
                        if (color != 0) {  // Don't overdraw black pixels
                            draw_pixel(x, y, color);
                        }
                    }
                }
            }
        }
    }

    // Draw all lily-pad targets
    for (int i = 0; i < 5; i++) {
        if (!targets[i].filled) {
            draw_sprite(29, targets[i].x, 32);
        } else {
            // Draw filled target (frog on lily pad)
            draw_sprite(30, targets[i].x, 32);  // Assuming sprite 30 is filled lily pad
        }
    }

    // Draw timer bar
    bar_draw();

    // Draw logs
    draw_log(log0.x, log0.y, log0_len);
    draw_log(log1.x, log1.y, log1_len);
    draw_log(log2.x, log2.y, log2_len);

    // Draw cars
    draw_sprite(car0.idx, car0.x, car0.y);
    draw_sprite(car1.idx, car1.x, car1.y);

    // Draw frog
    draw_sprite(frog.idx, frog.x, frog.y);

    // Draw lives at bottom-left
    for (int i = 0; i < lives; i++)
        draw_sprite(2, 8 + i*20, 242);

    // Draw score icon at top-left
    draw_sprite(27, 8, 8);
}

/* Initialize or restart the game */
static void init_game(void) {
    // Reset game objects to starting positions and speeds
    log0 = (struct Obj){   0,  48,   0, 48, 48,  1 };  // Top log, moves right
    log1 = (struct Obj){ 128,  80, 128, 80, 48,  2 };  // Middle log, moves right
    log2 = (struct Obj){ 128,  96, 128, 96, 48,  1 };  // Bottom log, moves right
    car0 = (struct Obj){ 128, 208, 128,208,  4, -5 };  // Top car, moves left
    car1 = (struct Obj){  64, 176,  64,176,  8,  3 };  // Bottom car, moves right

    frog.idx = 2;
    frog.dx  = 0;
    reset_frog(&frog);
    log_dx = 0;

    // Reset all lily-pad targets
    for (int i = 0; i < 5; i++) {
        targets[i].filled = 0;
    }
}

int main(void) {
    init_io();
    init_game();

    uint16_t ks, st;
    uint8_t  key, last = 0;

    while (1) {
        // Wait for hardware to finish clearing back buffer and be ready
        wait_frame_ready();

        if (game_over) {
            // Draw game over screen
            draw_complete_frame();  // Draw base frame first
            // Add "GAME OVER" sprites
            draw_sprite(49,  60, 100);  // G
            draw_sprite(50,  80, 100);  // A
            draw_sprite(51, 100, 100);  // M
            draw_sprite(52, 120, 100);  // E

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

        // Update object positions
        log0.px = log0.x; log1.px = log1.x; log2.px = log2.x;
        car0.px = car0.x; car1.px = car1.x;

        // Move logs (wrap around screen edges)
        log0.x += log0.dx;
        if (log0.x > FB_W) log0.x = -log0_len * TILE_W;
        if (log0.x < -log0_len * TILE_W) log0.x = FB_W;

        log1.x += log1.dx;
        if (log1.x > FB_W) log1.x = -log1_len * TILE_W;
        if (log1.x < -log1_len * TILE_W) log1.x = FB_W;

        log2.x += log2.dx;
        if (log2.x > FB_W) log2.x = -log2_len * TILE_W;
        if (log2.x < -log2_len * TILE_W) log2.x = FB_W;

        // Move cars (wrap around screen edges)
        car0.x += car0.dx;
        if (car0.x > FB_W) car0.x = -TILE_W;
        if (car0.x < -TILE_W) car0.x = FB_W;

        car1.x += car1.dx;
        if (car1.x > FB_W) car1.x = -TILE_W;
        if (car1.x < -TILE_W) car1.x = FB_W;

        // Handle frog riding on logs
        if (log_dx) {
            frog.px = frog.x;
            frog.x += log_dx;
            // Check if frog fell off the side while riding log
            if (frog.x < 0 || frog.x > FB_W - TILE_W) {
                if (--lives <= 0) {
                    game_over = 1;
                    continue;
                }
                init_game();
                continue;
            }
        }

        // Handle player input
        ks = KYPD_getKeyStates(&keypad);
        st = KYPD_getKeyPressed(&keypad, ks, &key);
        if (st == KYPD_SINGLE_KEY && key != last) {
            frog.px = frog.x; frog.py = frog.y;
            int moved = 0;
            switch (key) {
                case '2': case '8': // Up
                    frog.y -= TILE_H;
                    moved = 1;
                    if (frog.y < frog.py) score += 10;  // Forward progress bonus
                    break;
                case '5': // Down
                    frog.y += TILE_H;
                    moved = 1;
                    break;
                case '4': // Left
                    frog.x -= TILE_W;
                    moved = 1;
                    break;
                case '6': // Right
                    frog.x += TILE_W;
                    moved = 1;
                    break;
            }
            // Keep frog on screen
            if (frog.x < 0) frog.x = 0;
            if (frog.x > FB_W - TILE_W) frog.x = FB_W - TILE_W;
            if (frog.y < 0) frog.y = 0;
            if (frog.y > FB_H - TILE_H) frog.y = FB_H - TILE_H;

            last = key;
            if (moved) {
                log_dx = 0;     // Stop riding log when moving
                bar_init();     // Reset timer when moving
            }
        }
        else if (st != KYPD_SINGLE_KEY) {
            last = 0;
        }

        // Check if frog reached lily pad
        if (frog.y < RIVER_TOP) {
            for (int i = 0; i < 5; i++) {
                if (!targets[i].filled &&
                    abs(frog.x - targets[i].x) < TILE_W)
                {
                    targets[i].filled = 1;
                    score += 100;

                    // Check if all lily pads filled (win condition)
                    int all_filled = 1;
                    for (int j = 0; j < 5; j++) {
                        if (!targets[j].filled) {
                            all_filled = 0;
                            break;
                        }
                    }
                    if (all_filled) {
                        game_over = 1;  // Victory!
                    } else {
                        init_game();    // Reset for next frog
                    }
                    continue;
                }
            }
        }

        // Check if frog is on a log (in river)
        log_dx = check_frog_on_log(
            frog.x, frog.y,
            log0.x, log0.y, log0.dx,
            log1.x, log1.y, log1.dx,
            log2.x, log2.y, log2.dx
        );
        if (log_dx == -999) {  // Frog is drowning
            if (--lives <= 0) {
                game_over = 1;
            } else {
                init_game();
            }
            continue;
        }

        // Check collisions with cars
        if (check_collision(frog.x,frog.y,car0.x,car0.y) ||
            check_collision(frog.x,frog.y,car1.x,car1.y)) {
            if (--lives <= 0) {
                game_over = 1;
            } else {
                init_game();
            }
            continue;
        }

        // Update timer bar
        bar_tick();
        if (bar_cols == 0) {  // Timer expired
            if (--lives <= 0) {
                game_over = 1;
            } else {
                init_game();
            }
            continue;
        }

        // Draw complete frame to cleared back buffer
        draw_complete_frame();
    }

    return 0;
}
