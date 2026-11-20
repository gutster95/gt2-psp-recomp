/* Sampling profiler (prof.c). Runtime code marks what it is doing with PHASE_BEGIN/PHASE_END. */
#pragma once

enum { PH_GAME, PH_GTE, PH_GPU, PH_PRESENT, PH_IRQ, PH_CD, PH_OTHER, PH_GE_TEX, PH_GE_DRAW, PH_GE_PRIM, PH_GE_TOUCH, PH_GE_SYNC,
      PH_COUNT };
/* at the end of the scratchpad: the same address is the main CPU's scratchpad and, for code running on the Media
 * Engine (me.c), the ME's local eDRAM - each CPU has its own phase and the profiler (main CPU) sees the main CPU's */
#define g_phase (*(volatile int *)0x00013FFCu)
void prof_init(void);

#ifdef GT2_ME_PROF
/* Media Engine profile (profiling builds): on the ME every phase switch adds the cycles since the last switch (COP0
 * Count, the ME runs in kernel mode) to the old phase in its local memory (ME_PROF_ACC, 16 words, index 12 = waiting
 * for commands); me.c sums them into the shared block after every command, gpuvbl.c logs them. */
#define ME_PROF_ACC ((volatile uint32_t *)0x00013F80u)
#define ME_PROF_LAST (*(volatile uint32_t *)0x00013FF8u)
static inline int me_phase_set(int p)
{
    int sp_;
    __asm__ volatile("move %0, $sp" : "=r"(sp_));
    if ((uint32_t)sp_ >= 0x80000000u) {
        uint32_t now;
        __asm__ volatile("mfc0 %0, $9" : "=r"(now));
        ME_PROF_ACC[g_phase & 15] += now - ME_PROF_LAST;
        ME_PROF_LAST = now;
    }
    g_phase = p;
    return p;
}
#define PHASE_BEGIN(p) int prev_phase_ = g_phase; me_phase_set(p)
#define PHASE_END() me_phase_set(prev_phase_)
#else
#define PHASE_BEGIN(p) int prev_phase_ = g_phase; g_phase = (p)
#define PHASE_END() g_phase = prev_phase_
#endif
