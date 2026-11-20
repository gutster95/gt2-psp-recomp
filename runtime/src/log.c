/* Logging to <basedir>/gt2log.txt and fatal error screen. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include "rt.h"
#include "me.h"

char g_basedir[256];
static FILE *g_log;
static int g_lines;
static char g_log_path[300];
static uint64_t g_log_reopen;
static SceUID g_log_sema = -1;            /* the audio threads log too */

void log_init(const char *dir)
{
    strncpy(g_basedir, dir, sizeof g_basedir - 1);
    snprintf(g_log_path, sizeof g_log_path, "%sgt2log.txt", g_basedir);
    g_log = fopen(g_log_path, "w");
    g_log_sema = sceKernelCreateSema("log", 0, 1, 1, 0);
}

void log_close(void)
{
    /* bounded wait: on the quit path a stuck holder must not keep the game from exiting */
    SceUInt timeout = 500000;
    int got = g_log_sema >= 0 && sceKernelWaitSema(g_log_sema, 1, &timeout) >= 0;
    if (g_log) fclose(g_log);
    g_log = 0;
    if (got) sceKernelSignalSema(g_log_sema, 1);
}

void logf_(const char *fmt, ...)
{
    if (me_on_me()) {                      /* on the Media Engine: no system calls, the main CPU prints it (me.c) */
        uint32_t a[14];
        va_list ap;
        va_start(ap, fmt);
        for (int i = 0; i < 14; i++) a[i] = va_arg(ap, uint32_t);
        va_end(ap);
        me_log(fmt, a, 14);
        return;
    }
    if (!g_log || g_lines > 20000) return;
    if (g_log_sema >= 0) sceKernelWaitSema(g_log_sema, 1, 0);
    if (!g_log) { if (g_log_sema >= 0) sceKernelSignalSema(g_log_sema, 1); return; }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    g_lines++;
    /* The memory stick's directory entry (file size) is only updated on close: without a periodic reopen, a
     * crash or power-off on real hardware leaves only the first few lines readable. */
    uint64_t t = now_us();
    if (g_lines < 64 || t - g_log_reopen > 1000000) {
        g_log_reopen = t;
        fclose(g_log);
        g_log = fopen(g_log_path, "a");
    }
    if (g_log_sema >= 0) sceKernelSignalSema(g_log_sema, 1);
}

void dump_trace(void)
{
    logf_("last functions (oldest first):\n");
    for (int i = 0; i < TRACE_SIZE; i++) {
        uint32_t a = g_trace[(g_trace_pos + i) & (TRACE_SIZE - 1)];
        if (a) logf_("  %08x\n", (unsigned)a);
    }
}

void fatal(PsxCpu *c, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logf_("FATAL: %s\n", msg);
    if (c) {
        for (int i = 0; i < 32; i += 4)
            logf_("  r%02d-%02d: %08x %08x %08x %08x\n", i, i + 3, (unsigned)c->r[i], (unsigned)c->r[i + 1],
                  (unsigned)c->r[i + 2], (unsigned)c->r[i + 3]);
    }
    dump_trace();
    if (g_log) fclose(g_log);
    g_log = 0;
    pspDebugScreenInit();
    pspDebugScreenPrintf("GT2 runtime: FATAL\n%s\n\nsee gt2log.txt\n", msg);
    sceKernelDelayThread(10 * 1000 * 1000);
    sceKernelExitGame();
    for (;;) sceKernelDelayThread(1000000);
}
