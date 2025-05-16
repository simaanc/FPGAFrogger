// ──────────────────────────────────────────────────────────────────────────────
//  Frogger – VSYNC-locked incremental renderer
//  Hardware: Basys-3, MicroBlaze, two-buffer framebuffer (Project F bram_sdp)
// ──────────────────────────────────────────────────────────────────────────────
#include "xparameters.h"
#include "xgpio.h"
#include "sleep.h"            // usleep()
#include "xil_types.h"
#include "PmodKYPD.h"         // keypad API
#include <stdint.h>

#include "sprites.h"          // frog & log sprites (SPR_W, SPR_H, sprites[][])
#include "background.h"       // background tiles + tile-map
#undef  NUM_SPRITES           // silence duplicate macro warning

// ───────────────────────── GPIO peripheral IDs ───────────────────────────────
#define WE_DEVICE_ID    XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID  XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID   XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID XPAR_AXI_GPIO_VSYNC_DEVICE_ID   // VGA_Vsync → AXI-GPIO

#define GPIO_CH         1      // all AXI-GPIO blocks use channel 1

// ───────────────────────── keypad (PMOD KYPD) ────────────────────────────────
#define KYPD_GPIO_ID    XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE        "0FED789C456B123A"

// ───────────────────────── framebuffer geometry ──────────────────────────────
#define FB_W  224
#define FB_H  256

// shorthand
#define TILE_W SPR_W   // 16
#define TILE_H SPR_H   // 16

// ───────────────────────── global peripherals ────────────────────────────────
static XGpio gpio_we, gpio_addr, gpio_dat, gpio_vsync;
static PmodKYPD keypad;

// ───────────────────────── pixel helpers ──────────────────────────────────────
static inline void draw_pixel(int x, int y, uint8_t col4)
{
    uint16_t idx = y * FB_W + x;
    XGpio_DiscreteWrite(&gpio_addr, GPIO_CH, idx);
    XGpio_DiscreteWrite(&gpio_dat,  GPIO_CH, col4 & 0xF);
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 1);
    usleep(1);                                // tiny pulse > 1 µs
    XGpio_DiscreteWrite(&gpio_we,   GPIO_CH, 0);
}

static inline uint8_t bg_pixel(int x, int y)
{
    int tx = x / TILE_W, ty = y / TILE_H;
    int ox = x % TILE_W, oy = y % TILE_H;
    uint8_t tid = bg_tilemap[ty][tx];
    return background[tid][oy * TILE_W + ox];
}

static void restore_bg_region(int x, int y)          // restore one 16×16 block
{
    for (int dy = 0; dy < TILE_H; dy++)
        for (int dx = 0; dx < TILE_W; dx++)
            draw_pixel(x + dx, y + dy, bg_pixel(x + dx, y + dy));
}

static void draw_sprite(int n, int sx, int sy)       // transparent index 0
{
    const uint8_t *spr = sprites[n];
    for (int dy = 0; dy < TILE_H; dy++)
        for (int dx = 0; dx < TILE_W; dx++) {
            uint8_t c = spr[dy * TILE_W + dx];
            if (c) draw_pixel(sx + dx, sy + dy, c);
        }
}

// ───────────────────────── VSYNC helper ──────────────────────────────────────
static void wait_vsync_edge(void)                    // high → low
{
    while (!(XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1));  // wait high
    while (  XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1);   // wait low
}

// ───────────────────────── init helpers ──────────────────────────────────────
static void init_gpios(void)
{
    XGpio_Initialize(&gpio_we,   WE_DEVICE_ID);
    XGpio_Initialize(&gpio_addr, ADDR_DEVICE_ID);
    XGpio_Initialize(&gpio_dat,  DAT_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_we,   GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_addr, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_dat,  GPIO_CH, 0);

    XGpio_Initialize(&gpio_vsync, VSYNC_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_vsync, GPIO_CH, 0xFFFFFFFF);
}

static void init_keypad(void)
{
    KYPD_begin(&keypad, KYPD_GPIO_ID);
    KYPD_loadKeyTable(&keypad, (u8*)KEYTABLE);
}

// ───────────────────────── main ──────────────────────────────────────────────
int main(void)
{
    init_gpios();
    init_keypad();

    // object state
    struct Obj { int x, y, px, py, idx, dx; } logs[2], frog;

    // logs
    logs[0] = (struct Obj){  0, 48, 0, 48, 48,  2 };
    logs[1] = (struct Obj){128, 80,128,80, 48,-3 };

    // frog
    frog.x = (FB_W - TILE_W) / 2;
    frog.y = (FB_H - TILE_H) / 2;
    frog.px = frog.x; frog.py = frog.y;
    frog.idx = 1;  frog.dx = 0;

    u16 keystate, status; u8 key, last_key = 0;

    while (1)
    {
        /* ───────── start of frame: ensure we’re in the back buffer ───────── */
        wait_vsync_edge();

        /* 1 · erase old sprites */
        restore_bg_region(frog.px, frog.py);
        restore_bg_region(logs[0].px, logs[0].py);
        restore_bg_region(logs[1].px, logs[1].py);

        /* 2 · move logs */
        for (int i = 0; i < 2; i++) {
            logs[i].x += logs[i].dx;
            if (logs[i].x >  FB_W) logs[i].x = -TILE_W;
            if (logs[i].x < -TILE_W) logs[i].x =  FB_W;
        }

        /* 3 · keypad → frog */
        keystate = KYPD_getKeyStates(&keypad);
        status   = KYPD_getKeyPressed(&keypad, keystate, &key);
        if (status == KYPD_SINGLE_KEY && key != last_key) {
            switch (key) {
                case '2': frog.y -= TILE_H; break;
                case '5': frog.y += TILE_H; break;
                case '4': frog.x -= TILE_W; break;
                case '6': frog.x += TILE_W; break;
            }
            if (frog.x < 0)                 frog.x = 0;
            if (frog.x > FB_W - TILE_W)     frog.x = FB_W - TILE_W;
            if (frog.y < 0)                 frog.y = 0;
            if (frog.y > FB_H - TILE_H)     frog.y = FB_H - TILE_H;
            last_key = key;
        } else if (status != KYPD_SINGLE_KEY) {
            last_key = 0;
        }

        /* 4 · draw logs then frog */
        draw_sprite(logs[0].idx, logs[0].x, logs[0].y);
        draw_sprite(logs[1].idx, logs[1].x, logs[1].y);
        draw_sprite(frog.idx,    frog.x,    frog.y);

        /* 5 · store current positions for next frame */
        frog.px = frog.x; frog.py = frog.y;
        logs[0].px = logs[0].x; logs[0].py = logs[0].y;
        logs[1].px = logs[1].x; logs[1].py = logs[1].y;

        /* ───────── end of frame: wait for next VSYNC so swap occurs AFTER we’re done ───── */
        wait_vsync_edge();
    }
    return 0;
}
