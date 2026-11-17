/* High-level emulation of the PS1 BIOS functions GT2 uses (A0/B0/C0 tables, syscalls).
 * Function numbers and semantics follow the nocash psx-spx documentation. */
#include <stdio.h>
#include <string.h>
#include "rt.h"

uint32_t g_hook_entry_int;
uint32_t g_irq_chain[4];
int g_irq_enabled = 1;

static uint32_t g_event_next = 0xF1000000u;

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
    case 0xAB: c->r[2] = 1; logf_("BIOS _card_info(%d)\n", (int)a0); return;
    }
    fatal(c, "unimplemented BIOS A0:%02X (ra=%08x)", (unsigned)n, (unsigned)c->r[31]);
}

static void bios_b0(PsxCpu *c, uint32_t n)
{
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    switch (n) {
    case 0x07: c->r[2] = 1; return;                                 /* DeliverEvent */
    case 0x08:                                                      /* OpenEvent(class, spec, mode, func) */
        logf_("BIOS OpenEvent(%08x, %04x, %04x, %08x)\n", (unsigned)a0, (unsigned)a1, (unsigned)a2, (unsigned)a3);
        c->r[2] = g_event_next++;
        return;
    case 0x09: c->r[2] = 1; return;                                 /* CloseEvent */
    case 0x0C: c->r[2] = 1; return;                                 /* EnableEvent */
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
    case 0x4E: case 0x4F: c->r[2] = 0; logf_("BIOS card sector I/O (stub)\n"); return;
    case 0x50: return;                                              /* allow_new_card */
    case 0x51: c->r[2] = 0xFFFFFFFFu; return;                       /* Krom2RawAdd: no kanji ROM */
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
