/* GT2 PSP runtime entry point. */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include "rt.h"
#include "gpu.h"
#include "cdrom.h"
#include "sio.h"
#include "card.h"
#include "spu.h"
#include "ge.h"
#include "prof.h"
#include "me.h"
#include <stdio.h>

PSP_MODULE_INFO("GT2PSP", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(4096);
PSP_HEAP_SIZE_KB(-2048);

static PsxCpu g_cpu;

/* SCUS_944.88 sits at LBA 24 of the GT2 Rev 2 disc (see extracted/index.json); it is read from the pack. */
#define EXE_LBA 24

static int load_exe(void)
{
    static uint8_t hdr[2048];
    if (!cdrom_read_data(EXE_LBA, 1, hdr) || memcmp(hdr, "PS-X EXE", 8)) return 0;
    uint32_t pc0 = *(uint32_t *)(hdr + 0x10), taddr = *(uint32_t *)(hdr + 0x18), tsize = *(uint32_t *)(hdr + 0x1C);
    if (tsize > 0x1F0000 || (tsize & 2047)) return 0;
    if (!cdrom_read_data(EXE_LBA + 1, tsize / 2048, g_ram + (taddr & 0x1FFFFF))) return 0;
    logf_("%s: pc0=%08x text=%08x size=%08x\n", g_game == GAME_ARC ? "SCUS_944.55" : "SCUS_944.88", (unsigned)pc0,
          (unsigned)taddr, (unsigned)tsize);
    return 1;
}

/* Optional settings.txt next to the EBOOT, lines "name=0|1": widescreen, hires, pgxp, filter, hud_anchor. */
int g_log_prio;   /* settings.txt log_prio (log.c), 0 = keep 0x1F */

static void load_settings(void)
{
    extern int g_hud_anchor, g_near_clip, g_pad_analog, g_audio, g_autoexit, g_frames_forced, g_show_fps, g_gebench, g_membench, g_varena_uncached, g_walklog, g_ws_menus, g_gedump, g_gebenchfile, g_varena_edram, g_sharp, g_async_present, g_list_uncached, g_hud_defer, g_me_gpu, g_fps60, g_cd_prefetch, g_fx60, g_lowlod, g_performance, g_lr_shift, g_dpad_shift, g_fps60_dyn, g_track_lod, g_fps60_auto, g_race_buttons, g_period_us, g_fps60_flip[4], g_cd_pause_us, g_cd_seek_us, g_cd_first_us, g_card_fast, g_card_event_us, g_cd_ack_us, g_cd_legacy, g_me_snapshot;
    static const struct { const char *name; int *var; } opts[] = {
        {"widescreen", &g_widescreen}, {"hires", &g_hires}, {"pgxp", &g_pgxp}, {"filter", &g_tex_filter},
        {"hud_anchor", &g_hud_anchor}, {"near_clip", &g_near_clip}, {"analog", &g_pad_analog}, {"audio", &g_audio}, {"autoexit", &g_autoexit}, {"frames1x", &g_frames_forced}, {"ge", &g_use_ge}, {"fps", &g_show_fps}, {"gebench", &g_gebench}, {"membench", &g_membench}, {"varena_uncached", &g_varena_uncached}, {"walklog", &g_walklog}, {"widescreen_menus", &g_ws_menus}, {"gedump", &g_gedump}, {"gebenchfile", &g_gebenchfile}, {"varena_edram", &g_varena_edram}, {"sharp", &g_sharp}, {"async_present", &g_async_present}, {"list_uncached", &g_list_uncached}, {"hud_defer", &g_hud_defer}, {"me_gpu", &g_me_gpu}, {"fps60", &g_fps60}, {"cd_prefetch", &g_cd_prefetch}, {"fps60_effects", &g_fx60}, {"lowlod", &g_lowlod}, {"performance", &g_performance}, {"lr_shift", &g_lr_shift}, {"dpad_shift", &g_dpad_shift}, {"fps60_dynamic", &g_fps60_dyn}, {"track_lod", &g_track_lod}, {"fps60_auto", &g_fps60_auto}, {"race_buttons", &g_race_buttons}, {"cd_period_us", &g_period_us},
        {"cd_pause_us", &g_cd_pause_us}, {"cd_seek_us", &g_cd_seek_us}, {"cd_first_us", &g_cd_first_us}, {"card_fast", &g_card_fast}, {"cd_ack_us", &g_cd_ack_us}, {"cd_legacy", &g_cd_legacy}, {"me_snapshot", &g_me_snapshot}, {"log_prio", &g_log_prio}, {"card_event_us", &g_card_event_us},
        {"fps60_flip1", &g_fps60_flip[0]}, {"fps60_flip2", &g_fps60_flip[1]}, {"fps60_flip3", &g_fps60_flip[2]}, {"fps60_flip4", &g_fps60_flip[3]},
    };
    char path[300], line[64];
    snprintf(path, sizeof path, "%ssettings.txt", g_basedir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (unsigned i = 0; i < sizeof opts / sizeof opts[0]; i++)
                if (!strcmp(line, opts[i].name)) *opts[i].var = (opts[i].var == &g_autoexit || opts[i].var == &g_gebench || opts[i].var == &g_walklog || opts[i].var == &g_membench || opts[i].var == &g_gedump || opts[i].var == &g_period_us || opts[i].var == &g_cd_pause_us || opts[i].var == &g_cd_seek_us || opts[i].var == &g_cd_first_us || opts[i].var == &g_card_event_us || opts[i].var == &g_card_fast || opts[i].var == &g_cd_ack_us || opts[i].var == &g_log_prio || (opts[i].var >= &g_fps60_flip[0] && opts[i].var <= &g_fps60_flip[3])) ? atoi(eq + 1) : atoi(eq + 1) != 0;
        }
        fclose(f);
    }
    g_ws_menus &= g_widescreen;
    logf_("settings: widescreen %d hires %d pgxp %d filter %d hud_anchor %d\n", g_widescreen, g_hires, g_pgxp,
          g_tex_filter, g_hud_anchor);
}

/* HOME button: the XMB "quit game" dialog only appears with a registered exit callback. */
/* Quit: stop the audio threads first (they release their channels; a reserved SRC channel or a thread blocked in an
 * audio output can keep sceKernelExitGame from finishing), then close the log with a bounded wait. The game thread is
 * parked by rt_poll (core.c) once g_quit is set. */
void quit_game(const char *why)
{
    extern volatile int g_quit, g_audio_running;
    static volatile int quitting;
    if (quitting) for (;;) sceKernelDelayThread(100000);   /* second request (another thread): the first one finishes */
    quitting = 1;
    {
        extern void me_stop(void);
        me_stop();
    }
    logf_("exit requested (%s)\n", why);
    g_quit = 1;
    for (int i = 0; i < 100 && g_audio_running > 0; i++) sceKernelDelayThread(5000);
    logf_("audio stopped (%d still running)\n", g_audio_running);
#ifdef GT2_PGO_GEN
    {   /* profile-guided optimization, instrumented build: write the .gcda files (host0:/pgo, see scripts/pgo.sh) */
        extern void __gcov_dump(void);
        logf_("writing profile data\n");
        __gcov_dump();
        logf_("profile data written\n");
    }
#endif
    log_close();
    sceKernelExitGame();
}

static int exit_callback(int a, int b, void *arg)
{
    (void)a; (void)b; (void)arg;
    quit_game("HOME");
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("exit", exit_callback, 0);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

int main(int argc, char **argv)
{
    char dir[256] = "";
    if (argc > 0 && argv[0]) {
        strncpy(dir, argv[0], sizeof dir - 1);
        char *s = strrchr(dir, '/');
        if (s) s[1] = 0; else dir[0] = 0;
    }
    log_init(dir);
    SceUID cbt = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, 0);
    if (cbt >= 0) sceKernelStartThread(cbt, 0, 0);
    scePowerSetClockFrequency(333, 333, 166);   /* default is 222 MHz */
    logf_("CPU %d MHz, bus %d MHz\n", scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
    {   /* load addresses (cache set mapping differs between launches if these move relative to each other) */
        extern uint8_t g_ram[];
        int local = 0;
        void *heap = malloc(64);
        logf_("addresses: text %p ram %p stack %p heap %p\n", (void *)&load_settings, (void *)g_ram, (void *)&local, heap);
        free(heap);
    }
    logf_("GT2 PSP runtime, base dir '%s'\n", dir);
    load_settings();
    {   /* freeze hunt 2026-11-21: today's changes can be switched off one by one */
        extern void log_set_prio(int prio);
        extern int g_cd_legacy, g_card_fast, g_me_snapshot;
        log_set_prio(g_log_prio);
        logf_("suspects: cd_legacy %d card_fast %d me_snapshot %d log_prio 0x%x\n", g_cd_legacy, g_card_fast,
              g_me_snapshot, g_log_prio ? g_log_prio : 0x1F);
    }
    logf_("game: %s\n", g_game == GAME_ARC ? "Arcade Mode (SCUS_944.55)" : "Simulation Mode (SCUS_944.88)");
    if (g_game == GAME_ARC) {   /* the game patches (16:9 view, 60 fps, LOD) exist for the Simulation disc only */
        extern int g_fps60, g_performance, g_lowlod, g_ws_menus;
        g_widescreen = g_ws_menus = 0;
        g_fps60 = g_performance = g_lowlod = 0;
    }
    {   /* performance mode: 60 fps with the game's own trade-offs (no mirror/tire smoke) and low-detail opponents */
        extern int g_fps60, g_fx60, g_lowlod, g_performance;
        if (g_performance) { g_fps60 = 1; g_fx60 = 0; g_lowlod = 1; if (!g_track_lod) g_track_lod = 2; }
        logf_("mode: %s (fps60 %d, fps60_effects %d, lowlod %d)\n", g_performance ? "performance" : "quality", g_fps60, g_fx60,
              g_lowlod);
    }
    prof_init();

    extern void gte_init(void);
    gte_init();
    gpu_init();
    io_init();
    cdrom_init();
    sio_init();
    card_init();
    spu_init();
    extern void membench(void);
    membench();
    ge_bench_file();
    {   /* GPU emulation on the Media Engine (me.c): needs the 1x path with asynchronous presentation */
        extern int g_hires, g_gebench, g_gedump, g_async_present;
        if (g_hires || g_gebench || g_gedump || !g_async_present) g_me_gpu = 0;
        extern void ge_sync(void);
        ge_sync();
        me_gpu_start();
    }
    if (!load_exe()) fatal(0, "cannot load the game from GT2.PAK (EBOOT directory)");

    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.r[29] = 0x801FFF00u;            /* SYSTEM.CNF STACK */
    g_cpu.cop0[12] = 0x40000401u;          /* SR: COP2 enabled, IEc, IM2 */
    /* C0 table entry 6 (exception handler, as in the real BIOS at 0xC80): PsyQ's libapi reads two instruction words
     * from it (+112/+116, a lui/ori address pair) and patches the handler code at that address + 40. Point both
     * into the unused kernel RAM above 0x400 - physical 0x000-0x3FF holds the scratchpad (gt2rt.h). */
    mem_w32(0x674u + 6 * 4, 0x00000C80u);
    mem_w32(0xC80u + 112, 0x3C1A0000u);    /* lui k0, 0x0000 */
    mem_w32(0xC80u + 116, 0x375A1000u);    /* ori k0, k0, 0x1000 -> patches land at 0x1028 */
    run_program(&g_cpu);
    fatal(&g_cpu, "program finished");
    return 0;
}
