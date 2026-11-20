/* Sampling profiler (prof.c). Runtime code marks what it is doing with PHASE_BEGIN/PHASE_END. */
#pragma once

enum { PH_GAME, PH_GTE, PH_GPU, PH_PRESENT, PH_IRQ, PH_CD, PH_OTHER, PH_GE_TEX, PH_GE_DRAW, PH_GE_PRIM, PH_GE_TOUCH, PH_GE_SYNC,
      PH_COUNT };
/* in its own 64-byte line: code running on the Media Engine writes it too (me.c), its cache line must not hold other data */
extern volatile int g_phase_line[16];
#define g_phase (g_phase_line[0])
void prof_init(void);

#define PHASE_BEGIN(p) int prev_phase_ = g_phase; g_phase = (p)
#define PHASE_END() g_phase = prev_phase_
