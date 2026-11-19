#pragma once
#include <stdint.h>

typedef struct {
    int clip_x0, clip_y0, clip_x1, clip_y1;   /* drawing area (inclusive) */
    int off_x, off_y;                         /* drawing offset */
    uint32_t texpage;                         /* GP0(E1) bits */
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    uint16_t mask_set;                        /* 0x8000 if GP0(E6) bit 0 */
    int mask_check;                           /* GP0(E6) bit 1 */
} RasterState;

typedef struct {
    int textured, gouraud, semi, raw, semi_mode;
    int tpx, tpy, depth, clx, cly;
    uint32_t clut_word;
} PrimInfo;

/* The drawing state lives in the PSP's scratchpad (on-chip, 0x10000-0x13FFF): it is read for every primitive, and
 * in the data cache the streams of display list, vertex and packet data kept evicting it. */
#define g_rs (*(RasterState *)0x00010100u)
void raster_state_init(void);

void raster_polygon(const uint32_t *w);
void raster_rect(const uint32_t *w);
void raster_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int semi);
