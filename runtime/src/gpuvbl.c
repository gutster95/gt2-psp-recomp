/* Main-CPU side of the GPU: vblank (presentation request, dumps, screenshot combo, autoexit) and the frame
 * statistics collected by the game-side hooks. Kept out of gpu.c, whose data belongs to the Media Engine when it runs
 * the GPU path (me.c: the main CPU must not write that data block). */
#include <stdio.h>
#include <string.h>
#include <pspctrl.h>
#include "rt.h"
#include "gpu.h"
#include "ge.h"
#include "me.h"

void gpu_snapshot(void);
int vram_fwrite(FILE *f);
void gpu_present_core(void);

/* Frame work statistics: time from the return of one waiting VSync call to the next one (recomp.py hooks). */
static uint32_t g_work_hist[7];
static uint64_t g_work_max, g_vs_leave, g_vs_leave_wait;
static int g_vs_wait;
/* idle time: time the game spends inside its frame wait (0x8007d23c) or a waiting VSync - the real headroom */
static uint64_t g_idle_t0, g_idle_us;
static uint32_t g_idle_n;
void rt_vsync_enter(int waits)
{
    g_idle_t0 = now_us();
    g_vs_wait = waits;
    if (!g_vs_wait || !g_vs_leave) return;
    uint64_t w = now_us() - g_vs_leave;
    uint64_t mew = g_me_wait_us - g_vs_leave_wait;     /* of that, waiting for the Media Engine (DrawSync) */
    static const uint32_t lim[6] = {20000, 25000, 30000, 33000, 40000, 50000};
    int b = 0;
    while (b < 6 && w >= lim[b]) b++;
    g_work_hist[b]++;
    if (w > g_work_max) g_work_max = w;
    {   /* deterministic per-race statistics: the same frames of a replay in every run (frame index since the race
         * overlay was loaded), "race work" every 300 frames: average work and frames over 33 ms */
        extern uint32_t g_overlay_loads;
        static uint32_t seen = ~0u, idx, n_over, n_over_net;
        static uint64_t sum, sum_wait, max_net;
        if (g_cur_overlay != 0) return;
        if (seen != g_overlay_loads) { seen = g_overlay_loads; idx = 0; sum = 0; n_over = 0; sum_wait = 0; n_over_net = 0; max_net = 0; }
        uint64_t net = w - mew;                         /* main CPU work: game code, GTE, I/O emulation */
        sum += w;
        sum_wait += mew;
        n_over += w > 33366;
        n_over_net += net > 33366;
        if (net > max_net) max_net = net;
        if (++idx % 300 == 0) {
            logf_("race work: frames %u-%u avg %u us, %u over 33.4 ms | net (without ME wait) avg %u us, max %u us, %u over 33.4 ms\n",
                  (unsigned)(idx - 300), (unsigned)(idx - 1), (unsigned)(sum / 300), (unsigned)n_over,
                  (unsigned)((sum - sum_wait) / 300), (unsigned)max_net, (unsigned)n_over_net);
            sum = 0; n_over = 0; sum_wait = 0; n_over_net = 0; max_net = 0;
        }
    }
}
void rt_vsync_leave(void)
{
    uint64_t t = now_us();
    if (g_idle_t0) { g_idle_us += t - g_idle_t0; g_idle_n++; g_idle_t0 = 0; }
    if (g_vs_wait) { g_vs_leave = t; g_vs_leave_wait = g_me_wait_us; }
}

/* vblank on the main CPU: presentation (posted to the ME with me_gpu), debug dumps, screenshot combo, autoexit and
 * the statistics written by the main CPU (frame pacing, frame work from the game-side hooks) */
void gpu_present(void)
{
    static int frame;
    if (g_me_active) me_post_vblank();
    else gpu_present_core();
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
    if (dumps && !g_me_active && (frame >= (dumps_from ? (dumps_from < 0 ? -dumps_from : dumps_from) : 600) && frame % (dumps_from < 0 ? 30 : 150) == 0 && frame <= 9000)) {
        char path[300];
        snprintf(path, sizeof path, "%svram%d.bin", g_basedir, frame);
        ge_sync();
        FILE *f = dumps_from > 0 ? 0 : fopen(path, "wb");   /* negative start frame: VRAM dumps too */
        if (f) { vram_fwrite(f); fclose(f); }
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
        if (down && !combo_down && !g_me_active) gpu_snapshot();
        combo_down = down;
    }
    {   /* settings.txt snap_at1=<vblank>, snap_at2=<interval>: 16 timed snapshots with the real time logged (game
         * clock and replay comparisons against PCSX-Redux) */
        extern int g_snap_at[2];
        extern uint32_t g_vblank_count;
        static uint32_t taken;   /* snapshots so far: the next one is due at s0 + taken * d (this check does not
                                  * run on every vblank in races, an exact vblank match missed most of them) */
        uint32_t s0 = (uint32_t)g_snap_at[0], d = g_snap_at[1] > 0 ? (uint32_t)g_snap_at[1] : 600;
        if (s0 && taken < 16 && g_vblank_count >= s0 + taken * d && !g_me_active) {
            taken++;
            logf_("timed snapshot at vblank %u, real time %u ms\n", (unsigned)g_vblank_count, (unsigned)(now_us() / 1000));
            gpu_snapshot();
        }
    }
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
        {
            extern uint32_t g_pace_hist[5];
            extern uint32_t g_step_hist[4];
            extern int g_lock30;
            extern uint32_t g_lock_switches;
            if (g_step_hist[1] + g_step_hist[2])
                logf_("fps60 steps: %u x 1, %u x 2, %u frames with debt; locked to 30: %d, %u switches\n",
                      (unsigned)g_step_hist[1], (unsigned)g_step_hist[2], (unsigned)g_step_hist[3], g_lock30,
                      (unsigned)g_lock_switches);
            memset(g_step_hist, 0, sizeof(uint32_t) * 4);
            logf_("pacing: lists after 1/2/3/4+ vblanks: %u %u %u %u\n", (unsigned)g_pace_hist[1],
                  (unsigned)g_pace_hist[2], (unsigned)g_pace_hist[3], (unsigned)g_pace_hist[4]);
            memset(g_pace_hist, 0, sizeof(uint32_t) * 5);
        }
        logf_("frame work (ms, VSync wait excluded) <20/<25/<30/<33/<40/<50/50+: %u %u %u %u %u %u %u, max %u\n",
              (unsigned)g_work_hist[0], (unsigned)g_work_hist[1], (unsigned)g_work_hist[2], (unsigned)g_work_hist[3],
              (unsigned)g_work_hist[4], (unsigned)g_work_hist[5], (unsigned)g_work_hist[6], (unsigned)(g_work_max / 1000));
        memset(g_work_hist, 0, sizeof g_work_hist);
        g_work_max = 0;
        logf_("idle: %u ms of 2 s in the game's frame wait (%u waits, avg %u us)\n", (unsigned)(g_idle_us / 1000),
              (unsigned)g_idle_n, (unsigned)(g_idle_n ? g_idle_us / g_idle_n : 0));
        g_idle_us = 0; g_idle_n = 0;
        if (g_me_active) {
            static uint64_t last_wait;
            logf_("me: %u walks avg %u us max %u us (post -> done), game waited %u x, %u ms in 2 s\n",
                  (unsigned)g_me_walks, (unsigned)(g_me_walks ? g_me_walk_us / g_me_walks : 0), (unsigned)g_me_walk_max,
                  (unsigned)g_me_waits, (unsigned)((g_me_wait_us - last_wait) / 1000));
            last_wait = g_me_wait_us;
            g_me_walks = g_me_walk_us = g_me_walk_max = g_me_waits = 0;
            extern void me_prof_log(void);
            me_prof_log();
            extern uint64_t g_ge_busy_us;
            extern uint32_t g_ge_lists;
            logf_("ge: %u lists, GE busy avg %u us per list, %u ms in 2 s\n", (unsigned)g_ge_lists,
                  (unsigned)(g_ge_lists ? g_ge_busy_us / g_ge_lists : 0), (unsigned)(g_ge_busy_us / 1000));
            g_ge_busy_us = 0; g_ge_lists = 0;
        }
    }
}
