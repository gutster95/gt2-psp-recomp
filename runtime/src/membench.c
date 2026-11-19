/* settings.txt membench=1: measures the PSP's memory system once at startup (log): cached load hit, loads that miss
 * (stride 64 over 1 MB), dependent-chain misses, cached stores into fresh lines (write allocate), uncached stores. */
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include "rt.h"

int g_membench;

static uint8_t g_mb[1 << 20] __attribute__((aligned(64)));

#define N 200000
static uint64_t t_us(void) { return now_us(); }

void membench(void)
{
    if (!g_membench) return;
    volatile uint32_t *p = (volatile uint32_t *)g_mb;
    uint32_t sum = 0;
    for (int i = 0; i < (1 << 18); i++) p[i] = (uint32_t)i;
    sceKernelDcacheWritebackInvalidateAll();

    uint64_t t0 = t_us();
    for (int i = 0; i < N; i++) sum += p[0];
    uint64_t t1 = t_us();
    logf_("membench: load hit %u ns\n", (unsigned)((t1 - t0) * 1000 / N));

    /* independent misses: stride 64 over 1 MB */
    t0 = t_us();
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < (1 << 14); i++) sum += p[i * 16];
    t1 = t_us();
    logf_("membench: load miss (independent, stride 64) %u ns\n", (unsigned)((t1 - t0) * 1000 / (4 << 14)));

    /* dependent chain of misses: pointer chasing with a large stride */
    uint32_t *q = (uint32_t *)g_mb;
    for (int i = 0; i < (1 << 14); i++) q[i * 16] = (uint32_t)(((i * 7919 + 13) & ((1 << 14) - 1)) * 16);
    sceKernelDcacheWritebackInvalidateAll();
    uint32_t k = 0;
    t0 = t_us();
    for (int i = 0; i < (1 << 15); i++) k = q[k];
    t1 = t_us();
    logf_("membench: load miss (dependent chain) %u ns (%u)\n", (unsigned)((t1 - t0) * 1000 / (1 << 15)), (unsigned)k);

    /* cached stores into fresh lines (write allocate): one word per line */
    sceKernelDcacheWritebackInvalidateAll();
    t0 = t_us();
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < (1 << 14); i++) q[i * 16] = (uint32_t)i;
    t1 = t_us();
    logf_("membench: cached store, fresh line %u ns\n", (unsigned)((t1 - t0) * 1000 / (4 << 14)));

    /* cached stores, sequential full lines */
    t0 = t_us();
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < (1 << 18); i++) q[i] = (uint32_t)i;
    t1 = t_us();
    logf_("membench: cached store sequential %u ns per word\n", (unsigned)((t1 - t0) * 1000 / (4 << 18)));

    /* uncached stores, sequential */
    volatile uint32_t *u = (volatile uint32_t *)((uintptr_t)g_mb | 0x40000000u);
    t0 = t_us();
    for (int i = 0; i < (1 << 18); i++) u[i] = (uint32_t)i;
    t1 = t_us();
    logf_("membench: uncached store sequential %u ns per word\n", (unsigned)((t1 - t0) * 1000 / (1 << 18)));

    /* uncached loads */
    t0 = t_us();
    for (int i = 0; i < (1 << 16); i++) sum += u[i];
    t1 = t_us();
    logf_("membench: uncached load %u ns (%u)\n", (unsigned)((t1 - t0) * 1000 / (1 << 16)), (unsigned)sum);

    /* working set sweep: repeated loads over a buffer of size S, stride 64 */
    for (int sz = 4096; sz <= 65536; sz *= 2) {
        int lines = sz / 64;
        t0 = t_us();
        for (int r = 0; r < 200; r++)
            for (int i = 0; i < lines; i++) sum += p[i * 16];
        t1 = t_us();
        logf_("membench: loop over %d KB: %u ns per load\n", sz / 1024, (unsigned)((t1 - t0) * 1000 / (200 * lines)));
    }
}
