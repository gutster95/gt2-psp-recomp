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
#include "prof.h"

int g_use_ge = 1;

#define EDRAM 0x04000000u
#define FB_OFF(i) (0x100000u + (unsigned)(i) * 0x44000u)
#define VFMT (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
#define LIST_BYTES (1024 * 1024)

typedef struct { uint16_t u, v; uint32_t c; int16_t x, y, z, pad; } GeVtx;

static uint32_t g_list[LIST_BYTES / 4] __attribute__((aligned(64)));
static int g_open, g_shown;
static int g_last_vtype = -1, g_draws;    /* draw_array state */

/* Hot-path GE commands are written straight into pspgu's current display list (same as sceGuSendCommandi, without
 * the call). gu_list is pspgu's internal list descriptor {start, current, parent_context}. */
typedef struct { unsigned int *start, *current; int parent_context; } GuListPriv;
extern GuListPriv *gu_list;
static inline void cmd(uint32_t c, uint32_t a) { *gu_list->current++ = (c << 24) | (a & 0xFFFFFF); }

/* Vertex arena (uncached, so the GE sees the data without cache writeback); valid until the next ge_sync. */
#define VARENA (512 * 1024)
static uint8_t g_varena[VARENA] __attribute__((aligned(64)));
static uint32_t g_vtop;
static inline void *valloc_v(int bytes)
{
    void *p = (void *)((uintptr_t)(g_varena + g_vtop) | 0x40000000u);
    g_vtop += ((uint32_t)bytes + 15) & ~15u;
    return p;
}
static int g_cur_fb = -1;             /* GE draw target: -1 PS1 VRAM, 0/1 hi-res frame buffers, 2 scaled present */
static int g_scissor_is_width = 1;    /* sceGuScissor(x, y, w, h) semantics, checked at startup */
uint32_t g_ge_prims, g_ge_sw_prims, g_ge_tex_uploads, g_ge_syncs, g_ge_pgxp;

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
    int tex, tfx, blend, atest, stencil, dither, tex_psm, filter;
    int sc[4];
    uintptr_t tex_ptr;
    const void *clut;
} S;

/* -1 is a valid value for blend (opaque), so "unknown" is -99 there; everything else uses -1. The GE keeps its
 * state across display lists, so after a restart nothing may be assumed. */
static void invalidate_state(void) { memset(&S, 0xFF, sizeof S); S.blend = -99; }

/* ---- CLUT conversion (per batch arena; the GE copies a CLUT on load, entries stay valid until the sync) ----- */

#define CLUT_ARENA (128 * 1024)
static uint32_t g_clut_arena[CLUT_ARENA / 4] __attribute__((aligned(64)));
static uint32_t g_clut_top;
typedef struct { uint32_t key, ver; const uint32_t *p; int transparent; } ClutEnt;
static int g_clut_transparent;   /* the CLUT of the current texture has transparent entries (no filtering) */
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
    g_vtop = 0;
    g_ge_pending = 0;
    g_clut_top = 0;
    g_nclut = 0;
    g_ge_syncs++;
}

/* Variants of the 3D-mode setup, probed at startup (selftest_3d): PPSSPP accepts all of them, real hardware
 * drew nothing with variant 0. zs = clip z per unit of w (ndc z), clip = GU_CLIP_PLANES (depth clamp), inv = depth
 * range 65535..0. */
static const struct { float zs; int clip, inv; } TVAR[] = {
    {0, 0, 0}, {0.5f, 0, 0}, {0.5f, 1, 0}, {0, 1, 0}, {0.5f, 0, 1}, {-0.5f, 0, 0}, {0.5f, 1, 1},
};
#define NTVAR ((int)(sizeof TVAR / sizeof TVAR[0]))
static int g_tvar;

static void ge_begin(void)
{
    if (g_open) return;
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)0, 1024);
    sceGuSendCommandi(0x15, 0);                          /* drawing region: the whole PS1 VRAM */
    sceGuSendCommandi(0x16, (511 << 10) | 1023);
    sceGuOffset(2048, 2048);
    /* 3D mode (perspective-correct hi-res pass): vertices are (x*w, y*w, w); the projection moves w into the
     * homogeneous coordinate, the viewport has scale 1, so screen = (x, y) and attributes interpolate with 1/w. */
    static ScePspFMatrix4 ident = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    ScePspFMatrix4 proj = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, TVAR[g_tvar].zs, 1}, {0, 0, 0, 0}};
    sceGuSetMatrix(GU_PROJECTION, &proj);
    sceGuSetMatrix(GU_VIEW, &ident);
    sceGuSetMatrix(GU_MODEL, &ident);
    sceGuViewport(2048, 2048, 2, -2);
    if (TVAR[g_tvar].inv) sceGuDepthRange(65535, 0); else sceGuDepthRange(0, 65535);
    if (TVAR[g_tvar].clip) sceGuEnable(GU_CLIP_PLANES); else sceGuDisable(GU_CLIP_PLANES);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
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
    g_last_vtype = -1;
    g_cur_fb = -1;
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
        if (g_cluts[i].key == key && g_cluts[i].ver == ver) { g_clut_transparent = g_cluts[i].transparent; return g_cluts[i].p; }
    uint32_t bytes = (uint32_t)(n * 4 + 63) & ~63u;
    if (g_clut_top + bytes > CLUT_ARENA) { ge_sync(); ge_begin(); }
    uint32_t *dst = (uint32_t *)((uintptr_t)((uint8_t *)g_clut_arena + g_clut_top) | 0x40000000u);   /* uncached */
    g_clut_top += bytes;
    const uint16_t *src = g_vram + cly * 1024;
    int transparent = 0;
    for (int i = 0; i < n; i++) {
        uint32_t t = src[(clx + i) & 1023];
        transparent |= t == 0;
        uint32_t a = t == 0 ? 0 : (t & 0x8000) ? 0x70 : 0xFF;
        dst[i] = (a << 24) | (c5to8((t >> 10) & 31) << 16) | (c5to8((t >> 5) & 31) << 8) | c5to8(t & 31);
    }
    ClutEnt *e;
    if (g_nclut < NCLUT) e = &g_cluts[g_nclut++];
    else e = &g_cluts[g_clut_rr++ % NCLUT];
    e->key = key;
    e->ver = ver;
    e->p = dst;
    e->transparent = transparent;
    g_clut_transparent = transparent;
    return dst;
}

/* ---- texture page cache -------------------------------------------------------------------------------------- */

#define NSLOTS 40
typedef struct { uint32_t key, ver[2], use; uint8_t *buf; } Slot;
static Slot g_slots[NSLOTS];
static uint32_t g_use_clock;

void ge_note_read(int x, int y, int w, int h);

static const uint8_t *tex_page(int depth, int tpx, int tpy)
{
    ge_note_read(tpx, tpy, depth == 0 ? 64 : 128, 256);
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

static void set_filter(int lin)
{
    if (S.filter == lin) return;
    S.filter = lin;
    cmd(0xC6, lin ? ((GU_LINEAR << 8) | GU_LINEAR) : ((GU_NEAREST << 8) | GU_NEAREST));
}

static void set_tex(int on)
{
    if (S.tex == on) return;
    S.tex = on;
    cmd(0x1E, on ? 1 : 0);
}

static void set_tfx(int raw)
{
    if (S.tfx == raw) return;
    S.tfx = raw;
    /* GE TEXFUNC: bits 0-2 function, bit 8 alpha from texture, bit 16 colour doubling */
    cmd(0xC9, raw ? ((GU_TCC_RGBA << 8) | GU_TFX_REPLACE) : ((1 << 16) | (GU_TCC_RGBA << 8) | GU_TFX_MODULATE));
}

static void set_blend(int mode)                 /* -1: opaque, 0-3: PS1 semi-transparency mode */
{
    if (S.blend == mode) return;
    S.blend = mode;
    if (mode < 0) { cmd(0x21, 0); return; }
    cmd(0x21, 1);
    static const uint32_t fa[4] = {0x808080, 0xFFFFFF, 0xFFFFFF, 0x404040}, fb[4] = {0x808080, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF};
    /* B/2 + F/2, B + F, B - F, B + F/4 */
    cmd(0xDF, GU_FIX | (GU_FIX << 4) | ((mode == 2 ? GU_REVERSE_SUBTRACT : GU_ADD) << 8));
    cmd(0xE0, fa[mode]);
    cmd(0xE1, fb[mode]);
}

static void set_atest(int m)                    /* 0 off, 1 texel not transparent, 2 opaque texels, 3 STP texels */
{
    if (S.atest == m) return;
    S.atest = m;
    if (m == 0) { cmd(0x22, 0); return; }
    cmd(0x22, 1);
    if (m == 1) cmd(0xDB, GU_GREATER | (0 << 8) | (0xFF << 16));
    else if (m == 2) cmd(0xDB, GU_EQUAL | (0xC0 << 8) | (0xC0 << 16));
    else cmd(0xDB, GU_EQUAL | (0x40 << 8) | (0xC0 << 16));
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
        cmd(0xDC, GU_EQUAL | (0 << 8) | (0xFF << 16));
        cmd(0xDD, GU_KEEP | (GU_KEEP << 8) | ((g_rs.mask_set ? GU_INVERT : GU_KEEP) << 16));
    } else {
        cmd(0xDC, GU_ALWAYS | ((g_rs.mask_set ? 0xFF : 0) << 8) | (0xFF << 16));
        cmd(0xDD, GU_KEEP | (GU_KEEP << 8) | (GU_REPLACE << 16));
    }
}

static void set_dither(void)
{
    int d = (g_rs.texpage >> 9) & 1;
    if (S.dither == d) return;
    S.dither = d;
    cmd(0x20, d ? 1 : 0);
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

/* ---- hi-res frames ----------------------------------------------------------------------------------------------
 * A "frame" is a PS1 framebuffer region that has been displayed (display start + size). Primitives whose drawing
 * area lies in a frame are drawn twice: into the PS1 VRAM (exact 1x image for readback and render-to-texture) and,
 * mapped to PSP resolution, into one of the two PSP display buffers. Presenting a frame is then just a flip.
 * Mapping: 2D primitives (HUD, menus) keep the 4:3 picture in the centre 363x272. With widescreen, the GTE hack
 * (gte.c: projected X * 3/4) squeezes the whole 16:9 field of view into the PS1 screen width, so every visibility
 * test of the game (all in PS1 screen space) covers 16:9; 3D primitives - recognised by vertices the GTE produced
 * in the last frames - are then stretched over the full 480 pixels. */

int g_hires = 1, g_widescreen = 1;

typedef struct {
    int x, y, w, h, used, drawn, band_clear, n3d_cur, n3d_last;
    float kx2, ox2, kx3, ox3, ky, oy;   /* precomputed mappings PS1 -> PSP pixels (2D centre / 3D full width) */
} Frame;
int g_hud_anchor = 1;   /* widescreen: move HUD elements to the screen edges in 3D scenes (shifted, not stretched) */
static Frame g_frames[2];
static int g_frame_gen;   /* bumped whenever a frame's mapping changes */

/* Displayed frames are drawn into PS1 VRAM (1x) as well only once the game reads them (as a texture, by a VRAM
 * copy or a CPU readback); until then the 1x pass of frame primitives is skipped - about half the GE work. */
int g_frames_read;

void ge_note_read(int x, int y, int w, int h)
{
    if (g_frames_read || !g_hires) return;
    for (int k = 0; k < 2; k++) {
        const Frame *f = &g_frames[k];
        if (f->used && x < f->x + f->w && x + w > f->x && y < f->y + f->h && y + h > f->y) {
            g_frames_read = 1;
            logf_("ge: game reads a displayed frame (%d,%d %dx%d): 1x frame rendering enabled\n", x, y, w, h);
            return;
        }
    }
}

typedef struct { uint16_t u, v; uint32_t c; float x, y, z; } HVtx;
typedef struct { float u, v; uint32_t c; float x, y, z; } PVtx;
#define PFMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define HFMT (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

static inline float map_x(const Frame *f, float x) { return f->ox2 + x * f->kx2; }
static inline float map_x3(const Frame *f, float x) { return f->ox3 + x * f->kx3; }

/* Recently GTE-projected screen coordinates (hash set, entries expire after a few vblanks) */
#define XYSET 8192
static uint32_t g_xy_key[XYSET];
static uint16_t g_xy_stamp[XYSET];
static inline uint16_t xy_now(void) { return (uint16_t)(g_vblank_count >> 1); }
static inline uint32_t xy_hash(uint32_t k) { return (k * 0x9E3779B1u) >> 19; }

/* Precise data per projected vertex (PGXP-style): sub-pixel screen position and view depth, used to draw 3D
 * primitives perspective-correct and without the integer jitter of PS1 coordinates. */
static float g_xy_px[XYSET], g_xy_py[XYSET], g_xy_pz[XYSET];
int g_pgxp = 1;          /* perspective-correct, sub-pixel 3D in the hi-res pass */
int g_tex_filter = 1;    /* bilinear filtering of opaque 3D textures in the hi-res pass */

void ge_mark_3d_precise(uint32_t xy, float x, float y, float z)
{
    if (!g_widescreen || !g_hires) return;
    uint16_t now = xy_now();
    uint32_t h = xy_hash(xy), j = h;
    for (int i = 0; i < 8; i++) {
        uint32_t k = (h + i) & (XYSET - 1);
        if (g_xy_key[k] == xy || (uint16_t)(now - g_xy_stamp[k]) > 3) { j = k; break; }
    }
    g_xy_key[j] = xy;
    g_xy_stamp[j] = now;
    g_xy_px[j] = x;
    g_xy_py[j] = y;
    g_xy_pz[j] = z;
}

void ge_mark_3d(uint32_t xy) { ge_mark_3d_precise(xy, 0, 0, 0); }

static int xy_find(uint32_t xy)
{
    uint16_t now = xy_now();
    uint32_t h = xy_hash(xy);
    for (int i = 0; i < 8; i++) {
        uint32_t j = (h + i) & (XYSET - 1);
        if (g_xy_key[j] == xy && (uint16_t)(now - g_xy_stamp[j]) <= 3) return (int)j;
    }
    return -1;
}

static int is_3d_xy(uint32_t xy) { return xy_find(xy) >= 0; }
static inline float map_y(const Frame *f, float y) { return f->oy + y * f->ky; }

static void bind(int target)
{
    if (g_cur_fb == target) return;
    g_cur_fb = target;
    if (target < 0) sceGuDrawBufferList(GU_PSM_5551, (void *)0, 1024);
    else sceGuDrawBufferList(GU_PSM_5551, (void *)FB_OFF(target), 512);
    S.sc[0] = -2;                                  /* scissor must be re-sent for the new target */
}

static int frame_of(int x, int y)
{
    if (!g_hires) return -1;
    for (int k = 0; k < 2; k++) {
        const Frame *f = &g_frames[k];
        if (f->used && x >= f->x && x < f->x + f->w && y >= f->y && y < f->y + f->h) return k;
    }
    return -1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* scissor in the hi-res buffer for the current drawing area */
static void hires_scissor(const Frame *f, int is3d)
{
    /* the mapped rectangle only changes with the drawing area: cache it */
    static const Frame *lf;
    static int lis3d = -1, lgen = -1, lc[4] = {-1, -1, -1, -1}, r[4];
    if (f != lf || lgen != g_frame_gen || is3d != lis3d || lc[0] != g_rs.clip_x0 || lc[1] != g_rs.clip_y0 || lc[2] != g_rs.clip_x1 ||
        lc[3] != g_rs.clip_y1) {
        lf = f; lgen = g_frame_gen; lis3d = is3d; lc[0] = g_rs.clip_x0; lc[1] = g_rs.clip_y0; lc[2] = g_rs.clip_x1; lc[3] = g_rs.clip_y1;
        float (*mx)(const Frame *, float) = is3d ? map_x3 : map_x;
        r[0] = clampi((int)(mx(f, (float)g_rs.clip_x0) + 0.5f), 0, 479);
        r[2] = clampi((int)(mx(f, (float)g_rs.clip_x1 + 1) + 0.5f) - 1, 0, 479);
        r[1] = clampi((int)(map_y(f, (float)g_rs.clip_y0) + 0.5f), 0, 271);
        r[3] = clampi((int)(map_y(f, (float)g_rs.clip_y1 + 1) + 0.5f) - 1, 0, 271);
    }
    set_scissor(r[0], r[1], r[2], r[3]);
}

static void flat_sprite(float x0, float y0, float x1, float y1, uint32_t c)
{
    HVtx *v = sceGuGetMemory(2 * (int)sizeof(HVtx));
    v[0] = (HVtx){0, 0, c, x0, y0, 0};
    v[1] = (HVtx){0, 0, c, x1, y1, 0};
    sceGuDrawArray(GU_SPRITES, HFMT, 2, 0, v);
}

/* Before the first draw into a frame after it was shown: clear the widescreen side bands, which 2D-only screens
 * never overwrite. */
static void prepare_frame(int k)
{
    Frame *f = &g_frames[k];
    f->drawn = 1;
    if (!f->band_clear) return;
    f->band_clear = 0;
    f->n3d_last = f->n3d_cur;                  /* first draw of a new picture into this buffer */
    f->n3d_cur = 0;
    if (!g_widescreen) return;
    bind(k);
    set_scissor(0, 0, 479, 271);
    set_tex(0); set_blend(-1); set_atest(0);
    S.stencil = -1;
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    flat_sprite(0, 0, map_x(f, (float)f->x), 272, 0xFF000000u);
    flat_sprite(map_x(f, (float)(f->x + f->w)), 0, 480, 272, 0xFF000000u);
}

/* ---- drawing ------------------------------------------------------------------------------------------------- */

/* Draw call without pspgu's sceGuDrawArray, which calls the kernel (sceGeListUpdateStallAddr) every time. The
 * commands go straight into the list; every 32nd draw goes through sceGuDrawArray so the GE keeps running. */

static void draw_array(int prim, int vtype, int n, const void *v)
{
    if (++g_draws >= 32) {
        g_draws = 0;
        g_last_vtype = vtype;
        sceGuDrawArray(prim, vtype, n, 0, v);
        return;
    }
    if (vtype != g_last_vtype) { cmd(0x12, (uint32_t)vtype); g_last_vtype = vtype; }
    cmd(0x10, (uint32_t)(((uintptr_t)v >> 8) & 0xF0000));
    cmd(0x01, (uint32_t)((uintptr_t)v & 0xFFFFFF));
    cmd(0x04, (uint32_t)((prim << 16) | n));
}

static void passes(int prim, int fmt, int n, const void *gv, int tex, int semi, int mode)
{
    if (!semi) {
        set_blend(-1);
        set_atest(tex ? 1 : 0);
        draw_array(prim, fmt, n, gv);
    } else if (!tex) {
        set_blend(mode);
        set_atest(0);
        draw_array(prim, fmt, n, gv);
    } else {
        set_blend(-1);
        set_atest(2);                    /* texels without the STP bit: opaque */
        draw_array(prim, fmt, n, gv);
        set_blend(mode);
        set_atest(3);                    /* texels with the STP bit: blended */
        draw_array(prim, fmt, n, gv);
    }
}

/* vis1: visible in the PS1 drawing area; fk: hi-res frame or -1 */
static void draw(int prim, int n, const GeVtx *v, int tex, int semi, int mode, int vis1, int fk, int is3d,
                 const int *xyj)
{
    set_mask();
    set_dither();
    if (vis1 && (fk < 0 || g_frames_read)) {
        bind(-1);
        set_filter(0);
        set_scissor(g_rs.clip_x0, g_rs.clip_y0, g_rs.clip_x1, g_rs.clip_y1);
        GeVtx *gv = valloc_v(n * (int)sizeof(GeVtx));
        memcpy(gv, v, n * sizeof(GeVtx));
        passes(prim, VFMT, n, gv, tex, semi, mode);
    }
    if (fk >= 0) {
        Frame *f = &g_frames[fk];
        bind(fk);
        /* HUD anchoring (races only - overlay 0; menus with 3D content such as the dealer's car view keep their
         * 2D layout in the 4:3 middle): in a 3D scene, 2D elements in the left/right 40% of the 4:3 picture move to the 16:9
         * edges by the band width (58.5 px); elements in the middle or wider than half the screen stay. */
        /* Text is drawn glyph by glyph: a glyph next to the previous 2D element on the same line belongs to the same
         * string and takes its shift, so strings are never torn apart. */
        static float run_lo, run_hi, run_y0, run_y1, run_shift;
        static int run_valid;
        float shift = 0;
        if (is3d) { f->n3d_cur++; run_valid = 0; }
        else if (g_widescreen && g_hud_anchor && g_cur_overlay == 0 && (f->n3d_last > 50 || f->n3d_cur > 50)) {
            float lo = v[0].x, hi = v[0].x, y0 = v[0].y, y1 = v[0].y;
            for (int i = 1; i < n; i++) {
                if (v[i].x < lo) lo = v[i].x;
                if (v[i].x > hi) hi = v[i].x;
                if (v[i].y < y0) y0 = v[i].y;
                if (v[i].y > y1) y1 = v[i].y;
            }
            float cx = (lo + hi) * 0.5f - f->x;
            if (run_valid && y0 <= run_y1 && y1 >= run_y0 && lo <= run_hi + 12 && hi >= run_lo - 12) {
                shift = run_shift;
                if (lo < run_lo) run_lo = lo;
                if (hi > run_hi) run_hi = hi;
            } else {
                if (hi - lo <= f->w * 0.5f) {
                    if (cx < f->w * 0.4f) shift = -(480.0f - 363.0f) * 0.5f;
                    else if (cx > f->w * 0.6f) shift = (480.0f - 363.0f) * 0.5f;
                }
                run_lo = lo; run_hi = hi; run_y0 = y0; run_y1 = y1; run_shift = shift; run_valid = 1;
            }
        }
        hires_scissor(f, is3d || shift != 0);
        if (is3d && g_pgxp && xyj && prim == GU_TRIANGLE_STRIP) {
            PVtx *pv = valloc_v(n * (int)sizeof(PVtx));
            int ok = 1;
            for (int i = 0; i < n && ok; i++) {
                int rx = v[i].x - g_rs.off_x, ry = v[i].y - g_rs.off_y;
                int j = xyj ? xyj[i] : -1;
                if (j < 0 || g_xy_pz[j] < 1.0f || g_xy_px[j] - rx > 2 || rx - g_xy_px[j] > 2 ||
                    g_xy_py[j] - ry > 2 || ry - g_xy_py[j] > 2) { ok = 0; break; }
                float z = g_xy_pz[j];
                float hx = map_x3(f, g_xy_px[j] + g_rs.off_x), hy = map_y(f, g_xy_py[j] + g_rs.off_y);
                pv[i] = (PVtx){v[i].u / 256.0f, v[i].v / 256.0f, v[i].c, hx * z, hy * z, z};
            }
            if (ok) {
                /* bilinear only without transparent texels: filtering would pull their black into the edges */
                int filt = tex && !semi && g_tex_filter && !g_clut_transparent;
                set_filter(filt);
                passes(prim, PFMT, n, pv, tex, semi, mode);
                g_ge_pgxp++;
                g_ge_prims++;
                check_list();
                return;
            }
        }
        HVtx *hv = valloc_v(n * (int)sizeof(HVtx));
        for (int i = 0; i < n; i++) {
            hv[i].u = v[i].u;
            hv[i].v = v[i].v;
            hv[i].c = v[i].c;
            hv[i].x = is3d ? map_x3(f, v[i].x) : map_x(f, v[i].x) + shift;
            hv[i].y = map_y(f, v[i].y);
            hv[i].z = 0;
        }
        set_filter(0);
        passes(prim, HFMT, n, hv, tex, semi, mode);
    }
    g_ge_prims++;
    check_list();
}

static int too_big(const int *b) { return b[2] - b[0] >= 1024 || b[3] - b[1] >= 512; }

/* Clip the bounding box to the drawing area; 0 if nothing of it is inside. */
static int clip_box(int *b)
{
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

static void mirror(int x, int y, int w, int h);

/* Common tail of all primitives: decide targets, run the software fallback or the GE passes. */
static void submit(int prim, int n, const GeVtx *v, int *box, int tex, int depth, int page, uint32_t clut, int raw,
                   int semi, int mode, void (*sw)(const uint32_t *), const uint32_t *w, int is3d, const int *xyj)
{
    if (too_big(box)) return;
    int vis1 = clip_box(box);
    if (!vis1) return;
    int fk = frame_of(g_rs.clip_x0, g_rs.clip_y0);
    if (needs_sw(tex, depth) && sw) {
        ge_sync();
        sw(w);
        touch(box[0], box[1], box[2], box[3], 0);
        g_ge_sw_prims++;
        if (fk >= 0) mirror(box[0], box[1], box[2] - box[0] + 1, box[3] - box[1] + 1);
        return;
    }
    if (g_vtop > VARENA - 1024) ge_sync();
    ge_begin();
    if (fk >= 0) prepare_frame(fk);
    int ph = g_phase;
    g_phase = PH_GE_TEX;
    if (tex) { setup_texture(depth, page, clut); set_tfx(raw); }
    set_tex(tex);
    g_phase = PH_GE_DRAW;
    draw(prim, n, v, tex, semi, mode, vis1, fk, is3d && g_widescreen, xyj);
    g_phase = ph;
    if (vis1) touch(box[0], box[1], box[2], box[3], 1);
}

/* ---- primitives ---------------------------------------------------------------------------------------------- */

void ge_polygon(const uint32_t *w)
{
    uint32_t op = w[0] >> 24;
    int quad = op & 8, tex = (op & 4) != 0, gour = op & 0x10, semi = (op & 2) != 0, raw = op & 1;
    int n = quad ? 4 : 3, k = 0, n3d = 0, xyj[4];
    GeVtx v[4];
    uint32_t col = w[k++] & 0xFFFFFF, clut = 0, page = 0;
    int box[4] = {4096, 4096, -4096, -4096};
    for (int i = 0; i < n; i++) {
        if (i > 0 && gour) col = w[k++] & 0xFFFFFF;
        uint32_t xy = w[k++];
        xyj[i] = xy_find(xy);
        n3d += xyj[i] >= 0;
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
    submit(GU_TRIANGLE_STRIP, n, v, box, tex, depth, (int)page, clut, raw, semi, mode, raster_polygon, w, n3d >= 2, xyj);
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
    GeVtx v[2] = {
        {(uint16_t)u0, (uint16_t)v0, 0xFF000000u | col, (int16_t)x0, (int16_t)y0, 0, 0},
        {(uint16_t)(u0 + ww), (uint16_t)(v0 + hh), 0xFF000000u | col, (int16_t)(x0 + ww), (int16_t)(y0 + hh), 0, 0},
    };
    submit(GU_SPRITES, 2, v, box, tex, depth, (int)(g_rs.texpage & 0x1F) | (depth << 7), clut, raw, semi, mode,
           raster_rect, w, 0, 0);
}

void ge_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int semi)
{
    uint32_t x0r = (uint32_t)x0, y0r = (uint32_t)y0, x1r = (uint32_t)x1, y1r = (uint32_t)y1;
    x0 = sx11(x0) + g_rs.off_x; y0 = sx11(y0) + g_rs.off_y;
    x1 = sx11(x1) + g_rs.off_x; y1 = sx11(y1) + g_rs.off_y;
    int box[4] = {x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1};
    GeVtx v[2] = {
        {0, 0, 0xFF000000u | c0, (int16_t)x0, (int16_t)y0, 0, 0},
        {0, 0, 0xFF000000u | c1, (int16_t)x1, (int16_t)y1, 0, 0},
    };
    int n3d = is_3d_xy(((uint32_t)y0r << 16) | (x0r & 0xFFFF)) + is_3d_xy(((uint32_t)y1r << 16) | (x1r & 0xFFFF));
    submit(GU_LINES, 2, v, box, 0, 0, 0, 0, 0, semi, (g_rs.texpage >> 5) & 3, 0, 0, n3d >= 2, 0);
}

/* ---- fills, copies, uploads ---------------------------------------------------------------------------------- */

static void fill_state(void)
{
    set_tex(0);
    set_blend(-1);
    set_atest(0);
    S.stencil = -1;
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    S.dither = 0;
    sceGuDisable(GU_DITHER);
}

int ge_fill(int x, int y, int w, int h, uint16_t col)
{
    if (!g_use_ge || !w || !h || x + w > 1024 || y + h > 512) return 0;
    ge_begin();
    fill_state();
    bind(-1);
    set_scissor(x, y, x + w - 1, y + h - 1);     /* fill ignores the drawing area and the mask bit */
    uint32_t c = 0xFF000000u | ((uint32_t)((col >> 10) & 31) << 19) | ((uint32_t)((col >> 5) & 31) << 11) | ((uint32_t)(col & 31) << 3);
    GeVtx *gv = sceGuGetMemory(2 * (int)sizeof(GeVtx));
    gv[0] = (GeVtx){0, 0, c, (int16_t)x, (int16_t)y, 0, 0};
    gv[1] = (GeVtx){0, 0, c, (int16_t)(x + w), (int16_t)(y + h), 0, 0};
    sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, gv);
    touch(x, y, x + w - 1, y + h - 1, 1);
    for (int k = 0; k < 2; k++) {                /* same fill in the hi-res frames it touches */
        Frame *f = &g_frames[k];
        if (!g_hires || !f->used || x >= f->x + f->w || x + w <= f->x || y >= f->y + f->h || y + h <= f->y) continue;
        int whole = x <= f->x && y <= f->y && x + w >= f->x + f->w && y + h >= f->y + f->h;
        f->drawn = 1;
        bind(k);
        if (whole && g_widescreen) {
            f->band_clear = 0;
            set_scissor(0, 0, 479, 271);
            flat_sprite(0, 0, 480, 272, c);
        } else {
            set_scissor(0, 0, 479, 271);
            flat_sprite(map_x(f, (float)x), map_y(f, (float)y), map_x(f, (float)(x + w)), map_y(f, (float)(y + h)), c);
        }
    }
    check_list();
    return 1;
}

/* Copy a VRAM rectangle (written by the CPU, a GE copy or the software rasterizer) into the hi-res frames. */
static void mirror(int x, int y, int w, int h)
{
    for (int k = 0; k < 2; k++) {
        Frame *f = &g_frames[k];
        if (!g_hires || !f->used) continue;
        int ix0 = x > f->x ? x : f->x, iy0 = y > f->y ? y : f->y;
        int ix1 = x + w < f->x + f->w ? x + w : f->x + f->w, iy1 = y + h < f->y + f->h ? y + h : f->y + f->h;
        if (ix0 >= ix1 || iy0 >= iy1) continue;
        ge_begin();
        f->drawn = 1;
        bind(k);
        set_scissor(0, 0, 479, 271);
        set_blend(-1); set_atest(0); set_tex(1);
        S.stencil = -1;
        sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        sceGuTexMode(GU_PSM_5551, 0, 0, 0);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFlush();
        S.tfx = -1; S.tex_ptr = 0; S.tex_psm = -1;
        for (int s0 = ix0; s0 < ix1; s0 += 256) {
            int s1 = s0 + 256 < ix1 ? s0 + 256 : ix1, ax = s0 & ~7;
            sceGuTexImage(0, 512, 512, 1024, (void *)(EDRAM + (unsigned)iy0 * 2048 + (unsigned)ax * 2));
            HVtx *v = sceGuGetMemory(2 * (int)sizeof(HVtx));
            v[0] = (HVtx){(uint16_t)(s0 - ax), 0, 0xFFFFFFFFu, map_x(f, (float)s0), map_y(f, (float)iy0), 0};
            v[1] = (HVtx){(uint16_t)(s1 - ax), (uint16_t)(iy1 - iy0), 0xFFFFFFFFu, map_x(f, (float)s1), map_y(f, (float)iy1), 0};
            sceGuDrawArray(GU_SPRITES, HFMT, 2, 0, v);
        }
        sceGuTexFilter(GU_NEAREST, GU_NEAREST); S.filter = 0;
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        check_list();
    }
}

void ge_upload_done(int x, int y, int w, int h)
{
    if (x + w <= 1024 && y + h <= 512) mirror(x, y, w, h);
}

int ge_copy(int sx, int sy, int dx, int dy, int w, int h)
{
    if (!g_use_ge || sx + w > 1024 || dx + w > 1024 || sy + h > 512 || dy + h > 512) return 0;
    if (sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h) return 0;   /* overlapping: CPU */
    ge_begin();
    sceGuCopyImage(GU_PSM_5551, sx, sy, w, h, 1024, (void *)EDRAM, dx, dy, 1024, (void *)EDRAM);
    sceGuTexSync();
    touch(dx, dy, dx + w - 1, dy + h - 1, 1);
    mirror(dx, dy, w, h);
    check_list();
    return 1;
}

/* ---- presentation ------------------------------------------------------------------------------------------- */

/* Find or register the frame for a display start; -1 if hi-res is off. */
static int frame_for_display(int dx, int dy, int w, int h)
{
    if (!g_hires) return -1;
    for (int k = 0; k < 2; k++) {
        Frame *f = &g_frames[k];
        if (f->used && f->x == dx && f->y == dy && f->w == w && f->h == h) return k;
    }
    for (int k = 0; k < 2; k++)                  /* display mode changed: forget the old frames */
        if (g_frames[k].used && (g_frames[k].w != w || g_frames[k].h != h)) memset(g_frames, 0, sizeof g_frames);
    int k = !g_frames[0].used ? 0 : !g_frames[1].used ? 1 : (g_shown == 0 ? 1 : 0);
    Frame *f = &g_frames[k];
    *f = (Frame){dx, dy, w, h, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    f->kx2 = 363.0f / w; f->ox2 = 240.0f - (dx + w * 0.5f) * f->kx2;
    f->kx3 = 480.0f / w; f->ox3 = -dx * f->kx3;
    f->ky = 272.0f / h; f->oy = -dy * f->ky;
    g_frame_gen++;
    return k;
}

/* Fallback: scale the 1x display area from PS1 VRAM into buffer 2 (frames never drawn in hi-res, 24-bit, ...). */
static void present_scaled(int dx, int dy, int w, int h, int blank)
{
    const int ow = 363, oh = 272, ox = (480 - ow) / 2;
    ge_begin();
    bind(2);
    set_scissor(0, 0, 479, 271);
    sceGuDisable(GU_DITHER);
    S.dither = 0;
    set_blend(-1);
    set_atest(0);
    set_tex(0);
    flat_sprite(0, 0, 480, 272, 0xFF000000u);
    if (!blank) {
        int ax = dx & ~7, uo = dx - ax;
        set_tex(1);
        sceGuTexMode(GU_PSM_5551, 0, 0, 0);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFlush();
        S.tfx = -1; S.tex_ptr = 0; S.tex_psm = -1;
        /* GE textures are at most 512 wide: 640-pixel modes are drawn in strips */
        for (int s0 = 0; s0 < w; s0 += 256) {
            int s1 = s0 + 256 < w ? s0 + 256 : w;
            sceGuTexImage(0, 512, h > 256 ? 512 : 256, 1024,
                          (void *)(EDRAM + (unsigned)dy * 2048 + (unsigned)((ax + s0) & 1023) * 2));
            GeVtx *gv = sceGuGetMemory(2 * (int)sizeof(GeVtx));
            gv[0] = (GeVtx){(uint16_t)uo, 0, 0xFFFFFFFFu, (int16_t)(ox + s0 * ow / w), 0, 0, 0};
            gv[1] = (GeVtx){(uint16_t)(uo + s1 - s0), (uint16_t)h, 0xFFFFFFFFu, (int16_t)(ox + s1 * ow / w), (int16_t)oh, 0, 0};
            sceGuDrawArray(GU_SPRITES, VFMT, 2, 0, gv);
        }
        sceGuTexFilter(GU_NEAREST, GU_NEAREST); S.filter = 0;
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    }
    ge_sync();
}

void ge_present(int dx, int dy, int w, int h, int blank)
{
    PHASE_BEGIN(PH_PRESENT);
    int k = blank ? -1 : frame_for_display(dx, dy, w, h);
    int show;
    if (k >= 0 && g_frames[k].drawn) {
        ge_sync();
        show = k;
        g_frames[k ^ 1].band_clear = 1;
    } else {
        present_scaled(dx, dy, w, h, blank);
        show = 2;
    }
    if (show != g_shown)
        sceDisplaySetFrameBuf((void *)(EDRAM + FB_OFF(show)), 512, PSP_DISPLAY_PIXEL_FORMAT_5551,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
    g_shown = show;
    PHASE_END();
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

/* Debug self-test of the perspective path: the same triangle in through mode and in 3D mode with very different
 * w per vertex must cover the same pixels. */
static int draw_test_3d(int t)
{
    static const float tx[3] = {10, 300, 40}, ty[3] = {10, 60, 200}, tw[3][3] = {{1000, 1000, 1000}, {5, 2000, 300}, {2, 60000, 20}};
    {
        memset(g_vram, 0, 1024 * 2 * 256);
        ge_begin();
        set_tex(0); set_blend(-1); set_atest(0);
        sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        set_scissor(0, 0, 1023, 511);
        if (t == 0) {
            HVtx *v = sceGuGetMemory(3 * (int)sizeof(HVtx));
            for (int i = 0; i < 3; i++) v[i] = (HVtx){0, 0, 0xFF0000FFu, tx[i], ty[i], 0};
            sceGuDrawArray(GU_TRIANGLES, HFMT, 3, 0, v);
        } else {
            PVtx *v = sceGuGetMemory(3 * (int)sizeof(PVtx));
            for (int i = 0; i < 3; i++) { float w = tw[t - 1][i]; v[i] = (PVtx){0, 0, 0xFF0000FFu, tx[i] * w, ty[i] * w, w}; }
            sceGuDrawArray(GU_TRIANGLES, PFMT, 3, 0, v);
        }
        ge_sync();
        int hits = 0, x0 = 9999, x1 = -1, y0 = 9999, y1 = -1;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 512; x++)
                if (g_vram[y * 1024 + x] & 0x1F) {
                    hits++;
                    x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1; y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
                }
        logf_("ge selftest 3d variant %d case %d: %d pixels, bbox %d,%d-%d,%d\n", g_tvar, t, hits, x0, y0, x1, y1);
        return hits;
    }
}

/* Picks the first 3D-mode variant that covers the same pixels as through mode; none: perspective pass off. */
static void selftest_3d(void)
{
    g_tvar = 0;
    int ref = draw_test_3d(0), found = -1;
    for (int v = 0; v < NTVAR && found < 0; v++) {
        g_tvar = v;
        int ok = 1;
        for (int t = 1; t < 4 && ok; t++) {
            int h = draw_test_3d(t);
            if (h < ref - ref / 50 || h > ref + ref / 50) ok = 0;
        }
        if (ok) found = v;
    }
    if (found < 0) {
        g_tvar = 0;
        g_pgxp = 0;
        logf_("ge selftest 3d: no working variant - perspective-correct pass disabled\n");
    } else {
        g_tvar = found;
        logf_("ge selftest 3d: using variant %d\n", found);
    }
    memset(g_vram, 0, 1024 * 2 * 256);
}

void ge_init(void)
{
    /* Lines of the zero-initialized BSS (display list, vertex arena) may still be dirty in the data cache; written
     * back later they would overwrite what was written through the uncached mirror (PPSSPP has no cache). */
    sceKernelDcacheWritebackInvalidateAll();
    sceGuInit();
    selftest_3d();
    selftest();
    for (int i = 0; i < NSLOTS; i++) {
        g_slots[i].buf = memalign(64, 65536);
        if (!g_slots[i].buf) fatal(0, "out of memory (texture cache)");
        memset(g_slots[i].buf, 0, 65536);
        sceKernelDcacheWritebackInvalidateRange(g_slots[i].buf, 65536);
    }
    sceKernelDcacheWritebackInvalidateRange(g_clut_arena, sizeof g_clut_arena);
    g_shown = 2;
    sceDisplaySetFrameBuf((void *)(EDRAM + FB_OFF(2)), 512, PSP_DISPLAY_PIXEL_FORMAT_5551, PSP_DISPLAY_SETBUF_NEXTFRAME);
}

/* Debug: the display buffer shown last (480x272 inside a 512-pixel stride, 5551). */
const uint16_t *ge_display_buffer(void)
{
    return (const uint16_t *)(0x40000000u | (EDRAM + FB_OFF(g_shown)));
}
