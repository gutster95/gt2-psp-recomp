// Media Engine benchmark (GT2 port): the same kernels timed on the ME and on the main CPU, plus memory contention.
// Built with scripts/me/build-sample.sh bench against mcidclan's psp-media-engine-custom-core (MIT).
// Results on screen and in host0:/mebench.txt (PSPLink) or next to the EBOOT.
#include <pspdebug.h>
#include <psppower.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psputils.h>
#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <me-core-mapper/me-core.h>

PSP_MODULE_INFO("me-bench", 0, 1, 1);
PSP_HEAP_SIZE_KB(-1024);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_VFPU | PSP_THREAD_ATTR_USER);

meLibSetSharedUncached32(16);
#define shCmd    (meLibSharedMemory[0])   // command for the ME (0 = idle)
#define shA0     (meLibSharedMemory[1])
#define shA1     (meLibSharedMemory[2])
#define shA2     (meLibSharedMemory[3])
#define shDone   (meLibSharedMemory[4])   // sequence number of the finished command
#define shRes    (meLibSharedMemory[5])
#define shSeq    (meLibSharedMemory[6])
#define shExit   (meLibSharedMemory[7])
#define shAlive  (meLibSharedMemory[8])

// ---- kernels (no calls, no syscalls: run on either CPU) ----------------------------------------------------------
__attribute__((noinline)) static u32 k_alu(u32 n)               // dependent integer chain: ~1 op per cycle
{
    u32 x = 1;
    for (u32 i = 0; i < n; i++) x = (x ^ i) + (x >> 3);
    return x;
}
__attribute__((noinline)) static u32 k_mac(u32 n)               // GTE-like dot3 with HI/LO (mult/madd/madd)
{
    int32_t a = 1234, b = -567, c = 89, x = 3, y = 5, z = 7;
    u32 acc = 0;
    for (u32 i = 0; i < n; i++) {
        int32_t hi; u32 lo;
        asm volatile("mult %2, %3\n\tmadd %4, %5\n\tmadd %6, %7\n\tmfhi %0\n\tmflo %1"
                     : "=r"(hi), "=r"(lo) : "r"(a), "r"(x), "r"(b), "r"(y), "r"(c), "r"(z) : "hi", "lo");
        acc += lo ^ (u32)hi;
        x += 1; y -= 1; z ^= (int32_t)i;
    }
    return acc;
}
__attribute__((noinline)) static u32 k_read(u32 addr, u32 bytes, u32 reps)   // sequential word reads
{
    u32 s = 0;
    for (u32 r = 0; r < reps; r++) {
        const volatile u32 *p = (const volatile u32 *)addr;
        for (u32 i = 0; i < bytes / 4; i += 4) s += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
    }
    return s;
}
__attribute__((noinline)) static u32 k_write(u32 addr, u32 bytes, u32 reps)  // sequential word writes
{
    for (u32 r = 0; r < reps; r++) {
        volatile u32 *p = (volatile u32 *)addr;
        for (u32 i = 0; i < bytes / 4; i += 4) { p[i] = r; p[i + 1] = i; p[i + 2] = r; p[i + 3] = i; }
    }
    return reps;
}
__attribute__((noinline)) static u32 k_rand(u32 addr, u32 bytes, u32 n)      // dependent random reads (misses)
{
    const volatile u32 *p = (const volatile u32 *)addr;
    u32 mask = bytes / 4 - 1, j = 0, s = 0;
    for (u32 i = 0; i < n; i++) { j = (j * 1664525u + 1013904223u + s) & mask; s += p[j] & 1; }
    return s;
}

static u32 run_kernel(u32 cmd, u32 a0, u32 a1, u32 a2)
{
    switch (cmd) {
    case 1: return k_alu(a0);
    case 2: return k_mac(a0);
    case 3: return k_read(a0, a1, a2);
    case 4: return k_write(a0, a1, a2);
    case 5: return k_rand(a0, a1, a2);
    }
    return 0;
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
        u32 seq = shSeq;
        if (seq != seen && shCmd) {
            u32 r = run_kernel(shCmd, shA0, shA1, shA2);
            seen = seq;
            shRes = r;
            meLibSync();
            shDone = seq;
            meLibSync();
        }
    }
    shExit = 2;
    meLibHalt();
}

// ---- main CPU side --------------------------------------------------------------------------------------------
static char g_out[8192];
static int g_outn;
static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
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

static u64 now(void) { return sceKernelGetSystemTimeWide(); }

// start a command on the ME without waiting
static u32 g_seq;
static void me_start(u32 cmd, u32 a0, u32 a1, u32 a2)
{
    shDone = 0;
    shA0 = a0; shA1 = a1; shA2 = a2; shCmd = cmd;
    meLibSync();
    shSeq = ++g_seq;
    meLibSync();
}
static int me_wait(u64 timeout_us)
{
    u64 t0 = now();
    while (shDone != g_seq) if (now() - t0 > timeout_us) return 0;
    return 1;
}
static u32 me_time(u32 cmd, u32 a0, u32 a1, u32 a2)              // microseconds, 0 = timeout
{
    u64 t0 = now();
    me_start(cmd, a0, a1, a2);
    if (!me_wait(10000000)) return 0;
    return (u32)(now() - t0);
}
static u32 cpu_time(u32 cmd, u32 a0, u32 a1, u32 a2)
{
    u64 t0 = now();
    volatile u32 r = run_kernel(cmd, a0, a1, a2);
    (void)r;
    return (u32)(now() - t0);
}

static void save(void)
{
    SceUID fd = sceIoOpen("mebench.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) { sceIoWrite(fd, g_out, g_outn); sceIoClose(fd); }
}

static void finish(void)
{
    shExit = 1;
    save();
    sceKernelDelayThread(2000000);
    sceKernelExitGame();
}

int main()
{
    pspDebugScreenInit();
    scePowerSetClockFrequency(333, 333, 166);
    out("ME bench  CPU %d MHz bus %d MHz\n", scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
    const int tableId = meLibDefaultInit();
    out("table %d\n", tableId);
    if (tableId != ME_CORE_T2_IMG_TABLE && tableId != ME_CORE_IMG_TABLE) { out("no ME table\n"); finish(); }
    u64 t0 = now();
    while (shAlive == 0 && now() - t0 < 2000000) {}
    out("ME alive: %lu\n", (unsigned long)shAlive);
    if (!shAlive) { out("ME not running\n"); finish(); }

    const u32 MB = 1024 * 1024;
    u8 *buf = (u8 *)memalign(64, 2 * MB);
    memset(buf, 1, 2 * MB);
    sceKernelDcacheWritebackInvalidateAll();
    const u32 cached = (u32)buf, uncached = 0x40000000u | (u32)buf;
    const u32 edram_me = 0x80200000u;                 // ME local eDRAM, 2 MB in (stack is at the 4 MB top)
    const u32 N = 10000000;

    out("\nkernel                         ME us   CPU us\n");
    out("alu chain 10M                %7lu  %7lu\n", (unsigned long)me_time(1, N, 0, 0), (unsigned long)cpu_time(1, N, 0, 0));
    out("mac dot3 10M                 %7lu  %7lu\n", (unsigned long)me_time(2, N, 0, 0), (unsigned long)cpu_time(2, N, 0, 0));
    out("read RAM cached 1MBx8        %7lu  %7lu\n", (unsigned long)me_time(3, cached, MB, 8), (unsigned long)cpu_time(3, cached, MB, 8));
    out("read RAM 16KB x512 (hits)    %7lu  %7lu\n", (unsigned long)me_time(3, cached, 16384, 512), (unsigned long)cpu_time(3, cached, 16384, 512));
    out("read RAM uncached 1MBx8      %7lu  %7lu\n", (unsigned long)me_time(3, uncached, MB, 8), (unsigned long)cpu_time(3, uncached, MB, 8));
    out("write RAM uncached 1MBx8     %7lu  %7lu\n", (unsigned long)me_time(4, uncached, MB, 8), (unsigned long)cpu_time(4, uncached, MB, 8));
    out("write RAM cached 1MBx8       %7lu  %7lu\n", (unsigned long)me_time(4, cached, MB, 8), (unsigned long)cpu_time(4, cached, MB, 8));
    sceKernelDcacheWritebackInvalidateAll();
    out("random read RAM 2MB 1M       %7lu  %7lu\n", (unsigned long)me_time(5, cached, 2 * MB, 1000000), (unsigned long)cpu_time(5, cached, 2 * MB, 1000000));
    out("ME eDRAM write 1MBx8         %7lu\n", (unsigned long)me_time(4, edram_me, MB, 8));
    out("ME eDRAM read 1MBx8          %7lu\n", (unsigned long)me_time(3, edram_me, MB, 8));
    out("ME eDRAM random 1MB 1M       %7lu\n", (unsigned long)me_time(5, edram_me, MB, 1000000));
    out("GE eDRAM read (VRAM) 1MBx8   %7lu  %7lu\n", (unsigned long)me_time(3, 0x44000000u, MB, 8), (unsigned long)cpu_time(3, 0x44000000u, MB, 8));

    // contention: the main CPU's RAM read speed alone and while the ME streams uncached writes into RAM
    u32 alone = cpu_time(3, cached, MB, 16);
    me_start(4, uncached + MB, MB, 200);
    u32 busy = cpu_time(3, cached, MB, 16);
    int ok = me_wait(20000000);
    out("\ncontention: CPU read 1MBx16 alone %lu us, with ME writing RAM %lu us (%s)\n", (unsigned long)alone,
        (unsigned long)busy, ok ? "ok" : "ME timeout");
    alone = cpu_time(1, N, 0, 0);
    me_start(3, uncached, MB, 100);
    busy = cpu_time(1, N, 0, 0);
    ok = me_wait(20000000);
    out("contention: CPU alu 10M alone %lu us, with ME reading RAM %lu us (%s)\n", (unsigned long)alone,
        (unsigned long)busy, ok ? "ok" : "ME timeout");
    out("\ndone\n");
    finish();
    return 0;
}
