#include "PmodKYPD.h"
#include "background.h"
#include "letters.h"
#include "numbers.h"
#include "sleep.h"
#include "sprites.h"
#include "xgpio.h"
#include "xil_types.h"
#include "xparameters.h"
#include <stdint.h>
#include <stdlib.h>

#define FB_W 224
#define FB_H 256
#define TILE_W 16
#define TILE_H 16

#define WE_DEVICE_ID XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID XPAR_AXI_GPIO_VSYNC_DEVICE_ID
#define FRAME_RDY_DEVICE_ID XPAR_AXI_GPIO_FRAME_RDY_DEVICE_ID
#define CPU_DONE_DEVICE_ID XPAR_AXI_GPIO_CPU_DONE_DEVICE_ID
#define GPIO_CH 1

#define KYPD_GPIO_ID XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE "0FED789C456B123A"

#define BAR_W 118
#define BAR_H 8
#define BAR_Y0 (FB_H - BAR_H)
#define BAR_X1 (FB_W - 32 - 1)
#define BAR_X0 (BAR_X1 - BAR_W + 1)
#define BAR_COLOR 0x6
#define BAR_FRAMES (30 * 60)
#define FRAMES_PER_COL (BAR_FRAMES / BAR_W)

#define RIVER_TOP 48
#define RIVER_BOTTOM 112
#define ROAD_TOP 144
#define ROAD_BOTTOM 224

#define SPR_HEART 31
#define SPR_DEAD 15

#define LOGS_ROW0 3
#define LOGS_ROW1 2
#define LOGS_ROW2 3

#define CLUSTERS_ROW0 4
#define TPC_ROW0 2
#define CLUSTERS_ROW1 4
#define TPC_ROW1 3
#define TURTLES_ROW0 (CLUSTERS_ROW0 * TPC_ROW0)
#define TURTLES_ROW1 (CLUSTERS_ROW1 * TPC_ROW1)

#define DIVE_STAGES 30
#define SURFACE_TIME 90

#define DIR_UP 0
#define DIR_RIGHT 1
#define DIR_DOWN 2
#define DIR_LEFT 3

#define ANIM_FRAMES 3
#define ANIM_SPEED 1

// Color constants
#define COLOR_WHITE 0xE
#define COLOR_RED 0xA

struct Obj {
    int x, y, px, py, idx, dx;
};

static XGpio gpio_we, gpio_addr, gpio_dat, gpio_vsync, gpio_frame_rdy, gpio_done;
static PmodKYPD keypad;

static struct Obj car0, car1, frog;
static struct Obj cars_208[3]; // 3 cars moving left on y=208 (sprite 3)
static struct Obj cars_192[3]; // 3 cars moving right on y=192 (sprite 4)
static struct Obj cars_176[3]; // 3 cars moving left on y=176 (sprite 7)
static struct Obj cars_160[2]; // 2 fast cars moving right on y=160 (sprite 8)
static struct Obj cars_144[2]; // 2 slow cars moving left on y=144 (sprite 5+6)
static struct Obj log_row0[LOGS_ROW0], log_row1[LOGS_ROW1], log_row2[LOGS_ROW2];
static struct Obj turtle_row0[TURTLES_ROW0], turtle_row1[TURTLES_ROW1];

static int dive_timer_row[2] = {0, 0};
static int submerged_row[2] = {0, 0};
static int diving_cluster_row[2] = {1, 1};
static int log_row0_len = 4, log_row1_len = 6, log_row2_len = 3;
static struct {
    int x;
    int filled;
} targets[5] = {{8, 0}, {56, 0}, {104, 0}, {152, 0}, {200, 0}};

static int lives = 0, score = 0, game_over = 0, log_dx = 0;
static int bar_cols, bar_frame, frog_dir = DIR_UP;
static int anim_timer = 0, is_animating = 0;
static int fly_timer = 0, fly_visible = 1, fly_target = -1;
static int captured_frog_timer = 0;
static int turtle_anim_timer = 0;
static int turtle_anim_frame = 0;
static int frog_highest_y; // Track highest y position reached (lowest y value)

static inline void draw_pixel_fast(int x, int y, uint8_t c) {
    uint16_t idx = y * FB_W + x;
    XGpio_DiscreteWrite(&gpio_addr, GPIO_CH, idx);
    XGpio_DiscreteWrite(&gpio_dat, GPIO_CH, c & 0xF);
    XGpio_DiscreteWrite(&gpio_we, GPIO_CH, 1);
    XGpio_DiscreteWrite(&gpio_we, GPIO_CH, 0);
}

static inline void draw_pixel(int x, int y, uint8_t c) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        draw_pixel_fast(x, y, c);
}

static void draw_sprite_flipped(int sprite_idx, int sx, int sy, int flip_h, int flip_v) {
    if (sx < -TILE_W || sx >= FB_W || sy < -TILE_H || sy >= FB_H)
        return;

    const uint8_t *spr = sprites[sprite_idx];
    for (int dy = 0; dy < 16; ++dy) {
        int y = sy + dy;
        if ((unsigned)y >= FB_H)
            continue;

        for (int dx = 0; dx < 16; ++dx) {
            int src_x = flip_h ? (15 - dx) : dx;
            int src_y = flip_v ? (15 - dy) : dy;

            uint8_t c = spr[src_y * 16 + src_x];
            if (c) {
                int x = sx + dx;
                if ((unsigned)x < FB_W)
                    draw_pixel_fast(x, y, c);
            }
        }
    }
}

static void draw_sprite_fast(int n, int sx, int sy) {
    draw_sprite_flipped(n, sx, sy, 0, 0);
}

static void draw_sprite_8x8_colored(const uint8_t *sprite_data, int sx, int sy, uint8_t color) {
    if (sx < -8 || sx >= FB_W || sy < -8 || sy >= FB_H)
        return;

    for (int dy = 0; dy < 8; ++dy) {
        int y = sy + dy;
        if ((unsigned)y >= FB_H)
            continue;

        for (int dx = 0; dx < 8; ++dx) {
            uint8_t c = sprite_data[dy * 8 + dx];
            if (c) {  // If pixel is not transparent (was 0xE in original)
                int x = sx + dx;
                if ((unsigned)x < FB_W)
                    draw_pixel_fast(x, y, color);
            }
        }
    }
}

static void draw_sprite_8x8(const uint8_t *sprite_data, int sx, int sy) {
    draw_sprite_8x8_colored(sprite_data, sx, sy, COLOR_WHITE);
}

static void draw_number_colored(int number, int x, int y, uint8_t color) {
    // Handle negative numbers or zero
    if (number <= 0) {
        draw_sprite_8x8_colored(numbers[0], x, y, color);
        return;
    }
    
    // Prevent score from getting too large (avoid overflow)
    if (number > 999999) {
        number = 999999;
    }
    
    // Count digits to position correctly
    int temp = number;
    int digit_count = 0;
    while (temp > 0) {
        temp /= 10;
        digit_count++;
    }
    
    // Draw digits from right to left
    int current_x = x + (digit_count - 1) * 8;
    while (number > 0) {
        int digit = number % 10;
        draw_sprite_8x8_colored(numbers[digit], current_x, y, color);
        number /= 10;
        current_x -= 8;
    }
}

static void draw_number(int number, int x, int y) {
    draw_number_colored(number, x, y, COLOR_WHITE);
}

// Draw a single letter (A-Z) with specified color
static void draw_letter_colored(char letter, int x, int y, uint8_t color) {
    if (letter >= 'A' && letter <= 'Z') {
        draw_sprite_8x8_colored(letters[letter - 'A'], x, y, color);
    } else if (letter >= 'a' && letter <= 'z') {
        draw_sprite_8x8_colored(letters[letter - 'a'], x, y, color);  // Convert to uppercase
    }
}

// Draw a single letter (A-Z)
static void draw_letter(char letter, int x, int y) {
    draw_letter_colored(letter, x, y, COLOR_WHITE);
}

// Draw a string of text (letters and numbers only) with specified color
static void draw_text_colored(const char* text, int x, int y, uint8_t color) {
    int current_x = x;
    
    while (*text) {
        char c = *text;
        
        if (c >= 'A' && c <= 'Z') {
            draw_sprite_8x8_colored(letters[c - 'A'], current_x, y, color);
        } else if (c >= 'a' && c <= 'z') {
            draw_sprite_8x8_colored(letters[c - 'a'], current_x, y, color);  // Convert to uppercase
        } else if (c >= '0' && c <= '9') {
            draw_sprite_8x8_colored(numbers[c - '0'], current_x, y, color);
        } else if (c == ' ') {
            // Space - just advance position
        }
        // Skip any other characters
        
        current_x += 8;  // Move to next character position
        text++;
    }
}

// Draw a string of text (letters and numbers only)
static void draw_text(const char* text, int x, int y) {
    draw_text_colored(text, x, y, COLOR_WHITE);
}

// Get turtle animation sprite index
static int get_turtle_sprite(int base_sprite, int is_diving) {
    if (is_diving) {
        return 25;  // Diving turtles use sprite 25
    }

    // Animation sequence: 22 -> 23 -> 24 -> 23 -> 22
    switch (turtle_anim_frame) {
        case 0: return 22;
        case 1: return 23;
        case 2: return 24;
        case 3: return 23;
        default: return 22;
    }
}

// Update turtle animation
static void update_turtle_animation() {
    turtle_anim_timer++;
    if (turtle_anim_timer >= 3) {  // Change frame every 3 game frames for slower animation
        turtle_anim_timer = 0;
        turtle_anim_frame++;
        if (turtle_anim_frame >= 4) {
            turtle_anim_frame = 0;  // Loop back to start
        }
        
        // Update all non-diving turtle sprites for row 0
        for (int c = 0; c < CLUSTERS_ROW0; ++c) {
                for (int t = 0; t < TPC_ROW0; ++t) {
                    int idx = c * TPC_ROW0 + t;
                    turtle_row0[idx].idx = get_turtle_sprite(22, 0);  // Pass 0 for is_diving
                }
        }
        
        // Update all non-diving turtle sprites for row 1
        for (int c = 0; c < CLUSTERS_ROW1; ++c) {
                for (int t = 0; t < TPC_ROW1; ++t) {
                    int idx = c * TPC_ROW1 + t;
                    turtle_row1[idx].idx = get_turtle_sprite(23, 0);  // Pass 0 for is_diving
                }
        }
    }
}

static int get_frog_sprite() {
    if (!is_animating)
        return 2;

    int frame = anim_timer / ANIM_SPEED;
    int anim_sequence[] = {2, 0, 2};
    return anim_sequence[frame % 3];
}

static void draw_frog() {
    int sprite = get_frog_sprite();

    if (frog.x < -TILE_W || frog.x >= FB_W || frog.y < -TILE_H || frog.y >= FB_H)
        return;

    const uint8_t *spr = sprites[sprite];

    for (int dy = 0; dy < 16; ++dy) {
        int y = frog.y + dy;
        if ((unsigned)y >= FB_H)
            continue;

        for (int dx = 0; dx < 16; ++dx) {
            int src_x, src_y;

            // Calculate source coordinates based on direction
            switch (frog_dir) {
            case DIR_UP:
                src_x = dx;
                src_y = dy;
                break;
            case DIR_DOWN:
                src_x = 15 - dx;
                src_y = 15 - dy;
                break;
            case DIR_LEFT:
                src_x = 15 - dy;
                src_y = dx;
                break;
            case DIR_RIGHT:
                src_x = dy;
                src_y = 15 - dx;
                break;
            default:
                src_x = dx;
                src_y = dy;
                break;
            }

            uint8_t c = spr[src_y * 16 + src_x];
            if (c) {
                int x = frog.x + dx;
                if ((unsigned)x < FB_W)
                    draw_pixel_fast(x, y, c);
            }
        }
    }
}

static void draw_slow_car(int x, int y) {
    // Draw sprite 5 for first half
    draw_sprite_fast(5, x, y);
    // Draw sprite 6 for second half
    draw_sprite_fast(6, x + TILE_W, y);
}

static void draw_log_fast(int x, int y, int len) {
    if (len <= 0 || y < -TILE_H || y >= FB_H)
        return;
    if (x >= FB_W || x + len * TILE_W <= 0)
        return;

    if (len == 1) {
        draw_sprite_fast(47, x, y);
        return;
    }

    if (x > -TILE_W)
        draw_sprite_fast(46, x, y);
    if (x + (len - 1) * TILE_W < FB_W)
        draw_sprite_fast(48, x + (len - 1) * TILE_W, y);
    for (int i = 1; i < len - 1; ++i) {
        int tx = x + i * TILE_W;
        if (tx > -TILE_W && tx < FB_W)
            draw_sprite_fast(47, tx, y);
    }
}

static void wait_vsync(void) {
    while (!(XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1))
        ;
    while ((XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1))
        ;
}

static void wait_frame_ready(void) {
    while (!(XGpio_DiscreteRead(&gpio_frame_rdy, GPIO_CH) & 1))
        ;
}

static void bar_init(void) {
    bar_cols = BAR_W;
    bar_frame = 0;
}
static void bar_tick(void) {
    if (bar_cols && ++bar_frame >= FRAMES_PER_COL) {
        bar_frame = 0;
        --bar_cols;
    }
    
    // Prevent overflow
    if (bar_frame > 10000) bar_frame = 0;
    if (bar_cols < 0) bar_cols = 0;
}

static void update_fly_system() {
    fly_timer++;
    
    // Prevent timer overflow
    if (fly_timer > 10000) fly_timer = 0;

    if (fly_timer >= 30) {          // 30 frames = your desired timing
        fly_visible = !fly_visible; // Toggle visibility
        fly_timer = 0;

        if (fly_visible) {
            // Find available lily pads (not filled)
            int available[5];
            int count = 0;
            for (int i = 0; i < 5; i++) {
                if (!targets[i].filled) {
                    available[count++] = i;
                }
            }

            if (count > 0) {
                // Use frame counter + other variables for better randomness
                static int seed = 1;
                seed = (seed * 1103515245 + 12345) & 0x7fffffff; // Simple LCG
                int random_index = seed % count;
                fly_target = available[random_index];
            } else {
                fly_visible = 0; // No available pads
                fly_target = -1;
            }
        } else {
            fly_target = -1;
        }
    }

    // Update captured frog animation (2 seconds = 120 frames)
    captured_frog_timer++;
    
    // Prevent timer overflow
    if (captured_frog_timer > 10000) captured_frog_timer = 0;
    
    if (captured_frog_timer >= 120) {
        captured_frog_timer = 0;
    }
}

static void update_animation() {
    if (is_animating) {
        anim_timer++;
        
        // Prevent overflow
        if (anim_timer > 1000) {
            is_animating = 0;
            anim_timer = 0;
        }
        
        if (anim_timer >= ANIM_FRAMES) {
            is_animating = 0;
            anim_timer = 0;
        }
    }
}

static void start_animation() {
    is_animating = 1;
    anim_timer = 0;
}

static int check_frog_on_log(int fx, int fy) {
    if (fy < RIVER_TOP || fy > RIVER_BOTTOM)
        return 0;

    for (int i = 0; i < LOGS_ROW0; ++i)
        if (fy == log_row0[i].y && fx >= log_row0[i].x && fx < log_row0[i].x + log_row0_len * TILE_W)
            return log_row0[i].dx;
    for (int i = 0; i < LOGS_ROW1; ++i)
        if (fy == log_row1[i].y && fx >= log_row1[i].x && fx < log_row1[i].x + log_row1_len * TILE_W)
            return log_row1[i].dx;
    for (int i = 0; i < LOGS_ROW2; ++i)
        if (fy == log_row2[i].y && fx >= log_row2[i].x && fx < log_row2[i].x + log_row2_len * TILE_W)
            return log_row2[i].dx;

    if (fy == 64) {
        int sub = submerged_row[0];
        for (int c = 0; c < CLUSTERS_ROW0; ++c) {
            if (sub && c == diving_cluster_row[0])
                continue;
            for (int t = 0; t < TPC_ROW0; ++t) {
                int idx = c * TPC_ROW0 + t;
                int tx = turtle_row0[idx].x;
                if (fx >= tx && fx < tx + TILE_W)
                    return turtle_row0[idx].dx;
            }
        }
    }
    if (fy == 112) {
        int sub = submerged_row[1];
        for (int c = 0; c < CLUSTERS_ROW1; ++c) {
            if (sub && c == diving_cluster_row[1])
                continue;
            for (int t = 0; t < TPC_ROW1; ++t) {
                int idx = c * TPC_ROW1 + t;
                int tx = turtle_row1[idx].x;
                if (fx >= tx && fx < tx + TILE_W)
                    return turtle_row1[idx].dx;
            }
        }
    }
    return -999;
}

static int check_collision(int fx, int fy, int cx, int cy) {
    return (fy == cy && abs(fx - cx) <= TILE_W / 2);
}

static int check_car_collisions(int fx, int fy) {
    for (int i = 0; i < 3; i++) {
        if (check_collision(fx, fy, cars_208[i].x, cars_208[i].y) ||
            check_collision(fx, fy, cars_192[i].x, cars_192[i].y) ||
            check_collision(fx, fy, cars_176[i].x, cars_176[i].y))
            return 1;
    }
    for (int i = 0; i < 2; i++) {
        if (check_collision(fx, fy, cars_160[i].x, cars_160[i].y))
            return 1;
        // Check collision with 32px wide slow cars (sprite 5+6)
        if (fy == cars_144[i].y && fx >= cars_144[i].x - TILE_W / 2 && fx <= cars_144[i].x + TILE_W + TILE_W / 2)
            return 1;
    }
    return 0;
}

static void init_io(void) {
    XGpio_Initialize(&gpio_we, WE_DEVICE_ID);
    XGpio_Initialize(&gpio_addr, ADDR_DEVICE_ID);
    XGpio_Initialize(&gpio_dat, DAT_DEVICE_ID);
    XGpio_Initialize(&gpio_vsync, VSYNC_DEVICE_ID);
    XGpio_Initialize(&gpio_frame_rdy, FRAME_RDY_DEVICE_ID);
    XGpio_Initialize(&gpio_done, CPU_DONE_DEVICE_ID);

    XGpio_SetDataDirection(&gpio_we, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_addr, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_dat, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_done, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_vsync, GPIO_CH, 0xFFFFFFFF);
    XGpio_SetDataDirection(&gpio_frame_rdy, GPIO_CH, 0xFFFFFFFF);

    XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 0);

    KYPD_begin(&keypad, KYPD_GPIO_ID);
    KYPD_loadKeyTable(&keypad, (u8 *)KEYTABLE);
}

static void reset_frog(struct Obj *f) {
    f->x = (FB_W - TILE_W) / 2;
    f->y = FB_H - 2 * TILE_H;
    f->px = f->x;
    f->py = f->y;
    frog_dir = DIR_UP;
    is_animating = 0;
    anim_timer = 0;
    bar_init();
    fly_timer = 0;
    fly_visible = 1;
    captured_frog_timer = 0;
    turtle_anim_timer = 0;
    turtle_anim_frame = 0;
    frog_highest_y = f->y; // Reset highest y position tracker
}

static void init_log_arrays(void) {
    for (int i = 0; i < LOGS_ROW0; ++i)
        log_row0[i] = (struct Obj){i * 96, 48, 0, 0, 48, 1};
    for (int i = 0; i < LOGS_ROW1; ++i)
        log_row1[i] = (struct Obj){60 + i * 120, 80, 0, 0, 48, 2};
    for (int i = 0; i < LOGS_ROW2; ++i)
        log_row2[i] = (struct Obj){30 + i * 120, 96, 0, 0, 48, 1};
}

static void init_turtle_clusters(void) {
    for (int c = 0; c < CLUSTERS_ROW0; ++c)
        for (int t = 0; t < TPC_ROW0; ++t) {
            int i = c * TPC_ROW0 + t;
            int is_diving = (c == diving_cluster_row[0]);
            int spr = get_turtle_sprite(22, is_diving);
            turtle_row0[i] = (struct Obj){c * 60 + t * TILE_W, 64, 0, 0, spr, -2};
        }
    for (int c = 0; c < CLUSTERS_ROW1; ++c)
        for (int t = 0; t < TPC_ROW1; ++t) {
            int i = c * TPC_ROW1 + t;
            int is_diving = (c == diving_cluster_row[1]);
            int spr = get_turtle_sprite(23, is_diving);
            turtle_row1[i] = (struct Obj){c * 64 + t * TILE_W, 112, 0, 0, spr, -2};
        }

    dive_timer_row[0] = dive_timer_row[1] = 0;
    submerged_row[0] = submerged_row[1] = 0;
    turtle_anim_timer = 0;
    turtle_anim_frame = 0;
}

static void reset_world(void) {
    log_dx = 0;

    // Initialize all car rows
    for (int i = 0; i < 3; i++) {
        cars_208[i] = (struct Obj){i * 80 + 50, 208, 0, 0, 3, -1}; // Left moving
        cars_192[i] = (struct Obj){i * 90 + 30, 192, 0, 0, 4, 2};  // Right moving
        cars_176[i] = (struct Obj){i * 85 + 70, 176, 0, 0, 7, -1}; // Left moving
    }

    for (int i = 0; i < 2; i++) {
        cars_160[i] = (struct Obj){i * 120 + 40, 160, 0, 0, 8, 3};  // Fast right
        cars_144[i] = (struct Obj){i * 180 + 60, 144, 0, 0, 5, -1}; // Slow left (32px wide cars)
    }

    frog.idx = 2;
    frog.dx = 0;
    reset_frog(&frog);
    init_log_arrays();
    init_turtle_clusters();
}

static void start_new_game(void) {
    lives = 3;
    score = 0;
    game_over = 0;
    srand(0);
    
    // Reset lily pads only when starting a new game
    for (int i = 0; i < 5; ++i)
        targets[i].filled = 0;
        
    reset_world();
}

static void draw_complete_frame(void) {
    // Draw background tiles (with the skip check restored for performance)
    for (int ty = 0; ty < FB_H / TILE_H; ++ty)
        for (int tx = 0; tx < FB_W / TILE_W; ++tx) {
            uint8_t tid = bg_tilemap[ty][tx];
            if (!tid)
                continue; // Restore this for performance

            const uint8_t *tile = background[tid];
            int bx = tx * TILE_W, by = ty * TILE_H;
            for (int dy = 0; dy < TILE_H; ++dy) {
                const uint8_t *row = &tile[dy * TILE_W];
                int y = by + dy;
                for (int dx = 0; dx < TILE_W; ++dx) {
                    uint8_t c = row[dx];
                    if (c)
                        draw_pixel_fast(bx + dx, y, c);
                }
            }
        }

    // Draw lily pads (these are on water)
    for (int i = 0; i < 5; ++i) {
        if (targets[i].filled) {
            int captured_sprite = (captured_frog_timer < 60) ? 30 : 31;
            draw_sprite_fast(captured_sprite, targets[i].x, 32);
        }
        if (fly_visible && fly_target == i) {
            draw_sprite_fast(29, targets[i].x, 32);
        }
    }

    // Draw timer bar
    for (int x = BAR_X0 + (BAR_W - bar_cols); x < BAR_X0 + BAR_W; ++x)
        for (int y = 0; y < BAR_H; ++y)
            draw_pixel_fast(x, BAR_Y0 + y, BAR_COLOR);

    // Draw logs (these should have blue background built into the sprites)
    for (int i = 0; i < LOGS_ROW0; ++i)
        draw_log_fast(log_row0[i].x, log_row0[i].y, log_row0_len);
    for (int i = 0; i < LOGS_ROW1; ++i)
        draw_log_fast(log_row1[i].x, log_row1[i].y, log_row1_len);
    for (int i = 0; i < LOGS_ROW2; ++i)
        draw_log_fast(log_row2[i].x, log_row2[i].y, log_row2_len);

    // Draw turtles (these should have blue background built into the sprites)
    for (int c = 0; c < CLUSTERS_ROW0; ++c) {
        int hidden = (submerged_row[0] && c == diving_cluster_row[0]);
        for (int t = 0; t < TPC_ROW0; ++t) {
            int idx = c * TPC_ROW0 + t;
            if (!hidden)
                draw_sprite_fast(turtle_row0[idx].idx, turtle_row0[idx].x, turtle_row0[idx].y);
        }
    }
    for (int c = 0; c < CLUSTERS_ROW1; ++c) {
        int hidden = (submerged_row[1] && c == diving_cluster_row[1]);
        for (int t = 0; t < TPC_ROW1; ++t) {
            int idx = c * TPC_ROW1 + t;
            if (!hidden)
                draw_sprite_fast(turtle_row1[idx].idx, turtle_row1[idx].x, turtle_row1[idx].y);
        }
    }

    // Draw cars (these are on road, not water)
    for (int i = 0; i < 3; i++) {
        draw_sprite_fast(cars_208[i].idx, cars_208[i].x, cars_208[i].y);
        draw_sprite_fast(cars_192[i].idx, cars_192[i].x, cars_192[i].y);
        draw_sprite_fast(cars_176[i].idx, cars_176[i].x, cars_176[i].y);
    }
    for (int i = 0; i < 2; i++) {
        draw_sprite_fast(cars_160[i].idx, cars_160[i].x, cars_160[i].y);
        draw_slow_car(cars_144[i].x, cars_144[i].y);
    }

    // Draw frog
    draw_frog();

    // Draw lives and score using sprite #2 instead of life.h
    for (int i = 0; i < lives; ++i)
        draw_sprite_fast(2, 8 + i * 16, 242);
    
    // Draw score label and number
    draw_sprite_fast(27, 8, 8);  // Score label sprite
    draw_number(score, 60, 8);   // Score value
}

int main(void) {
    init_io();
    start_new_game();

    uint16_t ks, st;
    uint8_t key, last = 0;

    while (1) {
        XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 0);
        wait_frame_ready();

        update_animation();
        update_fly_system();
        update_turtle_animation();

        if (game_over) {
            draw_complete_frame();
            
            // Enhanced game over screen with RED text
            draw_text_colored("GAME OVER", 64, 100, COLOR_RED);
            draw_text_colored("FINAL SCORE", 48, 120, COLOR_RED);
            draw_number_colored(score, 96, 140, COLOR_RED);
            draw_text_colored("PRESS 5 TO RESTART", 32, 180, COLOR_RED);

            XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
            wait_vsync();

            ks = KYPD_getKeyStates(&keypad);
            st = KYPD_getKeyPressed(&keypad, ks, &key);
            if (st == KYPD_SINGLE_KEY && key == '5')
                start_new_game();
            continue;
        }

        // Store previous positions
        for (int i = 0; i < LOGS_ROW0; ++i)
            log_row0[i].px = log_row0[i].x;
        for (int i = 0; i < LOGS_ROW1; ++i)
            log_row1[i].px = log_row1[i].x;
        for (int i = 0; i < LOGS_ROW2; ++i)
            log_row2[i].px = log_row2[i].x;

        for (int i = 0; i < 3; i++) {
            cars_208[i].px = cars_208[i].x;
            cars_192[i].px = cars_192[i].x;
            cars_176[i].px = cars_176[i].x;
        }
        for (int i = 0; i < 2; i++) {
            cars_160[i].px = cars_160[i].x;
            cars_144[i].px = cars_144[i].x;
        }

        frog.px = frog.x;
        frog.py = frog.y;
        for (int i = 0; i < TURTLES_ROW0; ++i)
            turtle_row0[i].px = turtle_row0[i].x;
        for (int i = 0; i < TURTLES_ROW1; ++i)
            turtle_row1[i].px = turtle_row1[i].x;

        // Move logs
        for (int i = 0; i < LOGS_ROW0; ++i) {
            log_row0[i].x += log_row0[i].dx;
            if (log_row0[i].x > FB_W + TILE_W)
                log_row0[i].x = -(log_row0_len * TILE_W);
            if (log_row0[i].x < -(log_row0_len * TILE_W) - TILE_W)
                log_row0[i].x = FB_W + TILE_W;
        }
        for (int i = 0; i < LOGS_ROW1; ++i) {
            log_row1[i].x += log_row1[i].dx;
            if (log_row1[i].x > FB_W + TILE_W)
                log_row1[i].x = -(log_row1_len * TILE_W);
            if (log_row1[i].x < -(log_row1_len * TILE_W) - TILE_W)
                log_row1[i].x = FB_W + TILE_W;
        }
        for (int i = 0; i < LOGS_ROW2; ++i) {
            log_row2[i].x += log_row2[i].dx;
            if (log_row2[i].x > FB_W + TILE_W)
                log_row2[i].x = -(log_row2_len * TILE_W);
            if (log_row2[i].x < -(log_row2_len * TILE_W) - TILE_W)
                log_row2[i].x = FB_W + TILE_W;
        }

        // Move cars
        for (int i = 0; i < 3; i++) {
            cars_208[i].x += cars_208[i].dx;
            if (cars_208[i].x < -2 * TILE_W)
                cars_208[i].x = FB_W + TILE_W;

            cars_192[i].x += cars_192[i].dx;
            if (cars_192[i].x > FB_W + TILE_W)
                cars_192[i].x = -TILE_W;

            cars_176[i].x += cars_176[i].dx;
            if (cars_176[i].x < -2 * TILE_W)
                cars_176[i].x = FB_W + TILE_W;
        }

        for (int i = 0; i < 2; i++) {
            cars_160[i].x += cars_160[i].dx;
            if (cars_160[i].x > FB_W + TILE_W)
                cars_160[i].x = -TILE_W;

            cars_144[i].x += cars_144[i].dx;
            if (cars_144[i].x < -3 * TILE_W)
                cars_144[i].x = FB_W + TILE_W; // Account for 32px width
        }

        // Move turtles
        for (int i = 0; i < TURTLES_ROW0; ++i) {
            turtle_row0[i].x += turtle_row0[i].dx;
            if (turtle_row0[i].x > FB_W + TILE_W)
                turtle_row0[i].x = -TILE_W;
            if (turtle_row0[i].x < -2 * TILE_W)
                turtle_row0[i].x = FB_W + TILE_W;
        }
        for (int i = 0; i < TURTLES_ROW1; ++i) {
            turtle_row1[i].x += turtle_row1[i].dx;
            if (turtle_row1[i].x > FB_W + TILE_W)
                turtle_row1[i].x = -TILE_W;
            if (turtle_row1[i].x < -2 * TILE_W)
                turtle_row1[i].x = FB_W + TILE_W;
        }

        // Turtle dive FSM
        ++dive_timer_row[0];
        
        // Prevent timer overflow
        if (dive_timer_row[0] > 10000) dive_timer_row[0] = 0;
        
        if (dive_timer_row[0] == DIVE_STAGES) {
            submerged_row[0] = 1;
            int c = diving_cluster_row[0];
            for (int t = 0; t < TPC_ROW0; ++t)
                turtle_row0[c * TPC_ROW0 + t].idx = 26;
        } else if (dive_timer_row[0] == DIVE_STAGES + SURFACE_TIME) {
            int c = diving_cluster_row[0];
            submerged_row[0] = 0;
            // Reset to animated sprites when surfacing - FIX: pass 0 for is_diving
            for (int t = 0; t < TPC_ROW0; ++t)
                turtle_row0[c * TPC_ROW0 + t].idx = 23;  // Changed from 1 to 0
            dive_timer_row[0] = 0;
        }

        ++dive_timer_row[1];
        
        // Prevent timer overflow
        if (dive_timer_row[1] > 10000) dive_timer_row[1] = 0;
        
        if (dive_timer_row[1] == DIVE_STAGES) {
            submerged_row[1] = 1;
            int c = diving_cluster_row[1];
            for (int t = 0; t < TPC_ROW1; ++t)
                turtle_row1[c * TPC_ROW1 + t].idx = 26;
        } else if (dive_timer_row[1] == DIVE_STAGES + SURFACE_TIME) {
            int c = diving_cluster_row[1];
            submerged_row[1] = 0;
            // Reset to animated sprites when surfacing - FIX: pass 0 for is_diving
            for (int t = 0; t < TPC_ROW1; ++t)
                turtle_row1[c * TPC_ROW1 + t].idx = 23;  // Changed from 1 to 0
            dive_timer_row[1] = 0;
        }

        // Carry frog with log/turtle
        if (log_dx) {
            frog.x += log_dx;
            if (frog.x < 0 || frog.x > FB_W - TILE_W) {
                if (--lives <= 0)
                    game_over = 1;
                else
                    reset_world();
                continue;
            }
        }

        // Keypad input
        ks = KYPD_getKeyStates(&keypad);
        st = KYPD_getKeyPressed(&keypad, ks, &key);
        if (st == KYPD_SINGLE_KEY && key != last) {
            int moved = 0;
            switch (key) {
            case '2':
            case '8':
                start_animation(); // Start animation before moving
                frog.y -= TILE_H;
                moved = 1;
                frog_dir = DIR_UP;
                // Only award points if reaching a new highest position
                if (frog.y < frog_highest_y) {
                    score += 10;
                    frog_highest_y = frog.y;  // Update highest position
                    // Prevent score overflow
                    if (score < 0) score = 999999;
                }
                break;
            case '5':
                start_animation(); // Start animation before moving
                frog.y += TILE_H;
                moved = 1;
                frog_dir = DIR_DOWN;
                break;
            case '4':
                start_animation(); // Start animation before moving
                frog.x -= TILE_W;
                moved = 1;
                frog_dir = DIR_LEFT;
                break;
            case '6':
                start_animation(); // Start animation before moving
                frog.x += TILE_W;
                moved = 1;
                frog_dir = DIR_RIGHT;
                break;
            }
            if (frog.x < 0)
                frog.x = 0;
            if (frog.x > FB_W - TILE_W)
                frog.x = FB_W - TILE_W;
            if (frog.y < 0)
                frog.y = 0;
            if (frog.y > FB_H - TILE_H)
                frog.y = FB_H - TILE_H;
            last = key;
            if (moved)
                log_dx = 0;
        } else if (st != KYPD_SINGLE_KEY)
            last = 0;

        // Lily-pad landing
        if (frog.y < RIVER_TOP) {
            for (int i = 0; i < 5; ++i)
                if (!targets[i].filled && abs(frog.x - targets[i].x) < TILE_W) {
                    targets[i].filled = 1;

                    // Check if fly is on this lily pad for bonus points
                    if (fly_visible && fly_target == i) {
                        score += 200;
                        // Prevent score overflow
                        if (score < 0) score = 999999;
                        fly_visible = 0; // Remove fly
                        fly_target = -1;
                        fly_timer = 0;
                    } else {
                        score += 100;
                        // Prevent score overflow
                        if (score < 0) score = 999999;
                    }

                    int all = 1;
                    for (int j = 0; j < 5; ++j)
                        if (!targets[j].filled) {
                            all = 0;
                            break;
                        }

                    if (all)
                        game_over = 1;
                    else
                        reset_frog(&frog);
                    break;
                }
        }

        // Log/turtle collision
        log_dx = check_frog_on_log(frog.x, frog.y);
        if (log_dx == -999) {
            draw_complete_frame();
            draw_sprite_fast(SPR_DEAD, frog.x, frog.y);
            XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
            wait_vsync();
            sleep(1);
            if (--lives <= 0)
                game_over = 1;
            else
                reset_world();
            continue;
        }

        // Car collision
        if (check_car_collisions(frog.x, frog.y)) {
            draw_complete_frame();
            draw_sprite_fast(SPR_DEAD, frog.x, frog.y);
            XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
            wait_vsync();
            sleep(1);
            if (--lives <= 0)
                game_over = 1;
            else
                reset_world();
            continue;
        }

        // Timer bar
        bar_tick();
        if (bar_cols == 0) {
            if (--lives <= 0)
                game_over = 1;
            else
                reset_world();
            continue;
        }

        draw_complete_frame();
        XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
        wait_vsync();
    }
    return 0;
}