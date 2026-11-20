/* Sampling profiler (prof.c). Runtime code marks what it is doing with PHASE_BEGIN/PHASE_END. */
#pragma once

enum { PH_GAME, PH_GTE, PH_GPU, PH_PRESENT, PH_IRQ, PH_CD, PH_OTHER, PH_GE_TEX, PH_GE_DRAW, PH_GE_PRIM, PH_GE_TOUCH, PH_GE_SYNC,
      PH_COUNT };
/* at the end of the scratchpad: the same address is the main CPU's scratchpad and, for code running on the Media
 * Engine (me.c), the ME's local eDRAM - each CPU has its own phase and the profiler (main CPU) sees the main CPU's */
#define g_phase (*(volatile int *)0x00013FFCu)
void prof_init(void);

#define PHASE_BEGIN(p) int prev_phase_ = g_phase; g_phase = (p)
#define PHASE_END() g_phase = prev_phase_
