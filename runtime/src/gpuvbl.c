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
static uint64_t g_work_max, g_vs_leave;
static int g_vs_wait;
void rt_vsync_enter(int waits)
{
    g_vs_wait = waits;
    if (!g_vs_wait || !g_vs_leave) return;
    uint64_t w = now_us() - g_vs_leave;
    static const uint32_t lim[6] = {20000, 25000, 30000, 33000, 40000, 50000};
    int b = 0;
    while (b < 6 && w >= lim[b]) b++;
    g_work_hist[b]++;
    if (w > g_work_max) g_work_max = w;
    {   /* deterministic per-race statistics: the same frames of a replay in every run (frame index since the race
         * overlay was loaded), "race work" every 300 frames: average work and frames over 33 ms */
        extern uint32_t g_overlay_loads;
        static uint32_t seen = ~0u, idx, n_over;
        static uint64_t sum;
        if (g_cur_overlay != 0) return;
        if (seen != g_overlay_loads) { seen = g_overlay_loads; idx = 0; sum = 0; n_over = 0; }
        sum += w;
        n_over += w > 33366;
        if (++idx % 300 == 0) {
            logf_("race work: frames %u-%u avg %u us, %u over 33.4 ms\n", (unsigned)(idx - 300), (unsigned)(idx - 1),
                  (unsigned)(sum / 300), (unsigned)n_over);
            sum = 0; n_over = 0;
        }
    }
}
void rt_vsync_leave(void) { if (g_vs_wait) g_vs_leave = now_us(); }

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
            logf_("pacing: lists after 1/2/3/4+ vblanks: %u %u %u %u\n", (unsigned)g_pace_hist[1],
                  (unsigned)g_pace_hist[2], (unsigned)g_pace_hist[3], (unsigned)g_pace_hist[4]);
            memset(g_pace_hist, 0, sizeof(uint32_t) * 5);
        }
        logf_("frame work (ms, VSync wait excluded) <20/<25/<30/<33/<40/<50/50+: %u %u %u %u %u %u %u, max %u\n",
              (unsigned)g_work_hist[0], (unsigned)g_work_hist[1], (unsigned)g_work_hist[2], (unsigned)g_work_hist[3],
              (unsigned)g_work_hist[4], (unsigned)g_work_hist[5], (unsigned)g_work_hist[6], (unsigned)(g_work_max / 1000));
        memset(g_work_hist, 0, sizeof g_work_hist);
        g_work_max = 0;
    }
}
