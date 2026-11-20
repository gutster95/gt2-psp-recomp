/* GT2 PSP runtime: interface used by the recompiled code. */
#pragma once
#include <stdint.h>

typedef struct PsxCpu {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];
} PsxCpu;

typedef void (*RecompFunc)(PsxCpu *c);

typedef struct FuncEntry {
    uint32_t addr;
    RecompFunc fn;
} FuncEntry;

typedef struct ModuleTable {
    const char *name;
    const FuncEntry *funcs;
    int count;
} ModuleTable;

extern const ModuleTable g_modules[];
extern const int g_num_modules;

/* ---- memory ---------------------------------------------------------------------------------------- */

/* Forced inlining: in the huge generated functions GCC otherwise emits calls for every memory access. */
#define GT2_INLINE static inline __attribute__((always_inline))

#define PSX_RAM_SIZE 0x200000u
/* 2 MB main RAM (static array: address is a link-time constant), with guard areas before and after it: unchecked
 * accesses add the instruction's offset to the mapped base register (ram_*o), which can step a little outside. */
#define RAM_GUARD_LO 0x8000u
#define RAM_GUARD_HI 0x10000u
extern uint8_t g_ram_mem[];
extern uint8_t g_ram[];          /* = g_ram_mem + RAM_GUARD_LO, an assembler symbol (core.c): one constant base */
/* The 1 KB scratchpad (0x1F800000, the R3000A data cache) lives in the first KB of g_ram (physical 0x000-0x3FF,
 * BIOS RAM on the PS1): then g_ram + (a & 0x1FFFFF) reaches RAM and scratchpad alike (the ram_* accessors). */
#define g_scratch g_ram

uint32_t io_r8(uint32_t a);
uint32_t io_r16(uint32_t a);
uint32_t io_r32(uint32_t a);
void io_w8(uint32_t a, uint32_t v);
void io_w16(uint32_t a, uint32_t v);
void io_w32(uint32_t a, uint32_t v);

/* RAM: physical 0x00000000-0x007FFFFF (2 MB mirrored 4x), reached through KUSEG/KSEG0/KSEG1. */
#define RAM_HIT(p) ((p) < 0x00800000u)
#define SCR_HIT(p) (((p) & 0xFFFFFC00u) == 0x1F800000u)

/* Checked accesses: inline fast path for main RAM above 0x400, everything else out of line - the low KB of RAM
 * (redirected to a separate dummy area: physical 0x000-0x3FF holds the scratchpad, see above, and on the PS1 it is
 * BIOS RAM that the game only reaches through null pointers), the scratchpad and I/O. Scratchpad accesses at
 * addresses known at compile time stay inline. */
uint32_t mem_slow_r8(uint32_t a);
uint32_t mem_slow_r16(uint32_t a);
uint32_t mem_slow_r32(uint32_t a);
void mem_slow_w8(uint32_t a, uint32_t v);
void mem_slow_w16(uint32_t a, uint32_t v);
void mem_slow_w32(uint32_t a, uint32_t v);
#define RAM_FAST(p) (RAM_HIT(p) && ((p) & (PSX_RAM_SIZE - 0x400u)))
#define SCR_CONST(p) (__builtin_constant_p(p) && SCR_HIT(p))
#define MEM_READ(bits, T)                                                                                  \
    GT2_INLINE uint32_t mem_r##bits(uint32_t a)                                                            \
    {                                                                                                      \
        uint32_t p = a & 0x1FFFFFFFu;                                                                      \
        if (__builtin_expect(RAM_FAST(p), 1)) return *(T *)(g_ram + (p & (PSX_RAM_SIZE - 1)));             \
        if (SCR_CONST(p)) return *(T *)(g_ram + (p & 0x3FF));                                              \
        return mem_slow_r##bits(a);                                                                        \
    }
#define MEM_WRITE(bits, T)                                                                                 \
    GT2_INLINE void mem_w##bits(uint32_t a, T v)                                                           \
    {                                                                                                      \
        uint32_t p = a & 0x1FFFFFFFu;                                                                      \
        if (__builtin_expect(RAM_FAST(p), 1)) { *(T *)(g_ram + (p & (PSX_RAM_SIZE - 1))) = v; return; }    \
        if (SCR_CONST(p)) { *(T *)(g_ram + (p & 0x3FF)) = v; return; }                                     \
        mem_slow_w##bits(a, v);                                                                            \
    }
MEM_READ(8, uint8_t)
MEM_READ(16, uint16_t)
MEM_READ(32, uint32_t)
MEM_WRITE(8, uint8_t)
MEM_WRITE(16, uint16_t)
MEM_WRITE(32, uint32_t)


/* Unchecked accesses to RAM or scratchpad (no I/O): used for stack accesses (base register sp: sp always points into
 * main RAM - the recompiler emits rt_check_sp() wherever sp is set from anything but sp + constant) and for all
 * accesses of functions that were never seen touching I/O registers (memory profile, see recomp.py). */
void rt_check_sp(uint32_t sp);
#define RAMP(a) (g_ram + ((a) & (PSX_RAM_SIZE - 1)))
GT2_INLINE uint32_t ram_r8(uint32_t a) { return *RAMP(a); }
GT2_INLINE uint32_t ram_r16(uint32_t a) { return *(uint16_t *)RAMP(a); }
GT2_INLINE uint32_t ram_r32(uint32_t a) { return *(uint32_t *)RAMP(a); }
GT2_INLINE void ram_w8(uint32_t a, uint8_t v) { *RAMP(a) = v; }
GT2_INLINE void ram_w16(uint32_t a, uint16_t v) { *(uint16_t *)RAMP(a) = v; }
GT2_INLINE void ram_w32(uint32_t a, uint32_t v) { *(uint32_t *)RAMP(a) = v; }
/* Base + offset form: the host address of the base register is shared by all accesses through it (struct fields,
 * stack slots); with a constant base the exact address is used. Differs from ram_* only if base and base + offset
 * lie in different 2 MB mirrors (not done by the game; GT2_CHECK_RAMO builds log it). */
void ramo_bad(uint32_t b, int32_t o);
#ifdef GT2_CHECK_RAMO
#define RAMO_CHECK(b, o) do { if ((((b) & (PSX_RAM_SIZE - 1)) + (uint32_t)(o)) >= PSX_RAM_SIZE) ramo_bad((b), (o)); } while (0)
#else
#define RAMO_CHECK(b, o) ((void)0)
#endif
/* The empty asm hides that the host base is g_ram + something: otherwise GCC folds the offset into the constant
 * g_ram and needs one register per distinct offset; this way one register per base register suffices. */
GT2_INLINE uint8_t *ramp_opaque(uint32_t b) { uint8_t *h = RAMP(b); __asm__("" : "+r"(h)); return h; }
#define RAMPO(b, o) (__builtin_constant_p(b) ? RAMP((b) + (uint32_t)(o)) : ramp_opaque(b) + (o))
#define RAMO_FN(name, T, R)                                                                                    GT2_INLINE uint32_t ram_r##name##o(uint32_t b, int32_t o) { RAMO_CHECK(b, o); return *(R *)RAMPO(b, o); }     GT2_INLINE void ram_w##name##o(uint32_t b, int32_t o, T v) { RAMO_CHECK(b, o); *(R *)RAMPO(b, o) = v; }
RAMO_FN(8, uint8_t, uint8_t)
RAMO_FN(16, uint16_t, uint16_t)
RAMO_FN(32, uint32_t, uint32_t)
#define stk_r8 ram_r8
#define stk_r16 ram_r16
#define stk_r32 ram_r32
#define stk_w8 ram_w8
#define stk_w16 ram_w16
#define stk_w32 ram_w32

/* Memory profile build (generated code compiled with -DGT2_MEMPROF): every access records which function (fn_, a
 * constant in each generated function: module << 24 | address) touched RAM, I/O or the low 1 KB of RAM. */
uint32_t memprof_r(uint32_t a, uint32_t fn, int size);
void memprof_w(uint32_t a, uint32_t v, uint32_t fn, int size);
#ifdef GT2_MEMPROF
static const uint32_t fn_ = 0;        /* runtime helpers; generated functions shadow it */
#define mem_r8(a) memprof_r((a), fn_, 1)
#define mem_r16(a) memprof_r((a), fn_, 2)
#define mem_r32(a) memprof_r((a), fn_, 4)
#define mem_w8(a, v) memprof_w((a), (v), fn_, 1)
#define mem_w16(a, v) memprof_w((a), (v), fn_, 2)
#define mem_w32(a, v) memprof_w((a), (v), fn_, 4)
#endif

/* Unaligned word access (little endian semantics of the R3000A). */
GT2_INLINE uint32_t op_lwl(uint32_t a, uint32_t rt)
{
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return (rt & 0x00FFFFFFu) | (w << 24);
    case 1: return (rt & 0x0000FFFFu) | (w << 16);
    case 2: return (rt & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
GT2_INLINE uint32_t op_lwr(uint32_t a, uint32_t rt)
{
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (rt & 0xFF000000u) | (w >> 8);
    case 2: return (rt & 0xFFFF0000u) | (w >> 16);
    default: return (rt & 0xFFFFFF00u) | (w >> 24);
    }
}
GT2_INLINE void op_swl(uint32_t a, uint32_t rt)
{
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = (w & 0xFFFFFF00u) | (rt >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (rt >> 16); break;
    case 2: w = (w & 0xFF000000u) | (rt >> 8); break;
    default: w = rt; break;
    }
    mem_w32(al, w);
}
GT2_INLINE void op_swr(uint32_t a, uint32_t rt)
{
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = rt; break;
    case 1: w = (w & 0x000000FFu) | (rt << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (rt << 16); break;
    default: w = (w & 0x00FFFFFFu) | (rt << 24); break;
    }
    mem_w32(al, w);
}

/* ---- arithmetic -------------------------------------------------------------------------------------- */

GT2_INLINE void op_div(PsxCpu *c, uint32_t a, uint32_t b)
{
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) {                       /* R3000A results for division by zero */
        c->lo = n >= 0 ? 0xFFFFFFFFu : 1u;
        c->hi = a;
    } else if (a == 0x80000000u && d == -1) {
        c->lo = 0x80000000u;
        c->hi = 0;
    } else {
        c->lo = (uint32_t)(n / d);
        c->hi = (uint32_t)(n % d);
    }
}
GT2_INLINE void op_divu(PsxCpu *c, uint32_t a, uint32_t b)
{
    if (b == 0) { c->lo = 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}

/* ---- coprocessors -------------------------------------------------------------------------------------- */

uint32_t gte_read_data_slow(PsxCpu *c, int reg);
uint32_t gte_read_ctrl_slow(PsxCpu *c, int reg);
void gte_write_data_slow(PsxCpu *c, int reg, uint32_t v);
void gte_write_ctrl_slow(PsxCpu *c, int reg, uint32_t v);

/* GTE register transfers: the register number is a constant in generated code, so after inlining each transfer is
 * a single load/store except for the few registers with side effects (gte.c *_slow). */
GT2_INLINE uint32_t gte_read_data(PsxCpu *c, int reg)
{
    switch (reg) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:       /* VZ0-2, IR0-3: sign-extended */
        return (uint32_t)(int32_t)(int16_t)c->gte_data[reg];
    case 7: case 16: case 17: case 18: case 19:                     /* OTZ, SZ0-3 */
        return c->gte_data[reg] & 0xFFFF;
    case 15: return c->gte_data[14];                                 /* SXYP reads SXY2 */
    case 28: case 29: return gte_read_data_slow(c, reg);             /* IRGB/ORGB */
    default: return c->gte_data[reg];
    }
}
GT2_INLINE void gte_write_data(PsxCpu *c, int reg, uint32_t v)
{
    if (reg == 15 || reg >= 28) gte_write_data_slow(c, reg, v);     /* SXYP push, IRGB, ORGB, LZCS, LZCR */
    else c->gte_data[reg] = v;
}
void gte_flag_poison_hit(uint32_t unused);   /* gte.c: GT2_FLAG_POISON check */
GT2_INLINE uint32_t gte_read_ctrl(PsxCpu *c, int reg)
{
#ifdef GT2_FLAG_POISON
    if (reg == 31 && (c->gte_ctrl[31] & 0xFFFu) == 0xFFFu) gte_flag_poison_hit(0);
#endif
    switch (reg) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:   /* RT33 L33 LR33 H DQA ZSF3 ZSF4 */
        return (uint32_t)(int32_t)(int16_t)c->gte_ctrl[reg];
    default: return c->gte_ctrl[reg];
    }
}
GT2_INLINE void gte_write_ctrl(PsxCpu *c, int reg, uint32_t v)
{
    if (reg == 31) gte_write_ctrl_slow(c, reg, v);                   /* FLAG */
    else c->gte_ctrl[reg] = v;
}
#include "gte_fast.h"                     /* gte_cmd(): inline fast paths + gte_cmd_slow() */

uint32_t cop0_read(PsxCpu *c, int reg);
void cop0_write(PsxCpu *c, int reg, uint32_t v);
void cop0_rfe(PsxCpu *c);

/* ---- control flow / system ------------------------------------------------------------------------------ */

void rt_call(PsxCpu *c, uint32_t addr);      /* computed call (jalr), BIOS vectors 0xA0/0xB0/0xC0 */
void rt_call_ovl(PsxCpu *c, uint32_t addr);  /* call into the overlay window from resident code */
void rt_jump(PsxCpu *c, uint32_t addr);      /* computed tail jump */
void rt_syscall(PsxCpu *c, uint32_t pc);
void rt_break(PsxCpu *c, uint32_t pc);
void rt_invalid(PsxCpu *c, uint32_t pc, uint32_t word);

/* ---- tracing --------------------------------------------------------------------------------------------- */

#define TRACE_SIZE 256
extern uint32_t g_trace[TRACE_SIZE];
extern uint32_t g_trace_pos;
#ifdef GT2_NO_TRACE
#define TRACE(a) ((void)0)
#else
#define TRACE(a) (g_trace[g_trace_pos++ & (TRACE_SIZE - 1)] = (a))
#endif

/* ---- interrupt polling ----------------------------------------------------------------------------------- */

/* Emitted at every backward branch: lets the runtime deliver interrupts inside busy-wait loops. */
/* Game patches called from generated code (recomp.py HOOKS/OVERRIDE) */
extern int g_widescreen, g_fps60, g_fx60, g_lowlod, g_track_lod;
extern int g_ws_menus;        /* 16:9 for the menu screens with 3D views too */
extern uint32_t g_ws_keepw;   /* 16:9: original width for the view setup 0x80049780 (recomp.py) */
void wide_clip_track(PsxCpu *c, int kind, uint32_t entry, uint32_t pkt, uint32_t ot);
void rt_vsync_enter(int waits);   /* libetc VSync hooks (recomp.py): frame work statistics, gpu.c */
void rt_vsync_leave(void);
void rt_frame_step(uint32_t disp, int after_wait);   /* frame flip hooks (recomp.py, io.c) */

extern int32_t g_poll_budget;
void rt_poll(PsxCpu *c);
#ifdef GT2_POLL_FN
/* profiling: the function whose loop polled last (the TRACE ring only names the last function entered) */
extern volatile uint32_t g_poll_fn;
#define POLL(c) do { g_poll_fn = fn_; if (--g_poll_budget < 0) rt_poll(c); } while (0)
#else
#define POLL(c) do { if (--g_poll_budget < 0) rt_poll(c); } while (0)
#endif
