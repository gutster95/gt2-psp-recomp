/* Logging to <basedir>/gt2log.txt and fatal error screen. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include "rt.h"

char g_basedir[256];
static FILE *g_log;
static int g_lines;

void log_init(const char *dir)
{
    char path[300];
    strncpy(g_basedir, dir, sizeof g_basedir - 1);
    snprintf(path, sizeof path, "%sgt2log.txt", g_basedir);
    g_log = fopen(path, "w");
}

void logf_(const char *fmt, ...)
{
    if (!g_log || g_lines > 20000) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    g_lines++;
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
