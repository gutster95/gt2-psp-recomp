/* GPU emulation on the Media Engine (settings.txt me_gpu=1).
 *
 * The ME (second 333 MHz Allegrex, booted with mcidclan's psp-media-engine-custom-core, MIT) runs the whole PS1 GPU
 * emulation - gpu.c (GP0/GP1, DMA lists), ge.c (GE list building, texture/CLUT caches, presentation) and raster.c -
 * in parallel with the game code on the main CPU. The main CPU only posts commands into a ring (GP0/GP1 writes, DMA2
 * transfers, vblanks) and does the system calls the ME cannot make: a service thread enqueues the GE lists the ME
 * has finished and swaps the display, a vblank thread publishes the display vcount.
 *
 * Coherency: the ME's data cache is separate. The ME-side state (data/bss of ge.c, gpu.c, raster.c and libpspgu) lies
 * in its own 64-byte aligned block (runtime/ld/linkfile.prx.in), which the main CPU does not write while the ME runs;
 * the shared block below is accessed uncached by both; before DMA jobs the main CPU writes its cache back and the ME
 * drops its cached copies; the ME writes PS1 RAM (VRAM -> CPU transfers) uncached. The "scratchpad" state of ge.c
 * (0x10000) is ME-local eDRAM on the ME. DrawSync works as on the PS1: the DMA2 busy bit stays set until the ME has
 * finished the transfer. */
#include <string.h>
#include <malloc.h>
#include <stdint.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include "rt.h"
#include "me.h"
#include "gpu.h"
#include "prof.h"
#include <stdio.h>

int g_me_gpu = 1;          /* settings.txt me_gpu=0: GPU path on the main CPU */
int g_me_active;

#define RING 4096
#define LOGN 64
enum { CMD_GP0 = 1, CMD_GP1, CMD_WALK, CMD_BLOCK_IN, CMD_BLOCK_OUT, CMD_VBLANK, CMD_READ1, CMD_BLOCK_SNAP };

typedef struct {
    uint32_t head, tail;                 /* commands posted (main) / processed (ME), monotonic */
    uint32_t status, read_val;           /* published by the ME */
    uint32_t vcount, swap_vcount;
    uint32_t time_lo, time_hi;
    uint32_t req_seq, req_start, req_end, req_show, req_fps, req_wait, done_seq;
    uint32_t log_head, log_tail;
    uint32_t alive, exit, ready, started, vshadow;
    uint32_t cur_cmd, cur_arg, walk_pos, walk_pk;   /* ME progress (diagnostics: a walk that does not finish) */
    uint32_t mprof[16];                  /* ME cycles per profiler phase (GT2_ME_PROF builds) */
    uint32_t spad[1024];                 /* main CPU scratchpad state at the ME start (ge.c / raster.h) */
    uint32_t ring[RING][4];
    uint32_t log[LOGN][16];              /* format pointer, argument count, 14 arguments */
} __attribute__((aligned(64))) MeShared;
static MeShared g_mesh __attribute__((aligned(64)));
#define SH ((volatile MeShared *)((uintptr_t)&g_mesh | 0x40000000u))
static inline void sync_(void) { __asm__ volatile("sync" ::: "memory"); }

/* ---- ME cache maintenance (cache ops of the Allegrex, see mcidclan's me-lib.c) ---- */
void me_dcache_wbinv_all(void)
{
    sync_();
    for (uint32_t i = 0; i < 16384; i += 64)
        __asm__ volatile("cache 0x14, 0(%0)\n\tcache 0x14, 0(%0)" ::"r"(i));
    sync_();
}
void me_dcache_wb_range(const void *p, uint32_t bytes)
{
    uint32_t a = (uint32_t)p & ~63u, e = (uint32_t)p + bytes;
    sync_();
    for (; a < e; a += 64) __asm__ volatile("cache 0x1a, 0(%0)\n\tcache 0x1a, 0(%0)" ::"r"(a));
    sync_();
}

/* ---- ME side ---- */
void me_log(const char *fmt, const uint32_t *args, int n)
{
    uint32_t h = SH->log_head;
    if (h - SH->log_tail >= LOGN) return;            /* full: dropped */
    volatile uint32_t *e = SH->log[h % LOGN];
    e[0] = (uint32_t)fmt;
    e[1] = (uint32_t)n;
    for (int i = 0; i < n && i < 14; i++) e[2 + i] = args[i];
    sync_();
    SH->log_head = h + 1;
}
uint64_t me_now_us(void) { uint32_t hi, lo; do { hi = SH->time_hi; lo = SH->time_lo; } while (hi != SH->time_hi); return ((uint64_t)hi << 32) | lo; }
uint32_t me_vcount(void) { return SH->vcount; }
uint32_t me_swap_vcount(void) { return SH->swap_vcount; }

static uint32_t g_req;      /* ME-side request counter (ME region: gpu state lives in ME-only files... kept here, only the ME writes it) */
extern uint32_t g_submit_wait_us;       /* gpu.c (ME data block): ME time waiting for the service thread to take a list */
uint32_t me_ge_submit(const void *start, const void *end, int show, int fps, int wait)
{
    if (SH->done_seq != SH->req_seq) {                /* one request at a time */
        uint64_t t0 = me_now_us();
        while (SH->done_seq != SH->req_seq) {}
        g_submit_wait_us += (uint32_t)(me_now_us() - t0);
    }
    uint32_t s = SH->req_seq + 1;
    SH->req_start = (uint32_t)start;
    SH->req_end = (uint32_t)end;
    SH->req_show = (uint32_t)show;
    SH->req_fps = (uint32_t)fps;
    SH->req_wait = (uint32_t)wait;
    sync_();
    SH->req_seq = s;
    sync_();
    if (wait) while (SH->done_seq != s) {}
    (void)g_req;
    return s;
}
int me_ge_done(uint32_t seq) { return (int32_t)(SH->done_seq - seq) >= 0; }

/* GT2_ME_PROF: the ME's current profiler phase, written uncached by the ME (prof.h), sampled by prof.c */
uint32_t g_me_phase_word[16] __attribute__((aligned(64)));
/* called by gpu_walk on the ME every 1024 packets: where the walk is */
void me_walk_progress(uint32_t a, uint32_t packets) { SH->walk_pos = a; SH->walk_pk = packets; }

extern void gpu_walk(uint32_t madr);
extern void gpu_present_core(void);

static void me_exec(volatile uint32_t *e)
{
    uint32_t cmd = e[0], a = e[1], n = e[2];
    switch (cmd) {
    case CMD_GP0: gpu_gp0(a); break;
    case CMD_GP1: gpu_gp1(a); break;
    case CMD_WALK: me_dcache_wbinv_all(); gpu_walk(a); break;
    case CMD_BLOCK_IN:
        me_dcache_wbinv_all();
        if ((a & 0x1FFFFC) + 4 * n <= PSX_RAM_SIZE) gpu_packet((const uint32_t *)(g_ram + (a & 0x1FFFFC)), n);   /* uploads */
        else for (uint32_t i = 0; i < n; i++) gpu_gp0(*(uint32_t *)(g_ram + ((a + 4 * i) & 0x1FFFFC)));
        break;
    case CMD_BLOCK_SNAP:                         /* uploads from a snapshot taken when the game started the DMA */
        me_dcache_wbinv_all();
        gpu_packet((const uint32_t *)a, n);
        break;
    case CMD_BLOCK_OUT:
        for (uint32_t i = 0; i < n; i++)
            *(volatile uint32_t *)(((uintptr_t)(g_ram + ((a + 4 * i) & 0x1FFFFC))) | 0x40000000u) = gpu_read();
        break;
    case CMD_VBLANK: gpu_present_core(); break;
    case CMD_READ1: SH->read_val = gpu_read(); break;
    }
    SH->status = gpu_status();
}

void meLibOnProcess(void);
__attribute__((noinline, aligned(4))) void meLibOnProcess(void)
{
    memcpy((void *)0x00010000u, (const void *)SH->spad, sizeof g_mesh.spad);   /* ge.c / raster.h state */
    {
        extern uint16_t *g_vram;
        extern void ge_me_attach(uint16_t *shadow);
        g_vram = (uint16_t *)(SH->vshadow | 0x40000000u);   /* PS1 VRAM shadow in RAM (the ME cannot reach eDRAM) */
        ge_me_attach(g_vram);
    }
#ifdef GT2_ME_FPROF
    *(volatile uint32_t *)0x00013E00u = 0;
#endif
    me_dcache_wbinv_all();
    sync_();
    SH->ready = 1;
    uint32_t t = SH->tail;
    for (;;) {
        SH->alive = SH->alive + 1;
        if (t == SH->head) {
            if (SH->exit) break;
            continue;
        }
#ifdef GT2_ME_PROF
        me_phase_set(PH_OTHER);
        me_exec(SH->ring[t % RING]);
        me_phase_set(12);                      /* waiting for the next command */
#else
        SH->cur_cmd = SH->ring[t % RING][0];
        SH->cur_arg = SH->ring[t % RING][1];
        me_exec(SH->ring[t % RING]);
#endif
        t++;
        sync_();
        SH->tail = t;
    }
    SH->exit = 2;
    for (;;) __asm__ volatile(".word 0x70000000");   /* halt */
}

/* ---- main CPU side ---- */
#include <me-core-mapper/me-core.h>   /* boot code of the ME (meLibHandler, meLibOnPreProcess); included once */

static uint32_t g_dma_idx;            /* ring position after the pending DMA2 job, 0 = none */
/* headroom statistics (main CPU): time the game waited for the ME (DrawSync polling DMA2 while busy) and how long
 * the ME took per ordering-table walk (post -> completion seen by the main CPU) */
uint64_t g_me_wait_us;                /* total */
static uint64_t g_wait_t0, g_walk_t0;
uint32_t g_me_walks, g_me_walk_us, g_me_walk_max, g_me_waits;
void me_note_wait(void) { if (!g_wait_t0) { g_wait_t0 = now_us(); g_me_waits++; } }

static void post(uint32_t cmd, uint32_t a, uint32_t n)
{
    uint32_t h = SH->head;
    while (h - SH->tail >= RING) me_service();        /* ring full: let the ME catch up */
    volatile uint32_t *e = SH->ring[h % RING];
    e[0] = cmd; e[1] = a; e[2] = n;
    sync_();
    SH->head = h + 1;
}

void me_post_gp0(uint32_t v) { post(CMD_GP0, v, 0); }
void me_post_gp1(uint32_t v) { post(CMD_GP1, v, 0); }
void me_post_vblank(void) { post(CMD_VBLANK, 0, 0); }
void me_post_walk(uint32_t madr)
{
    sceKernelDcacheWritebackAll();                    /* the ME reads the ordering table and packets from RAM */
    post(CMD_WALK, madr, 0);
    g_dma_idx = SH->head;
    g_walk_t0 = now_us();
}
/* CPU -> VRAM block DMA: the ME runs behind the game, and GT2 reuses the source buffer right after the DMA (the race
 * HUD builds each text's glyphs in one buffer and uploads them string by string). Reading g_ram later uploaded the
 * next string's pixels (wrong HUD glyphs, "2:22.222"). The words are copied at DMA time into this ring, as the PS1
 * DMA would have read them then. The ring has two halves; before a half is reused, only the ME commands that read
 * from it must be done (normally long ago) - never the whole queue (this also runs inside interrupt handlers). */
#define SNAP_WORDS (256 * 1024)
#define SNAP_HALF (SNAP_WORDS / 2)
int g_me_snapshot = 1;     /* settings.txt me_snapshot=0: uploads read from PS1 RAM by the ME (freeze hunt) */
static uint32_t *g_snap;   /* heap (keeps the BSS layout of the hot data) */
static uint32_t g_snap_pos, g_snap_idx[2];   /* write position; ring index after the last command reading each half */
int me_post_block(uint32_t madr, uint32_t words, int to_gpu)
{
    if (!g_snap) g_snap = memalign(64, SNAP_WORDS * 4);
    if (to_gpu && g_me_snapshot && g_snap && words <= SNAP_HALF && (madr & 0x1FFFFC) + 4 * words <= PSX_RAM_SIZE) {
        static uint32_t cur_half;
        uint32_t half = g_snap_pos / SNAP_HALF;
        if (g_snap_pos + words > (half + 1) * SNAP_HALF) {          /* does not fit: start of the other half */
            half ^= 1;
            g_snap_pos = half * SNAP_HALF;
        }
        if (half != cur_half) {                                     /* entering a half: its old uploads are done */
            while ((int32_t)(SH->tail - g_snap_idx[half]) < 0) me_service();
            cur_half = half;
        }
        uint32_t *d = &g_snap[g_snap_pos];
        memcpy(d, g_ram + (madr & 0x1FFFFC), words * 4);
        g_snap_pos = (g_snap_pos + words + 15) & ~15u;   /* 64-byte aligned slices: no shared cache lines */
        if (g_snap_pos >= SNAP_WORDS) g_snap_pos = 0;
        sceKernelDcacheWritebackRange(d, words * 4);
        post(CMD_BLOCK_SNAP, (uint32_t)d, words);
        g_snap_idx[half] = SH->head;
        /* The words are copied: for the game the DMA is complete, as on the PS1 where it reads them now. The ME
         * uploads them in queue order. Waiting for the ME here cost the game ~0.4 s per 2 s in movies. */
        return 1;
    }
    if (to_gpu) sceKernelDcacheWritebackAll();
    else sceKernelDcacheWritebackInvalidateRange(g_ram + madr, words * 4);   /* the ME writes it uncached */
    post(to_gpu ? CMD_BLOCK_IN : CMD_BLOCK_OUT, madr, words);
    g_dma_idx = SH->head;
    return 0;
}
void me_join(void)
{
    while (SH->tail != SH->head) me_service();
}
uint32_t me_gpu_read(void)
{
    post(CMD_READ1, 0, 0);
    me_join();
    return SH->read_val;
}
uint32_t me_gpu_status(void) { return SH->status; }

void me_service(void)
{
    if (g_dma_idx && (int32_t)(SH->tail - g_dma_idx) >= 0) {
        g_dma_idx = 0;
        uint64_t t1 = now_us();
        if (g_wait_t0) { g_me_wait_us += t1 - g_wait_t0; g_wait_t0 = 0; }
        if (g_walk_t0) {
            uint32_t d = (uint32_t)(t1 - g_walk_t0);
            g_me_walks++; g_me_walk_us += d;
            if (d > g_me_walk_max) g_me_walk_max = d;
            g_walk_t0 = 0;
        }
        extern void io_dma_done(int ch);
        io_dma_done(2);
    }
    uint64_t t = now_us();
    SH->time_hi = (uint32_t)(t >> 32);
    SH->time_lo = (uint32_t)t;
    while (SH->log_tail != SH->log_head) {
        volatile uint32_t *e = SH->log[SH->log_tail % LOGN];
        uint32_t a[14];
        for (int i = 0; i < 14; i++) a[i] = e[2 + i];
        logf_((const char *)e[0], a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]);
        SH->log_tail = SH->log_tail + 1;
    }
}

/* service thread: GE lists finished by the ME -> GE queue, display swaps */
extern void ge_draw_fps_digits(int buf, int value);
extern void ge_display_buffer_set(int buf);
uint64_t g_ge_busy_us;                /* statistics: GE time per list (service thread) */
uint32_t g_ge_lists;
static int svc_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uint32_t seen = 0;
    for (;;) {
        uint32_t r = SH->req_seq;
        if (r != seen) {
            uint64_t t0 = now_us();
            int qid = sceGeListEnQueue((const void *)SH->req_start, 0, -1, 0);
            int sr = qid >= 0 ? sceGeListSync(qid, 0) : -1;
            g_ge_busy_us += now_us() - t0;          /* the GE is idle when a list is queued: its drawing time */
            g_ge_lists++;
            static int logged;
            if (logged < 8 || qid < 0) {
                logged++;
                logf_("me svc: list %08x-%08x qid %08x sync %08x first words %08x %08x\n", (unsigned)SH->req_start,
                      (unsigned)SH->req_end, (unsigned)qid, (unsigned)sr, (unsigned)((volatile uint32_t *)SH->req_start)[0],
                      (unsigned)((volatile uint32_t *)SH->req_start)[1]);
            }
            int show = (int)SH->req_show;
            if (show >= 0) {
                ge_draw_fps_digits(show, (int)SH->req_fps);
                ge_display_buffer_set(show);
                SH->swap_vcount = sceDisplayGetVcount();
            }
            seen = r;
            sync_();
            SH->done_seq = r;
            continue;
        }
        uint64_t t = now_us();
        SH->time_hi = (uint32_t)(t >> 32);
        SH->time_lo = (uint32_t)t;
        sceKernelDelayThread(200);
    }
    return 0;
}
static int vblank_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceDisplayWaitVblankStart();
        SH->vcount = sceDisplayGetVcount();
    }
    return 0;
}

/* quit: let the ME finish its command and halt (it must not run code while the main CPU writes profile data) */
void me_stop(void)
{
    if (!g_me_active) return;
    SH->exit = 1;
    uint64_t t0 = now_us();
    while (SH->exit != 2 && now_us() - t0 < 500000) {}
    g_me_active = 0;
}

void me_gpu_start(void)
{
    if (!g_me_gpu) return;
    memcpy((void *)SH->spad, (const void *)0x00010000u, sizeof g_mesh.spad);
    {   /* PS1 VRAM shadow for the CPU side of the GPU path (ge.c), filled from the eDRAM VRAM */
        void *sh = memalign(64, 1024 * 512 * 2);
        if (!sh) { logf_("me: no memory for the VRAM shadow\n"); return; }
        memcpy((void *)((uintptr_t)sh | 0x40000000u), (const void *)0x44000000u, 1024 * 512 * 2);
        sceKernelDcacheWritebackInvalidateRange(sh, 1024 * 512 * 2);
        SH->vshadow = (uint32_t)sh;
    }
    SH->vcount = sceDisplayGetVcount();
    me_service();
    sceKernelDcacheWritebackInvalidateAll();
    SceUID t = sceKernelCreateThread("me_svc", svc_thread, 0x11, 0x4000, PSP_THREAD_ATTR_USER, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
    t = sceKernelCreateThread("me_vbl", vblank_thread, 0x10, 0x1000, PSP_THREAD_ATTR_USER, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
    int table = meLibDefaultInit();
    logf_("me: table %d\n", table);
    if (table != ME_CORE_T2_IMG_TABLE && table != ME_CORE_IMG_TABLE) { logf_("me: no ME, GPU stays on the main CPU\n"); return; }
    uint64_t t0 = now_us();
    while (!SH->ready && now_us() - t0 < 2000000) {}
    if (!SH->ready) { logf_("me: ME did not start\n"); return; }
    g_me_active = 1;
    logf_("me: GPU emulation runs on the Media Engine\n");
}

#ifdef GT2_ME_FPROF
/* Function-level ME profile (-finstrument-functions on the GPU path, see scripts): every function entry/exit on the
 * ME keeps a small call stack in the ME's local memory and publishes the innermost function in g_me_phase_word[1];
 * the main CPU's profiler samples it (prof.c). On the main CPU (stack below 0x80000000) the hooks do nothing. */
#define FSTACK ((volatile uint32_t *)0x00013E00u)
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
    (void)site;
    int sp_;
    __asm__ volatile("move %0, $sp" : "=r"(sp_));
    if ((uint32_t)sp_ < 0x80000000u) return;
    uint32_t d = FSTACK[0];
    if (d < 100) FSTACK[1 + d] = (uint32_t)fn;
    FSTACK[0] = d + 1;
    g_me_phase_uc[1] = (uint32_t)fn;
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)fn; (void)site;
    int sp_;
    __asm__ volatile("move %0, $sp" : "=r"(sp_));
    if ((uint32_t)sp_ < 0x80000000u) return;
    uint32_t d = FSTACK[0];
    if (d) d--;
    FSTACK[0] = d;
    g_me_phase_uc[1] = d && d <= 100 ? FSTACK[d] : 0;
}
#endif

/* every 2 s (gpuvbl.c): an ordering-table walk pending for more than 0.5 s - where the ME is */
void me_prof_log(void)
{
    if (!g_walk_t0) return;
    uint64_t d = now_us() - g_walk_t0;
    if (d < 500000) return;
    logf_("me: walk pending %u ms: ME cmd %u arg %08x, walk at %06x after %u packets, ring tail %u head %u dma_idx %u, "
          "alive %u, GE req %u done %u\n", (unsigned)(d / 1000), (unsigned)SH->cur_cmd, (unsigned)SH->cur_arg,
          (unsigned)SH->walk_pos, (unsigned)SH->walk_pk, (unsigned)SH->tail, (unsigned)SH->head, (unsigned)g_dma_idx,
          (unsigned)SH->alive, (unsigned)SH->req_seq, (unsigned)SH->done_seq);
}
