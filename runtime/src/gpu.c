/* PS1 GPU front end: command parsing, VRAM transfers, display registers, presentation.
 * Primitive rasterisation (to be mapped onto the PSP GE) is not implemented yet; primitives are counted. */
#include <malloc.h>
#include <string.h>
#include <pspdisplay.h>
#include <pspge.h>
#include "rt.h"
#include "gpu.h"

uint16_t *g_vram;

static uint32_t g_fifo[16];
static int g_fifo_n, g_fifo_need;
static int g_polyline;

/* VRAM transfer state (GP0 A0 / C0) */
static int g_xfer_dir;               /* 0 none, 1 CPU->VRAM, 2 VRAM->CPU */
static int g_xx, g_xy, g_xw, g_xh, g_xi;

static uint32_t g_disp_x, g_disp_y, g_disp_mode, g_hrange = 0xC60260, g_vrange = 0x3FC10;
static uint32_t g_status = 0x14802000u;
static uint32_t g_draw_mode;
static uint32_t g_prims, g_prims_frame;

void gpu_init(void)
{
    sceDisplaySetMode(0, 480, 272);
    memset((void *)0x44000000u, 0, 512 * 272 * 2);
    sceDisplaySetFrameBuf((void *)0x04000000, 512, PSP_DISPLAY_PIXEL_FORMAT_5551, PSP_DISPLAY_SETBUF_NEXTFRAME);
    g_vram = memalign(64, 1024 * 512 * 2);
    memset(g_vram, 0, 1024 * 512 * 2);
}

static int cmd_len(uint32_t cmd)
{
    uint32_t op = cmd >> 24;
    switch (op >> 5) {
    case 1: {                                   /* polygon */
        int verts = (op & 8) ? 4 : 3;
        int per = 1 + ((op & 4) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
        return 1 + verts * per - ((op & 0x10) ? 1 : 0);
    }
    case 2:                                     /* line */
        if (op & 8) return -1;                  /* polyline: until terminator */
        return (op & 0x10) ? 4 : 3;
    case 3: {                                   /* rectangle */
        int n = 2 + ((op & 4) ? 1 : 0);
        if (((op >> 3) & 3) == 0) n++;
        return n;
    }
    case 4: return 4;                           /* VRAM -> VRAM copy */
    case 5: return 3;                           /* CPU -> VRAM */
    case 6: return 3;                           /* VRAM -> CPU */
    }
    if (op == 0x02) return 3;                   /* fill */
    return 1;
}

static void vram_put(uint16_t px)
{
    int x = (g_xx + g_xi % g_xw) & 1023, y = (g_xy + g_xi / g_xw) & 511;
    g_vram[y * 1024 + x] = px;
    g_xi++;
}

static void exec(void)
{
    uint32_t c = g_fifo[0], op = c >> 24;
    if (op == 0x02) {                           /* fill rectangle */
        int x = g_fifo[1] & 0x3F0, y = (g_fifo[1] >> 16) & 0x1FF;
        int w = ((g_fifo[2] & 0x3FF) + 15) & ~15, h = (g_fifo[2] >> 16) & 0x1FF;
        uint16_t col = (uint16_t)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = col;
        return;
    }
    if ((op >> 5) == 5 || (op >> 5) == 6) {    /* start VRAM transfer */
        g_xx = g_fifo[1] & 0x3FF; g_xy = (g_fifo[1] >> 16) & 0x1FF;
        g_xw = g_fifo[2] & 0x3FF; g_xh = (g_fifo[2] >> 16) & 0x1FF;
        if (!g_xw) g_xw = 1024;
        if (!g_xh) g_xh = 512;
        g_xi = 0;
        g_xfer_dir = (op >> 5) == 5 ? 1 : 2;
        return;
    }
    if ((op >> 5) == 4) {                       /* VRAM copy */
        int sx = g_fifo[1] & 0x3FF, sy = (g_fifo[1] >> 16) & 0x1FF;
        int dx = g_fifo[2] & 0x3FF, dy = (g_fifo[2] >> 16) & 0x1FF;
        int w = g_fifo[3] & 0x3FF, h = (g_fifo[3] >> 16) & 0x1FF;
        if (!w) w = 1024;
        if (!h) h = 512;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                g_vram[((dy + j) & 511) * 1024 + ((dx + i) & 1023)] = g_vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
        return;
    }
    if (op >= 0xE1 && op <= 0xE6) {
        if (op == 0xE1) { g_draw_mode = c; g_status = (g_status & ~0x7FFu) | (c & 0x7FF); }
        return;
    }
    if (op >= 0x20 && op < 0x80) g_prims++;
}

void gpu_gp0(uint32_t v)
{
    if (g_xfer_dir == 1) {
        vram_put((uint16_t)v);
        if (g_xi < g_xw * g_xh) vram_put((uint16_t)(v >> 16));
        if (g_xi >= g_xw * g_xh) g_xfer_dir = 0;
        return;
    }
    if (g_polyline) {
        if ((v & 0xF000F000u) == 0x50005000u && g_fifo_n >= 3) { g_polyline = 0; g_fifo_n = 0; g_prims++; }
        else g_fifo_n++;
        return;
    }
    if (g_fifo_n == 0) {
        int n = cmd_len(v);
        if (n < 0) { g_polyline = 1; g_fifo_n = 1; g_fifo[0] = v; return; }
        g_fifo_need = n;
    }
    g_fifo[g_fifo_n++] = v;
    if (g_fifo_n >= g_fifo_need) {
        exec();
        g_fifo_n = 0;
    }
}

uint32_t gpu_read(void)
{
    if (g_xfer_dir == 2) {
        uint32_t lo, hi;
        int x = (g_xx + g_xi % g_xw) & 1023, y = (g_xy + g_xi / g_xw) & 511;
        lo = g_vram[y * 1024 + x]; g_xi++;
        x = (g_xx + g_xi % g_xw) & 1023; y = (g_xy + g_xi / g_xw) & 511;
        hi = g_vram[y * 1024 + x]; g_xi++;
        if (g_xi >= g_xw * g_xh) g_xfer_dir = 0;
        return lo | (hi << 16);
    }
    return 0;
}

void gpu_gp1(uint32_t v)
{
    uint32_t op = (v >> 24) & 0x3F;
    switch (op) {
    case 0x00: g_fifo_n = 0; g_xfer_dir = 0; g_polyline = 0; g_status = 0x14802000u; break;
    case 0x01: g_fifo_n = 0; g_xfer_dir = 0; g_polyline = 0; break;
    case 0x03: g_status = (g_status & ~0x800000u) | ((v & 1) << 23); break;   /* display enable (1=off) */
    case 0x04: g_status = (g_status & ~0x60000000u) | ((v & 3) << 29); break;
    case 0x05: g_disp_x = v & 0x3FE; g_disp_y = (v >> 10) & 0x1FF; break;
    case 0x06: g_hrange = v & 0xFFFFFF; break;
    case 0x07: g_vrange = v & 0xFFFFF; break;
    case 0x08:
        g_disp_mode = v & 0xFF;
        g_status = (g_status & ~0x7F4000u) | ((v & 0x3F) << 17) | ((v & 0x40) << 10) | ((v & 0x80) << 7);
        break;
    }
}

uint32_t gpu_status(void)
{
    /* Always ready: ready-for-command, ready-to-send-VRAM, ready-for-DMA; DMA request mirrors direction. */
    return (g_status & ~0x1E000000u) | 0x1C000000u | (g_xfer_dir == 2 ? 0x08000000u : 0);
}

void gpu_present(void)
{
    static const int hres[4] = {256, 320, 512, 640};
    int w = (g_disp_mode & 0x40) ? 368 : hres[g_disp_mode & 3];
    int h = (g_disp_mode & 0x04) && (g_disp_mode & 0x20) ? 480 : 240;
    uint16_t *fb = (uint16_t *)(0x44000000u);       /* uncached VRAM, 512-pixel stride */
    int ow = w < 480 ? w : 480, oh = h < 272 ? h : 272;
    int ox = (480 - ow) / 2, oy = (272 - oh) / 2;
    int disabled = g_status & 0x800000u;
    for (int y = 0; y < 272; y++) {
        uint16_t *dst = fb + y * 512;
        if (disabled || y < oy || y >= oy + oh) { memset(dst, 0, 480 * 2); continue; }
        const uint16_t *src = g_vram + ((g_disp_y + y - oy) & 511) * 1024;
        memset(dst, 0, ox * 2);
        for (int x = 0; x < ow; x++) dst[ox + x] = src[(g_disp_x + x) & 1023] | 0x8000;
        memset(dst + ox + ow, 0, (480 - ox - ow) * 2);
    }
    sceDisplaySetFrameBuf((void *)0x04000000, 512, PSP_DISPLAY_PIXEL_FORMAT_5551, PSP_DISPLAY_SETBUF_NEXTFRAME);
    static int frame;
    if (++frame % 120 == 0)
        logf_("gpu: frame %d prims/frame %u disp %u,%u mode %02x status %08x\n", frame,
              (unsigned)(g_prims - g_prims_frame) / 1, (unsigned)g_disp_x, (unsigned)g_disp_y,
              (unsigned)g_disp_mode, (unsigned)g_status);
    g_prims_frame = g_prims;
}
