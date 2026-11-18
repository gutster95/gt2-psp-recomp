/* PS1 GPU primitives rendered by the PSP GE. See ge.c and docs/ARCHITECTURE.md ("GE renderer"). */
#pragma once
#include <stdint.h>

extern int g_use_ge;                 /* 1: GE renderer (default), 0: software rasterizer only */

void ge_init(void);
void ge_sync(void);                  /* finish all queued GE work; call before the CPU touches VRAM */
void ge_polygon(const uint32_t *w);
void ge_rect(const uint32_t *w);
void ge_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int semi);
int ge_fill(int x, int y, int w, int h, uint16_t col);           /* 0: not handled, caller uses the CPU */
int ge_copy(int sx, int sy, int dx, int dy, int w, int h);       /* 0: not handled */
void ge_vram_written(int x, int y, int w, int h);                /* CPU wrote VRAM: invalidate texture cache */
void ge_present(int x, int y, int w, int h, int blank);          /* show the PS1 display area, flip */
const uint16_t *ge_display_buffer(void);                        /* debug: last shown 512x272 buffer */
