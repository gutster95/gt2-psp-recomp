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
#include "prof.h"
#include "raster.h"
#include "ge.h"
#include "me.h"
void ge_push(int x, int y, int w, int h);
void ge_pull(int x, int y, int w, int h);

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

/* CPU -> VRAM transfer: n words of pixel pairs; returns the words taken (the transfer may end inside the packet).
 * Column/row counters instead of a division per pixel, and whole packets instead of one gpu_gp0 call per word:
 * on the Media Engine (uncached VRAM shadow) the per-word path was ~6% of its time in races. */
static uint32_t xfer_put(const uint32_t *w, uint32_t n)
{
    int total = g_xw * g_xh, col = g_xi % g_xw, row = g_xi / g_xw;
    uint32_t i = 0;
    while (i < n && g_xi < total) {
        uint32_t v = w[i++];
        for (int h = 0; h < 2 && g_xi < total; h++, v >>= 16) {
            g_vram[((g_xy + row) & 511) * 1024 + ((g_xx + col) & 1023)] = (uint16_t)v;
            g_xi++;
            if (++col == g_xw) { col = 0; row++; }
        }
    }
    if (g_xi >= total) { g_xfer_dir = 0; ge_upload_done(g_xx, g_xy, g_xw, g_xh); }
    return i;
}

/* Screenshot on L+R+SELECT: snapN.bmp (the PSP screen) next to the EBOOT, N = the first free number. With
 * snapvram.txt there also snapN_vram.bin (PS1 VRAM) and snapN_screen.bin (vram2png layout). Every write goes through
 * a buffer in RAM: sceIoWrite straight from eDRAM (the old VRAM dump) hung the whole PSP - the memory stick driver
 * transfers by DMA from the source buffer. */
/* PS1 VRAM to / from a file through a RAM buffer (never let sceIoWrite/sceIoRead DMA from or into eDRAM) */
int vram_fwrite(FILE *f)
{
    static uint16_t r[1024];
    for (int y = 0; y < 512; y++) {
        memcpy(r, g_vram + y * 1024, sizeof r);
        if (fwrite(r, 2, 1024, f) != 1024) return 0;
    }
    return 1;
}
int vram_fread(FILE *f)
{
    static uint16_t r[1024];
    for (int y = 0; y < 512; y++) {
        if (fread(r, 2, 1024, f) != 1024) return 0;
        memcpy(g_vram + y * 1024, r, sizeof r);
    }
    return 1;
}

static int file_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

void gpu_snapshot(void)
{
    static int n;
    static uint16_t r[1024];
    char path[300];
    ge_sync();
    const uint16_t *fb = ge_display_buffer();
    for (;; n++) {
        snprintf(path, sizeof path, "%ssnap%d.bmp", g_basedir, n);
        if (!file_exists(path) || n >= 999) break;
    }
    /* the whole BMP is built in RAM and written with one call: row by row it took seconds over PSPLink's USB */
    static uint8_t bmp[54 + 480 * 272 * 3];
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t *hdr = bmp;
        static const struct { int off; uint32_t v; } F[] = {   /* byte-wise: unaligned stores trap on the PSP */
            {2, 54 + 480 * 272 * 3}, {10, 54}, {14, 40}, {18, 480}, {22, 272}, {26, 1}, {28, 24}, {34, 480 * 272 * 3}};
        memset(hdr, 0, 54);
        hdr[0] = 'B'; hdr[1] = 'M';
        for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++)
            for (int b = 0; b < 4; b++) hdr[F[i].off + b] = (uint8_t)(F[i].v >> (8 * b));
        for (int y = 271; y >= 0; y--) {
            uint8_t *row = bmp + 54 + (271 - y) * 480 * 3;
            for (int x = 0; x < 480; x++) {
                uint16_t p = fb[y * 512 + x];
                row[x * 3 + 2] = (uint8_t)((p & 31) << 3);
                row[x * 3 + 1] = (uint8_t)(((p >> 5) & 31) << 3);
                row[x * 3 + 0] = (uint8_t)(((p >> 10) & 31) << 3);
            }
        }
        fwrite(bmp, 1, sizeof bmp, f);
        fclose(f);
    }
    char flag[300];
    snprintf(flag, sizeof flag, "%ssnapvram.txt", g_basedir);
    if (file_exists(flag)) {
        snprintf(path, sizeof path, "%ssnap%d_vram.bin", g_basedir, n);
        f = fopen(path, "wb");
        if (f) {
            vram_fwrite(f);
            fclose(f);
        }
        snprintf(path, sizeof path, "%ssnap%d_screen.bin", g_basedir, n);
        f = fopen(path, "wb");
        if (f) {
            for (int y = 0; y < 512; y++) {
                memset(r, 0, sizeof r);
                if (y < 272) memcpy(r, fb + y * 512, 480 * 2);
                fwrite(r, 2, 1024, f);
            }
            fclose(f);
        }
    }
    extern int g_gedump;
    g_gedump = 1;                             /* and the next big display list to geframe.bin (io.c gpu_dump) */
    logf_("snapshot %d written (snap%d.bmp)\n", n, n);
    n++;
}

extern int g_bench_stage;
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
        ge_push(x, y, w, h);
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
        if (g_xfer_dir == 2) ge_pull(g_xx, g_xy, g_xw, g_xh);   /* Media Engine: VRAM shadow (ge.c) */
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
        ge_pull(sx, sy, w, h);
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
            if (g_bench_stage == 15) return;     /* benchmark: dispatch only */
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
    if (g_xfer_dir == 1) { xfer_put(&v, 1); return; }
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
        if (g_xfer_dir == 1) { i += xfer_put(w + i, n - i); continue; }
        if (g_polyline || g_fifo_n) { gpu_gp0(w[i++]); continue; }
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


/* Presentation of the PS1 display area and the GPU statistics (on the Media Engine with me_gpu, see me.c) */
void gpu_present_core(void)
{
    static const int hres[4] = {256, 320, 512, 640};
    int w = (g_disp_mode & 0x40) ? 368 : hres[g_disp_mode & 3];
    {   /* visible width from the horizontal display range GP1(06) (in GPU clocks, nocash psx-spx): GT2's menus run in
         * the 368 mode with a range of ~352 pixels; showing all 368 put VRAM garbage at the right edge */
        static const int div[4] = {10, 8, 5, 4};
        int d = (g_disp_mode & 0x40) ? 7 : div[g_disp_mode & 3];
        int x1 = (int)(g_hrange & 0xFFF), x2 = (int)((g_hrange >> 12) & 0xFFF);
        int vis = ((x2 - x1) / d + 2) & ~3;
        static uint32_t logged = 0xFFFFFFFFu;
        if (g_hrange != logged) { logged = g_hrange; logf_("display range x %d-%d: %d of %d pixels\n", x1, x2, vis, w); }
        if (vis >= 64 && vis < w) w = vis;
    }
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
    if (frame == 100 || frame == 1000 || frame == 2000) g_log_prims = 24;
    if (++frame % 120 == 0) {
        extern uint32_t g_gpu_lists;
        extern uint64_t g_gpu_us;
        uint32_t gpu_lists = ME_UC(g_gpu_lists);
        uint64_t gpu_us = ME_UC(g_gpu_us);
        static uint32_t last_lists, last_prims;
        static uint64_t last_t, last_gpu;
        uint64_t t = now_us();
        uint32_t dt_ms = last_t ? (uint32_t)((t - last_t) / 1000) : 0;
        logf_("gpu: vblank %d | last 120 vblanks took %u ms (display lists %u ms), %u display lists, %u prims | "
              "disp %u,%u mode %02x status %08x\n", frame, (unsigned)dt_ms, (unsigned)((gpu_us - last_gpu) / 1000),
              (unsigned)(gpu_lists - last_lists), (unsigned)(g_prims - last_prims),
              (unsigned)g_disp_x, (unsigned)g_disp_y, (unsigned)g_disp_mode, (unsigned)g_status);
        extern uint32_t g_ge_prims, g_ge_sw_prims, g_ge_tex_uploads, g_ge_syncs, g_ge_pgxp;
        logf_("ge: %u prims (%u perspective-correct), %u software, %u texture page copies, %u syncs (totals)\n",
              (unsigned)g_ge_prims, (unsigned)g_ge_pgxp, (unsigned)g_ge_sw_prims, (unsigned)g_ge_tex_uploads,
              (unsigned)g_ge_syncs);
        {
            extern uint32_t g_sync_stats[16], g_sync_us[16];
            static uint32_t last[16];
            uint32_t d[16];
            for (int i = 0; i < 16; i++) { d[i] = g_sync_stats[i] - last[i]; last[i] = g_sync_stats[i]; }
            logf_("ge syncs 11-15 (kick-inflight pull present-onscreen present-sync pending-show): %u %u %u %u %u; "
                  "ME wait ms by reason 0-15:", (unsigned)d[11], (unsigned)d[12], (unsigned)d[13], (unsigned)d[14],
                  (unsigned)d[15]);
            for (int i = 0; i < 16; i++) { logf_(" %u", (unsigned)(g_sync_us[i] / 1000)); g_sync_us[i] = 0; }
            logf_("\n");
            logf_("ge syncs by reason (other fill upload readback copy list clut-drawn clut-full sw vtx present): "
                  "%u %u %u %u %u %u %u %u %u %u %u\n", (unsigned)d[0], (unsigned)d[1], (unsigned)d[2], (unsigned)d[3],
                  (unsigned)d[4], (unsigned)d[5], (unsigned)d[6], (unsigned)d[7], (unsigned)d[8], (unsigned)d[9],
                  (unsigned)d[10]);
        }
        {
            extern uint64_t g_wait_us[3]; extern uint32_t g_wait_n[3]; extern uint32_t g_vtop_max, g_list_max;
            logf_("vertex arena: max %u bytes per batch, display list max %u bytes\n", (unsigned)g_vtop_max,
                  (unsigned)g_list_max);
            g_vtop_max = g_list_max = 0;
            extern uint32_t g_hud_draws, g_hud_prims, g_hud_us;
            logf_("hud: %u draws, %u prims, %u us\n", (unsigned)g_hud_draws, (unsigned)g_hud_prims, (unsigned)g_hud_us);
            g_hud_draws = g_hud_prims = g_hud_us = 0;
            logf_("waits: scanout %u x %u ms, present %u x %u ms, ge sync %u x %u ms\n",
                  (unsigned)g_wait_n[0], (unsigned)(g_wait_us[0] / 1000), (unsigned)g_wait_n[1], (unsigned)(g_wait_us[1] / 1000),
                  (unsigned)g_wait_n[2], (unsigned)(g_wait_us[2] / 1000));
            memset(g_wait_us, 0, sizeof g_wait_us); memset(g_wait_n, 0, sizeof g_wait_n);
        }
        extern uint32_t g_wide_clipped, g_wide_tris;
        logf_("widescreen clipper: %u polygons, %u triangles (totals)\n", (unsigned)ME_UC(g_wide_clipped),
              (unsigned)ME_UC(g_wide_tris));
        last_lists = gpu_lists;
        last_prims = g_prims;
        last_gpu = gpu_us;
        last_t = t;
    }
    g_prims_frame = g_prims;
}

/* ---- DMA2 linked list (ordering table) walk, moved from io.c: runs on the ME with me_gpu ---- */
uint32_t g_ot_entries, g_ot_empty;
extern int g_bench_stage;
extern int g_hud_mode;
void ge_hud_begin(void);
int g_walklog;                 /* settings.txt walklog=<vblank>: log every packet of one race frame (debug) */

/* Race HUD (16:9, see ge.c hud_squeeze): the race's ordering table (ClearOTagR, walked from its top entry madr
 * downwards) has 8515 entries in four layers, each starting with a drawing-area packet; the last layer, linked from
 * OT[3] = madr - 0x850C, is the HUD. Found with settings.txt walklog=<vblank> (one summary line per list). */
#define RACE_HUD_OT 0x850Cu

void gpu_walk(uint32_t madr)
{
    PHASE_SCOPE(PH_GPU);
    uint32_t a = madr, guard = 0, prev = 0;
    uint32_t hud_ot = (g_widescreen && ME_UC(g_cur_overlay) == 0) ? madr - RACE_HUD_OT : 1;
    int wl = g_walklog && ME_UC(g_vblank_count) >= (uint32_t)g_walklog;
    char buf[400];
    int bl = 0, pk = 0;
    for (;;) {
        uint32_t hdr = *(uint32_t *)(g_ram + a);
        if (!wl) {
            /* Runs of empty ordering table entries (most of the race's 2 x 8515): a tight loop, ~4x faster than the
             * general path. Stops at a packet, the list end, the HUD layer's entry and after 0x40000 links. */
            uint32_t skipped = 0;
            while (!(hdr >> 24) && !(hdr & 0x800000) && a != hud_ot && skipped < 0x40000) {
                a = hdr & 0x1FFFFC;
                hdr = *(uint32_t *)(g_ram + a);
                skipped++;
            }
            g_ot_entries += skipped;
            g_ot_empty += skipped;
            guard += skipped;
        }
        uint32_t n = hdr >> 24;
        g_ot_entries++;
        if (wl && n) {
            const uint32_t *p = (const uint32_t *)(g_ram + a + 4);
            if ((p[0] >> 24) == 0xE3 && bl < 360)
                bl += snprintf(buf + bl, sizeof buf - bl, " %d@%06x:%08x", pk, (unsigned)prev, (unsigned)p[1]);
            pk++;
        }
        if (!n) {
            g_ot_empty++;
            if (a == hud_ot) ge_hud_begin();
        }
        else if (g_bench_stage == 1) { volatile uint32_t x = *(uint32_t *)(g_ram + a + 4); (void)x; }
        else if (a + 4 + 4 * n <= PSX_RAM_SIZE) gpu_packet((const uint32_t *)(g_ram + a + 4), n);
        else for (uint32_t i = 0; i < n; i++) gpu_gp0(*(uint32_t *)(g_ram + ((a + 4 + 4 * i) & 0x1FFFFC)));
        if ((hdr & 0x800000) || ++guard > 0x40000) break;
        prev = a;
        a = hdr & 0x1FFFFC;
    }
    g_hud_mode = 0;
    if (wl) {
        buf[bl] = 0;
        logf_("wls %u %06x %d:%s\n", (unsigned)g_vblank_count, (unsigned)madr, pk, buf);
        if (ME_UC(g_vblank_count) > (uint32_t)g_walklog + 900) g_walklog = 0;
    }
}
