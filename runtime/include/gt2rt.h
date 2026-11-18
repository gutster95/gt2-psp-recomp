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
extern uint8_t g_ram[];         /* 2 MB main RAM (static array: address is a link-time constant) */
extern uint8_t g_scratch[1024]; /* 0x1F800000 data cache used as scratchpad */

uint32_t io_r8(uint32_t a);
uint32_t io_r16(uint32_t a);
uint32_t io_r32(uint32_t a);
void io_w8(uint32_t a, uint32_t v);
void io_w16(uint32_t a, uint32_t v);
void io_w32(uint32_t a, uint32_t v);

/* RAM: physical 0x00000000-0x007FFFFF (2 MB mirrored 4x), reached through KUSEG/KSEG0/KSEG1. */
#define RAM_HIT(p) ((p) < 0x00800000u)
#define SCR_HIT(p) (((p) & 0xFFFFFC00u) == 0x1F800000u)

GT2_INLINE uint32_t mem_r8(uint32_t a)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (RAM_HIT(p)) return g_ram[p & (PSX_RAM_SIZE - 1)];
    if (SCR_HIT(p)) return g_scratch[p & 0x3FF];
    return io_r8(a);
}
GT2_INLINE uint32_t mem_r16(uint32_t a)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (RAM_HIT(p)) return *(uint16_t *)(g_ram + (p & (PSX_RAM_SIZE - 1)));
    if (SCR_HIT(p)) return *(uint16_t *)(g_scratch + (p & 0x3FF));
    return io_r16(a);
}
GT2_INLINE uint32_t mem_r32(uint32_t a)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (RAM_HIT(p)) return *(uint32_t *)(g_ram + (p & (PSX_RAM_SIZE - 1)));
    if (SCR_HIT(p)) return *(uint32_t *)(g_scratch + (p & 0x3FF));
    return io_r32(a);
}
GT2_INLINE void mem_w8(uint32_t a, uint8_t v)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (RAM_HIT(p)) { g_ram[p & (PSX_RAM_SIZE - 1)] = v; return; }
    if (SCR_HIT(p)) { g_scratch[p & 0x3FF] = v; return; }
    io_w8(a, v);
}
GT2_INLINE void mem_w16(uint32_t a, uint16_t v)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (RAM_HIT(p)) { *(uint16_t *)(g_ram + (p & (PSX_RAM_SIZE - 1))) = v; return; }
    if (SCR_HIT(p)) { *(uint16_t *)(g_scratch + (p & 0x3FF)) = v; return; }
    io_w16(a, v);
}
GT2_INLINE void mem_w32(uint32_t a, uint32_t v)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (RAM_HIT(p)) { *(uint32_t *)(g_ram + (p & (PSX_RAM_SIZE - 1))) = v; return; }
    if (SCR_HIT(p)) { *(uint32_t *)(g_scratch + (p & 0x3FF)) = v; return; }
    io_w32(a, v);
}

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
GT2_INLINE uint32_t gte_read_ctrl(PsxCpu *c, int reg)
{
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
extern int g_widescreen;
void wide_clip_track(PsxCpu *c, int kind, uint32_t entry, uint32_t pkt, uint32_t ot);

extern int32_t g_poll_budget;
void rt_poll(PsxCpu *c);
#define POLL(c) do { if (--g_poll_budget < 0) rt_poll(c); } while (0)
