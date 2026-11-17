/* Software rasterizer for PS1 GPU primitives, drawing into the emulated 1024x512 VRAM.
 * Follows nocash psx-spx: 11-bit signed vertices + drawing offset, drawing area clip, texture pages with
 * 4/8/15-bit texels and CLUTs, texture window, semi-transparency modes, mask bit handling.
 * Accuracy first; a PSP GE accelerated path can replace the hot paths later. */
#include <string.h>
#include "rt.h"
#include "gpu.h"
#include "raster.h"

RasterState g_rs = { .clip_x1 = 1023, .clip_y1 = 511 };

static inline int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

static inline uint16_t blend(uint16_t bg, uint16_t fg, int mode)
{
    int br = bg & 31, bgg = (bg >> 5) & 31, bb = (bg >> 10) & 31;
    int fr = fg & 31, fgg = (fg >> 5) & 31, fb = (fg >> 10) & 31;
    int r, g, b;
    switch (mode) {
    case 0: r = (br + fr) >> 1; g = (bgg + fgg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bgg + fgg; b = bb + fb; break;
    case 2: r = br - fr; g = bgg - fgg; b = bb - fb; break;
    default: r = br + (fr >> 2); g = bgg + (fgg >> 2); b = bb + (fb >> 2); break;
    }
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 31) g = 31;
    if (b < 0) b = 0; else if (b > 31) b = 31;
    return (uint16_t)(r | (g << 5) | (b << 10) | (fg & 0x8000));
}

static inline void plot(int x, int y, uint16_t c, int semi, int semi_mode)
{
    uint16_t *p = &g_vram[y * 1024 + x];
    if (g_rs.mask_check && (*p & 0x8000)) return;
    if (semi) c = blend(*p, c, semi_mode);
    *p = c | g_rs.mask_set;
}

/* Texel fetch: returns 0 for a fully transparent texel. */
static inline uint16_t texel(int u, int v, int tpx, int tpy, int depth, int clx, int cly)
{
    u &= 255; v &= 255;
    u = (u & ~(g_rs.tw_mask_x * 8)) | ((g_rs.tw_off_x & g_rs.tw_mask_x) * 8);
    v = (v & ~(g_rs.tw_mask_y * 8)) | ((g_rs.tw_off_y & g_rs.tw_mask_y) * 8);
    int y = (tpy + v) & 511;
    switch (depth) {
    case 0: {
        uint16_t w = g_vram[y * 1024 + ((tpx + (u >> 2)) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 15;
        return g_vram[cly * 1024 + ((clx + idx) & 1023)];
    }
    case 1: {
        uint16_t w = g_vram[y * 1024 + ((tpx + (u >> 1)) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 255;
        return g_vram[cly * 1024 + ((clx + idx) & 1023)];
    }
    default:
        return g_vram[y * 1024 + ((tpx + u) & 1023)];
    }
}

static inline uint16_t modulate(uint16_t t, int r, int g, int b)
{
    int tr = ((t & 31) * r) >> 7, tg = (((t >> 5) & 31) * g) >> 7, tb = (((t >> 10) & 31) * b) >> 7;
    if (tr > 31) tr = 31;
    if (tg > 31) tg = 31;
    if (tb > 31) tb = 31;
    return (uint16_t)(tr | (tg << 5) | (tb << 10) | (t & 0x8000));
}

static inline uint16_t rgb15(int r, int g, int b)
{
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* ---- triangles -------------------------------------------------------------------------------------- */

typedef struct { int x, y, r, g, b, u, v; } Vtx;

static void triangle(const Vtx *v0, const Vtx *v1, const Vtx *v2, const PrimInfo *pi)
{
    int minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    if (v1->x < minx) minx = v1->x; if (v1->x > maxx) maxx = v1->x;
    if (v2->x < minx) minx = v2->x; if (v2->x > maxx) maxx = v2->x;
    if (v1->y < miny) miny = v1->y; if (v1->y > maxy) maxy = v1->y;
    if (v2->y < miny) miny = v2->y; if (v2->y > maxy) maxy = v2->y;
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;          /* hardware drops these */
    if (minx < g_rs.clip_x0) minx = g_rs.clip_x0;
    if (miny < g_rs.clip_y0) miny = g_rs.clip_y0;
    if (maxx > g_rs.clip_x1) maxx = g_rs.clip_x1;
    if (maxy > g_rs.clip_y1) maxy = g_rs.clip_y1;
    if (minx > maxx || miny > maxy) return;

    int64_t area = (int64_t)(v1->x - v0->x) * (v2->y - v0->y) - (int64_t)(v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return;
    const Vtx *a = v0, *b = v1, *c = v2;
    if (area < 0) { b = v2; c = v1; area = -area; }

    /* edge functions E(x,y) = (y0-y1)*x + (x1-x0)*y + (x0*y1 - x1*y0), top-left fill rule via bias */
    int A01 = a->y - b->y, B01 = b->x - a->x;
    int A12 = b->y - c->y, B12 = c->x - b->x;
    int A20 = c->y - a->y, B20 = a->x - c->x;
    int bias0 = (A12 > 0 || (A12 == 0 && B12 < 0)) ? 0 : -1;    /* edge b-c, weight of a */
    int bias1 = (A20 > 0 || (A20 == 0 && B20 < 0)) ? 0 : -1;    /* edge c-a, weight of b */
    int bias2 = (A01 > 0 || (A01 == 0 && B01 < 0)) ? 0 : -1;    /* edge a-b, weight of c */

    int64_t w0_row = (int64_t)A12 * (minx - b->x) + (int64_t)B12 * (miny - b->y);
    int64_t w1_row = (int64_t)A20 * (minx - c->x) + (int64_t)B20 * (miny - c->y);
    int64_t w2_row = (int64_t)A01 * (minx - a->x) + (int64_t)B01 * (miny - a->y);

    /* attribute gradients in 16.16 fixed point */
    const int shade = pi->gouraud, tex = pi->textured;
    int64_t inv = ((int64_t)1 << 32) / area;
#define GRAD(f) \
    int64_t d##f##dx = (((int64_t)(b->f - a->f) * (c->y - a->y) - (int64_t)(c->f - a->f) * (b->y - a->y)) * inv) >> 16; \
    int64_t d##f##dy = (((int64_t)(c->f - a->f) * (b->x - a->x) - (int64_t)(b->f - a->f) * (c->x - a->x)) * inv) >> 16;
    GRAD(r) GRAD(g) GRAD(b) GRAD(u) GRAD(v)
#undef GRAD
    int tpx = pi->tpx, tpy = pi->tpy, depth = pi->depth, clx = pi->clx, cly = pi->cly;
    int fr = a->r, fg = a->g, fb = a->b;
    uint16_t flat = rgb15(fr, fg, fb);
    /* per-pixel steps in 16.16 (fit in 32 bits: |d/dx| <= 255 << 16) */
    int32_t sr = (int32_t)drdx, sg = (int32_t)dgdx, sb = (int32_t)dbdx, su = (int32_t)dudx, sv = (int32_t)dvdx;

    int32_t W0 = (int32_t)w0_row, W1 = (int32_t)w1_row, W2 = (int32_t)w2_row;
    for (int y = miny; y <= maxy; y++, W0 += B12, W1 += B20, W2 += B01) {
        int32_t w0 = W0 + bias0, w1 = W1 + bias1, w2 = W2 + bias2;
        /* skip to the first covered pixel of this row */
        int x = minx;
        while (x <= maxx && (w0 | w1 | w2) < 0) { w0 += A12; w1 += A20; w2 += A01; x++; }
        if (x > maxx) continue;
        int64_t ox = x - a->x, oy = y - a->y;
        int32_t r = (a->r << 16) + (int32_t)(drdx * ox + drdy * oy) + 0x8000;
        int32_t g = (a->g << 16) + (int32_t)(dgdx * ox + dgdy * oy) + 0x8000;
        int32_t bl = (a->b << 16) + (int32_t)(dbdx * ox + dbdy * oy) + 0x8000;
        int32_t u = (a->u << 16) + (int32_t)(dudx * ox + dudy * oy) + 0x8000;
        int32_t v = (a->v << 16) + (int32_t)(dvdx * ox + dvdy * oy) + 0x8000;
        for (; x <= maxx && (w0 | w1 | w2) >= 0; x++) {
            int cr = shade ? r >> 16 : fr, cg = shade ? g >> 16 : fg, cb = shade ? bl >> 16 : fb;
            if (tex) {
                uint16_t t = texel(u >> 16, v >> 16, tpx, tpy, depth, clx, cly);
                if (t) {
                    if (!pi->raw) t = modulate(t, cr, cg, cb);
                    plot(x, y, t, pi->semi && (t & 0x8000), pi->semi_mode);
                }
            } else {
                uint16_t col = shade ? rgb15(cr < 0 ? 0 : cr > 255 ? 255 : cr, cg < 0 ? 0 : cg > 255 ? 255 : cg,
                                             cb < 0 ? 0 : cb > 255 ? 255 : cb) : flat;
                plot(x, y, col, pi->semi, pi->semi_mode);
            }
            w0 += A12; w1 += A20; w2 += A01;
            r += sr; g += sg; bl += sb; u += su; v += sv;
        }
    }
}

static void setup_tex(PrimInfo *pi, uint32_t clut_word, uint32_t page_word)
{
    uint32_t page = page_word >> 16;
    uint32_t clut = clut_word >> 16;
    pi->tpx = (page & 15) * 64;
    pi->tpy = ((page >> 4) & 1) * 256;
    pi->semi_mode = (page >> 5) & 3;
    pi->depth = (page >> 7) & 3;
    pi->clx = (clut & 0x3F) * 16;
    pi->cly = (clut >> 6) & 0x1FF;
    /* textured polygons also update the global texpage (GPUSTAT bits 0-8, 11) */
    g_rs.texpage = (g_rs.texpage & ~0x9FFu) | (page & 0x9FF);
}

void raster_polygon(const uint32_t *w)
{
    uint32_t op = w[0] >> 24;
    int quad = op & 8, tex = op & 4, gour = op & 0x10;
    PrimInfo pi = {0};
    pi.textured = tex != 0;
    pi.gouraud = gour != 0;
    pi.semi = (op & 2) != 0;
    pi.raw = (op & 1) != 0;
    pi.semi_mode = (g_rs.texpage >> 5) & 3;
    Vtx v[4];
    int n = quad ? 4 : 3, k = 0;
    uint32_t col = w[k++] & 0xFFFFFF;
    for (int i = 0; i < n; i++) {
        if (i > 0 && gour) col = w[k++] & 0xFFFFFF;
        uint32_t xy = w[k++];
        v[i].x = sx11(xy) + g_rs.off_x;
        v[i].y = sx11(xy >> 16) + g_rs.off_y;
        v[i].r = col & 0xFF; v[i].g = (col >> 8) & 0xFF; v[i].b = (col >> 16) & 0xFF;
        v[i].u = v[i].v = 0;
        if (tex) {
            uint32_t uv = w[k++];
            v[i].u = uv & 0xFF;
            v[i].v = (uv >> 8) & 0xFF;
            if (i == 0) pi.clut_word = uv;
            if (i == 1) setup_tex(&pi, pi.clut_word, uv);
        }
    }
    if (!gour) for (int i = 1; i < n; i++) { v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b; }
    triangle(&v[0], &v[1], &v[2], &pi);
    if (quad) triangle(&v[1], &v[3], &v[2], &pi);
}

void raster_rect(const uint32_t *w)
{
    uint32_t op = w[0] >> 24;
    int tex = op & 4, k = 1;
    int sz = (op >> 3) & 3;
    PrimInfo pi = {0};
    pi.textured = tex != 0;
    pi.semi = (op & 2) != 0;
    pi.raw = (op & 1) != 0;
    uint32_t col = w[0] & 0xFFFFFF;
    uint32_t xy = w[k++];
    int x0 = sx11(xy) + g_rs.off_x, y0 = sx11(xy >> 16) + g_rs.off_y;
    int u0 = 0, v0 = 0;
    pi.tpx = (g_rs.texpage & 15) * 64;
    pi.tpy = ((g_rs.texpage >> 4) & 1) * 256;
    pi.semi_mode = (g_rs.texpage >> 5) & 3;
    pi.depth = (g_rs.texpage >> 7) & 3;
    if (tex) {
        uint32_t uv = w[k++];
        u0 = uv & 0xFF; v0 = (uv >> 8) & 0xFF;
        pi.clx = ((uv >> 16) & 0x3F) * 16;
        pi.cly = (uv >> 22) & 0x1FF;
    }
    int ww, hh;
    switch (sz) {
    case 0: ww = w[k] & 0x3FF; hh = (w[k] >> 16) & 0x1FF; break;
    case 1: ww = hh = 1; break;
    case 2: ww = hh = 8; break;
    default: ww = hh = 16; break;
    }
    int r = col & 0xFF, g = (col >> 8) & 0xFF, b = (col >> 16) & 0xFF;
    uint16_t flat = rgb15(r, g, b);
    for (int y = 0; y < hh; y++) {
        int py = y0 + y;
        if (py < g_rs.clip_y0 || py > g_rs.clip_y1) continue;
        for (int x = 0; x < ww; x++) {
            int px = x0 + x;
            if (px < g_rs.clip_x0 || px > g_rs.clip_x1) continue;
            if (tex) {
                uint16_t t = texel(u0 + x, v0 + y, pi.tpx, pi.tpy, pi.depth, pi.clx, pi.cly);
                if (!t) continue;
                if (!pi.raw) t = modulate(t, r, g, b);
                plot(px, py, t, pi.semi && (t & 0x8000), pi.semi_mode);
            } else {
                plot(px, py, flat, pi.semi, pi.semi_mode);
            }
        }
    }
}

void raster_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int semi)
{
    x0 = sx11(x0) + g_rs.off_x; y0 = sx11(y0) + g_rs.off_y;
    x1 = sx11(x1) + g_rs.off_x; y1 = sx11(y1) + g_rs.off_y;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= 1024 || ady >= 512) return;
    int steps = adx > ady ? adx : ady;
    int mode = (g_rs.texpage >> 5) & 3;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (steps ? dx * i / steps : 0), y = y0 + (steps ? dy * i / steps : 0);
        if (x < g_rs.clip_x0 || x > g_rs.clip_x1 || y < g_rs.clip_y0 || y > g_rs.clip_y1) continue;
        int t = steps ? i * 256 / steps : 0;
        int r = ((c0 & 0xFF) * (256 - t) + (c1 & 0xFF) * t) >> 8;
        int g = (((c0 >> 8) & 0xFF) * (256 - t) + ((c1 >> 8) & 0xFF) * t) >> 8;
        int b = (((c0 >> 16) & 0xFF) * (256 - t) + ((c1 >> 16) & 0xFF) * t) >> 8;
        plot(x, y, rgb15(r, g, b), semi, mode);
    }
}
