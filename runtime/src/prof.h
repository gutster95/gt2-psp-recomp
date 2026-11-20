/* Sampling profiler (prof.c). Runtime code marks what it is doing with PHASE_BEGIN/PHASE_END. */
#pragma once

enum { PH_GAME, PH_GTE, PH_GPU, PH_PRESENT, PH_IRQ, PH_CD, PH_OTHER, PH_GE_TEX, PH_GE_DRAW, PH_GE_PRIM, PH_GE_TOUCH, PH_GE_SYNC,
      PH_COUNT };
/* at the end of the scratchpad: the same address is the main CPU's scratchpad and, for code running on the Media
 * Engine (me.c), the ME's local eDRAM - each CPU has its own phase and the profiler (main CPU) sees the main CPU's */
#define g_phase (*(volatile int *)0x00013FFCu)
void prof_init(void);

#ifdef GT2_ME_PROF
/* Media Engine profile (profiling builds): the ME has no running timer or cycle counter, so on the ME every phase
 * switch also writes the phase to a word in the shared block (uncached); the main CPU's profiler thread samples it
 * (prof.c). Phase 12 = the ME waits for a command. */
extern uint32_t g_me_phase_word[16];   /* me.c, own cache lines; accessed uncached only */
#define g_me_phase_uc ((volatile uint32_t *)((uintptr_t)g_me_phase_word | 0x40000000u))
static inline int me_phase_set(int p)
{
    int sp_;
    __asm__ volatile("move %0, $sp" : "=r"(sp_));
    if ((uint32_t)sp_ >= 0x80000000u) *g_me_phase_uc = (uint32_t)p;
    g_phase = p;
    return p;
}
#define PHASE_BEGIN(p) int prev_phase_ = g_phase; me_phase_set(p)
#define PHASE_END() me_phase_set(prev_phase_)
static inline void phase_restore_(int *p) { me_phase_set(*p); }
/* phase for the rest of the enclosing block (every return restores the previous one) */
#define PHASE_SCOPE(p) int scope_phase_ __attribute__((cleanup(phase_restore_))) = g_phase; me_phase_set(p)
#else
#define PHASE_BEGIN(p) int prev_phase_ = g_phase; g_phase = (p)
#define PHASE_END() g_phase = prev_phase_
#define PHASE_SCOPE(p) ((void)0)   /* only in ME profiling builds: the extra writes cost in the hot GPU path */
#endif
