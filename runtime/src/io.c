/* PS1 I/O area 0x1F801000-0x1F802FFF: interrupt controller, DMA, timers, GPU/SPU/CD ports.
 * Unhandled registers behave like plain memory and are logged on first access. */
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "gpu.h"
#include "cdrom.h"
#include "sio.h"
#include "card.h"
#include "spu.h"
#include "prof.h"
#include "mdec.h"
#include "ge.h"
#include "raster.h"
#include <pspkernel.h>

uint32_t g_i_stat, g_i_mask;
/* indexed by address - 0x1F800000, so offsets below read like the hardware addresses */
static uint8_t g_io[0x3000] __attribute__((aligned(4)));
static uint8_t g_seen[0x3000];
uint32_t g_vblank_count;
uint32_t g_gpu_lists;          /* GPU linked-list DMAs (~ game frames drawn) */
uint64_t g_gpu_us;             /* time spent executing GPU display lists (rasterizer) */
uint32_t g_pace_hist[5];       /* display lists by vblanks since the previous one: [1], [2], [3], [4+] */

#define IO32(off) (*(uint32_t *)(g_io + (off)))
#define IO16(off) (*(uint16_t *)(g_io + (off)))

void io_init(void)
{
    memset(g_io, 0, sizeof g_io);
    IO32(0x1060) = 0x00000B88;   /* RAM_SIZE */
}

static void seen(uint32_t off, const char *rw, uint32_t v)
{
    if (g_seen[off]) return;
    g_seen[off] = 1;
    logf_("io first %s %08x = %08x\n", rw, (unsigned)(0x1F800000u + off), (unsigned)v);
}

void raise_irq(int bit) { g_i_stat |= 1u << bit; }

void io_vblank(void)
{
    g_vblank_count++;
    card_tick();
    raise_irq(0);
    gpu_present();
}

/* ---- DMA -------------------------------------------------------------------------------------------------- */

static void dma_irq(int ch)
{
    uint32_t dicr = IO32(0x10F4);
    if (dicr & (1u << (16 + ch))) {
        dicr |= 1u << (24 + ch);
        IO32(0x10F4) = dicr;
        if ((dicr & 0x00800000u) && (dicr & 0x7F000000u)) {
            IO32(0x10F4) |= 0x80000000u;
            raise_irq(3);
        }
    }
}

uint32_t g_ot_entries, g_ot_empty;
int g_bench_stage;              /* benchmark: stop the primitive path after stage n (0 = full) */
int g_gebench;                /* settings.txt gebench=<vblank>: benchmark the GPU list path once (see below) */

int g_walklog;                 /* settings.txt walklog=<vblank>: log every packet of one race frame (debug) */

/* Race HUD (16:9, see ge.c hud_squeeze): the race's ordering table (ClearOTagR, walked from its top entry madr
 * downwards) has 8515 entries in four layers, each starting with a drawing-area packet; the last layer, linked from
 * OT[3] = madr - 0x850C, is the HUD. Found with settings.txt walklog=<vblank> (one summary line per list). */
#define RACE_HUD_OT 0x850Cu

static void gpu_walk(uint32_t madr)
{
    uint32_t a = madr, guard = 0, prev = 0;
    uint32_t hud_ot = (g_widescreen && g_cur_overlay == 0) ? madr - RACE_HUD_OT : 1;
    int wl = g_walklog && g_vblank_count >= (uint32_t)g_walklog;
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
        if (g_vblank_count > (uint32_t)g_walklog + 900) g_walklog = 0;
    }
}

/* Deterministic benchmark of the GPU -> GE path: the first big display list after vblank <gebench> is executed 100
 * more times (same RAM contents, so the same primitives); logs the time per pass without and with the GE's work. */
/* settings.txt gedump=<vblank>: the first big display list after that vblank is saved with PS1 RAM, VRAM and the
 * drawing state to geframe.bin; gebenchfile=1 replays that file at startup (ge_bench_file) - the same frame for
 * every build, so GPU path changes can be compared exactly (the live gebench frame depends on timing). */
int g_gedump, g_gebenchfile;
typedef struct { uint32_t magic, madr, draw_mode; int overlay; RasterState rs; } GeFrameHdr;
#define GEFRAME_MAGIC 0x47454631u

static void gpu_dump(uint32_t madr)
{
    char path[300];
    snprintf(path, sizeof path, "%sgeframe.bin", g_basedir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    ge_sync();
    GeFrameHdr h = {GEFRAME_MAGIC, madr, gpu_draw_mode(), g_cur_overlay, g_rs};
    fwrite(&h, sizeof h, 1, f);
    fwrite(g_ram, 1, PSX_RAM_SIZE, f);
    vram_fwrite(f);
    fclose(f);
    logf_("gedump: frame at vblank %u saved (madr %06x)\n", (unsigned)g_vblank_count, (unsigned)madr);
}

static void gpu_bench_passes(uint32_t madr, uint32_t prims);

void ge_bench_file(void)
{
    if (!g_gebenchfile) return;
    char path[300];
    snprintf(path, sizeof path, "%sgeframe.bin", g_basedir);
    FILE *f = fopen(path, "rb");
    if (!f) { logf_("gebenchfile: no geframe.bin\n"); return; }
    GeFrameHdr h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == GEFRAME_MAGIC &&
             fread(g_ram, 1, PSX_RAM_SIZE, f) == PSX_RAM_SIZE && vram_fread(f);
    fclose(f);
    if (!ok) { logf_("gebenchfile: bad geframe.bin\n"); return; }
    sceKernelDcacheWritebackAll();
    ge_vram_written(0, 0, 1024, 512);
    int ovl = g_cur_overlay;
    g_cur_overlay = h.overlay;
    g_rs = h.rs;
    gpu_set_draw_mode(h.draw_mode);
    extern uint32_t g_ge_prims;
    uint32_t p0 = g_ge_prims;
    gpu_walk(h.madr);
    ge_sync();
    gpu_bench_passes(h.madr, g_ge_prims - p0);
    g_cur_overlay = ovl;
    memset(g_ram, 0, PSX_RAM_SIZE);
}

static void gpu_bench(uint32_t madr)
{
    extern uint32_t g_ge_prims;
    uint32_t p0 = g_ge_prims;
    gpu_walk(madr);
    ge_sync();
    uint32_t prims = g_ge_prims - p0;
    if (prims < 500) return;                    /* not a race frame */
    if (g_gedump) { g_gedump = 0; gpu_dump(madr); return; }
    g_gebench = 0;
    gpu_bench_passes(madr, prims);
}

static void gpu_bench_passes(uint32_t madr, uint32_t prims)
{
    static const int order[] = {1, 15, 2, 3, 7, 8, 9, 4, 10, 11, 12, 13, 5, 6, 14, 0};
    for (unsigned si = 0; si < sizeof order / sizeof order[0]; si++) {
        g_bench_stage = order[si];
        /* each pass from an empty list, the GE's tail after the CPU work (tg) measured separately: tw is CPU only */
        uint64_t t0 = now_us(), tw = 0, tg = 0;
        for (int i = 0; i < 100; i++) {
            uint64_t a = now_us();
            gpu_walk(madr);
            uint64_t b = now_us();
            ge_sync();
            tw += b - a;
            tg += now_us() - b;
        }
        uint64_t t1 = now_us();
        logf_("gebench stage %d: GE tail %u us per pass\n", g_bench_stage, (unsigned)(tg / 100));
        extern uint32_t g_memo_hits, g_memo_miss, g_clut_conv;
        logf_("gebench: memo %u hits %u misses, %u CLUT conversions (100 passes)\n", (unsigned)g_memo_hits,
              (unsigned)g_memo_miss, (unsigned)g_clut_conv);
        g_memo_hits = g_memo_miss = g_clut_conv = 0;
        logf_("gebench stage %d:%u prims, CPU %u us per pass (%u ns per prim), with GE %u us\n", g_bench_stage,
              (unsigned)prims, (unsigned)(tw / 100), (unsigned)(tw * 10 / prims), (unsigned)((t1 - t0) / 100));
    }
    g_bench_stage = 0;
}

static void dma_run(int ch)
{
    uint32_t base = 0x1080 + ch * 0x10;
    uint32_t madr = IO32(base) & 0x1FFFFC, bcr = IO32(base + 4), chcr = IO32(base + 8);
    switch (ch) {
    case 2: /* GPU */
        if ((chcr & 0x600) == 0x400) {          /* linked list */
            PHASE_BEGIN(PH_GPU);
            g_gpu_lists++;
            {   /* frame pacing: vblanks since the previous display list (1, 2, 3, 4+), logged by gpu.c */
                static uint32_t last_vb;
                uint32_t d = g_vblank_count - last_vb;
                last_vb = g_vblank_count;
                g_pace_hist[d >= 4 ? 4 : d]++;
            }
            uint64_t t0 = now_us();
            if ((g_gebench && g_vblank_count >= (uint32_t)g_gebench) || (g_gedump && g_vblank_count >= (uint32_t)g_gedump))
                gpu_bench(madr);
            gpu_walk(madr);
            g_gpu_us += now_us() - t0;
            PHASE_END();
        } else {                                /* block / sync mode */
            uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
            if (chcr & 1) {
                for (uint32_t i = 0; i < words; i++) gpu_gp0(*(uint32_t *)(g_ram + ((madr + 4 * i) & 0x1FFFFC)));
            } else {
                for (uint32_t i = 0; i < words; i++) *(uint32_t *)(g_ram + ((madr + 4 * i) & 0x1FFFFC)) = gpu_read();
            }
        }
        break;
    case 0: /* MDEC in */
        mdec_dma_in(madr, (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1));
        break;
    case 1: /* MDEC out: deferred while the decode command is still receiving its input */
        if (!mdec_dma_out(madr, (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1))) return;
        break;
    case 3: /* CD-ROM */
        cdrom_dma(madr, (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1));
        break;
    case 4: /* SPU: sound RAM transfer (block mode: BS words x BA blocks) */
        spu_dma(madr, (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1), chcr & 1);
        break;
    case 6: { /* OTC: reverse-clear ordering table */
        uint32_t n = bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000;
        uint32_t a = madr;
        for (uint32_t i = 0; i < n; i++) {
            *(uint32_t *)(g_ram + a) = (i == n - 1) ? 0x00FFFFFFu : ((a - 4) & 0x1FFFFC);
            a = (a - 4) & 0x1FFFFC;
        }
        break;
    }
    default:
        logf_("DMA%d start madr=%08x bcr=%08x chcr=%08x (ignored)\n", ch, (unsigned)madr, (unsigned)bcr, (unsigned)chcr);
        break;
    }
    IO32(base + 8) = chcr & ~0x11000000u;
    dma_irq(ch);
}

void io_dma_done(int ch)
{
    IO32(0x1088 + ch * 0x10) &= ~0x11000000u;
    dma_irq(ch);
}

/* ---- timers ------------------------------------------------------------------------------------------- */

static uint32_t timer_count(int t)
{
    uint64_t us = now_us();
    uint32_t mode = IO32(0x1104 + t * 0x10);
    uint32_t v;
    if (t == 1 && (mode & 0x100)) v = (uint32_t)(us / 63);           /* hblank: ~15734 Hz */
    else if (t == 2 && (mode & 0x200)) v = (uint32_t)(us * 33869 / 8000); /* sysclk/8 */
    else v = (uint32_t)(us * 33869 / 1000);                           /* sysclk 33.8688 MHz */
    uint32_t target = IO32(0x1108 + t * 0x10) & 0xFFFF;
    if ((mode & 0x8) && target) v %= target + 1;
    return v & 0xFFFF;
}

/* ---- register access ---------------------------------------------------------------------------------- */

static uint32_t io_read(uint32_t a, int size)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x1F801000u || p >= 0x1F803000u) {
        if (p >= 0x1FC00000u && p < 0x1FC80000u) return 0;   /* BIOS ROM */
        logf_("read%d from unmapped %08x\n", size * 8, (unsigned)a);
        return 0;
    }
    uint32_t off = p - 0x1F800000u;
    uint32_t v;
    if (off >= 0x1800 && off < 0x1804) return cdrom_read(off - 0x1800);
    if (off >= 0x1040 && off < 0x1050) return sio_read(off - 0x1040, size);
    if (off >= 0x1C00 && off < 0x2000) {
        if (size == 4) return spu_read16(off) | (spu_read16(off + 2) << 16);
        v = spu_read16(off);
        return size == 1 ? (v >> ((off & 1) * 8)) & 0xFF : v;
    }
    switch (off & ~3u) {
    case 0x1070: v = g_i_stat; break;
    case 0x1074: v = g_i_mask; break;
    case 0x1810: v = gpu_read(); break;
    case 0x1820: case 0x1824: v = mdec_read(off & 4); break;
    case 0x1814: v = gpu_status() | ((g_vblank_count & 1) << 31); break;
    case 0x1100: case 0x1110: case 0x1120: v = timer_count((off - 0x1100) >> 4); break;
    default:
        v = size == 4 ? IO32(off & ~3u) : size == 2 ? IO16(off & ~1u) : g_io[off];
        seen(off, "read", v);
        return v;
    }
    if (size == 2) v = (off & 2) ? v >> 16 : v & 0xFFFF;
    else if (size == 1) v = (v >> ((off & 3) * 8)) & 0xFF;
    return v;
}

static void io_write(uint32_t a, uint32_t v, int size)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (p == 0x1FFE0130u) return;                  /* cache control */
    if (p < 0x1F801000u || p >= 0x1F803000u) {
        logf_("write%d to unmapped %08x = %08x\n", size * 8, (unsigned)a, (unsigned)v);
        return;
    }
    uint32_t off = p - 0x1F800000u;
    if (off >= 0x1800 && off < 0x1804) { cdrom_write(off - 0x1800, v); return; }
    if (off >= 0x1040 && off < 0x1050) { sio_write(off - 0x1040, v, size); return; }
    if (off >= 0x1C00 && off < 0x2000) {
        if (size == 4) { spu_write16(off, v & 0xFFFF); spu_write16(off + 2, v >> 16); }
        else spu_write16(off, v);
        return;
    }
    switch (off & ~3u) {
    case 0x1070: g_i_stat &= v; return;           /* acknowledge */
    case 0x1074: g_i_mask = v & 0x7FF; return;
    case 0x1810: gpu_gp0(v); return;
    case 0x1820: case 0x1824: mdec_write(off & 4, v); return;
    case 0x1814: gpu_gp1(v); return;
    case 0x10F4: {                                 /* DICR: writing 1 to flag bits clears them */
        uint32_t d = IO32(0x10F4);
        d = (d & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FFFFFFu);
        if ((d & 0x00800000u) && (d & 0x7F000000u)) d |= 0x80000000u;
        IO32(0x10F4) = d;
        return;
    }
    }
    if (size == 4) IO32(off & ~3u) = v;
    else if (size == 2) IO16(off & ~1u) = (uint16_t)v;
    else g_io[off] = (uint8_t)v;
    if (off >= 0x1080 && off < 0x10F0 && (off & 0xF) == 8 && (v & 0x01000000u)) {
        int ch = (off - 0x1080) >> 4;
        if (IO32(0x10F0) & (8u << (ch * 4))) dma_run(ch);
        return;
    }
    seen(off, "write", v);
}

uint32_t io_r8(uint32_t a) { return io_read(a, 1); }
uint32_t io_r16(uint32_t a) { return io_read(a, 2); }
uint32_t io_r32(uint32_t a) { return io_read(a, 4); }
void io_w8(uint32_t a, uint32_t v) { io_write(a, v, 1); }
void io_w16(uint32_t a, uint32_t v) { io_write(a, v, 2); }
void io_w32(uint32_t a, uint32_t v) { io_write(a, v, 4); }

/* Out-of-line paths of the checked accessors (gt2rt.h): low KB of RAM -> dummy area, scratchpad, I/O. */
static uint8_t g_lowram[0x400] __attribute__((aligned(4)));
static uint8_t *slow_ptr(uint32_t a)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (RAM_HIT(p)) return g_lowram + (p & 0x3FF);
    if (SCR_HIT(p)) return g_ram + (p & 0x3FF);
    return 0;
}
uint32_t mem_slow_r8(uint32_t a) { uint8_t *h = slow_ptr(a); return h ? *h : io_r8(a); }
uint32_t mem_slow_r16(uint32_t a) { uint8_t *h = slow_ptr(a); return h ? *(uint16_t *)h : io_r16(a); }
uint32_t mem_slow_r32(uint32_t a) { uint8_t *h = slow_ptr(a); return h ? *(uint32_t *)h : io_r32(a); }
void mem_slow_w8(uint32_t a, uint32_t v) { uint8_t *h = slow_ptr(a); if (h) *h = (uint8_t)v; else io_w8(a, v); }
void mem_slow_w16(uint32_t a, uint32_t v) { uint8_t *h = slow_ptr(a); if (h) *(uint16_t *)h = (uint16_t)v; else io_w16(a, v); }
void mem_slow_w32(uint32_t a, uint32_t v) { uint8_t *h = slow_ptr(a); if (h) *(uint32_t *)h = v; else io_w32(a, v); }

/* ---- memory profile (GT2_MEMPROF builds of the generated code, see gt2rt.h) ------------------------------- */

#define MP_SLOTS 8192
static uint32_t g_mp_fn[MP_SLOTS];
static uint8_t g_mp_fl[MP_SLOTS];
static int g_mp_used, g_mp_dirty;
static uint8_t g_mp_scr[256];

static void mp_note(uint32_t fn, uint32_t a, int w)
{
    static int n_low;
    uint32_t p = a & 0x1FFFFFFFu;
    int fl = 1;
    if (SCR_HIT(p) && !g_mp_scr[(p & 0x3FF) >> 2]) { g_mp_scr[(p & 0x3FF) >> 2] = 1; g_mp_dirty = 1; }
    if (!RAM_HIT(p) && !SCR_HIT(p)) fl |= 2;                    /* I/O, BIOS ROM, expansion, cache control */
    else if (RAM_HIT(p) && (p & (PSX_RAM_SIZE - 1)) < 0x400) {    /* low RAM: shared with the scratchpad */
        fl |= 4;
        if (n_low < 200) { n_low++; logf_("memprof: fn %08x %s low RAM %08x\n", (unsigned)fn, w ? "writes" : "reads", (unsigned)a); }
    }
    uint32_t h = (fn * 0x9E3779B1u) >> 19;
    for (int i = 0; i < MP_SLOTS; i++) {
        uint32_t j = (h + i) & (MP_SLOTS - 1);
        if (g_mp_fn[j] == fn) {
            if ((g_mp_fl[j] | fl) != g_mp_fl[j]) { g_mp_fl[j] |= fl; g_mp_dirty = 1; }
            return;
        }
        if (!g_mp_fn[j]) { g_mp_fn[j] = fn; g_mp_fl[j] = fl; g_mp_used++; g_mp_dirty = 1; return; }
    }
}

uint32_t memprof_r(uint32_t a, uint32_t fn, int size)
{
    mp_note(fn, a, 0);
    return size == 1 ? mem_r8(a) : size == 2 ? mem_r16(a) : mem_r32(a);
}

void memprof_w(uint32_t a, uint32_t v, uint32_t fn, int size)
{
    mp_note(fn, a, 1);
    if (size == 1) mem_w8(a, (uint8_t)v); else if (size == 2) mem_w16(a, (uint16_t)v); else mem_w32(a, v);
}

/* Called from the poll loop every few seconds: memprof.txt = "<module << 24 | address> <flags>" per function. */
void memprof_dump(void)
{
    if (!g_mp_dirty) return;
    g_mp_dirty = 0;
    char path[300];
    snprintf(path, sizeof path, "%smemprof.txt", g_basedir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# scratchpad words used:");
    for (int j = 0; j < 256; j++)
        if (g_mp_scr[j]) fprintf(f, " %03x", j * 4);
    fprintf(f, "\n");
    for (int j = 0; j < MP_SLOTS; j++)
        if (g_mp_fn[j]) fprintf(f, "%08x %d\n", (unsigned)g_mp_fn[j], g_mp_fl[j]);
    fclose(f);
}
