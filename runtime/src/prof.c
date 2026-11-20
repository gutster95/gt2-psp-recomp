/* Sampling profiler: a higher-priority thread wakes every millisecond and records what the main thread is doing -
 * the runtime phase (g_phase) and, for game code, the most recently entered recompiled function (TRACE ring).
 * Every 10 s the distribution and the top functions are written to the log. Enabled when profile.txt exists. */
#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include "rt.h"
#include "prof.h"

/* g_phase: scratchpad word, see prof.h */

static const char *const NAMES[PH_COUNT] = {"game code", "GTE", "GPU lists (GE build)", "present/GE wait", "IRQ/poll",
                                           "CD", "other", "GE texture setup", "GE draw emit", "GE prim decode", "GE touch", "GE sync wait"};
static uint32_t g_ph[PH_COUNT], g_total;
volatile uint32_t g_poll_fn;

#define FN_SLOTS 4096
static uint32_t g_fn_addr[FN_SLOTS], g_fn_cnt[FN_SLOTS];

#ifdef GT2_ME_PROF
static uint32_t g_meph[16], g_metotal;
#endif
#ifdef GT2_ME_FPROF
static uint32_t g_mf_addr[512], g_mf_cnt[512];
#endif

static void sample(void)
{
#ifdef GT2_ME_PROF
    { extern int g_me_active; if (g_me_active) { g_meph[*g_me_phase_uc & 15]++; g_metotal++; } }
#ifdef GT2_ME_FPROF
    {   /* ME function samples (innermost instrumented function, me.c) */
        extern int g_me_active;
        uint32_t fn = g_me_active ? g_me_phase_uc[1] : 0;
        if (*g_me_phase_uc == 12) fn = 1;       /* idle */
        uint32_t h = (fn * 0x9E3779B1u) >> 23;
        for (int i = 0; i < 16; i++) {
            uint32_t j = (h + i) & 511;
            if (g_mf_addr[j] == fn || !g_mf_cnt[j]) { g_mf_addr[j] = fn; g_mf_cnt[j]++; break; }
        }
    }
#endif
#endif
    int ph = g_phase;
    if (ph < 0 || ph >= PH_COUNT) ph = PH_OTHER;
    g_ph[ph]++;
    g_total++;
#ifdef GT2_POLL_FN
    if (ph != PH_GAME && ph != PH_IRQ) return;
    uint32_t fn = g_poll_fn ^ (ph == PH_IRQ ? 0x40000000u : 0x80000000u);   /* loop that polled last; 0x4x: in an IRQ */
#else
    if (ph != PH_GAME) return;
    uint32_t fn = g_trace[(g_trace_pos - 1) & (TRACE_SIZE - 1)];
#endif
    uint32_t h = (fn * 0x9E3779B1u) >> 20;
    for (int i = 0; i < 16; i++) {
        uint32_t j = (h + i) & (FN_SLOTS - 1);
        if (g_fn_addr[j] == fn || !g_fn_addr[j]) { g_fn_addr[j] = fn; g_fn_cnt[j]++; return; }
    }
}

static void report(void)
{
    if (!g_total) return;
    logf_("profile (%u samples):", (unsigned)g_total);
    for (int i = 0; i < PH_COUNT; i++) logf_(" %s %u%%", NAMES[i], (unsigned)(g_ph[i] * 100 / g_total));
    logf_("\n");
#ifdef GT2_ME_PROF
    if (g_metotal) {
        static const char *const MN[13] = {"game", "GTE", "GPU walk/lists", "present", "IRQ", "CD", "other", "tex setup",
                                           "draw emit", "prim decode", "touch", "GE sync wait", "idle (no command)"};
        logf_("ME profile (%u samples):", (unsigned)g_metotal);
        for (int i = 0; i < 13; i++) if (g_meph[i]) logf_(" %s %u%%", MN[i], (unsigned)(g_meph[i] * 100 / g_metotal));
        logf_("\n");
        memset(g_meph, 0, sizeof g_meph);
        g_metotal = 0;
    }
#endif
#ifdef GT2_ME_FPROF
    for (int k = 0; k < 40; k++) {
        int best = -1;
        for (int j = 0; j < 512; j++) if (g_mf_cnt[j] && (best < 0 || g_mf_cnt[j] > g_mf_cnt[best])) best = j;
        if (best < 0) break;
        logf_("  mefn %08x %u\n", (unsigned)g_mf_addr[best], (unsigned)g_mf_cnt[best]);
        g_mf_cnt[best] = 0;
    }
    memset(g_mf_addr, 0, sizeof g_mf_addr);
#endif
    for (int k = 0; k < 100; k++) {                      /* top functions by samples */
        int best = -1;
        for (int j = 0; j < FN_SLOTS; j++)
            if (g_fn_cnt[j] && (best < 0 || g_fn_cnt[j] > g_fn_cnt[best])) best = j;
        if (best < 0) break;
        logf_("  fn %08x %u\n", (unsigned)g_fn_addr[best], (unsigned)g_fn_cnt[best]);
        g_fn_cnt[best] = 0;
    }
    extern uint32_t g_gte_count[64];
    logf_("  GTE commands per 10 s:");
    for (int i = 0; i < 64; i++) if (g_gte_count[i]) logf_(" %02x:%u", i, (unsigned)g_gte_count[i]);
    logf_("\n");
    memset(g_gte_count, 0, sizeof(uint32_t) * 64);
    memset(g_ph, 0, sizeof g_ph);
    memset(g_fn_addr, 0, sizeof g_fn_addr);
    memset(g_fn_cnt, 0, sizeof g_fn_cnt);
    g_total = 0;
}

static int prof_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uint64_t next = now_us() + 10000000;
    for (;;) {
        sceKernelDelayThread(1000);
        sample();
        if (now_us() >= next) { next += 10000000; report(); }
    }
    return 0;
}

void prof_init(void)
{
    char path[300];
    snprintf(path, sizeof path, "%sprofile.txt", g_basedir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    fclose(f);
    SceUID t = sceKernelCreateThread("prof", prof_thread, 0x10, 0x4000, PSP_THREAD_ATTR_USER, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
    logf_("profiler started\n");
}
