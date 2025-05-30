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

/* ------------------------------------------------------------------ */
/* Geometry */
#define FB_W     224
#define FB_H     256
#define TILE_W   SPR_W
#define TILE_H   SPR_H

/* ------------------------------------------------------------------ */
/* AXI-GPIO IDs (edit if BSP differs) */
#define WE_DEVICE_ID         XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID       XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID        XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID      XPAR_AXI_GPIO_VSYNC_DEVICE_ID
#define FRAME_RDY_DEVICE_ID  XPAR_AXI_GPIO_FRAME_RDY_DEVICE_ID
#define CPU_DONE_DEVICE_ID   XPAR_AXI_GPIO_CPU_DONE_DEVICE_ID
#define GPIO_CH              1

/* ------------------------------------------------------------------ */
/* Keypad (PMOD KYPD) */
#define KYPD_GPIO_ID  XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE      "0FED789C456B123A"

/* ------------------------------------------------------------------ */
/* Timer bar */
#define BAR_W          118
#define BAR_H          8
#define BAR_Y0         (FB_H - BAR_H)
#define BAR_X1         (FB_W - 32 - 1)
#define BAR_X0         (BAR_X1 - BAR_W + 1)
#define BAR_COLOR      0x6
#define BAR_FRAMES     (30 * 60)
#define FRAMES_PER_COL (BAR_FRAMES / BAR_W)

/* ------------------------------------------------------------------ */
/* River / road extents */
#define RIVER_TOP     48
#define RIVER_BOTTOM 112
#define ROAD_TOP     144
#define ROAD_BOTTOM  224

/* ------------------------------------------------------------------ */
/* Sprite indices */
#define SPR_HEART 31
#define SPR_DEAD  15

/* ------------------------------------------------------------------ */
/* ---------  ROW-SPECIFIC COUNTS (logs & turtles)  ------------------ */
/*  ► Change these numbers and *only that row* is affected            */

/* Logs */
#define LOGS_ROW0  3
#define LOGS_ROW1  2
#define LOGS_ROW2  3

/* Turtle clusters / turtles-per-cluster */
#define CLUSTERS_ROW0 4
#define TPC_ROW0      2           /* turtles per cluster (row 0) */
#define CLUSTERS_ROW1 4
#define TPC_ROW1      3           /* turtles per cluster (row 1) */

#define TURTLES_ROW0  (CLUSTERS_ROW0 * TPC_ROW0)
#define TURTLES_ROW1  (CLUSTERS_ROW1 * TPC_ROW1)

/* ------------------------------------------------------------------ */
/* Turtle dive timing */
#define DIVE_STAGES   30   /* frames 25 → 26 */
#define SURFACE_TIME  90   /* frames fully submerged */

/* ------------------------------------------------------------------ */
/* Generic object */
struct Obj {
    int x, y;      /* current pos */
    int px, py;    /* prev pos (for redraw optimisation) */
    int idx;       /* sprite index                       */
    int dx;        /* velocity (+→right, −→left)         */
};

/* ------------------------------------------------------------------ */
/* Globals */
static XGpio gpio_we, gpio_addr, gpio_dat;
static XGpio gpio_vsync, gpio_frame_rdy, gpio_done;
static PmodKYPD keypad;

/* Cars & frog */
static struct Obj car0, car1, frog;

/* Logs – three independent rows */
static struct Obj log_row0[LOGS_ROW0];
static struct Obj log_row1[LOGS_ROW1];
static struct Obj log_row2[LOGS_ROW2];

/* Turtles – two independent rows */
static struct Obj turtle_row0[TURTLES_ROW0];
static struct Obj turtle_row1[TURTLES_ROW1];

/* Turtle dive FSM state (per row) */
static int dive_timer_row[2]      = {0, 0};  /* current frame in cycle   */
static int submerged_row[2]       = {0, 0};  /* 0 = surfaced, 1 = down   */
static int diving_cluster_row[2]  = {1, 1};  /* which cluster is allowed
                                                to dive (0-based)        */

/* Log lengths (still per row, can be tweaked freely) */
static int log_row0_len = 4;
static int log_row1_len = 6;
static int log_row2_len = 3;

/* Lily-pad targets */
static struct { int x; int filled; } targets[5] =
    {{  8,0 },{ 56,0 },{104,0 },{152,0 },{200,0 }};

/* Game state */
static int lives = 0, score = 0, game_over = 0, log_dx = 0;
static int bar_cols, bar_frame;

/* ------------------------------------------------------------------ */
/* Fast-path pixel put – unchanged                                     */
static inline void draw_pixel_fast(int x,int y,uint8_t c4){
    uint16_t idx = y*FB_W + x;
    XGpio_DiscreteWrite(&gpio_addr,GPIO_CH,idx);
    XGpio_DiscreteWrite(&gpio_dat ,GPIO_CH,c4 & 0xF);
    XGpio_DiscreteWrite(&gpio_we  ,GPIO_CH,1);
    XGpio_DiscreteWrite(&gpio_we  ,GPIO_CH,0);
}
static inline void draw_pixel(int x,int y,uint8_t c4){
    if((unsigned)x<FB_W && (unsigned)y<FB_H) draw_pixel_fast(x,y,c4);
}

/* ------------------------------------------------------------------ */
/* Sprite & log drawing – unchanged bodies                             */
static void draw_sprite_fast(int n, int sx, int sy)
{
    if (sx < -TILE_W || sx >= FB_W || sy < -TILE_H || sy >= FB_H)
        return;

    const uint8_t *spr = sprites[n];
    for (int dy = 0; dy < 16; ++dy)
    {
        int y = sy + dy;
        if ((unsigned)y >= FB_H)
            continue;
        const uint8_t *row = &spr[dy << 4];
        for (int dx = 0; dx < 16; ++dx)
        {
            uint8_t c = row[dx];
            if (c)
            {
                int x = sx + dx;
                if ((unsigned)x < FB_W)
                    draw_pixel_fast(x, y, c);
            }
        }
    }
}
static void draw_log_fast(int x, int y, int len)
{
    if (len <= 0 || y < -TILE_H || y >= FB_H)
        return;
    if (x >= FB_W || x + len * TILE_W <= 0)
        return;

    if (len == 1)
    {
        draw_sprite_fast(47, x, y);
        return;
    }

    if (x > -TILE_W)
        draw_sprite_fast(46, x, y); /* left */
    if (x + (len - 1) * TILE_W < FB_W)
        draw_sprite_fast(48, x + (len - 1) * TILE_W, y); /* right */
    for (int i = 1; i < len - 1; ++i)
    {
        int tx = x + i * TILE_W;
        if (tx > -TILE_W && tx < FB_W)
            draw_sprite_fast(47, tx, y);
    }
}

/* ------------------------------------------------------------------ */
static void wait_vsync(void)
{
    while (!(XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1))
        ;
    while ((XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1))
        ;
}
static void wait_frame_ready(void)
{
    while (!(XGpio_DiscreteRead(&gpio_frame_rdy, GPIO_CH) & 1))
        ;
}

/* ------------------------------------------------------------------ */
static void bar_init(void){ bar_cols=BAR_W; bar_frame=0; }
static void bar_tick(void){
    if(bar_cols && ++bar_frame>=FRAMES_PER_COL){
        bar_frame=0; --bar_cols;
    }
}

/* ------------------------------------------------------------------ */
/* Frog-on-log/turtle check – uses *row specific* loop limits          */
static int check_frog_on_log(int fx,int fy){
    if(fy<RIVER_TOP || fy>RIVER_BOTTOM) return 0;

    /* ---- logs ---- */
    for(int i=0;i<LOGS_ROW0;++i)
        if(fy==log_row0[i].y && fx>=log_row0[i].x &&
           fx<log_row0[i].x+log_row0_len*TILE_W) return log_row0[i].dx;
    for(int i=0;i<LOGS_ROW1;++i)
        if(fy==log_row1[i].y && fx>=log_row1[i].x &&
           fx<log_row1[i].x+log_row1_len*TILE_W) return log_row1[i].dx;
    for(int i=0;i<LOGS_ROW2;++i)
        if(fy==log_row2[i].y && fx>=log_row2[i].x &&
           fx<log_row2[i].x+log_row2_len*TILE_W) return log_row2[i].dx;

    /* ---- turtles row 0 (y 64) ---- */
    if(fy==64){
        int sub=submerged_row[0];
        for(int c=0;c<CLUSTERS_ROW0;++c){
            if(sub && c==diving_cluster_row[0]) continue;
            for(int t=0;t<TPC_ROW0;++t){
                int idx=c*TPC_ROW0+t;
                int tx = turtle_row0[idx].x;
                if(fx>=tx && fx<tx+TILE_W) return turtle_row0[idx].dx;
            }
        }
    }
    /* ---- turtles row 1 (y 112) ---- */
    if(fy==112){
        int sub=submerged_row[1];
        for(int c=0;c<CLUSTERS_ROW1;++c){
            if(sub && c==diving_cluster_row[1]) continue;
            for(int t=0;t<TPC_ROW1;++t){
                int idx=c*TPC_ROW1+t;
                int tx = turtle_row1[idx].x;
                if(fx>=tx && fx<tx+TILE_W) return turtle_row1[idx].dx;
            }
        }
    }
    return -999;          /* drowning */
}
static int check_collision(int fx,int fy,int cx,int cy){
    return (fy==cy && abs(fx-cx)<=TILE_W/2);
}

/* ------------------------------------------------------------------ */
/* GPIO init */
static void init_io(void)
{
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

/* ------------------------------------------------------------------ */
/* Reset helpers */
static void reset_frog(struct Obj *f)
{
    f->x = (FB_W - TILE_W) / 2;
    f->y = FB_H - 2 * TILE_H;
    f->px = f->x;
    f->py = f->y;
    bar_init();
}
static void init_log_arrays(void){
    /* row 0 */
    for(int i=0;i<LOGS_ROW0;++i)
        log_row0[i]=(struct Obj){   i*96, 48, 0,0, 48, 1};
    /* row 1 */
    for(int i=0;i<LOGS_ROW1;++i)
        log_row1[i]=(struct Obj){60+i*120, 80, 0,0, 48, 2};
    /* row 2 */
    for(int i=0;i<LOGS_ROW2;++i)
        log_row2[i]=(struct Obj){30+i*120, 96, 0,0, 48, 1};
}

/* turtles: row-spec counts/macros */
static void init_turtle_clusters(void){
    /* -------- row 0 : 2-turtle clusters -------- */
    for(int c=0;c<CLUSTERS_ROW0;++c)
        for(int t=0;t<TPC_ROW0;++t){
            int i=c*TPC_ROW0+t;
            int spr = (c==diving_cluster_row[0])?25:22;   /* first dive cluster */
            turtle_row0[i]=(struct Obj){
                c*60 + t*TILE_W, 64, 0,0, spr, -2};
        }
    /* -------- row 1 : 3-turtle clusters -------- */
    for(int c=0;c<CLUSTERS_ROW1;++c)
        for(int t=0;t<TPC_ROW1;++t){
            int i=c*TPC_ROW1+t;
            int spr = (c==diving_cluster_row[1])?25:23;
            turtle_row1[i]=(struct Obj){
                c*64 + t*TILE_W, 112, 0,0, spr, -2};
        }

    dive_timer_row[0]=dive_timer_row[1]=0;
    submerged_row[0]=submerged_row[1]=0;
}

/* reset world (keep lives/score) */
static void reset_world(void)
{
    log_dx = 0;
    car0 = (struct Obj){128, 208, 0, 0, 4, -2};
    car1 = (struct Obj){64, 176, 0, 0, 8, 2};
    frog.idx = 2;
    frog.dx = 0;
    reset_frog(&frog);
    init_log_arrays();
    init_turtle_clusters();
    for (int i = 0; i < 5; ++i)
        targets[i].filled = 0;
}
/* new game */
static void start_new_game(void)
{
    lives = 3;
    score = 0;
    game_over = 0;
    reset_world();
}

/* ------------------------------------------------------------------- */
/*                              Draw frame                             */
static void draw_complete_frame(void)
{
    /* background tiles */
    for (int ty = 0; ty < FB_H / TILE_H; ++ty)
        for (int tx = 0; tx < FB_W / TILE_W; ++tx)
        {
            uint8_t tid = bg_tilemap[ty][tx];
            if (!tid)
                continue;
            const uint8_t *tile = background[tid];
            int bx = tx * TILE_W, by = ty * TILE_H;
            for (int dy = 0; dy < TILE_H; ++dy)
            {
                const uint8_t *row = &tile[dy * TILE_W];
                int y = by + dy;
                for (int dx = 0; dx < TILE_W; ++dx)
                {
                    uint8_t c = row[dx];
                    if (c)
                        draw_pixel_fast(bx + dx, y, c);
                }
            }
        }
    /* lily pads */
    for (int i = 0; i < 5; ++i)
        draw_sprite_fast(targets[i].filled ? 30 : 29, targets[i].x, 32);
    /* timer bar */
    for (int x = BAR_X0; x < BAR_X0 + bar_cols; ++x)
        for (int y = 0; y < BAR_H; ++y)
            draw_pixel_fast(x, BAR_Y0 + y, BAR_COLOR);
    /* logs */
    for(int i=0;i<LOGS_ROW0;++i) draw_log_fast(log_row0[i].x,
                                               log_row0[i].y,log_row0_len);
    for(int i=0;i<LOGS_ROW1;++i) draw_log_fast(log_row1[i].x,
                                               log_row1[i].y,log_row1_len);
    for(int i=0;i<LOGS_ROW2;++i) draw_log_fast(log_row2[i].x,
                                               log_row2[i].y,log_row2_len);

    /* turtles row 0 */
    for(int c=0;c<CLUSTERS_ROW0;++c){
        int hidden = (submerged_row[0] && c==diving_cluster_row[0]);
        for(int t=0;t<TPC_ROW0;++t){
            int idx=c*TPC_ROW0+t;
            if(!hidden) draw_sprite_fast(turtle_row0[idx].idx,
                                         turtle_row0[idx].x,
                                         turtle_row0[idx].y);
        }
    }
    /* turtles row 1 */
    for(int c=0;c<CLUSTERS_ROW1;++c){
        int hidden = (submerged_row[1] && c==diving_cluster_row[1]);
        for(int t=0;t<TPC_ROW1;++t){
            int idx=c*TPC_ROW1+t;
            if(!hidden) draw_sprite_fast(turtle_row1[idx].idx,
                                         turtle_row1[idx].x,
                                         turtle_row1[idx].y);
        }
    }

    /* cars & frog */
    draw_sprite_fast(car0.idx, car0.x, car0.y);
    draw_sprite_fast(car1.idx, car1.x, car1.y);
    draw_sprite_fast(frog.idx, frog.x, frog.y);
    /* hearts */
    for (int i = 0; i < lives; ++i)
        draw_sprite_fast(SPR_HEART, 8 + i * 20, 242);
    /* score icon */
    draw_sprite_fast(27, 8, 8);
}

/* ============================================================= *
 *                           MAIN                                *
 * ============================================================= */
int main(void)
{
    init_io();          /* GPIO + keypad */
    start_new_game();   /* lives = 3, reset world */

    uint16_t ks, st;    /* keypad state & transition */
    uint8_t  key, last = 0;

    while (1)
    {
        /* ------------ handshake with FPGA ------------ */
        XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 0);   /* not done */
        wait_frame_ready();                            /* back-buffer clear */

        /* --------------- GAME-OVER screen ------------- */
        if (game_over)
        {
            draw_complete_frame();
            draw_sprite_fast(49,  40, 100);  /* F */
            draw_sprite_fast(50,  60, 100);  /* R */
            draw_sprite_fast(51,  80, 100);  /* O */
            draw_sprite_fast(52, 100, 100);  /* G */
            draw_sprite_fast(52, 120, 100);  /* G */
            draw_sprite_fast(53, 140, 100);  /* E */
            draw_sprite_fast(50, 160, 100);  /* R */

            XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
            wait_vsync();

            ks = KYPD_getKeyStates(&keypad);
            st = KYPD_getKeyPressed(&keypad, ks, &key);
            if (st == KYPD_SINGLE_KEY && key == '5')
                start_new_game();
            continue;
        }

        /* -------- remember previous positions -------- */
        for (int i = 0; i < LOGS_ROW0; ++i) log_row0[i].px = log_row0[i].x;
        for (int i = 0; i < LOGS_ROW1; ++i) log_row1[i].px = log_row1[i].x;
        for (int i = 0; i < LOGS_ROW2; ++i) log_row2[i].px = log_row2[i].x;

        car0.px = car0.x;  car1.px = car1.x;
        frog.px = frog.x;  frog.py = frog.y;

        for (int i = 0; i < TURTLES_ROW0; ++i) turtle_row0[i].px = turtle_row0[i].x;
        for (int i = 0; i < TURTLES_ROW1; ++i) turtle_row1[i].px = turtle_row1[i].x;

        /* ---------------- move logs ------------------ */
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

        /* ---------------- move cars ------------------ */
        car0.x += car0.dx;
        if (car0.x > FB_W + TILE_W)  car0.x = -TILE_W;
        if (car0.x < -2 * TILE_W)    car0.x = FB_W + TILE_W;

        car1.x += car1.dx;
        if (car1.x > FB_W + TILE_W)  car1.x = -TILE_W;
        if (car1.x < -2 * TILE_W)    car1.x = FB_W + TILE_W;

        /* -------------- move turtles ----------------- */
        for (int i = 0; i < TURTLES_ROW0; ++i) {
            turtle_row0[i].x += turtle_row0[i].dx;
            if (turtle_row0[i].x > FB_W + TILE_W)  turtle_row0[i].x = -TILE_W;
            if (turtle_row0[i].x < -2 * TILE_W)    turtle_row0[i].x = FB_W + TILE_W;
        }
        for (int i = 0; i < TURTLES_ROW1; ++i) {
            turtle_row1[i].x += turtle_row1[i].dx;
            if (turtle_row1[i].x > FB_W + TILE_W)  turtle_row1[i].x = -TILE_W;
            if (turtle_row1[i].x < -2 * TILE_W)    turtle_row1[i].x = FB_W + TILE_W;
        }

        /* -------- turtle dive FSM (top row) ---------- */
        ++dive_timer_row[0];
        if (dive_timer_row[0] == DIVE_STAGES) {
            submerged_row[0] = 1;
            int c = diving_cluster_row[0];
            for (int t = 0; t < TPC_ROW0; ++t)
                turtle_row0[c * TPC_ROW0 + t].idx = 26;
        }
        else if (dive_timer_row[0] == DIVE_STAGES + SURFACE_TIME) {
            int c = diving_cluster_row[0];
            for (int t = 0; t < TPC_ROW0; ++t)
                turtle_row0[c * TPC_ROW0 + t].idx = 22;
            submerged_row[0] = 0;
            for (int t = 0; t < TPC_ROW0; ++t)
                turtle_row0[c * TPC_ROW0 + t].idx = 25;
            dive_timer_row[0] = 0;
        }

        /* -------- turtle dive FSM (bottom row) ------- */
        ++dive_timer_row[1];
        if (dive_timer_row[1] == DIVE_STAGES) {
            submerged_row[1] = 1;
            int c = diving_cluster_row[1];
            for (int t = 0; t < TPC_ROW1; ++t)
                turtle_row1[c * TPC_ROW1 + t].idx = 26;
        }
        else if (dive_timer_row[1] == DIVE_STAGES + SURFACE_TIME) {
            int c = diving_cluster_row[1];
            for (int t = 0; t < TPC_ROW1; ++t)
                turtle_row1[c * TPC_ROW1 + t].idx = 23;
            submerged_row[1] = 0;
            for (int t = 0; t < TPC_ROW1; ++t)
                turtle_row1[c * TPC_ROW1 + t].idx = 25;
            dive_timer_row[1] = 0;
        }

        /* ------------ carry frog with log/turtle ------ */
        if (log_dx) {
            frog.x += log_dx;
            if (frog.x < 0 || frog.x > FB_W - TILE_W) {
                if (--lives <= 0) game_over = 1;
                else              reset_world();
                continue;
            }
        }

        /* ---------------- keypad input ---------------- */
        ks = KYPD_getKeyStates(&keypad);
        st = KYPD_getKeyPressed(&keypad, ks, &key);
        if (st == KYPD_SINGLE_KEY && key != last)
        {
            int moved = 0;
            switch (key)
            {
                case '2': case '8':   /* up */
                    frog.y -= TILE_H; moved = 1;
                    if (frog.y < frog.py) score += 10;
                    break;
                case '5':             /* down */
                    frog.y += TILE_H; moved = 1; break;
                case '4':             /* left */
                    frog.x -= TILE_W; moved = 1; break;
                case '6':             /* right */
                    frog.x += TILE_W; moved = 1; break;
            }
            if (frog.x < 0)                  frog.x = 0;
            if (frog.x > FB_W - TILE_W)      frog.x = FB_W - TILE_W;
            if (frog.y < 0)                  frog.y = 0;
            if (frog.y > FB_H - TILE_H)      frog.y = FB_H - TILE_H;
            last = key;
            if (moved) log_dx = 0;           /* stop carry */
        }
        else if (st != KYPD_SINGLE_KEY)
            last = 0;

        /* ------------- lily-pad landing --------------- */
        if (frog.y < RIVER_TOP)
        {
            for (int i = 0; i < 5; ++i)
                if (!targets[i].filled && abs(frog.x - targets[i].x) < TILE_W)
                {
                    targets[i].filled = 1;
                    score += 100;

                    int all = 1;
                    for (int j = 0; j < 5; ++j)
                        if (!targets[j].filled) { all = 0; break; }

                    if (all) game_over = 1;
                    else     reset_frog(&frog);
                    break;
                }
        }

        /* -------- log / turtle collision -------------- */
        log_dx = check_frog_on_log(frog.x, frog.y);
        if (log_dx == -999)
        {
            draw_complete_frame();
            draw_sprite_fast(SPR_DEAD, frog.x, frog.y);
            XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
            wait_vsync();
            sleep(1);
            if (--lives <= 0) game_over = 1;
            else              reset_world();
            continue;
        }

        /* --------------- car collision ---------------- */
        if (check_collision(frog.x, frog.y, car0.x, car0.y) ||
            check_collision(frog.x, frog.y, car1.x, car1.y))
        {
            draw_complete_frame();
            draw_sprite_fast(SPR_DEAD, frog.x, frog.y);
            XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
            wait_vsync();
            sleep(1);
            if (--lives <= 0) game_over = 1;
            else              reset_world();
            continue;
        }

        /* ---------------- timer bar ------------------- */
        bar_tick();
        if (bar_cols == 0)
        {
            if (--lives <= 0) game_over = 1;
            else              reset_world();
            continue;
        }

        /* -------------- DRAW & PRESENT ---------------- */
        draw_complete_frame();
        XGpio_DiscreteWrite(&gpio_done, GPIO_CH, 1);
        wait_vsync();
    }
    return 0;
}
