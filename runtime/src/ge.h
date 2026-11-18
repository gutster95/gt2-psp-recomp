/* PS1 GPU primitives rendered by the PSP GE. See ge.c and docs/ARCHITECTURE.md ("GE renderer"). */
#pragma once
#include <stdint.h>

extern int g_use_ge;                 /* 1: GE renderer (default), 0: software rasterizer only */
extern int g_hires;                  /* displayed frames rendered at 480x272 into the PSP display buffers */
extern int g_widescreen;             /* 16:9: GTE projection X * 3/4, frames use the full 480 width */

void ge_init(void);
void ge_sync(void);                  /* finish all queued GE work; call before the CPU touches VRAM */
void ge_polygon(const uint32_t *w);
void ge_rect(const uint32_t *w);
void ge_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int semi);
int ge_fill(int x, int y, int w, int h, uint16_t col);           /* 0: not handled, caller uses the CPU */
int ge_copy(int sx, int sy, int dx, int dy, int w, int h);       /* 0: not handled */
void ge_vram_written(int x, int y, int w, int h);                /* CPU wrote VRAM: invalidate texture cache */
void ge_note_read(int x, int y, int w, int h);                   /* game reads VRAM (texture/copy/readback) */
void ge_upload_done(int x, int y, int w, int h);                 /* CPU upload finished: mirror to hi-res */
void ge_present(int x, int y, int w, int h, int blank);          /* show the PS1 display area, flip */
void ge_mark_3d(uint32_t xy);
void ge_mark_3d_precise(uint32_t xy, float x, float y, float z);   /* + sub-pixel position, view depth */
extern int g_pgxp, g_tex_filter;                                   /* GTE-projected vertex (3D, not HUD) */
const uint16_t *ge_display_buffer(void);                        /* debug: last shown 512x272 buffer */
