/* Widescreen support in the game's own renderer: near-plane clipping for the track/scenery renderer.
 *
 * GT2's renderer (overlay 0, 0x80018d40) discards a polygon when the GTE reports an error, i.e. a vertex is too
 * close to or behind the camera. In 4:3 those polygons are outside the picture; with the wider 16:9 view the near
 * ground at the left and right edges consists of exactly such polygons. The recompiler inserts a call to
 * wide_clip_track() at each discard: the polygon is transformed to view space, clipped against the near plane and a
 * guard band (PS1 GPU coordinates are 11-bit, polygons at most 1023x511), projected like the GTE (with the
 * widescreen X scale) and emitted as triangles of the matching type, linked into the same ordering-table slot.
 * Packets live in a ring buffer in the otherwise unused kernel RAM (0x80001000-0x8000BFFF). */
#include <string.h>
#include "rt.h"
#include "ge.h"

void gte_view_space(PsxCpu *c, int x, int y, int z, int32_t out[3]);
void gte_screen_params(PsxCpu *c, int32_t *h, int32_t *ofx, int32_t *ofy);

uint32_t g_wide_clipped, g_wide_tris;   /* statistics */
int g_near_clip = 0;                     /* settings.txt: near_clip */

/* polygon kinds of the 8 loops: F3 F4 G3 G4 FT3 FT4 GT3 GT4 */
typedef struct { int n, stride, gouraud, textured; } Kind;
static const Kind KINDS[8] = {
    {3, 4, 0, 0}, {4, 4, 0, 0}, {3, 8, 1, 0}, {4, 8, 1, 0},
    {3, 8, 0, 1}, {4, 8, 0, 1}, {3, 12, 1, 1}, {4, 12, 1, 1},
};

typedef struct { float x, y, z, r, g, b, u, v; } V;

#define RING_LO 0x80001000u
#define RING_HI 0x8000C000u
static uint32_t g_ring = RING_LO;

static uint32_t alloc_pkt(uint32_t bytes)
{
    if (g_ring + bytes > RING_HI) g_ring = RING_LO;
    uint32_t p = g_ring;
    g_ring += bytes;
    return p;
}

static int clip(V *in, int n, V *out, float a, float b, float cz, float e)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        V *p = &in[i], *q = &in[(i + 1) % n];
        float dp = a * p->x + b * p->y + cz * p->z + e, dq = a * q->x + b * q->y + cz * q->z + e;
        if (dp >= 0) out[m++] = *p;
        if ((dp >= 0) != (dq >= 0)) {
            float t = dp / (dp - dq);
            V *o = &out[m++];
            o->x = p->x + (q->x - p->x) * t; o->y = p->y + (q->y - p->y) * t; o->z = p->z + (q->z - p->z) * t;
            o->r = p->r + (q->r - p->r) * t; o->g = p->g + (q->g - p->g) * t; o->b = p->b + (q->b - p->b) * t;
            o->u = p->u + (q->u - p->u) * t; o->v = p->v + (q->v - p->v) * t;
        }
    }
    return m;
}

static inline uint32_t clamp255(float f) { int i = (int)(f + 0.5f); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); }

void wide_clip_track(PsxCpu *c, int kind, uint32_t entry, uint32_t pkt, uint32_t ot)
{
    if (!g_near_clip) return;
    const Kind *k = &KINDS[kind & 7];
    uint32_t w0 = mem_r32(entry), w1 = mem_r32(entry + 4);
    uint32_t idx[4] = {w0 & 0xFFF, (w0 >> 12) & 0xFFF, 0, 0};
    if (k->n == 3) idx[2] = (w1 >> 12) & 0xFFF;
    else { idx[2] = w1 & 0xFFF; idx[3] = (w1 >> 12) & 0xFFF; }
    uint32_t vbase = mem_r32(0x1F800070u);
    uint32_t code = mem_r8(pkt + 7) & ~0x08u;          /* triangle variant of the packet type */
    uint32_t flat = mem_r32(pkt + 4) & 0xFFFFFF;
    uint32_t clut = 0, tpage = 0;

    /* polygon loop: triangle P0 P1 P2; quad P0 P1 P3 P2 (PS1 quads are 0-1-2 / 1-3-2) */
    static const int order3[3] = {0, 1, 2}, order4[4] = {0, 1, 3, 2};
    const int *order = k->n == 3 ? order3 : order4;
    V a[16], b[16];
    for (int j = 0; j < k->n; j++) {
        int i = order[j];
        uint32_t va = vbase + idx[i] * 8;
        int32_t view[3];
        gte_view_space(c, (int16_t)mem_r16(va), (int16_t)mem_r16(va + 2), (int16_t)mem_r16(va + 4), view);
        V *p = &a[j];
        p->x = (float)view[0]; p->y = (float)view[1]; p->z = (float)view[2];
        uint32_t col = k->gouraud ? mem_r32(pkt + 4 + k->stride * i) & 0xFFFFFF : flat;
        p->r = (float)(col & 0xFF); p->g = (float)((col >> 8) & 0xFF); p->b = (float)((col >> 16) & 0xFF);
        p->u = p->v = 0;
        if (k->textured) {
            uint32_t uv = mem_r32(pkt + (k->gouraud ? 12 : 12) + k->stride * i);
            p->u = (float)(uv & 0xFF); p->v = (float)((uv >> 8) & 0xFF);
            if (i == 0) clut = uv >> 16;
            if (i == 1) tpage = uv >> 16;
        }
    }

    int32_t h, ofx16, ofy16;
    gte_screen_params(c, &h, &ofx16, &ofy16);
    float H = (float)h, kx = H * (g_widescreen ? 0.75f : 1.0f), ofx = ofx16 / 65536.0f, ofy = ofy16 / 65536.0f;
    float zn = H * 0.5f + 4.0f;
    const float GX = 495.0f, GY = 250.0f;          /* guard band around the screen centre (11-bit, <1024x512) */
    int n = k->n;
    n = clip(a, n, b, 0, 0, 1, -zn);                 /* z >= zn */
    n = clip(b, n, a, kx, 0, GX, 0);                 /* x*kx/z >= -GX */
    n = clip(a, n, b, -kx, 0, GX, 0);                /* x*kx/z <= GX */
    n = clip(b, n, a, 0, H, GY, 0);
    n = clip(a, n, b, 0, -H, GY, 0);
    if (n < 3) return;
    g_wide_clipped++;

    uint32_t xy[16];
    for (int i = 0; i < n; i++) {
        float fx = ofx + b[i].x * kx / b[i].z, fy = ofy + b[i].y * H / b[i].z;
        int sx = (int)(fx + (fx >= 0 ? 0.5f : -0.5f)), sy = (int)(fy + (fy >= 0 ? 0.5f : -0.5f));
        xy[i] = ((uint32_t)(sy & 0xFFFF) << 16) | (uint32_t)(sx & 0xFFFF);
        ge_mark_3d_precise(xy[i], fx, fy, b[i].z);
    }
    uint32_t slot = ot + ((int32_t)w0 > 0 ? 4 : 0);
    int words = k->textured ? (k->gouraud ? 9 : 7) : (k->gouraud ? 6 : 4);
    for (int t = 1; t + 1 < n; t++) {                /* triangle fan */
        const int vi[3] = {0, t, t + 1};
        uint32_t p = alloc_pkt(4 + words * 4);
        uint32_t o = p + 4;
        for (int j = 0; j < 3; j++) {
            const V *v = &b[vi[j]];
            uint32_t col = clamp255(v->r) | (clamp255(v->g) << 8) | (clamp255(v->b) << 16);
            if (j == 0 || k->gouraud) { mem_w32(o, col | (j == 0 ? code << 24 : 0)); o += 4; }
            mem_w32(o, xy[vi[j]]); o += 4;
            if (k->textured) {
                uint32_t hi = j == 0 ? clut : j == 1 ? tpage : 0;
                mem_w32(o, (hi << 16) | (clamp255(v->v) << 8) | clamp255(v->u)); o += 4;
            }
        }
        uint32_t head = mem_r32(slot);
        mem_w32(p, ((uint32_t)words << 24) | (head & 0xFFFFFF));
        mem_w32(slot, (head & 0xFF000000u) | (p & 0xFFFFFF));
        g_wide_tris++;
    }
}
