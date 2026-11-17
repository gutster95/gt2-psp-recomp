/* Core of the runtime: program start, dispatch of computed calls, setjmp/longjmp (unwind & resume),
 * overlay tracking and interrupt delivery. See docs/ARCHITECTURE.md. */
#include <string.h>
#include <psprtc.h>
#include "rt.h"
#include "funcs.h"
#include "cdrom.h"

uint8_t *g_ram;
uint8_t g_scratch[1024] __attribute__((aligned(16)));
uint32_t g_trace[TRACE_SIZE];
uint32_t g_trace_pos;
int32_t g_poll_budget = 1000;
int g_cur_overlay = -1;

#define OVL_LO 0x80010000u
#define OVL_HI 0x8005D600u

/* Module order in g_modules: resident, boot, ovl0..ovl5 (see program.py). */
static RecompFunc find_in(int mod, uint32_t addr)
{
    const ModuleTable *m = &g_modules[mod];
    int lo = 0, hi = m->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t a = m->funcs[mid].addr;
        if (a == addr) return m->funcs[mid].fn;
        if (a < addr) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

RecompFunc lookup(uint32_t addr)
{
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    if (addr >= OVL_LO && addr < OVL_HI)
        return find_in(g_cur_overlay < 0 ? 1 : 2 + g_cur_overlay, addr);
    return find_in(0, addr);
}

static void dispatch(PsxCpu *c, uint32_t addr, const char *what)
{
    if (addr == 0xA0 || addr == 0xB0 || addr == 0xC0) {
        bios_call(c, addr);
        return;
    }
    RecompFunc f = lookup(addr);
    if (!f) fatal(c, "%s to unknown address %08x (overlay %d)", what, (unsigned)addr, g_cur_overlay);
    f(c);
}

void rt_call(PsxCpu *c, uint32_t addr) { dispatch(c, addr, "call"); }
void rt_call_ovl(PsxCpu *c, uint32_t addr) { dispatch(c, addr, "overlay call"); }
void rt_jump(PsxCpu *c, uint32_t addr) { dispatch(c, addr, "jump"); }

void rt_break(PsxCpu *c, uint32_t pc) { fatal(c, "break at %08x", (unsigned)pc); }
void rt_invalid(PsxCpu *c, uint32_t pc, uint32_t w) { fatal(c, "invalid instruction %08x at %08x", (unsigned)w, (unsigned)pc); }

/* ---- setjmp / longjmp: unwind & resume ------------------------------------------------------------- */

static jmp_buf g_top;

void hle_setjmp(PsxCpu *c)
{
    uint32_t b = c->r[4];
    static const int regs[12] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
    for (int i = 0; i < 12; i++) mem_w32(b + 4 * i, c->r[regs[i]]);
    c->r[2] = 0;
}

void hle_longjmp(PsxCpu *c)
{
    uint32_t b = c->r[4];
    static const int regs[12] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
    for (int i = 0; i < 12; i++) c->r[regs[i]] = mem_r32(b + 4 * i);
    c->r[2] = c->r[5];
    logf_("longjmp buf=%08x -> ra=%08x v0=%08x sp=%08x\n", (unsigned)b, (unsigned)c->r[31],
          (unsigned)c->r[2], (unsigned)c->r[29]);
    irq_abandon();                          /* a longjmp out of an interrupt handler ends it */
    longjmp(g_top, 1);
}

void run_program(PsxCpu *c)
{
    volatile int started = 0;
    setjmp(g_top);
    if (!started) {
        started = 1;
        c->r[31] = SENTINEL_RA;
        f_8005d600(c);
        logf_("program entry returned, v0=%08x\n", (unsigned)c->r[2]);
        return;
    }
    /* Resumed after a longjmp: keep "returning" to r31. */
    for (;;) {
        uint32_t ra = c->r[31];
        if (ra == SENTINEL_RA) {
            logf_("program returned to sentinel, v0=%08x\n", (unsigned)c->r[2]);
            return;
        }
        RecompFunc f = lookup(ra);
        if (!f) fatal(c, "resume at %08x: not a function start", (unsigned)ra);
        f(c);
        if (c->r[31] == ra) fatal(c, "resume at %08x returned without changing ra", (unsigned)ra);
    }
}

/* ---- overlay loader hook ------------------------------------------------------------------------------ */

void f_8005dad8(PsxCpu *c)
{
    int n = (int)c->r[4];
    logf_("overlay load %d (entry %08x)\n", n, (unsigned)mem_r32(0x80091174u + 4 * n));
    f_8005dad8_impl(c);
    g_cur_overlay = n;
}

/* ---- time & interrupts ---------------------------------------------------------------------------------- */

uint64_t now_us(void)
{
    u64 t;
    sceRtcGetCurrentTick(&t);
    return t;
}

static uint64_t g_next_vblank;
static int g_in_irq;
static jmp_buf g_irq_jb;

/* Emulates the BIOS exception handler for an interrupt: priority chains (SysEnqIntRP), then the
 * HookEntryInt hook. ReturnFromException anywhere inside ends the whole interrupt (longjmp to g_irq_jb). */
static uint32_t g_n_polls, g_n_irqs;

static void call_irq_handlers(PsxCpu *c)
{
    static PsxCpu saved;
    g_n_irqs++;
    uint32_t saved_istat = g_i_stat;
    uint32_t trace_start = g_trace_pos;
    saved = *c;
    g_in_irq = 1;
    if (setjmp(g_irq_jb) == 0) {
        for (int prio = 0; prio < 4; prio++) {
            uint32_t e = g_irq_chain[prio];
            int guard = 0;
            while (e && guard++ < 16) {          /* entry = {next, func2, func1, 0} */
                uint32_t next = mem_r32(e), f2 = mem_r32(e + 4), f1 = mem_r32(e + 8);
                if (f1) {
                    c->r[31] = SENTINEL_RA;
                    c->r[29] = 0x801FFC00u;
                    rt_call(c, f1);
                    if (c->r[2] && f2) { c->r[4] = c->r[2]; c->r[31] = SENTINEL_RA; rt_call(c, f2); }
                }
                e = next;
            }
        }
        if (g_hook_entry_int) {                  /* buffer like setjmp; entered like longjmp(hook, 1) */
            uint32_t b = g_hook_entry_int;
            static const int regs[12] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
            for (int i = 0; i < 12; i++) c->r[regs[i]] = mem_r32(b + 4 * i);
            c->r[2] = 1;
            uint32_t pc = c->r[31];
            c->r[31] = SENTINEL_RA;
            rt_call(c, pc);
        }
    }
    {
        static int dbg;
        if (g_cur_overlay == 1 && dbg < 12 && g_n_irqs > 2600) {
            dbg++;
            logf_("irq #%u done: i_stat %03x->%03x dicr %08x, trace:", (unsigned)g_n_irqs, (unsigned)saved_istat,
                  (unsigned)g_i_stat, (unsigned)io_r32(0x1F8010F4u));
            uint32_t n = g_trace_pos - trace_start, from = n > 16 ? g_trace_pos - 16 : trace_start;
            logf_(" (%u calls, last:)", (unsigned)n);
            for (uint32_t i = from; i != g_trace_pos; i++)
                logf_(" %08x", (unsigned)g_trace[i & (TRACE_SIZE - 1)]);
            logf_("\n");
        }
    }
    *c = saved;
    g_in_irq = 0;
}

void irq_abandon(void) { g_in_irq = 0; }

void bios_return_from_exception(PsxCpu *c)
{
    (void)c;
    if (!g_in_irq) fatal(c, "ReturnFromException outside of an interrupt");
    longjmp(g_irq_jb, 1);
}

void rt_poll(PsxCpu *c)
{
    g_poll_budget = 2000;
    g_n_polls++;
    uint64_t t = now_us();
    static uint64_t next_report;
    if (t >= next_report) {
        next_report = t + 2000000;
        logf_("alive: tpos %u polls %u irqs %u dicr %08x in_irq %d ovl %d i_stat %03x i_mask %03x irq %d sr %08x last fns %08x %08x %08x %08x\n", (unsigned)g_trace_pos, (unsigned)g_n_polls, (unsigned)g_n_irqs, (unsigned)io_r32(0x1F8010F4u), g_in_irq, g_cur_overlay,
              (unsigned)g_i_stat, (unsigned)g_i_mask, g_irq_enabled, (unsigned)c->cop0[12],
              (unsigned)g_trace[(g_trace_pos - 4) & (TRACE_SIZE - 1)], (unsigned)g_trace[(g_trace_pos - 3) & (TRACE_SIZE - 1)],
              (unsigned)g_trace[(g_trace_pos - 2) & (TRACE_SIZE - 1)], (unsigned)g_trace[(g_trace_pos - 1) & (TRACE_SIZE - 1)]);
    }
    cdrom_tick();
    if (t >= g_next_vblank) {
        g_next_vblank = (g_next_vblank && t - g_next_vblank < 100000) ? g_next_vblank + 16683 : t + 16683;
        io_vblank();
    }
    if (!g_in_irq && g_irq_enabled && (c->cop0[12] & 0x401) == 0x401 && (g_i_stat & g_i_mask))
        call_irq_handlers(c);
}
