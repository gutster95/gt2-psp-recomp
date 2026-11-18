/* GT2 PSP runtime entry point. */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <psppower.h>
#include "rt.h"
#include "gpu.h"
#include "cdrom.h"
#include "sio.h"
#include "card.h"
#include "spu.h"
#include "ge.h"
#include "prof.h"
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
    logf_("SCUS_944.88: pc0=%08x text=%08x size=%08x\n", (unsigned)pc0, (unsigned)taddr, (unsigned)tsize);
    return 1;
}

/* Optional settings.txt next to the EBOOT, lines "name=0|1": widescreen, hires, pgxp, filter, hud_anchor. */
static void load_settings(void)
{
    extern int g_hud_anchor, g_near_clip, g_pad_analog, g_audio, g_autoexit, g_frames_forced;
    static const struct { const char *name; int *var; } opts[] = {
        {"widescreen", &g_widescreen}, {"hires", &g_hires}, {"pgxp", &g_pgxp}, {"filter", &g_tex_filter},
        {"hud_anchor", &g_hud_anchor}, {"near_clip", &g_near_clip}, {"analog", &g_pad_analog}, {"audio", &g_audio}, {"autoexit", &g_autoexit}, {"frames1x", &g_frames_forced}, {"ge", &g_use_ge},
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
                if (!strcmp(line, opts[i].name)) *opts[i].var = opts[i].var == &g_autoexit ? atoi(eq + 1) : atoi(eq + 1) != 0;
        }
        fclose(f);
    }
    logf_("settings: widescreen %d hires %d pgxp %d filter %d hud_anchor %d\n", g_widescreen, g_hires, g_pgxp,
          g_tex_filter, g_hud_anchor);
}

/* HOME button: the XMB "quit game" dialog only appears with a registered exit callback. */
static int exit_callback(int a, int b, void *arg)
{
    (void)a; (void)b; (void)arg;
    logf_("exit requested (HOME)\n");
    log_close();
    sceKernelExitGame();
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
    logf_("GT2 PSP runtime, base dir '%s'\n", dir);
    load_settings();
    prof_init();

    extern void gte_init(void);
    gte_init();
    gpu_init();
    io_init();
    cdrom_init();
    sio_init();
    card_init();
    spu_init();
    if (!load_exe()) fatal(0, "cannot load the game from GT2.PAK (EBOOT directory)");

    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.r[29] = 0x801FFF00u;            /* SYSTEM.CNF STACK */
    g_cpu.cop0[12] = 0x40000401u;          /* SR: COP2 enabled, IEc, IM2 */
    run_program(&g_cpu);
    fatal(&g_cpu, "program finished");
    return 0;
}
