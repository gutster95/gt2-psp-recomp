/* PS1 GPU front end: command parsing, VRAM transfers, display registers, presentation.
 * Primitives go to the GE renderer (ge.c); the software rasterizer (raster.c) is the fallback and reference.
 * The PS1 VRAM lives in PSP eDRAM (see ge.c); g_vram is its uncached CPU mirror. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <pspge.h>
#include "rt.h"
#include "gpu.h"
#include "raster.h"
#include "ge.h"

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
static int g_log_prims;

uint32_t gpu_draw_mode(void) { return g_draw_mode; }
void gpu_set_draw_mode(uint32_t m) { g_draw_mode = m; g_status = (g_status & ~0x7FFu) | (m & 0x7FF); }

void gpu_init(void)
{
    sceDisplaySetMode(0, 480, 272);
    memset((void *)0x44000000u, 0, 0x1CC000);       /* PS1 VRAM + three display buffers */
    raster_state_init();
    g_vram = (uint16_t *)0x44000000u;
    ge_init();
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

/* Screenshot on L+R+SELECT (real hardware debugging): snapN.bmp (the PSP screen) plus snapN_vram.bin (PS1 VRAM)
 * and snapN_screen.bin (PSP screen in the vram2png layout) next to the EBOOT. */
static void snapshot(void)
{
    static int n;
    char path[300];
    ge_sync();
    const uint16_t *fb = ge_display_buffer();
    snprintf(path, sizeof path, "%ssnap%d.bmp", g_basedir, n);
    FILE *f = fopen(path, "wb");
    if (f) {
        static uint8_t hdr[54];
        uint32_t img = 480 * 272 * 3;
        static const struct { int off; uint32_t v; } F[] = {   /* byte-wise: unaligned stores trap on the PSP */
            {2, 54 + 480 * 272 * 3}, {10, 54}, {14, 40}, {18, 480}, {22, 272}, {26, 1}, {28, 24}, {34, 480 * 272 * 3}};
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++)
            for (int b = 0; b < 4; b++) hdr[F[i].off + b] = (uint8_t)(F[i].v >> (8 * b));
        (void)img;
        fwrite(hdr, 1, 54, f);
        static uint8_t row[480 * 3];
        for (int y = 271; y >= 0; y--) {
            for (int x = 0; x < 480; x++) {
                uint16_t p = fb[y * 512 + x];
                row[x * 3 + 2] = (uint8_t)((p & 31) << 3);
                row[x * 3 + 1] = (uint8_t)(((p >> 5) & 31) << 3);
                row[x * 3 + 0] = (uint8_t)(((p >> 10) & 31) << 3);
            }
            fwrite(row, 1, sizeof row, f);
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%ssnap%d_vram.bin", g_basedir, n);
    f = fopen(path, "wb");
    if (f) { fwrite(g_vram, 2, 1024 * 512, f); fclose(f); }
    snprintf(path, sizeof path, "%ssnap%d_screen.bin", g_basedir, n);
    f = fopen(path, "wb");
    if (f) {
        static uint16_t r[1024];
        for (int y = 0; y < 512; y++) {
            memset(r, 0, sizeof r);
            if (y < 272) memcpy(r, fb + y * 512, 480 * 2);
            fwrite(r, 2, 1024, f);
        }
        fclose(f);
    }
    logf_("snapshot %d written (snap%d.bmp)\n", n, n);
    n++;
}

static void exec(const uint32_t *f)
{
    uint32_t c = f[0], op = c >> 24;
    if (op == 0x02) {                           /* fill rectangle */
        int x = f[1] & 0x3F0, y = (f[1] >> 16) & 0x1FF;
        int w = ((f[2] & 0x3FF) + 15) & ~15, h = (f[2] >> 16) & 0x1FF;
        uint16_t col = (uint16_t)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
        if (ge_fill(x, y, w, h, col)) return;
        g_sync_why = 1;
        ge_sync();
        ge_vram_written(x, y, w, h);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = col;
        return;
    }
    if ((op >> 5) == 5 || (op >> 5) == 6) {    /* start VRAM transfer */
        g_xx = f[1] & 0x3FF; g_xy = (f[1] >> 16) & 0x1FF;
        g_xw = f[2] & 0x3FF; g_xh = (f[2] >> 16) & 0x1FF;
        if (!g_xw) g_xw = 1024;
        if (!g_xh) g_xh = 512;
        g_xi = 0;
        g_xfer_dir = (op >> 5) == 5 ? 1 : 2;
        if (g_xfer_dir == 2) ge_note_read(g_xx, g_xy, g_xw, g_xh);
        g_sync_why = g_xfer_dir == 1 ? 2 : 3;
        ge_sync();                              /* the CPU accesses VRAM directly */
        if (g_xfer_dir == 1) ge_vram_written(g_xx, g_xy, g_xw, g_xh);
        return;
    }
    if ((op >> 5) == 4) {                       /* VRAM copy */
        int sx = f[1] & 0x3FF, sy = (f[1] >> 16) & 0x1FF;
        int dx = f[2] & 0x3FF, dy = (f[2] >> 16) & 0x1FF;
        int w = f[3] & 0x3FF, h = (f[3] >> 16) & 0x1FF;
        if (!w) w = 1024;
        if (!h) h = 512;
        ge_note_read(sx, sy, w, h);
        if (ge_copy(sx, sy, dx, dy, w, h)) return;
        g_sync_why = 4;
        ge_sync();
        ge_vram_written(dx, dy, w, h);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                g_vram[((dy + j) & 511) * 1024 + ((dx + i) & 1023)] = g_vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
        ge_upload_done(dx, dy, w, h);
        return;
    }
    switch (op) {
    case 0xE1:
        g_draw_mode = c;
        g_rs.texpage = c & 0xFFF;
        g_status = (g_status & ~0x7FFu) | (c & 0x7FF);
        return;
    case 0xE2:
        g_rs.tw_mask_x = c & 31; g_rs.tw_mask_y = (c >> 5) & 31;
        g_rs.tw_off_x = (c >> 10) & 31; g_rs.tw_off_y = (c >> 15) & 31;
        return;
    case 0xE3: g_rs.clip_x0 = c & 0x3FF; g_rs.clip_y0 = (c >> 10) & 0x1FF; return;
    case 0xE4: g_rs.clip_x1 = c & 0x3FF; g_rs.clip_y1 = (c >> 10) & 0x1FF; return;
    case 0xE5: g_rs.off_x = ((int32_t)(c << 21)) >> 21; g_rs.off_y = ((int32_t)(c << 10)) >> 21; return;
    case 0xE6:
        g_rs.mask_set = (c & 1) ? 0x8000 : 0;
        g_rs.mask_check = (c >> 1) & 1;
        g_status = (g_status & ~0x1800u) | ((c & 3) << 11);
        return;
    }
    if (op >= 0x20 && op < 0x80) {
        g_prims++;
        if (g_log_prims > 0) {
            g_log_prims--;
            logf_("prim %08x %08x %08x %08x %08x texpage %03x\n", (unsigned)f[0], (unsigned)f[1],
                  (unsigned)f[2], (unsigned)f[3], (unsigned)f[4], (unsigned)g_rs.texpage);
        }
        if (g_use_ge) {
            if (op < 0x40) ge_polygon(f);
            else if (op < 0x60) {
                int gour = op & 0x10;
                uint32_t c1 = gour ? f[2] : c;
                uint32_t p1 = gour ? f[3] : f[2];
                ge_line((int)(f[1] & 0xFFFF), (int)(f[1] >> 16), c & 0xFFFFFF,
                        (int)(p1 & 0xFFFF), (int)(p1 >> 16), c1 & 0xFFFFFF, (op & 2) != 0);
            } else ge_rect(f);
            return;
        }
        if (op < 0x40) raster_polygon(f);
        else if (op < 0x60) {
            int gour = op & 0x10;
            uint32_t c1 = gour ? f[2] : c;
            uint32_t p1 = gour ? f[3] : f[2];
            raster_line((int)(f[1] & 0xFFFF), (int)(f[1] >> 16), c & 0xFFFFFF,
                        (int)(p1 & 0xFFFF), (int)(p1 >> 16), c1 & 0xFFFFFF, (op & 2) != 0);
        } else raster_rect(f);
    }
}

/* Polyline: vertices (and colours for gouraud) stream in until the terminator word. */
static uint32_t g_pl_col, g_pl_xy, g_pl_pending_col;
static int g_pl_gour, g_pl_have_xy, g_pl_expect_col;

static void polyline_word(uint32_t v)
{
    if ((v & 0xF000F000u) == 0x50005000u && g_pl_have_xy) { g_polyline = 0; return; }
    if (g_pl_gour && g_pl_expect_col) { g_pl_pending_col = v & 0xFFFFFF; g_pl_expect_col = 0; return; }
    uint32_t col = g_pl_gour ? g_pl_pending_col : g_pl_col;
    if (g_pl_have_xy)
        (g_use_ge ? ge_line : raster_line)((int)(g_pl_xy & 0xFFFF), (int)(g_pl_xy >> 16), g_pl_col, (int)(v & 0xFFFF), (int)(v >> 16),
                    col, (g_fifo[0] >> 25) & 1);
    g_pl_xy = v;
    g_pl_col = col;
    g_pl_have_xy = 1;
    if (g_pl_gour) g_pl_expect_col = 1;
}

void gpu_gp0(uint32_t v)
{
    if (g_xfer_dir == 1) {
        vram_put((uint16_t)v);
        if (g_xi < g_xw * g_xh) vram_put((uint16_t)(v >> 16));
        if (g_xi >= g_xw * g_xh) { g_xfer_dir = 0; ge_upload_done(g_xx, g_xy, g_xw, g_xh); }
        return;
    }
    if (g_polyline) {
        polyline_word(v);
        return;
    }
    if (g_fifo_n == 0) {
        int n = cmd_len(v);
        if (n < 0) {
            g_polyline = 1;
            g_fifo[0] = v;
            g_pl_gour = (v >> 28) & 1;
            g_pl_col = g_pl_pending_col = v & 0xFFFFFF;
            g_pl_have_xy = 0;
            g_pl_expect_col = 0;
            g_prims++;
            return;
        }
        g_fifo_need = n;
    }
    g_fifo[g_fifo_n++] = v;
    if (g_fifo_n >= g_fifo_need) {
        exec(g_fifo);
        g_fifo_n = 0;
    }
}

/* A whole DMA packet: complete commands are executed straight from PS1 RAM; transfers, polylines and commands
 * split across packets go word by word through gpu_gp0(). */
void gpu_packet(const uint32_t *w, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        if (g_xfer_dir == 1 || g_polyline || g_fifo_n) { gpu_gp0(w[i++]); continue; }
        int len = cmd_len(w[i]);
        if (len < 0 || i + (uint32_t)len > n) { gpu_gp0(w[i++]); continue; }
        exec(&w[i]);
        i += (uint32_t)len;
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
    int disabled = g_status & 0x800000u;
    static int frame;
    if (g_use_ge) {
        ge_present((int)g_disp_x, (int)g_disp_y, w, h, disabled);
        goto stats;
    }
    uint16_t *fb = (uint16_t *)(0x44188000u);          /* uncached display buffer 0, 512-pixel stride */
    /* The PS1 picture is 4:3 whatever the resolution: scale it to 363x272, centred (nearest neighbour). */
    const int ow = ge_wide_now() ? 480 : 363, oh = 272, ox = (480 - ow) / 2;   /* 16:9: anamorphic picture */
    static int xmap[363];
    for (int x = 0; x < ow; x++) xmap[x] = (g_disp_x + x * w / ow) & 1023;
    for (int y = 0; y < oh; y++) {
        uint16_t *dst = fb + y * 512;
        if (disabled) { memset(dst, 0, 480 * 2); continue; }
        const uint16_t *src = g_vram + ((g_disp_y + y * h / oh) & 511) * 1024;
        memset(dst, 0, ox * 2);
        for (int x = 0; x < ow; x++) dst[ox + x] = src[xmap[x]] | 0x8000;
        memset(dst + ox + ow, 0, (480 - ox - ow) * 2);
    }
    sceDisplaySetFrameBuf((void *)0x04188000, 512, PSP_DISPLAY_PIXEL_FORMAT_5551, PSP_DISPLAY_SETBUF_NEXTFRAME);
stats:
    static int dumps = -1, dumps_from;                       /* debug snapshots only if dumps.txt exists next to the EBOOT */
    if (dumps < 0) {
        char path[300];
        snprintf(path, sizeof path, "%sdumps.txt", g_basedir);
        FILE *f = fopen(path, "r");
        dumps = f != 0;
        if (f) {                                 /* optional content: first frame; then screens only (USB is slow) */
            if (fscanf(f, "%d", &dumps_from) != 1) dumps_from = 0;
            fclose(f);
        }
    }
    if (dumps && (frame >= (dumps_from ? (dumps_from < 0 ? -dumps_from : dumps_from) : 600) && frame % (dumps_from < 0 ? 30 : 150) == 0 && frame <= 9000)) {
        char path[300];
        snprintf(path, sizeof path, "%svram%d.bin", g_basedir, frame);
        ge_sync();
        FILE *f = dumps_from > 0 ? 0 : fopen(path, "wb");   /* negative start frame: VRAM dumps too */
        if (f) { fwrite(g_vram, 2, 1024 * 512, f); fclose(f); }
        if (g_use_ge) {                          /* the PSP screen, as a 512x512 "VRAM" image for vram2png */
            snprintf(path, sizeof path, "%sscreen%d.bin", g_basedir, frame);
            f = fopen(path, "wb");
            if (f) {
                static uint16_t row[1024];
                const uint16_t *fb = ge_display_buffer();
                for (int y = 0; y < 512; y++) {
                    memset(row, 0, sizeof row);
                    if (y < 272) memcpy(row, fb + y * 512, 480 * 2);
                    fwrite(row, 2, 1024, f);
                }
                fclose(f);
            }
        }
    }
    {
        static int combo_down;
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned want = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT;
        int down = (pad.Buttons & want) == want;
        if (down && !combo_down) snapshot();
        combo_down = down;
    }
    if (frame == 100 || frame == 1000 || frame == 2000) g_log_prims = 24;
    {   /* settings.txt autoexit=N: quit after N seconds (PSPLink profiling runs) */
        extern int g_autoexit;
        static uint64_t start;
        if (!start) start = now_us();
        if (g_autoexit > 0 && now_us() - start > (uint64_t)g_autoexit * 1000000u) {
            extern void quit_game(const char *why);
            quit_game("autoexit");
        }
    }
    if (++frame % 120 == 0) {
        extern uint32_t g_gpu_lists;
        extern uint64_t g_gpu_us;
        static uint32_t last_lists, last_prims;
        static uint64_t last_t, last_gpu;
        uint64_t t = now_us();
        uint32_t dt_ms = last_t ? (uint32_t)((t - last_t) / 1000) : 0;
        logf_("gpu: vblank %d | last 120 vblanks took %u ms (display lists %u ms), %u display lists, %u prims | "
              "disp %u,%u mode %02x\n", frame, (unsigned)dt_ms, (unsigned)((g_gpu_us - last_gpu) / 1000),
              (unsigned)(g_gpu_lists - last_lists), (unsigned)(g_prims - last_prims),
              (unsigned)g_disp_x, (unsigned)g_disp_y, (unsigned)g_disp_mode);
        extern uint32_t g_ge_prims, g_ge_sw_prims, g_ge_tex_uploads, g_ge_syncs, g_ge_pgxp;
        logf_("ge: %u prims (%u perspective-correct), %u software, %u texture page copies, %u syncs (totals)\n",
              (unsigned)g_ge_prims, (unsigned)g_ge_pgxp, (unsigned)g_ge_sw_prims, (unsigned)g_ge_tex_uploads,
              (unsigned)g_ge_syncs);
        {
            extern uint32_t g_sync_stats[16];
            static uint32_t last[16];
            char buf[160];
            int n = 0;
            for (int i = 0; i < 11; i++) {
                n += snprintf(buf + n, sizeof buf - n, " %u", (unsigned)(g_sync_stats[i] - last[i]));
                last[i] = g_sync_stats[i];
            }
            logf_("ge syncs by reason (other fill upload readback copy list clut-drawn clut-full sw vtx present):%s\n", buf);
        }
        extern uint32_t g_wide_clipped, g_wide_tris;
        logf_("widescreen clipper: %u polygons, %u triangles (totals)\n", (unsigned)g_wide_clipped,
              (unsigned)g_wide_tris);
        last_lists = g_gpu_lists;
        last_prims = g_prims;
        last_gpu = g_gpu_us;
        last_t = t;
    }
    g_prims_frame = g_prims;
}
