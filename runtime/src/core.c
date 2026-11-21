/* Core of the runtime: program start, dispatch of computed calls, setjmp/longjmp (unwind & resume),
 * overlay tracking and interrupt delivery. See docs/ARCHITECTURE.md. */
#include <stdio.h>
#include <string.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspthreadman.h>
#include "rt.h"
#include "funcs.h"
#include "cdrom.h"
#include "prof.h"
#include "me.h"

/* fixed address: no pointer load per access; guard areas around it for base + offset accesses (gt2rt.h) */
uint8_t g_ram_mem[RAM_GUARD_LO + PSX_RAM_SIZE + RAM_GUARD_HI] __attribute__((aligned(65536)));
__asm__(".globl g_ram\n.set g_ram, g_ram_mem + 0x8000");
_Static_assert(RAM_GUARD_LO == 0x8000, "g_ram symbol offset");
uint32_t g_trace[TRACE_SIZE];
uint32_t g_trace_pos;
int32_t g_poll_budget = 1000;
/* Backward branches between two rt_poll calls (in races only ~180 calls per second: vblank delivery latency) */
#define POLL_INTERVAL 2000
uint64_t g_poll_now;                       /* time of the current rt_poll (for the devices it ticks) */
int g_cur_overlay = -1;

#define OVL_LO 0x80010000u

/* One build per disc (CMake GT2_GAME, src/gt2psp/profiles.py): Simulation Mode (generated/) or Arcade Mode
 * (generated/arcade/, symbols with the prefix "a"). The launcher (runtime/launcher) starts one of the two PRXs. */
typedef struct {
    const ModuleTable *mods;               /* resident, boot, ovl0..ovl5 (see program.py) */
    uint32_t ovl_hi;                       /* end of the overlay window = start of the resident code */
    uint32_t ovl_table;                    /* overlay entry addresses (logging) */
    void (*entry)(PsxCpu *c);
    const FuncEntry *resume_inside;        /* return addresses inside functions that a longjmp resumes */
    const int *num_resume_inside;
} Game;
#ifdef GT2_ARC_BUILD
extern const ModuleTable a_g_modules[];
extern const FuncEntry a_g_resume_inside[];
extern const int a_g_num_resume_inside;
int g_game = GAME_ARC;
static const Game G_[1] = {{a_g_modules, 0x8005D570u, 0x80090EB8u, af_8005d570, a_g_resume_inside,
                            &a_g_num_resume_inside}};
#else
int g_game = GAME_SIM;
static const Game G_[1] = {{g_modules, 0x8005D600u, 0x80091174u, f_8005d600, 0, 0}};
#endif

static RecompFunc find_in(int mod, uint32_t addr)
{
    const ModuleTable *m = &G_->mods[mod];
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
    if (addr >= OVL_LO && addr < G_->ovl_hi)
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

void rt_check_sp(uint32_t sp)
{
    if ((sp & 0x1FFFFFFFu) >= 0x00800000u) fatal(0, "stack pointer %08x outside main RAM", (unsigned)sp);
}

void ramo_bad(uint32_t b, int32_t o)
{
    static int n;
    if (n < 50) { n++; logf_("ramo: base %08x offset %d leaves the 2 MB mirror of the base\n", (unsigned)b, (int)o); }
}

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
    static int logged;   /* the Arcade menus switch tasks with longjmp several times per frame */
    if (logged < 40) {
        logged++;
        logf_("longjmp buf=%08x -> ra=%08x v0=%08x sp=%08x\n", (unsigned)b, (unsigned)c->r[31],
              (unsigned)c->r[2], (unsigned)c->r[29]);
    }
    irq_abandon();                          /* a longjmp out of an interrupt handler ends it */
    g_phase = PH_GAME;
    longjmp(g_top, 1);
}

void run_program(PsxCpu *c)
{
    volatile int started = 0;
    setjmp(g_top);
    if (!started) {
        started = 1;
        c->r[31] = SENTINEL_RA;
        G_->entry(c);
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
        if (!f && G_->resume_inside)          /* a return address inside a function: its entry switch jumps there */
            for (int i = 0; i < *G_->num_resume_inside; i++)
                if (G_->resume_inside[i].addr == ra) { c->resume_pc = ra; f = G_->resume_inside[i].fn; break; }
        if (!f) fatal(c, "resume at %08x: not a function start", (unsigned)ra);
        f(c);
        if (c->r[31] == ra) fatal(c, "resume at %08x returned without changing ra", (unsigned)ra);
    }
}

/* ---- overlay loader hook ------------------------------------------------------------------------------ */

uint32_t g_overlay_loads;   /* bumped by every overlay load (gpu.c race work statistics) */
static void overlay_load(PsxCpu *c, void (*impl)(PsxCpu *c))
{
    int n = (int)c->r[4];
    logf_("overlay load %d (entry %08x) at vblank %u\n", n, (unsigned)mem_r32(G_->ovl_table + 4 * n),
          (unsigned)g_vblank_count);
    impl(c);
    g_cur_overlay = n;
    g_overlay_loads++;
}
#ifdef GT2_ARC_BUILD
void af_8005da48(PsxCpu *c) { overlay_load(c, af_8005da48_impl); }
#else
void f_8005dad8(PsxCpu *c) { overlay_load(c, f_8005dad8_impl); }
#endif

/* ---- time & interrupts ---------------------------------------------------------------------------------- */

uint64_t g_vblank_sync_us;   /* set by the presentation when it waited for the display's vblank (ge.c) */

uint64_t now_us(void)
{
    if (me_on_me()) return me_now_us();    /* Media Engine: no system calls (time published by the main CPU) */
    u64 t;
    sceRtcGetCurrentTick(&t);
    return t;
}

/* Stack for interrupt handlers called by the emulated BIOS exception handler: inside the 64 KB kernel RAM
 * (0x80000000-0x8000FFFF), which the game does not use. */
#define IRQ_STACK_TOP 0x8000E000u

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
                    c->r[29] = IRQ_STACK_TOP;   /* BIOS kernel stack, never the game's stack */
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
    *c = saved;
    g_in_irq = 0;
}

void irq_abandon(void) { g_in_irq = 0; }
int rt_in_irq(void) { return g_in_irq; }

/* Run a game callback (BIOS event handler) in interrupt context: own stack, CPU state restored afterwards. */
void rt_callback(PsxCpu *c, uint32_t func)
{
    PsxCpu saved = *c;
    int was = g_in_irq;
    g_in_irq = 1;
    c->r[31] = SENTINEL_RA;
    c->r[29] = IRQ_STACK_TOP;
    rt_call(c, func);
    *c = saved;
    g_in_irq = was;
}

void bios_return_from_exception(PsxCpu *c)
{
    (void)c;
    if (!g_in_irq) fatal(c, "ReturnFromException outside of an interrupt");
    longjmp(g_irq_jb, 1);
}

/* Real CPU clock: 256k single-cycle instructions against the 1 MHz system timer (best of 3 against preemption).
 * scePowerGetCpuClockFrequencyInt cannot be trusted: ARK's overclock above 333 MHz programs the PLL directly and
 * the power service keeps reporting 333. Costs ~2.3 ms every 2 s. Shown under the fps counter (ge.c). */
int g_cpu_mhz;
static int measure_mhz(void)
{
    uint32_t best = 0xFFFFFFFFu;
    for (int k = 0; k < 3; k++) {
        int st = sceKernelSuspendDispatchThread();      /* no thread switch in the middle (they made it read 313-399) */
        uint32_t t0 = sceKernelGetSystemTimeLow();
        /* all in asm: with the loop in C, gcc unrolled it into 1 MB of nops that ran from memory (4x too slow) */
        int n = 256;
        __asm__ volatile(".set push\n\t.set noreorder\n1:\n\t.rept 997\n\tnop\n\t.endr\n\taddiu %0, %0, -1\n\tbnez %0, 1b\n\tnop\n\t.set pop"
                         : "+r"(n));
        uint32_t dt = sceKernelGetSystemTimeLow() - t0;
        sceKernelResumeDispatchThread(st);
        if (dt < best) best = dt;
    }
    return best ? (int)((256000u + best / 2) / best) : 0;
}

void rt_poll(PsxCpu *c)
{
    PHASE_BEGIN(PH_IRQ);
    extern volatile int g_quit;
    if (g_quit) for (;;) sceKernelDelayThread(100000);   /* quitting (main.c quit_game): park the game thread */
    g_poll_budget = POLL_INTERVAL;
    g_n_polls++;
#ifdef GT2_SPIN_FIND
    {   /* debug (trace builds): report functions that loop on rt_poll without calling anything (wait loops) */
        static uint32_t lastpos, same, logged[16];
        if (g_trace_pos == lastpos) {
            if (++same == 200) {
                uint32_t fn = g_trace[(g_trace_pos - 1) & (TRACE_SIZE - 1)];
                int k;
                for (k = 0; k < 16 && logged[k] && logged[k] != fn; k++) {}
                if (k < 16 && !logged[k]) { logged[k] = fn; logf_("spin: %08x at vblank %u\n", (unsigned)fn, (unsigned)g_vblank_count); }
            }
        } else { same = 0; lastpos = g_trace_pos; }
    }
#endif
    uint64_t t = now_us();
    g_poll_now = t;
    static uint64_t next_report;
    if (t >= next_report) {
        next_report = t + 2000000;
        /* The log writer (priority 0x48, below the game thread - above it froze the PSP) only runs when the game
         * thread sleeps; busy scenes (60 fps races, movies) never do. A short sleep every 2 s lets it write. */
        sceKernelDelayThread(500);
        extern void memprof_dump(void);
        memprof_dump();
        g_cpu_mhz = measure_mhz();
        logf_("cpu: %d MHz measured (power service says %d)\n", g_cpu_mhz, scePowerGetCpuClockFrequencyInt());
        extern uint32_t g_ot_entries, g_ot_empty;
        logf_("ot: %u entries (%u empty) in 2 s\n", (unsigned)g_ot_entries, (unsigned)g_ot_empty);
        g_ot_entries = g_ot_empty = 0;
        extern void cdrom_stats(void);
        cdrom_stats();
        extern void mdec_stats(void);
        mdec_stats();
        logf_("alive: tpos %u polls %u irqs %u dicr %08x in_irq %d ovl %d i_stat %03x i_mask %03x irq %d sr %08x last fns %08x %08x %08x %08x\n", (unsigned)g_trace_pos, (unsigned)g_n_polls, (unsigned)g_n_irqs, (unsigned)io_r32(0x1F8010F4u), g_in_irq, g_cur_overlay,
              (unsigned)g_i_stat, (unsigned)g_i_mask, g_irq_enabled, (unsigned)c->cop0[12],
              (unsigned)g_trace[(g_trace_pos - 4) & (TRACE_SIZE - 1)], (unsigned)g_trace[(g_trace_pos - 3) & (TRACE_SIZE - 1)],
              (unsigned)g_trace[(g_trace_pos - 2) & (TRACE_SIZE - 1)], (unsigned)g_trace[(g_trace_pos - 1) & (TRACE_SIZE - 1)]);
    }
#ifdef GT2_POLL_FN
    /* profiling: where the time inside rt_poll goes (cd, ME service, vblank device work, IRQ handlers, events) */
    static uint64_t pt[5], pnext;
    uint64_t t0 = t, t1;
#define PT_(i) (t1 = now_us(), pt[i] += t1 - t0, t0 = t1)
#else
#define PT_(i) ((void)0)
#endif
    cdrom_tick();
    PT_(0);
    {
        extern void ge_poll_async(void);
        if (g_me_active) me_service();
        else ge_poll_async();
    }
    PT_(1);
    if (t >= g_next_vblank) {
        g_next_vblank = (g_next_vblank && t - g_next_vblank < 100000) ? g_next_vblank + 16683 : t + 16683;
        g_vblank_sync_us = 0;
        io_vblank();
        extern void menu_poll(void);
        if (!g_in_irq) menu_poll();               /* L+R+SELECT: settings menu (pauses the game here) */
        if (g_vblank_sync_us) g_next_vblank = g_vblank_sync_us + 16683 - 1200;   /* see ge.c present_scaled */
    }
    PT_(2);
    if (!g_in_irq && g_irq_enabled && (c->cop0[12] & 0x401) == 0x401 && (g_i_stat & g_i_mask))
        call_irq_handlers(c);
    PT_(3);
    if (!g_in_irq && g_irq_enabled) bios_poll_events(c);
    PT_(4);
#undef PT_
#ifdef GT2_POLL_FN
    if (t >= pnext) {
        pnext = t + 2000000;
        logf_("poll time (ms in 2 s): cd %u me %u vblank %u irq %u events %u\n", (unsigned)(pt[0] / 1000),
              (unsigned)(pt[1] / 1000), (unsigned)(pt[2] / 1000), (unsigned)(pt[3] / 1000), (unsigned)(pt[4] / 1000));
        memset(pt, 0, sizeof pt);
    }
#endif
    PHASE_END();
}
