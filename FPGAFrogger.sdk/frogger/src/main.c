//  Frogger (Basys-3)  VSYNC-locked renderer
#include "xparameters.h"
#include "xgpio.h"
#include "xil_types.h"
#include "sleep.h"            // busy-wait helpers (no usleep per pixel)
#include "PmodKYPD.h"
#include <stdint.h>
#include <stdlib.h>
 
#include "sprites.h"          // frog & log sprites (16Ã—16)
#include "background.h"       // background tile-map
#undef  NUM_SPRITES           // silence duplicate warning
 
/*  framebuffer geometry  */
#define FB_W 224
#define FB_H 256
#define TILE_W SPR_W          /* 16 */
#define TILE_H SPR_H
 
/*  GPIO peripheral IDs  */
#define WE_DEVICE_ID     XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID   XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID    XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID  XPAR_AXI_GPIO_VSYNC_DEVICE_ID
#define GPIO_CH          1     /* all AXI-GPIO blocks use channel-1 */
 
/*  keypad (PMOD KYPD)  */
#define KYPD_GPIO_ID     XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE         "0FED789C456B123A"
 
/*  green timer bar (bottom of screen)  */
#define BAR_W          118                 /* width  in pixels           */
#define BAR_H          8                   /* height in pixels           */
#define BAR_Y0         (FB_H - BAR_H)      /* y = 248  255              */
#define BAR_X1         (FB_W - 32 - 1)     /* right edge 32 px from RHS  */
#define BAR_X0         (BAR_X1 - BAR_W + 1)
#define BAR_COLOR      0x6                 /* palette index 6 (green)    */
#define BAR_FRAMES     (30 * 60)           /* 30 s lifetime @ 60 fps     */
#define FRAMES_PER_COL (BAR_FRAMES / BAR_W)/* 15 frames per column     */
 
/*  peripherals  */
static XGpio gpio_we, gpio_addr, gpio_dat, gpio_vsync;
static PmodKYPD keypad;
 
/*  tight pixel write (back buffer)  */
static inline void draw_pixel(int x, int y, uint8_t c4)
{
    uint16_t idx = y * FB_W + x;
    XGpio_DiscreteWrite(&gpio_addr, GPIO_CH, idx);
    XGpio_DiscreteWrite(&gpio_dat,  GPIO_CH, c4 & 0xF);
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 1);
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 0);     /* ~20 ns pulse */
}
 
/*  background palette index at (x,y)  */
static inline uint8_t bg_pixel(int x, int y)
{
    int tx = x / TILE_W, ty = y / TILE_H;
    int ox = x & 0xF,    oy = y & 0xF;
    uint8_t tid = bg_tilemap[ty][tx];
    return background[tid][oy * TILE_W + ox];
}
 
/*  restore one 16Ã—16 block from background  */
static void restore_block(int x, int y)
{
    for (int dy = 0; dy < TILE_H; dy++)
        for (int dx = 0; dx < TILE_W; dx++)
            draw_pixel(x + dx, y + dy, bg_pixel(x + dx, y + dy));
}
 
/*  draw sprite (transparent index 0)  */
static void draw_sprite(int n, int sx, int sy)
{
    const uint8_t *spr = sprites[n];
    for (int dy = 0; dy < TILE_H; dy++)
        for (int dx = 0; dx < TILE_W; dx++) {
            uint8_t c = spr[dy * TILE_W + dx];
            if (c) draw_pixel(sx + dx, sy + dy, c);
        }
}
 
/*  VSYNC high †’ low edge (end of blank)  */
static void wait_vsync_edge(void)
{
    while (!(XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1)); /* wait high */
    while (  XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1);  /* wait low  */
}
 
/*  green bar helpers  */
static int bar_cols   = BAR_W;   /* columns still green                       */
static int bar_frame  = 0;       /* frames since last shrink                  */
 
static inline void bar_set_column(int x, uint8_t col4)
{
    for (int y = 0; y < BAR_H; y++)
        draw_pixel(x, BAR_Y0 + y, col4);
}
static void bar_init(void)                      /* paint full bar once       */
{
    for (int x = BAR_X0; x <= BAR_X1; x++)
        bar_set_column(x, BAR_COLOR);
}
static void bar_tick(void)                      /* call once per frame       */
{
    if (bar_cols == 0) return;
    if (++bar_frame >= FRAMES_PER_COL) {
        bar_frame = 0;
        int x = BAR_X0 + bar_cols - 1;          /* right-most green column   */
        bar_set_column(x, bg_pixel(x, BAR_Y0)); /* restore column           */
        --bar_cols;
    }
}
 
/*  init GPIO & keypad  */
static void init_io(void)
{
    XGpio_Initialize(&gpio_we,   WE_DEVICE_ID);
    XGpio_Initialize(&gpio_addr, ADDR_DEVICE_ID);
    XGpio_Initialize(&gpio_dat,  DAT_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_we,GPIO_CH,0);
    XGpio_SetDataDirection(&gpio_addr,GPIO_CH,0);
    XGpio_SetDataDirection(&gpio_dat,GPIO_CH,0);
 
    XGpio_Initialize(&gpio_vsync, VSYNC_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_vsync, GPIO_CH, 0xFFFFFFFF);
 
    KYPD_begin(&keypad, KYPD_GPIO_ID);
    KYPD_loadKeyTable(&keypad, (u8*)KEYTABLE);
}
 
/*  main loop  */
int main(void)
{
    init_io();
    bar_init();                                 /* paint timer bar once */
 
    struct Obj { int x,y,px,py,idx,dx; }        /* moving sprites       */
      log0={  0, 48, 0,48, 48,  2},             /* y multiple of 16     */
      log1={128, 80,128,80, 48, -5},
	  log2={128, 112,128,112, 48, -5},
	  car0={128, 208,128,208, 4, -5},
      frog={ (FB_W-16)/2, FB_H-2*TILE_H, 0,0,1,0};
    frog.px=frog.x; frog.py=frog.y;
 
    u16 ks, st; u8 key, last=0;
 
    while (1)
    {
        /*  enter back buffer  */
        wait_vsync_edge();
 
        /* erase previous sprite areas */
        restore_block(log0.px, log0.py);
        restore_block(log1.px, log1.py);
        restore_block(log2.px, log2.py);
        restore_block(car0.px, car0.py);
        restore_block(frog.px, frog.py);
 
        /* move logs */
        log0.px=log0.x; log0.py=log0.y;
        log1.px=log1.x; log1.py=log1.y;
        log2.px=log2.x; log2.py=log2.y;
        car0.px=car0.x; car0.py=car0.y;
 
        log0.x += log0.dx;
        if (log0.x >  FB_W)  log0.x = -TILE_W;
        if (log0.x < -TILE_W)log0.x =  FB_W;
 
        log1.x += log1.dx;
        if (log1.x >  FB_W)  log1.x = -TILE_W;
        if (log1.x < -TILE_W)log1.x =  FB_W;
 
        log2.x += log2.dx;
        if (log2.x >  FB_W)  log2.x = -TILE_W;
        if (log2.x < -TILE_W)log2.x =  FB_W;
 
        car0.x += car0.dx;
        if (car0.x >  FB_W)  car0.x = -TILE_W;
        if (car0.x < -TILE_W)car0.x =  FB_W;
 
 
        /* keypad -> frog move */
        ks = KYPD_getKeyStates(&keypad);
        st = KYPD_getKeyPressed(&keypad, ks, &key);
        if (st == KYPD_SINGLE_KEY && key != last) {
            frog.px=frog.x; frog.py=frog.y;
            switch (key) {
                case '2': frog.y -= TILE_H; break;
                case '5': frog.y += TILE_H; break;
                case '4': frog.x -= TILE_W; break;
                case '6': frog.x += TILE_W; break;
            }
            if (frog.x < 0) frog.x = 0;
            if (frog.x > FB_W - TILE_W) frog.x = FB_W - TILE_W;
            if (frog.y < 0) frog.y = 0;
            if (frog.y > FB_H - TILE_H) frog.y = FB_H - TILE_H;
            last = key;
        } else if (st != KYPD_SINGLE_KEY) last = 0;
 
        /* draw new sprites */
        draw_sprite(log0.idx, log0.x, log0.y);
        draw_sprite(log1.idx, log1.x, log1.y);
        draw_sprite(log2.idx, log2.x, log2.y);
        draw_sprite(car0.idx, car0.x, car0.y);
        draw_sprite(frog.idx, frog.x, frog.y);
 
        if (frog.y == car0.y && (abs(frog.x - car0.x) <= 1)) {
            restore_block(car0.px, car0.py);
        	restore_block(frog.px, frog.py);
            draw_sprite(15, frog.x, frog.y);
        	break;
        }
 
        /* timer bar shrink */
        bar_tick();
 
        /*  wait for next VSYNC so swap happens after drawing  */
        wait_vsync_edge();
    }
    return 0;
}
