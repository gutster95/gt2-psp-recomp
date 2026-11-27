/* High-level emulation of the PS1 BIOS functions GT2 uses (A0/B0/C0 tables, syscalls).
 * Function numbers and semantics follow the nocash psx-spx documentation. */
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "card.h"
#include "game.h"

uint32_t g_hook_entry_int;
uint32_t g_irq_chain[4];
int g_irq_enabled = 1;

/* Events (OpenEvent/EnableEvent/DeliverEvent). Handles are 0xF1000000 + index. Callback-mode events (mode
 * 0x1000) call their function when delivered; GT2 uses them for the memory card results. */
typedef struct { uint32_t cls, spec, mode, func; int open, enabled; } Event;
#define MAX_EVENTS 32
static Event g_events[MAX_EVENTS];
static int g_nevents;

/* events delivered a little later from rt_poll (memory card results) */
typedef struct { uint32_t cls, spec; uint64_t at; } Pending;
static Pending g_pending[16];
static int g_npending;

int g_card_event_us = 300;   /* settings.txt card_event_us: memory card result delay (a real card: ~1 ms per sector) */
void bios_event_later(uint32_t cls, uint32_t spec)
{
    if (g_npending >= 16) { logf_("BIOS: event queue full\n"); return; }
    g_pending[g_npending++] = (Pending){cls, spec, now_us() + (uint64_t)g_card_event_us};
}

/* Memory card fast path (card_fast, default on). GT2 drives the card from a task that runs once per vblank
 * (0x8007EACC, context 0x801F06E0): it looks at the result of the last sector operation and starts the next one -
 * one 128-byte sector per frame, so loading a GT mode save (254 sectors) took 4.2 s. Right after a card result is
 * delivered, the task runs once more (interrupt context, like its vblank call), so the next sector starts at once:
 * 254 sectors in 105 ms, same save state. */
int g_card_fast = 1;   /* 1: reads only (verified), 2: writes too (saving not verified yet) */
extern int g_card_last_write;
/* the card task of this disc (game.h, profiles.py card_task/card_ctx); 0 when the code does not match */
static uint32_t card_task(void)
{
    static int ok = -1;
    uint32_t a = GT2_CARD_TASK;
    if (ok < 0) ok = mem_r32(a) == GT2_CARD_TASK_W0 && mem_r32(a + 4) == GT2_CARD_TASK_W1;
    return ok ? a : 0;
}

static void deliver(PsxCpu *c, uint32_t cls, uint32_t spec)
{
    for (int i = 0; i < g_nevents; i++) {
        Event *e = &g_events[i];
        if (!e->open || e->cls != cls || e->spec != spec) continue;
        if (e->enabled && e->mode == 0x1000 && e->func) rt_callback(c, e->func);
    }
}

/* called from rt_poll outside interrupt handlers */
void bios_poll_events(PsxCpu *c)
{
    extern uint64_t g_poll_now;
    uint64_t t = g_poll_now;
    while (g_npending && g_pending[0].at <= t) {
        Pending p = g_pending[0];
        memmove(g_pending, g_pending + 1, sizeof(Pending) * (--g_npending));
        deliver(c, p.cls, p.spec);
        if (g_card_fast && (g_card_fast >= 2 || !g_card_last_write) && p.cls == 0xF0000011u && card_task())
            rt_callback(c, card_task());
    }
}

static void ram_copy(uint32_t dst, uint32_t src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) mem_w8(dst + i, (uint8_t)mem_r8(src + i));
}

static void ram_string(uint32_t a, char *out, int max)
{
    int i = 0;
    for (; i < max - 1; i++) {
        char ch = (char)mem_r8(a + i);
        if (!ch) break;
        out[i] = ch;
    }
    out[i] = 0;
}

/* Minimal printf over PS1 memory: %d %i %u %x %X %s %c %p with flags/width copied through. */
static void ps1_printf(PsxCpu *c, uint32_t fmt_addr, int first_arg)
{
    char fmt[256], out[512], spec[16];
    ram_string(fmt_addr, fmt, sizeof fmt);
    int argi = first_arg, o = 0;
    for (const char *p = fmt; *p && o < (int)sizeof out - 64; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        int s = 0;
        spec[s++] = *p++;
        while (*p && strchr("-+ #0123456789.lh", *p) && s < 12) spec[s++] = *p++;
        if (!*p) break;
        spec[s++] = *p;
        spec[s] = 0;
        uint32_t v = argi < 4 ? c->r[4 + argi] : mem_r32(c->r[29] + 4 * argi);
        argi++;
        if (*p == 's') {
            char str[128];
            ram_string(v, str, sizeof str);
            o += snprintf(out + o, sizeof out - o, "%s", str);
        } else if (*p == '%') {
            out[o++] = '%';
            argi--;
        } else {
            o += snprintf(out + o, sizeof out - o, spec, v);
        }
    }
    out[o] = 0;
    logf_("[ps1] %s", out);
}

static void bios_a0(PsxCpu *c, uint32_t n)
{
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    switch (n) {
    case 0x18: {                       /* strncmp */
        int r = 0;
        for (uint32_t i = 0; i < a2; i++) {
            int x = (int)mem_r8(a0 + i), y = (int)mem_r8(a1 + i);
            if (x != y) { r = x - y; break; }
            if (!x) break;
        }
        c->r[2] = (uint32_t)r;
        return;
    }
    case 0x27: ram_copy(a1, a0, a2); return;                         /* bcopy(src, dst, len) */
    case 0x28: for (uint32_t i = 0; i < a1; i++) mem_w8(a0 + i, 0); return;   /* bzero */
    case 0x3A: fatal(c, "BIOS _exit(%d)", (int)a0);
    case 0x3F: ps1_printf(c, a0, 1); return;
    case 0x44: return;                                              /* FlushCache */
    case 0x49: io_w32(0x1F801810u, a0); return;                     /* GPU_cw */
    case 0x70: logf_("BIOS _bu_init\n"); return;
    case 0x72: return;                                              /* CdRemove */
    case 0xAB: c->r[2] = card_info(a0); return;                     /* _card_info */
    }
    fatal(c, "unimplemented BIOS A0:%02X (ra=%08x)", (unsigned)n, (unsigned)c->r[31]);
}

static void bios_b0(PsxCpu *c, uint32_t n)
{
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    switch (n) {
    case 0x07: {                                                    /* DeliverEvent(class, spec) */
        PsxCpu saved = *c;
        deliver(c, a0, a1);
        *c = saved;
        c->r[2] = 1;
        return;
    }
    case 0x08: {                                                    /* OpenEvent(class, spec, mode, func) */
        int i = 0;
        while (i < g_nevents && g_events[i].open) i++;
        if (i >= MAX_EVENTS) { c->r[2] = 0xFFFFFFFFu; return; }
        if (i == g_nevents) g_nevents++;
        g_events[i] = (Event){a0, a1, a2, a3, 1, 0};
        c->r[2] = 0xF1000000u + (uint32_t)i;
        return;
    }
    case 0x09:                                                      /* CloseEvent */
        if ((a0 & 0xFFFF) < MAX_EVENTS) g_events[a0 & 0xFFFF].open = 0;
        c->r[2] = 1;
        return;
    case 0x0C:                                                      /* EnableEvent */
        if ((a0 & 0xFFFF) < MAX_EVENTS) g_events[a0 & 0xFFFF].enabled = 1;
        c->r[2] = 1;
        return;
    case 0x0D:                                                      /* DisableEvent */
        if ((a0 & 0xFFFF) < MAX_EVENTS) g_events[a0 & 0xFFFF].enabled = 0;
        c->r[2] = 1;
        return;
    case 0x12: c->r[2] = 1; logf_("BIOS InitPad(%08x, %d, %08x, %d)\n", (unsigned)a0, (int)a1, (unsigned)a2, (int)a3); return;
    case 0x13: c->r[2] = 1; return;                                 /* StartPad */
    case 0x15: c->r[2] = 1; return;                                 /* OutdatedPadInitAndStart */
    case 0x17: bios_return_from_exception(c);                       /* ReturnFromException */
    case 0x18: g_hook_entry_int = 0; return;                        /* ResetEntryInt */
    case 0x19:                                                      /* HookEntryInt */
        if (g_hook_entry_int != a0) logf_("BIOS HookEntryInt(%08x)\n", (unsigned)a0);
        g_hook_entry_int = a0;
        return;
    case 0x3F: {                                                    /* puts */
        char s[256];
        ram_string(a0, s, sizeof s);
        logf_("[ps1] %s\n", s);
        return;
    }
    case 0x4A: case 0x4B: case 0x4C: c->r[2] = 1; return;           /* InitCard / StartCard / StopCard */
    case 0x4E: c->r[2] = card_write(a0, a1, a2); return;             /* _card_write(port, sector, src) */
    case 0x4F: c->r[2] = card_read(a0, a1, a2); return;              /* _card_read(port, sector, dst) */
    case 0x50: return;                                              /* allow_new_card */
    case 0x51: {                                                    /* Krom2RawAdd(Shift-JIS) */
        /* The address of the 16x15 glyph (30 bytes) in the BIOS ROM's kanji font (psx-spx). There is no ROM here -
         * reads of it give 0, so the glyphs stay blank - but the address must be a real, even ROM address: the
         * Japanese GT2 takes Krom2RawAdd(0x8140) as its font base and computes all other glyphs from it; the old
         * answer -1 ("no ROM") made it read halfwords at odd addresses = an address error that powered off the PSP. */
        uint32_t s = a0 & 0xFFFF, hi = s >> 8, lo = s & 0xFF, r = 0xBFC66000u;
        if (lo >= 0x40 && lo <= 0xFC && lo != 0x7F) {
            uint32_t col = lo - 0x40 - (lo > 0x7F);
            if (s >= 0x8140 && s <= 0x84BE) r = 0xBFC66000u + ((hi - 0x81) * 0xBC + col) * 0x1E;
            else if (s >= 0x889F && s <= 0x9872) r = 0xBFC7F8DEu + ((hi - 0x88) * 0xBC + col - 0x5E) * 0x1E;
        }
        c->r[2] = r;
        return;
    }
    case 0x56: c->r[2] = 0x00000674u; return;                       /* GetC0Table */
    case 0x57: c->r[2] = 0x00000874u; return;                       /* GetB0Table */
    case 0x5B: return;                                              /* ChangeClearPad */
    }
    fatal(c, "unimplemented BIOS B0:%02X (ra=%08x)", (unsigned)n, (unsigned)c->r[31]);
}

static void bios_c0(PsxCpu *c, uint32_t n)
{
    uint32_t a0 = c->r[4], a1 = c->r[5];
    switch (n) {
    case 0x02:                                                      /* SysEnqIntRP(prio, entry) */
        logf_("BIOS SysEnqIntRP(%d, %08x) func2=%08x func1=%08x\n", (int)a0, (unsigned)a1,
              (unsigned)mem_r32(a1 + 4), (unsigned)mem_r32(a1 + 8));
        if (a0 < 4) { mem_w32(a1, g_irq_chain[a0]); g_irq_chain[a0] = a1; }
        c->r[2] = 0;
        return;
    case 0x03:                                                      /* SysDeqIntRP(prio, entry) */
        if (a0 < 4) {
            uint32_t prev = 0, e = g_irq_chain[a0];
            while (e && e != a1) { prev = e; e = mem_r32(e); }
            if (e) {
                if (prev) mem_w32(prev, mem_r32(e));
                else g_irq_chain[a0] = mem_r32(e);
            }
        }
        return;
    case 0x0A: c->r[2] = 1; return;                                 /* ChangeClearRCnt */
    }
    fatal(c, "unimplemented BIOS C0:%02X (ra=%08x)", (unsigned)n, (unsigned)c->r[31]);
}

void bios_call(PsxCpu *c, uint32_t table)
{
    uint32_t n = c->r[9];
    if (table == 0xA0) bios_a0(c, n);
    else if (table == 0xB0) bios_b0(c, n);
    else bios_c0(c, n);
}

void rt_syscall(PsxCpu *c, uint32_t pc)
{
    switch (c->r[4]) {
    case 1: c->r[2] = (uint32_t)g_irq_enabled; g_irq_enabled = 0; return;   /* EnterCriticalSection */
    case 2: g_irq_enabled = 1; return;                                      /* ExitCriticalSection */
    }
    fatal(c, "syscall(%d) at %08x", (int)c->r[4], (unsigned)pc);
}

/* ---- COP0 ------------------------------------------------------------------------------------------------ */

uint32_t cop0_read(PsxCpu *c, int reg) { return c->cop0[reg]; }
void cop0_write(PsxCpu *c, int reg, uint32_t v) { c->cop0[reg] = v; }
void cop0_rfe(PsxCpu *c)
{
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
}
