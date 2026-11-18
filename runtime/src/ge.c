/* PS1 GPU primitives on the PSP GE.
 *
 * eDRAM layout (2 MB at 0x04000000):
 *   0x000000  PS1 VRAM, 1024x512 x 16 bit. The PS1 pixel format (R5 G5 B5, bit 15 = mask bit) is bit-identical to
 *             the GE's 5551 format, so the GE renders straight into the PS1 VRAM image, and the CPU (uploads,
 *             readback, software fallback) works on the same memory through the uncached mirror 0x44000000.
 *   0x100000  display buffers 0 and 1 (512x272, 5551) at 0x100000 / 0x144000.
 *
 * Primitives are drawn in through mode: vertex coordinates are PS1 VRAM coordinates (offset 2048 lets them go
 * negative), the drawing area is the scissor, the PS1 mask bit is the GE stencil bit (5551 alpha = stencil).
 *
 * Textures: a PS1 texture page cannot be sampled in place (a 4/8-bit page has a row pitch of 4096/2048 texels, the
 * GE allows at most 2047), so pages are copied by GE block transfers into a cache of T4/T8 textures in RAM. The
 * copy is part of the command stream, which keeps render-to-texture ordered without CPU syncs. A per-page version
 * counter (16x2 pages of 64x256) is bumped on every write by GE or CPU and tells when a cached page is stale.
 * CLUTs are converted by the CPU to 8888 with an alpha code: 0x00 = transparent (PS1 colour 0x0000), 0xFF = opaque,
 * 0x70 = semi-transparency bit set. Semi-transparent textured primitives are drawn twice (alpha test on the code):
 * opaque texels without blending, STP texels with the PS1 blend mode.
 * Texture modulation uses the GE colour doubling bit: GE result = tex * col * 2 / 255 ~ PS1 tex * col / 128.
 *
 * Not on the GE yet (software rasterizer after a sync): 15-bit direct textures, texture windows. */
#include <string.h>
#include <malloc.h>
#include <pspgu.h>
#include <pspge.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include "rt.h"
#include "gpu.h"
#include "raster.h"
#include "ge.h"

int g_use_ge = 1;

#define EDRAM 0x04000000u
#define FB_OFF(i) (0x100000u + (unsigned)(i) * 0x44000u)
#define VFMT (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
#define LIST_BYTES (1024 * 1024)

typedef struct { uint16_t u, v; uint32_t c; int16_t x, y, z, pad; } GeVtx;

static uint32_t g_list[LIST_BYTES / 4] __attribute__((aligned(64)));
static int g_open, g_fb;
static int g_scissor_is_width = 1;    /* sceGuScissor(x, y, w, h) semantics, checked at startup */
uint32_t g_ge_prims, g_ge_sw_prims, g_ge_tex_uploads, g_ge_syncs;

static inline int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

/* ---- VRAM page versions ------------------------------------------------------------------------------------ */

static uint32_t g_page_ver[32];
static uint32_t g_ge_pending;           /* pages the GE has drawn into since the last sync */

static void touch(int x0, int y0, int x1, int y1, int by_ge)   /* inclusive */
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 1023) x1 = 1023;
    if (y1 > 511) y1 = 511;
    if (x0 > x1 || y0 > y1) return;
    for (int py = y0 >> 8; py <= y1 >> 8; py++)
        for (int px = x0 >> 6; px <= x1 >> 6; px++) {
            g_page_ver[py * 16 + px]++;
            if (by_ge) g_ge_pending |= 1u << (py * 16 + px);
        }
}

void ge_vram_written(int x, int y, int w, int h)
{
    if (x + w > 1024 || y + h > 512) { touch(0, 0, 1023, 511, 0); return; }   /* wrapped transfer */
    touch(x, y, x + w - 1, y + h - 1, 0);
}

/* ---- GE state cache ---------------------------------------------------------------------------------------- */

static struct {
    int tex, tfx, blend, atest, stencil, dither, tex_psm;
    int sc[4];
    uintptr_t tex_ptr;
    const void *clut;
} S;

static void invalidate_state(void) { memset(&S, 0xFF, sizeof S); }

/* ---- CLUT conversion (per batch arena; the GE copies a CLUT on load, entries stay valid until the sync) ----- */

#define CLUT_ARENA (128 * 1024)
static uint32_t g_clut_arena[CLUT_ARENA / 4] __attribute__((aligned(64)));
static uint32_t g_clut_top;
typedef struct { uint32_t key, ver; const uint32_t *p; } ClutEnt;
#define NCLUT 64
static ClutEnt g_cluts[NCLUT];
static int g_nclut, g_clut_rr;

static void ge_begin(void);

void ge_sync(void)
{
    if (!g_open) return;
    sceGuFinish();
    sceGuSync(0, 0);
    g_open = 0;
    g_ge_pending = 0;
    g_clut_top = 0;
    g_nclut = 0;
    g_ge_syncs++;
}

static void ge_begin(void)
{
    if (g_open) return;
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)0, 1024);
    sceGuSendCommandi(0x15, 0);                          /* drawing region: the whole PS1 VRAM */
    sceGuSendCommandi(0x16, (511 << 10) | 1023);
    sceGuOffset(2048, 2048);
    sceGuViewport(2048, 2048, 1024, 512);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_COLOR_LOGIC_OP);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    invalidate_state();
    g_open = 1;
}

static void check_list(void)
{
    if (sceGuCheckList() > LIST_BYTES - 64 * 1024) ge_sync();
}

static inline uint32_t c5to8(uint32_t v) { return (v << 3) | (v >> 2); }

static const uint32_t *clut_get(int clx, int cly, int n)
{
    int p = (cly >> 8) * 16 + (clx >> 6), p2 = (cly >> 8) * 16 + (((clx + n - 1) & 1023) >> 6);
    if (g_ge_pending & ((1u << p) | (1u << p2))) { ge_sync(); ge_begin(); }   /* palette drawn by the GE */
    uint32_t key = (n == 256 ? 0x80000000u : 0) | ((uint32_t)cly << 10) | (uint32_t)clx;
    uint32_t ver = g_page_ver[p] ^ (g_page_ver[p2] * 0x9E3779B1u);
    for (int i = 0; i < g_nclut; i++)
        if (g_cluts[i].key == key && g_cluts[i].ver == ver) return g_cluts[i].p;
    uint32_t bytes = (uint32_t)(n * 4 + 63) & ~63u;
    if (g_clut_top + bytes > CLUT_ARENA) { ge_sync(); ge_begin(); }
    uint32_t *dst = (uint32_t *)((uintptr_t)((uint8_t *)g_clut_arena + g_clut_top) | 0x40000000u);   /* uncached */
    g_clut_top += bytes;
    const uint16_t *src = g_vram + cly * 1024;
    for (int i = 0; i < n; i++) {
        uint32_t t = src[(clx + i) & 1023];
        uint32_t a = t == 0 ? 0 : (t & 0x8000) ? 0x70 : 0xFF;
        dst[i] = (a << 24) | (c5to8((t >> 10) & 31) << 16) | (c5to8((t >> 5) & 31) << 8) | c5to8(t & 31);
    }
    ClutEnt *e;
    if (g_nclut < NCLUT) e = &g_cluts[g_nclut++];
    else e = &g_cluts[g_clut_rr++ % NCLUT];
    e->key = key;
    e->ver = ver;
    e->p = dst;
    return dst;
}

/* ---- texture page cache -------------------------------------------------------------------------------------- */

#define NSLOTS 40
typedef struct { uint32_t key, ver[2], use; uint8_t *buf; } Slot;
static Slot g_slots[NSLOTS];
static uint32_t g_use_clock;

static const uint8_t *tex_page(int depth, int tpx, int tpy)
{
    int np = depth == 0 ? 1 : 2;
    int p0 = (tpy >> 8) * 16 + (tpx >> 6), p1 = (p0 & 16) | ((p0 + 1) & 15);
    uint32_t key = 1 + ((uint32_t)depth << 8) + (uint32_t)p0;
    Slot *s = 0, *lru = &g_slots[0];
    for (int i = 0; i < NSLOTS; i++) {
        if (g_slots[i].key == key) { s = &g_slots[i]; break; }
        if (g_slots[i].use < lru->use) lru = &g_slots[i];
    }
    int fresh = 0;
    if (!s) { s = lru; s->key = key; fresh = 1; }
    uint32_t v0 = g_page_ver[p0], v1 = np > 1 ? g_page_ver[p1] : 0;
    if (fresh || s->ver[0] != v0 || s->ver[1] != v1) {
        int w = 64 * np;
        if (tpx + w > 1024) w = 1024 - tpx;
        sceGuCopyImage(GU_PSM_5551, tpx, tpy, w, 256, 1024, (void *)EDRAM, 0, 0, 64 * np, s->buf);
        sceGuTexSync();
        sceGuTexFlush();
        s->ver[0] = v0;
        s->ver[1] = v1;
        g_ge_tex_uploads++;
    }
    s->use = ++g_use_clock;
    return s->buf;
}

/* ---- render state -------------------------------------------------------------------------------------------- */

static void set_tex(int on)
{
    if (S.tex == on) return;
    S.tex = on;
    if (on) sceGuEnable(GU_TEXTURE_2D); else sceGuDisable(GU_TEXTURE_2D);
}

static void set_tfx(int raw)
{
    if (S.tfx == raw) return;
    S.tfx = raw;
    /* GE TEXFUNC: bits 0-2 function, bit 8 alpha from texture, bit 16 colour doubling */
    sceGuSendCommandi(201, raw ? ((GU_TCC_RGBA << 8) | GU_TFX_REPLACE) : ((1 << 16) | (GU_TCC_RGBA << 8) | GU_TFX_MODULATE));
}

static void set_blend(int mode)                 /* -1: opaque, 0-3: PS1 semi-transparency mode */
{
    if (S.blend == mode) return;
    S.blend = mode;
    if (mode < 0) { sceGuDisable(GU_BLEND); return; }
    sceGuEnable(GU_BLEND);
    switch (mode) {
    case 0: sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x808080, 0x808080); break;              /* B/2 + F/2 */
    case 1: sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break;              /* B + F */
    case 2: sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break; /* B - F */
    default: sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x404040, 0xFFFFFF); break;             /* B + F/4 */
    }
}

static void set_atest(int m)                    /* 0 off, 1 texel not transparent, 2 opaque texels, 3 STP texels */
{
    if (S.atest == m) return;
    S.atest = m;
    if (m == 0) { sceGuDisable(GU_ALPHA_TEST); return; }
    sceGuEnable(GU_ALPHA_TEST);
    if (m == 1) sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    else if (m == 2) sceGuAlphaFunc(GU_EQUAL, 0xC0, 0xC0);
    else sceGuAlphaFunc(GU_EQUAL, 0x40, 0xC0);
}

static void set_scissor(int x0, int y0, int x1, int y1)   /* inclusive */
{
    if (S.sc[0] == x0 && S.sc[1] == y0 && S.sc[2] == x1 && S.sc[3] == y1) return;
    S.sc[0] = x0; S.sc[1] = y0; S.sc[2] = x1; S.sc[3] = y1;
    /* via the SDK: pspgu keeps its own scissor state and re-sends it, raw GE commands would be overridden */
    if (g_scissor_is_width) sceGuScissor(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    else sceGuScissor(x0, y0, x1 + 1, y1 + 1);
}

static void set_mask(void)
{
    int code = (g_rs.mask_check ? 2 : 0) | (g_rs.mask_set ? 1 : 0);
    if (S.stencil == code) return;
    S.stencil = code;
    if (g_rs.mask_check) {               /* draw only where the mask bit is clear */
        sceGuStencilFunc(GU_EQUAL, 0, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, g_rs.mask_set ? GU_INVERT : GU_KEEP);
    } else {
        sceGuStencilFunc(GU_ALWAYS, g_rs.mask_set ? 0xFF : 0, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    }
}

static void set_dither(void)
{
    int d = (g_rs.texpage >> 9) & 1;
    if (S.dither == d) return;
    S.dither = d;
    if (d) sceGuEnable(GU_DITHER); else sceGuDisable(GU_DITHER);
}

static void common_state(void)
{
    set_scissor(g_rs.clip_x0, g_rs.clip_y0, g_rs.clip_x1, g_rs.clip_y1);
    set_mask();
    set_dither();
}

static void setup_texture(int depth, int page, uint32_t clut)
{
    const uint32_t *cl = clut_get((int)(clut & 0x3F) * 16, (int)(clut >> 6) & 0x1FF, depth ? 256 : 16);
    const uint8_t *buf = tex_page(depth, (page & 15) * 64, ((page >> 4) & 1) * 256);
    if (S.clut != cl) { sceGuClutLoad(depth ? 32 : 2, cl); S.clut = cl; }
    if (S.tex_ptr != (uintptr_t)buf || S.tex_psm != depth) {
        sceGuTexMode(depth ? GU_PSM_T8 : GU_PSM_T4, 0, 0, 0);
        sceGuTexImage(0, 256, 256, 256, buf);
        S.tex_ptr = (uintptr_t)buf;
        S.tex_psm = depth;
    }
}

static void draw(int prim, int n, const GeVtx *v, int tex, int semi, int mode)
{
    GeVtx *gv = sceGuGetMemory(n * (int)sizeof(GeVtx));
    memcpy(gv, v, n * sizeof(GeVtx));
    if (!semi) {
        set_blend(-1);
        set_atest(tex ? 1 : 0);
        sceGuDrawArray(prim, VFMT, n, 0, gv);
    } else if (!tex) {
        set_blend(mode);
        set_atest(0);
        sceGuDrawArray(prim, VFMT, n, 0, gv);
    } else {
        set_blend(-1);
        set_atest(2);                    /* texels without the STP bit: opaque */
        sceGuDrawArray(prim, VFMT, n, 0, gv);
        set_blend(mode);
        set_atest(3);                    /* texels with the STP bit: blended */
        sceGuDrawArray(prim, VFMT, n, 0, gv);
    }
    g_ge_prims++;
    check_list();
}

/* Clip the bounding box to the drawing area; 0 if nothing is drawn (also drops primitives the PS1 rejects). */
static int clip_box(int *b)
{
    if (b[2] - b[0] >= 1024 || b[3] - b[1] >= 512) return 0;
    if (b[0] < g_rs.clip_x0) b[0] = g_rs.clip_x0;
    if (b[1] < g_rs.clip_y0) b[1] = g_rs.clip_y0;
    if (b[2] > g_rs.clip_x1) b[2] = g_rs.clip_x1;
    if (b[3] > g_rs.clip_y1) b[3] = g_rs.clip_y1;
    return b[0] <= b[2] && b[1] <= b[3];
}

static int needs_sw(int tex, int depth)
{
    return tex && (depth >= 2 || g_rs.tw_mask_x || g_rs.tw_mask_y);
}

/* ---- primitives ---------------------------------------------------------------------------------------------- */

void ge_polygon(const uint32_t *w)
{
    uint32_t op = w[0] >> 24;
    int quad = op & 8, tex = (op & 4) != 0, gour = op & 0x10, semi = (op & 2) != 0, raw = op & 1;
    int n = quad ? 4 : 3, k = 0;
    GeVtx v[4];
    uint32_t col = w[k++] & 0xFFFFFF, clut = 0, page = 0;
    int box[4] = {4096, 4096, -4096, -4096};
    for (int i = 0; i < n; i++) {
        if (i > 0 && gour) col = w[k++] & 0xFFFFFF;
        uint32_t xy = w[k++];
        int x = sx11(xy) + g_rs.off_x, y = sx11(xy >> 16) + g_rs.off_y;
        if (x < box[0]) box[0] = x;
        if (y < box[1]) box[1] = y;
        if (x > box[2]) box[2] = x;
        if (y > box[3]) box[3] = y;
        v[i].x = (int16_t)x; v[i].y = (int16_t)y; v[i].z = 0; v[i].pad = 0;
        v[i].c = 0xFF000000u | col;
        v[i].u = v[i].v = 0;
        if (tex) {
            uint32_t uv = w[k++];
            v[i].u = uv & 0xFF;
            v[i].v = (uv >> 8) & 0xFF;
            if (i == 0) clut = uv >> 16;
            if (i == 1) page = uv >> 16;
        }
    }
    if (tex) g_rs.texpage = (g_rs.texpage & ~0x9FFu) | (page & 0x9FF);
    int depth = (int)(page >> 7) & 3, mode = (g_rs.texpage >> 5) & 3;
    if (!clip_box(box)) return;
    if (needs_sw(tex, depth)) {
        ge_sync();
        raster_polygon(w);
        touch(box[0], box[1], box[2], box[3], 0);
        g_ge_sw_prims++;
        return;
    }
    ge_begin();
    if (tex) { setup_texture(depth, (int)page, clut); set_tfx(raw); }
    set_tex(tex);
    common_state();
    draw(GU_TRIANGLE_STRIP, n, v, tex, semi, mode);
    touch(box[0], box[1], box[2], box[3], 1);
}

void ge_rect(const uint32_t *w)
{
    uint32_t op = w[0] >> 24;
    int tex = (op & 4) != 0, semi = (op & 2) != 0, raw = op & 1, sz = (op >> 3) & 3, k = 1;
    uint32_t col = w[0] & 0xFFFFFF, xy = w[k++], clut = 0;
    int x0 = sx11(xy) + g_rs.off_x, y0 = sx11(xy >> 16) + g_rs.off_y, u0 = 0, v0 = 0;
    if (tex) {
        uint32_t uv = w[k++];
        u0 = uv & 0xFF;
        v0 = (uv >> 8) & 0xFF;
        clut = uv >> 16;
    }
    int ww, hh;
    switch (sz) {
    case 0: ww = w[k] & 0x3FF; hh = (w[k] >> 16) & 0x1FF; break;
    case 1: ww = hh = 1; break;
    case 2: ww = hh = 8; break;
    default: ww = hh = 16; break;
    }
    if (!ww || !hh) return;
    int depth = (g_rs.texpage >> 7) & 3, mode = (g_rs.texpage >> 5) & 3;
    int box[4] = {x0, y0, x0 + ww - 1, y0 + hh - 1};
    if (!clip_box(box)) return;
    if (needs_sw(tex, depth)) {
        ge_sync();
        raster_rect(w);
        touch(box[0], box[1], box[2], box[3], 0);
        g_ge_sw_prims++;
        return;
    }
    ge_begin();
    if (tex) { setup_texture(depth, (int)(g_rs.texpage & 0x1F) | (depth << 7), clut); set_tfx(raw); }
    set_tex(tex);
    common_state();
    GeVtx v[2] = {
        {(uint16_t)u0, (uint16_t)v0, 0xFF000000u | col, (int16_t)x0, (int16_t)y0, 0, 0},
        {(uint16_t)(u0 + ww), (uint16_t)(v0 + hh), 0xFF000000u | col, (int16_t)(x0 + ww), (int16_t)(y0 + hh), 0, 0},
    };
    draw(GU_SPRITES, 2, v, tex, semi, mode);
    touch(box[0], box[1], box[2], box[3], 1);
}

void ge_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int semi)
{
    x0 = sx11(x0) + g_rs.off_x; y0 = sx11(y0) + g_rs.off_y;
    x1 = sx11(x1) + g_rs.off_x; y1 = sx11(y1) + g_rs.off_y;
    int box[4] = {x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1};
    if (!clip_box(box)) return;
    ge_begin();
    set_tex(0);
    common_state();
    GeVtx v[2] = {
        {0, 0, 0xFF000000u | c0, (int16_t)x0, (int16_t)y0, 0, 0},
        {0, 0, 0xFF000000u | c1, (int16_t)x1, (int16_t)y1, 0, 0},
    };
    draw(GU_LINES, 2, v, 0, semi, (g_rs.texpage >> 5) & 3);
    touch(box[0], box[1], box[2], box[3], 1);
}

int ge_fill(int x, int y, int w, int h, uint16_t col)
{
    if (!g_use_ge || !w || !h || x + w > 1024 || y + h > 512) return 0;
    ge_begin();
    set_tex(0);
    set_blend(-1);
    set_atest(0);
    set_scissor(x, y, x + w - 1, y + h - 1);     /* fill ignores the drawing area and the mask bit */
    S.stencil = -1;
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    S.dither = 0;
    sceGuDisable(GU_DITHER);
    uint32_t c = 0xFF000000u | ((uint32_t)((col >> 10) & 31) << 19) | ((uint32_t)((col >> 5) & 31) << 11) | ((uint32_t)(col & 31) << 3);
    GeVtx *gv = sceGuGetMemory(2 * (int)sizeof(GeVtx));
    gv[0] = (GeVtx){0, 0, c, (int16_t)x, (int16_t)y, 0, 0};
    gv[1] = (GeVtx){0, 0, c, (int16_t)(x + w), (int16_t)(y + h), 0, 0};
    sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, gv);
    touch(x, y, x + w - 1, y + h - 1, 1);
    check_list();
    return 1;
}

int ge_copy(int sx, int sy, int dx, int dy, int w, int h)
{
    if (!g_use_ge || sx + w > 1024 || dx + w > 1024 || sy + h > 512 || dy + h > 512) return 0;
    if (sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h) return 0;   /* overlapping: CPU */
    ge_begin();
    sceGuCopyImage(GU_PSM_5551, sx, sy, w, h, 1024, (void *)EDRAM, dx, dy, 1024, (void *)EDRAM);
    sceGuTexSync();
    touch(dx, dy, dx + w - 1, dy + h - 1, 1);
    check_list();
    return 1;
}

/* ---- presentation ------------------------------------------------------------------------------------------- */

void ge_present(int dx, int dy, int w, int h, int blank)
{
    /* the PS1 picture is 4:3 whatever its resolution: 363x272, centred, bilinear */
    const int ow = 363, oh = 272, ox = (480 - ow) / 2;
    ge_begin();
    sceGuDrawBufferList(GU_PSM_5551, (void *)FB_OFF(g_fb), 512);
    set_scissor(0, 0, 479, 271);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_DITHER);
    set_blend(-1);
    set_atest(0);
    set_tex(0);
    GeVtx *bg = sceGuGetMemory(2 * (int)sizeof(GeVtx));
    bg[0] = (GeVtx){0, 0, 0xFF000000u, 0, 0, 0, 0};
    bg[1] = (GeVtx){0, 0, 0xFF000000u, 480, 272, 0, 0};
    sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, bg);
    if (!blank) {
        int ax = dx & ~7, uo = dx - ax;
        set_tex(1);
        sceGuTexMode(GU_PSM_5551, 0, 0, 0);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFlush();
        /* GE textures are at most 512 wide: 640-pixel modes are drawn as two strips */
        for (int s0 = 0; s0 < w; s0 += 256) {
            int s1 = s0 + 256 < w ? s0 + 256 : w;
            sceGuTexImage(0, 512, h > 256 ? 512 : 256, 1024,
                          (void *)(EDRAM + (unsigned)dy * 2048 + (unsigned)((ax + s0) & 1023) * 2));
            GeVtx *gv = sceGuGetMemory(2 * (int)sizeof(GeVtx));
            gv[0] = (GeVtx){(uint16_t)uo, 0, 0xFFFFFFFFu, (int16_t)(ox + s0 * ow / w), 0, 0, 0};
            gv[1] = (GeVtx){(uint16_t)(uo + s1 - s0), (uint16_t)h, 0xFFFFFFFFu, (int16_t)(ox + s1 * ow / w), (int16_t)oh, 0, 0};
            sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, gv);
        }
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    }
    sceGuDrawBufferList(GU_PSM_5551, (void *)0, 1024);
    sceGuEnable(GU_STENCIL_TEST);
    ge_sync();
    sceDisplaySetFrameBuf((void *)(EDRAM + FB_OFF(g_fb)), 512, PSP_DISPLAY_PIXEL_FORMAT_5551, PSP_DISPLAY_SETBUF_NEXTFRAME);
    g_fb ^= 1;
}

/* Startup check of the scissor semantics of this pspsdk (logged): a 100-pixel sprite is drawn with
 * sceGuScissor(40, 0, 20, 512); width semantics clip it to 20 columns. */

static void selftest(void)
{
    ge_begin();
    set_tex(0); set_blend(-1); set_atest(0);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuScissor(40, 0, 20, 512);
    GeVtx *gv = sceGuGetMemory(2 * (int)sizeof(GeVtx));
    gv[0] = (GeVtx){0, 0, 0xFF0000FFu, 0, 16, 0, 0};
    gv[1] = (GeVtx){0, 0, 0xFF0000FFu, 100, 17, 0, 0};
    sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, gv);
    ge_sync();
    int hits = 0;
    for (int x = 0; x < 128; x++) if (g_vram[16 * 1024 + x] & 0x1F) hits++;
    g_scissor_is_width = hits == 20;
    logf_("ge selftest: sceGuScissor(40,0,20,512) let %d pixels through -> %s semantics\n", hits,
          g_scissor_is_width ? "width" : "end");
    memset(g_vram, 0, 1024 * 2 * 32);
}

void ge_init(void)
{
    sceGuInit();
    selftest();
    for (int i = 0; i < NSLOTS; i++) {
        g_slots[i].buf = memalign(64, 65536);
        if (!g_slots[i].buf) fatal(0, "out of memory (texture cache)");
        memset(g_slots[i].buf, 0, 65536);
        sceKernelDcacheWritebackInvalidateRange(g_slots[i].buf, 65536);
    }
    sceKernelDcacheWritebackInvalidateRange(g_clut_arena, sizeof g_clut_arena);
    sceDisplaySetFrameBuf((void *)(EDRAM + FB_OFF(0)), 512, PSP_DISPLAY_PIXEL_FORMAT_5551, PSP_DISPLAY_SETBUF_NEXTFRAME);
}

/* Debug: the display buffer shown last (480x272 inside a 512-pixel stride, 5551). */
const uint16_t *ge_display_buffer(void)
{
    return (const uint16_t *)(0x40000000u | (EDRAM + FB_OFF(g_fb ^ 1)));
}
