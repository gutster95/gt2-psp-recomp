/* GTE (COP2). Register storage only for now; commands are logged. TODO: full implementation. */
#include "rt.h"

uint32_t gte_read_data(PsxCpu *c, int reg) { return c->gte_data[reg]; }
uint32_t gte_read_ctrl(PsxCpu *c, int reg) { return c->gte_ctrl[reg]; }
void gte_write_data(PsxCpu *c, int reg, uint32_t v) { c->gte_data[reg] = v; }
void gte_write_ctrl(PsxCpu *c, int reg, uint32_t v) { c->gte_ctrl[reg] = v; }

void gte_cmd(PsxCpu *c, uint32_t insn)
{
    static uint8_t seen[64];
    (void)c;
    if (!seen[insn & 63]) {
        seen[insn & 63] = 1;
        logf_("GTE command %02x not implemented\n", (unsigned)(insn & 63));
    }
}
