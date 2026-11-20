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
/* The log is written by a low-priority thread: formatting goes into a ring buffer, the writer appends it to the file
 * about once a second. Writing (and closing, which updates the file size on the memory stick) in the calling thread
 * stalled the game for tens of milliseconds whenever the 2-second statistics were printed - a visible hitch. */
#define LOG_RING (64 * 1024)
static char g_ring[LOG_RING];
static volatile uint32_t g_ring_w, g_ring_r;   /* free-running positions */
static SceUID g_log_fd = -1;
static int g_lines;
static char g_log_path[300];
static SceUID g_log_sema = -1;            /* the audio threads log too */
static SceUID g_log_wake = -1;
static SceUID g_log_io = -1;              /* one drain at a time (writer thread, fatal/quit path) */
static volatile int g_log_quit;

/* Writes what is in the ring; closes and reopens the file so the size on the stick stays current. */
static void log_drain(void)
{
    if (g_log_fd < 0) return;
    uint32_t w = g_ring_w, r = g_ring_r;
    if (w == r) return;
    SceUInt timeout = 2000000;
    if (g_log_io >= 0 && sceKernelWaitSema(g_log_io, 1, &timeout) < 0) return;
    r = g_ring_r;
    while (r != w) {
        uint32_t off = r % LOG_RING, n = w - r;
        if (n > LOG_RING - off) n = LOG_RING - off;
        sceIoWrite(g_log_fd, g_ring + off, n);
        r += n;
    }
    g_ring_r = r;
    if (!g_log_quit) {
        sceIoClose(g_log_fd);
        g_log_fd = sceIoOpen(g_log_path, PSP_O_WRONLY | PSP_O_APPEND, 0777);
    }
    if (g_log_io >= 0) sceKernelSignalSema(g_log_io, 1);
}

static int log_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!g_log_quit) {
        SceUInt timeout = 1000000;
        sceKernelWaitSema(g_log_wake, 1, &timeout);
        log_drain();
    }
    return 0;
}

void log_init(const char *dir)
{
    strncpy(g_basedir, dir, sizeof g_basedir - 1);
    snprintf(g_log_path, sizeof g_log_path, "%sgt2log.txt", g_basedir);
    g_log_fd = sceIoOpen(g_log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    g_log_sema = sceKernelCreateSema("log", 0, 1, 1, 0);
    g_log_wake = sceKernelCreateSema("logwake", 0, 0, 1, 0);
    g_log_io = sceKernelCreateSema("logio", 0, 1, 1, 0);
    SceUID t = sceKernelCreateThread("logw", log_thread, 0x48, 0x1000, PSP_THREAD_ATTR_USER, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
}

/* Synchronous: the fatal path and the quit path. */
static void log_flush_now(void)
{
    SceUInt timeout = 500000;
    int got = g_log_sema >= 0 && sceKernelWaitSema(g_log_sema, 1, &timeout) >= 0;
    log_drain();
    if (got) sceKernelSignalSema(g_log_sema, 1);
}

void log_close(void)
{
    g_log_quit = 1;
    log_flush_now();
    if (g_log_fd >= 0) sceIoClose(g_log_fd);
    g_log_fd = -1;
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
    if (g_log_fd < 0 || g_lines > 200000) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    if (g_log_sema >= 0) sceKernelWaitSema(g_log_sema, 1, 0);
    uint32_t w = g_ring_w;
    if (w - g_ring_r + (uint32_t)n <= LOG_RING) {        /* full: the line is dropped (never block the game) */
        for (int i = 0; i < n; i++) g_ring[(w + i) % LOG_RING] = line[i];
        g_ring_w = w + n;
    }
    g_lines++;
    int wake = g_lines < 64 || g_ring_w - g_ring_r > LOG_RING / 2;   /* boot lines promptly, and never overflow */
    if (g_log_sema >= 0) sceKernelSignalSema(g_log_sema, 1);
    if (wake && g_log_wake >= 0) sceKernelSignalSema(g_log_wake, 1);
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
    log_close();
    pspDebugScreenInit();
    pspDebugScreenPrintf("GT2 runtime: FATAL\n%s\n\nsee gt2log.txt\n", msg);
    sceKernelDelayThread(10 * 1000 * 1000);
    sceKernelExitGame();
    for (;;) sceKernelDelayThread(1000000);
}
