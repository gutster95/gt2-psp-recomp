// Media Engine probe for the GT2 GPU offload design: ME local memory at the main CPU's scratchpad address,
// hardware system timer from the ME, cache coherency between the CPUs. Results in host0:/meprobe.txt.
#include <pspdebug.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <me-core-mapper/me-core.h>

PSP_MODULE_INFO("me-probe", 0, 1, 1);
PSP_HEAP_SIZE_KB(-1024);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_VFPU | PSP_THREAD_ATTR_USER);

meLibSetSharedUncached32(32);
#define shCmd   (meLibSharedMemory[0])
#define shSeq   (meLibSharedMemory[1])
#define shDone  (meLibSharedMemory[2])
#define shR     (&meLibSharedMemory[8])     // results [8..31]
#define shExit  (meLibSharedMemory[3])
#define shAlive (meLibSharedMemory[4])

static volatile u32 g_cached_var[16] __attribute__((aligned(64)));   // cached variable for the coherency test

static inline void me_dcache_wbinv_all(void)
{
    for (u32 i = 0; i < 8192; i += 64) { asm volatile("cache 0x14, 0(%0)\n\tcache 0x14, 0(%0)" :: "r"(i)); }
    asm volatile("sync");
}

__attribute__((noinline)) static void me_cmd(u32 cmd)
{
    volatile u32 *r = shR;
    switch (cmd) {
    case 1: {   // scratchpad address range: write a pattern, read it back
        volatile u32 *p = (volatile u32 *)0x00010000u;
        for (u32 i = 0; i < 1024; i++) p[i] = 0xA5000000u | i;
        u32 bad = 0;
        for (u32 i = 0; i < 1024; i++) bad += p[i] != (0xA5000000u | i);
        r[0] = bad; r[1] = p[5];
        break;
    }
    case 2: {   // hardware system timer
        volatile u32 *t = (volatile u32 *)0xBC600000u;
        u32 a = *t;
        for (volatile u32 i = 0; i < 3330000; i++) {}
        u32 b = *t;
        r[2] = a; r[3] = b;
        break;
    }
    case 3: r[4] = g_cached_var[0]; break;                      // cached read (may be stale)
    case 4: me_dcache_wbinv_all(); r[5] = g_cached_var[0]; break;   // after invalidating the ME's data cache
    case 5: g_cached_var[1] = 0x12345678u; break;               // cached write, not written back
    case 6: me_dcache_wbinv_all(); break;                       // write back
    case 7: {   // VRAM (GE eDRAM) seen from the ME under several aliases: read the main CPU's pattern, write back
        static const u32 base[4] = {0xA4100000u, 0x84100000u, 0x44100000u, 0x04100000u};
        for (int k = 0; k < 4; k++) {
            volatile u32 *v = (volatile u32 *)base[k];
            r[6 + k] = v[0];
            if (k == 0) v[2] = 0xBEEF0002u;
        }
        break;
    }
    }
}

__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
    HW_SYS_BUS_CLOCK_ENABLE = -1;
    meLibSync();
    meCoreBusClockPreserve(0x0f);
    u32 seen = 0;
    while (!shExit) {
        shAlive = shAlive + 1;
        u32 s = shSeq;
        if (s != seen) { me_cmd(shCmd); seen = s; meLibSync(); shDone = s; meLibSync(); }
    }
    shExit = 2;
    meLibHalt();
}

static char g_out[4096];
static int g_outn;
static void out(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pspDebugScreenPrintf("%s", buf);
    int n = (int)strlen(buf);
    if (g_outn + n < (int)sizeof g_out) { memcpy(g_out + g_outn, buf, n); g_outn += n; }
}
static u32 g_seq;
static void run(u32 cmd)
{
    shCmd = cmd; meLibSync(); shSeq = ++g_seq; meLibSync();
    u64 t0 = sceKernelGetSystemTimeWide();
    while (shDone != g_seq && sceKernelGetSystemTimeWide() - t0 < 3000000) {}
}
static void finish(void)
{
    shExit = 1;
    SceUID fd = sceIoOpen("meprobe.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) { sceIoWrite(fd, g_out, g_outn); sceIoClose(fd); }
    sceKernelDelayThread(1000000);
    sceKernelExitGame();
}

int main()
{
    pspDebugScreenInit();
    scePowerSetClockFrequency(333, 333, 166);
    const int tableId = meLibDefaultInit();
    out("table %d\n", tableId);
    if (tableId != ME_CORE_T2_IMG_TABLE && tableId != ME_CORE_IMG_TABLE) finish();
    u64 t0 = sceKernelGetSystemTimeWide();
    while (!shAlive && sceKernelGetSystemTimeWide() - t0 < 2000000) {}
    if (!shAlive) { out("ME not running\n"); finish(); }

    volatile u32 *spad = (volatile u32 *)0x00010000u;          // main CPU scratchpad
    spad[5] = 0x11111111u;
    run(1);
    out("1 scratchpad addr on ME: %lu bad words, ME p[5] %08lx, main scratchpad p[5] %08lx\n",
        (unsigned long)shR[0], (unsigned long)shR[1], (unsigned long)spad[5]);

    u32 m0 = sceKernelGetSystemTimeLow();
    run(2);
    u32 m1 = sceKernelGetSystemTimeLow();
    out("2 hw timer on ME: %08lx -> %08lx (delta %lu), main systime %08lx -> %08lx (delta %lu)\n",
        (unsigned long)shR[2], (unsigned long)shR[3], (unsigned long)(shR[3] - shR[2]), (unsigned long)m0,
        (unsigned long)m1, (unsigned long)(m1 - m0));

    g_cached_var[0] = 111; sceKernelDcacheWritebackInvalidateAll();
    run(3);
    u32 first = shR[4];
    g_cached_var[0] = 222; sceKernelDcacheWritebackInvalidateAll();
    run(3);
    u32 second = shR[4];
    run(4);
    out("3 coherency: ME read %lu, after main wrote 222: ME cached read %lu, after ME invalidate %lu\n",
        (unsigned long)first, (unsigned long)second, (unsigned long)shR[5]);
    run(5);
    sceKernelDcacheWritebackInvalidateAll();
    u32 before = g_cached_var[1];
    run(6);
    sceKernelDcacheWritebackInvalidateAll();
    out("4 ME write: main sees %08lx before ME writeback, %08lx after\n", (unsigned long)before,
        (unsigned long)g_cached_var[1]);
    volatile u32 *vr = (volatile u32 *)0x44100000u;
    vr[0] = 0xCAFE0000u; vr[1] = 0xCAFE0001u; vr[2] = 0;
    run(7);
    out("5 VRAM from ME (main wrote cafe0000): A4 %08lx 84 %08lx 44 %08lx 04 %08lx, main sees ME write via A4: %08lx\n",
        (unsigned long)shR[6], (unsigned long)shR[7], (unsigned long)shR[8], (unsigned long)shR[9], (unsigned long)vr[2]);
    out("done\n");
    finish();
    return 0;
}
