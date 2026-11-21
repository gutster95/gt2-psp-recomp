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
#include "me.h"

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

int g_fps60;   /* settings.txt fps60: frame interval 1 instead of 2 (recomp.py, Silent's 60 FPS cheat); experiment */
int g_fx60 = 1;   /* fps60_effects: keep mirror and tire smoke at 60 fps (0: the game turns them off) */
int g_lowlod;     /* lowlod: other cars always at the lowest LOD (recomp.py) */
int g_track_lod;  /* track_lod: distance classes with the full scenery model (0 = game: 3, replays all) */
int g_performance;   /* performance=1: fps60, fps60_effects=0, lowlod=1 (main.c) */

/* the race frame interval byte (0x801D5864 on the Simulation disc; Arcade: 0x5A0 lower, all references to the race
 * display struct 0x801D585C/0x801D52BC moved alike, src/gt2psp/profiles.py) */
#ifdef GT2_ARC_BUILD
static const uint32_t g_interval = 0x1D52C4u;
#else
static const uint32_t g_interval = 0x1D5864u;
#endif
int g_fstep_log;   /* settings.txt fstep_log=1 (rt_frame_step diagnostics) */
static int g_fps60_patched;   /* the frame interval currently 1 is ours (was 2) */
int g_fps60_dyn = 1;          /* settings.txt fps60_dynamic: game time step = vblanks that really passed */
uint32_t g_step_hist[4];      /* statistics: time steps given (1, 2, 3+) and frames with debt */

/* Frame flip hooks (recomp.py, 0x800809b0, disp = the game's display struct, struct+24 = interval). At 60 fps the game
 * simulates one vblank of time per frame; a frame that took two vblanks ran the game in slow motion. fps60_dynamic:
 * before the wait the interval is 1 (no frame is stretched to two vblanks), after it the vblanks the frame really
 * took (1 or 2, more is carried over, at most 4) - the next frame simulates that much time, like the game's own
 * 30 fps mode with interval 2. The frame rate floats between 30 and 60, game time follows real time. */
static int g_dyn_owned;         /* the interval byte is managed here (io_vblank leaves it alone) */
static uint32_t g_dyn_wait;     /* RAM offset of the display struct's wait byte we manage (0: none) */
/* fps60_auto (default on): stable frame rates. Frames that miss 60 fps alternate 1 and 2 vblanks (judder). When a
 * scene keeps missing (3 of the last 32 frames), the game is locked to 30 fps (every frame waits 2 vblanks); it goes
 * back to 60 once every frame of two seconds was ready in 15.5 ms (work = end of the previous wait to the start of
 * this one, including the wait for the Media Engine). The time step follows real time in both. */
int g_fps60_auto = 1;
int g_lock30;                 /* currently locked to 30 fps (fps counter, statistics) */
uint32_t g_lock_switches;
void rt_frame_step(uint32_t disp, int after_wait)
{
    static uint32_t last;
    static int debt;
    uint32_t a = (disp & 0x1FFFFF) + 24;
    {
        static uint32_t seen[8];
        int k;
        for (k = 0; k < 8 && seen[k] && seen[k] != disp; k++) {}
        if (k < 8 && !seen[k]) {
            seen[k] = disp;
            logf_("frame flip: display struct %08x, interval %u, step byte %u (vblank %u)\n", (unsigned)disp,
                  (unsigned)g_ram[a & 0x1FFFFF], (unsigned)g_ram[(a + 8) & 0x1FFFFF], (unsigned)g_vblank_count);
        }
    }
    /* the wait uses a copy of the interval in the display struct (a stack variable); the simulation reads the
     * global interval at 0x801D5864 */
    const uint32_t G = g_interval;
    if (!g_fps60 || !g_fps60_dyn) {
        if (g_dyn_owned) {
            g_ram[G] = (uint8_t)(g_fps60 ? 1 : 2);
            /* the wait byte we set to 1 must go back too: with wait 1 and the game's step 2 the race ran at 60 fps
             * with 30 fps time steps (double speed), and 30 fps could not be switched back on */
            if (!g_fps60 && g_dyn_wait == a && a < PSX_RAM_SIZE) g_ram[a] = 2;
            g_dyn_owned = 0;
        }
        g_dyn_wait = 0;
        return;
    }
    static uint64_t t_aw;                             /* end of the previous wait */
    static uint32_t win_n, win_slow, ok_windows, miss_hist;
    if (!after_wait) {
        if (g_ram[G] == 1 || g_dyn_owned) {
            g_dyn_owned = 1;
            if (g_fps60_auto && t_aw) {
                uint64_t work = now_us() - t_aw;
                win_n++;
                win_slow += work > 15500;
                if (g_lock30 && win_n >= 60) {        /* ~2 s at 30 fps: all frames fast enough for 60? */
                    ok_windows = win_slow ? 0 : ok_windows + 1;
                    if (ok_windows >= 1) { g_lock30 = 0; g_lock_switches++; miss_hist = 0; ok_windows = 0; }
                    win_n = win_slow = 0;
                } else if (!g_lock30 && win_n >= 120) {
                    win_n = win_slow = 0;
                }
            }
            /* the wait: one vblank, or two when locked to 30 fps */
            if (a < PSX_RAM_SIZE && (g_ram[a] == g_ram[G] || g_dyn_wait == a)) {
                g_ram[a] = (uint8_t)(g_fps60_auto && g_lock30 ? 2 : 1);
                g_dyn_wait = a;
            }
        }
        return;
    }
    uint32_t now = g_vblank_count, el = last ? now - last : 1;
    last = now;
    t_aw = now_us();
    if (g_fstep_log) {   /* settings.txt fstep_log=1: the frame step state about once a second */
        static uint32_t n;
        if ((n++ % 60) == 0)
            logf_("fstep: disp %08x wait %u interval@%06x %u el %u owned %d fps60 %d lock30 %d\n", (unsigned)disp,
                  (unsigned)g_ram[a & 0x1FFFFF], (unsigned)G, (unsigned)g_ram[G], (unsigned)el, g_dyn_owned, g_fps60,
                  g_lock30);
    }
    if (!g_dyn_owned) return;
    if (g_fps60_auto && !g_lock30) {                  /* misses of the last 32 frames (bit history) */
        miss_hist = (miss_hist << 1) | (el >= 2);
        if (__builtin_popcount(miss_hist) >= 3) {
            g_lock30 = 1; g_lock_switches++; win_n = win_slow = 0; ok_windows = 0;
        }
    }
    int step = (int)el + debt;
    if (step < 1) step = 1;
    int give = step > 2 ? 2 : step;
    debt = step - give;
    if (debt > 4) debt = 4;
    g_ram[G] = (uint8_t)give;
    g_step_hist[give]++;
    if (debt) g_step_hist[3]++;
}

/* settings menu (menu.c) switched fps60: back to the game's interval 2 if we had made it 1 */
void io_fps60_changed(void)
{
    if (!g_fps60 && g_fps60_patched && g_ram[g_interval] == 1) g_ram[g_interval] = 2;
    g_fps60_patched = 0;
}

int g_fps60_flip[4];   /* settings.txt fps60_flip1..4: test - toggle fps60 at these vblanks, like the settings menu */
void io_vblank(void)
{
    g_vblank_count++;
    for (int i = 0; i < 4; i++)
        if (g_fps60_flip[i] && g_vblank_count == (uint32_t)g_fps60_flip[i]) {
            g_fps60 ^= 1;
            io_fps60_changed();
            logf_("fps60_flip: fps60 %d at vblank %u\n", g_fps60, (unsigned)g_vblank_count);
        }
    if (g_fps60 && !g_dyn_owned && g_ram[g_interval] == 2) { g_ram[g_interval] = 1; g_fps60_patched = 1; }
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

int g_bench_stage;              /* benchmark: stop the primitive path after stage n (0 = full) */
int g_gebench;                /* settings.txt gebench=<vblank>: benchmark the GPU list path once (see below) */

void gpu_walk(uint32_t madr);   /* gpu.c */

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
            if (g_me_active) {                  /* the ME walks it; DMA2 stays busy until then (me_service) */
                me_post_walk(madr);
                PHASE_END();
                return;
            }
            uint64_t t0 = now_us();
            if ((g_gebench && g_vblank_count >= (uint32_t)g_gebench) || (g_gedump && g_vblank_count >= (uint32_t)g_gedump))
                gpu_bench(madr);
            gpu_walk(madr);
            g_gpu_us += now_us() - t0;
            PHASE_END();
        } else {                                /* block / sync mode */
            uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
            if (g_me_active) { me_post_block(madr, words, chcr & 1); return; }
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
    case 0x1810: v = g_me_active ? me_gpu_read() : gpu_read(); break;
    case 0x1820: case 0x1824: v = mdec_read(off & 4); break;
    case 0x1814: v = (g_me_active ? me_gpu_status() : gpu_status()) | ((g_vblank_count & 1) << 31); break;
    case 0x1100: case 0x1110: case 0x1120: v = timer_count((off - 0x1100) >> 4); break;
    case 0x10A8:                                   /* DMA2 CHCR: busy while the ME works (DrawSync polls it) */
        v = IO32(0x10A8);
        if (g_me_active && (v & 0x01000000u)) me_note_wait();
        break;
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
    case 0x1810: if (g_me_active) me_post_gp0(v); else gpu_gp0(v); return;
    case 0x1820: case 0x1824: mdec_write(off & 4, v); return;
    case 0x1814: if (g_me_active) me_post_gp1(v); else gpu_gp1(v); return;
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
