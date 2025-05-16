// ──────────────────────────────────────────────────────────────────────────────
//  Frogger - VSYNC-locked, bounding-box incremental renderer
// ──────────────────────────────────────────────────────────────────────────────
#include "PmodKYPD.h"
#include "sleep.h" // usleep(usec)
#include "xgpio.h"
#include "xil_types.h"
#include "xparameters.h"
#include <stdint.h>

#include "background.h" // background[][SPR_W*SPR_H] + bg_tilemap[][]
#include "sprites.h"    // SPR_W, SPR_H, sprites[][]
#undef NUM_SPRITES      // avoid redefine warning

/* Framebuffer */
#define FB_W 224
#define FB_H 256
#define TILE_W SPR_W // 16
#define TILE_H SPR_H

/* AXI-GPIO IDs */
#define WE_DEVICE_ID XPAR_AXI_GPIO_WE_DEVICE_ID
#define ADDR_DEVICE_ID XPAR_AXI_GPIO_ADDR_DEVICE_ID
#define DAT_DEVICE_ID XPAR_AXI_GPIO_DAT_DEVICE_ID
#define VSYNC_DEVICE_ID XPAR_AXI_GPIO_VSYNC_DEVICE_ID
#define GPIO_CH 1

/* Keypad */
#define KYPD_GPIO_ID XPAR_PMODKYPD_0_AXI_LITE_GPIO_BASEADDR
#define KEYTABLE "0FED789C456B123A"

/* ── Peripherals ─────────────────────────────────────────────────────────── */
static XGpio gpio_we, gpio_addr, gpio_dat, gpio_vsync;
static PmodKYPD keypad;

/* ── Low-level pixel write (back buffer) ─────────────────────────────────── */
static inline void draw_pixel(int x, int y, uint8_t c4) {
    uint16_t idx = y * FB_W + x;
    XGpio_DiscreteWrite(&gpio_addr, GPIO_CH, idx);
    XGpio_DiscreteWrite(&gpio_dat, GPIO_CH, c4 & 0xF);
    XGpio_DiscreteWrite(&gpio_we, GPIO_CH, 1);
    // ~300 ns pulse on Basys-3 @ 100 MHz – safe for BRAM
    XGpio_DiscreteWrite(&gpio_we, GPIO_CH, 0);
}

/* ── Read palette index from background tile-map ─────────────────────────── */
static inline uint8_t bg_pixel(int x, int y) {
    int tx = x / TILE_W, ty = y / TILE_H;
    int ox = x % TILE_W, oy = y % TILE_H;
    uint8_t tid = bg_tilemap[ty][tx];
    return background[tid][oy * TILE_W + ox];
}

/* ── Read palette index from a sprite (0 = transparent / “no pixel”) ─────── */
static inline uint8_t spr_pixel(int n, int sx, int sy, int x, int y) {
    int dx = x - sx, dy = y - sy;
    if ((uint32_t)dx >= TILE_W || (uint32_t)dy >= TILE_H)
        return 0;
    return sprites[n][dy * TILE_W + dx];
}

/* ── Block until VGA_Vsync pin goes high->low (end of v-blank) ───────────── */
static void wait_vsync_edge(void) {
    while (!(XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1))
        ; // wait high
    while ((XGpio_DiscreteRead(&gpio_vsync, GPIO_CH) & 1))
        ; // wait low
}

/* ── Init helpers ─────────────────────────────────────────────────────────── */
static void init_gpios(void) {
    XGpio_Initialize(&gpio_we, WE_DEVICE_ID);
    XGpio_Initialize(&gpio_addr, ADDR_DEVICE_ID);
    XGpio_Initialize(&gpio_dat, DAT_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_we, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_addr, GPIO_CH, 0);
    XGpio_SetDataDirection(&gpio_dat, GPIO_CH, 0);

    XGpio_Initialize(&gpio_vsync, VSYNC_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_vsync, GPIO_CH, 0xFFFFFFFF);
}
static void init_keypad(void) {
    KYPD_begin(&keypad, KYPD_GPIO_ID);
    KYPD_loadKeyTable(&keypad, (u8 *)KEYTABLE);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(void) {
    init_gpios();
    init_keypad();

    /* Moving objects */
    struct Obj {
        int x, y, px, py, idx, dx;
    } log0 = {0, 48, 0, 48, 48, 2},
      log1 = {128, 80, 128, 80, 48, -3},
      frog = {(FB_W - TILE_W) / 2,
              (FB_H - TILE_H) / 2,
              0, 0, 1, 0};
    frog.px = frog.x;
    frog.py = frog.y;

    u16 keystate, status;
    u8 key, last_key = 0;

    while (1) {
        /* ────────────────── ENTER BACK BUFFER ────────────────── */
        wait_vsync_edge();

        /* ── 1. move objects (store old → px/py first) ───────── */
        log0.px = log0.x;
        log0.py = log0.y;
        log1.px = log1.x;
        log1.py = log1.y;
        frog.px = frog.x;
        frog.py = frog.y;

        /* logs wrap */
        log0.x += log0.dx;
        if (log0.x > FB_W)
            log0.x = -TILE_W;
        if (log0.x < -TILE_W)
            log0.x = FB_W;
        log1.x += log1.dx;
        if (log1.x > FB_W)
            log1.x = -TILE_W;
        if (log1.x < -TILE_W)
            log1.x = FB_W;

        /* keypad → frog */
        keystate = KYPD_getKeyStates(&keypad);
        status = KYPD_getKeyPressed(&keypad, keystate, &key);
        if (status == KYPD_SINGLE_KEY && key != last_key) {
            switch (key) {
            case '2':
                frog.y -= TILE_H;
                break;
            case '5':
                frog.y += TILE_H;
                break;
            case '4':
                frog.x -= TILE_W;
                break;
            case '6':
                frog.x += TILE_W;
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
            last_key = key;
        } else if (status != KYPD_SINGLE_KEY)
            last_key = 0;

        /* ── 2. build bounding rectangle of all changed pixels ── */
        int minx = FB_W, miny = FB_H, maxx = 0, maxy = 0;
#define EXTENT(o)                                    \
    {                                                \
        int x0 = (o.x < o.px ? o.x : o.px);          \
        int y0 = (o.y < o.py ? o.y : o.py);          \
        int x1 = (o.x > o.px ? o.x : o.px) + TILE_W; \
        int y1 = (o.y > o.py ? o.y : o.py) + TILE_H; \
        if (x0 < minx)                               \
            minx = x0;                               \
        if (y0 < miny)                               \
            miny = y0;                               \
        if (x1 > maxx)                               \
            maxx = x1;                               \
        if (y1 > maxy)                               \
            maxy = y1;                               \
    }
        EXTENT(log0);
        EXTENT(log1);
        EXTENT(frog);
#undef EXTENT
        if (minx < 0)
            minx = 0;
        if (miny < 0)
            miny = 0;
        if (maxx > FB_W)
            maxx = FB_W;
        if (maxy > FB_H)
            maxy = FB_H;

        /* ── 3. repaint that rectangle (BG → logs → frog) ─────── */
        for (int y = miny; y < maxy; y++)
            for (int x = minx; x < maxx; x++) {
                uint8_t c = bg_pixel(x, y);
                uint8_t p;
                p = spr_pixel(log0.idx, log0.x, log0.y, x, y);
                if (p)
                    c = p;
                p = spr_pixel(log1.idx, log1.x, log1.y, x, y);
                if (p)
                    c = p;
                p = spr_pixel(frog.idx, frog.x, frog.y, x, y);
                if (p)
                    c = p;
                draw_pixel(x, y, c);
            }

        /* ────────────────── FINISHED FRAME – wait again so swap happens AFTER ─── */
        wait_vsync_edge();
    }
    return 0;
}
