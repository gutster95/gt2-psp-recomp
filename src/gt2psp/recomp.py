"""MIPS R3000A -> C translator.

Every function of every module (see program.py) becomes one C function ``void SYM(PsxCpu *c)``. Symbols:

* resident: ``f_<addr>``, boot: ``b_<addr>``, overlay N: ``o<N>_<addr>``.

Translation rules (see docs/ARCHITECTURE.md):

* Branch/jump delay slots are resolved statically: the branch condition (or jump target register) is evaluated
  first, then the delay slot is emitted, then the transfer.
* ``jal`` to a known function is a direct C call. Calls into the overlay window ``0x80010000-0x8005d600`` from a
  module that does not own it go through ``rt_call_ovl`` (dispatch on the currently loaded overlay).
* ``jr ra`` is a C ``return``. Any other ``jr`` becomes a ``switch`` over the known code labels inside the
  function, falling back to ``rt_jump`` (tail call through the dispatcher). ``jalr`` uses ``rt_call``.
* A branch or ``j`` whose target is outside the current function is a tail call.
* Load delay slots are not modelled (compiled code does not depend on them).
* Hooked functions (HLE) are emitted with an ``_impl`` suffix or not at all; the runtime provides the symbol.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass, field
from pathlib import Path

from .mips import (K_BRANCH, K_CALL, K_JALR, K_JR, K_JUMP, K_TRAP, Insn, decode)
from .program import LONGJMP, SETJMP, Module

OVL_WINDOW = (0x80010000, 0x8005D600)

# Functions fully replaced by the runtime (no code emitted, calls go to the given symbol).
REPLACE = {
    SETJMP: "hle_setjmp",
    LONGJMP: "hle_longjmp",
}
# Functions wrapped by the runtime: code is emitted as <sym>_impl, the runtime defines <sym>.
WRAP = {
    0x8005DAD8,   # overlay loader: runtime records which overlay is now mapped
}


# Game-specific patches (see docs/ARCHITECTURE.md, "Widescreen"):
# HOOKS: C code inserted before the instruction at (module, address); OVERRIDE: C code replacing an instruction.
# Overlay 0, 0x80018d40: track/scenery renderer with prebuilt GPU packets, 8 loops (F3 F4 G3 G4 FT3 FT4 GT3 GT4).
# Each loop discards a polygon when the GTE flags an error (vertex too close to / behind the camera); with
# widescreen the hook passes it to the runtime's near-plane clipper instead.
_TRACK_LOOPS = [  # kind, bltz address, flag reg, entry reg, packet reg, mask lui address
    (0, 0x80018E90, 3, 10, 5, 0x80018E24),
    (1, 0x80018FB8, 7, 9, 8, 0x80018F18),
    (2, 0x80019098, 3, 9, 5, 0x8001902C),
    (3, 0x800191BC, 7, 9, 8, 0x8001911C),
    (4, 0x8001929C, 3, 9, 5, 0x80019230),
    (5, 0x800193C8, 7, 9, 8, 0x80019328),
    (6, 0x800194BC, 3, 10, 5, 0x80019450),
    (7, 0x800195EC, 7, 9, 8, 0x8001954C),
]
HOOKS: dict[tuple[str, int], str] = {}
OVERRIDE: dict[tuple[str, int], str] = {}
for _k, _b, _f, _e, _p, _m in _TRACK_LOOPS:
    HOOKS[("ovl0", _b)] = (f"if ((int32_t)c->r[{_f}] < 0 && g_widescreen) "
                           f"wide_clip_track(c, {_k}, c->r[{_e}], c->r[{_p}], c->r[14]);")


# View window of the 3D camera (world-space frustum used for culling), from Silent's (CookiePLMonster) GT2
# "16:9 Widescreen Hack 2.0" for NTSC-U 1.2: the half-width +-160 (race) / +-60 (rear view mirror) is scaled by
# 4/3 so objects in the 16:9 side areas are not culled.
OVERRIDE[("ovl0", 0x800100D0)] = "c->r[5] = c->r[6] + (g_widescreen ? (uint32_t)-213 : (uint32_t)-160);"
OVERRIDE[("ovl0", 0x800100D4)] = "c->r[6] = c->r[6] + (g_widescreen ? 213u : 160u);"
OVERRIDE[("ovl0", 0x800295EC)] = "c->r[5] = g_widescreen ? (uint32_t)-80 : (uint32_t)-60;"
OVERRIDE[("ovl0", 0x800295F0)] = "c->r[6] = g_widescreen ? 80u : 60u;"


def sym(mod: str, addr: int) -> str:
    if mod == "resident":
        return f"f_{addr:08x}"
    if mod == "boot":
        return f"b_{addr:08x}"
    return f"o{mod[3:]}_{addr:08x}"


def R(n: int) -> str:
    return "0u" if n == 0 else f"c->r[{n}]"


@dataclass
class Stats:
    funcs: int = 0
    insns: int = 0
    dyn_calls: int = 0
    dyn_jumps: int = 0
    ds_branch: int = 0
    unknown_targets: set = field(default_factory=set)


class Translator:
    def __init__(self, mods: list[Module]):
        self.mods = {m.name: m for m in mods}
        self.starts = {m.name: {f.start for f in m.funcs} for m in mods}
        self.sorted_starts = {m.name: sorted(self.starts[m.name]) for m in mods}
        self.stats = Stats()

    # --- symbol resolution ------------------------------------------------------------------------------
    def resolve(self, mod: str, target: int) -> str | None:
        """Static symbol for a call/jump target from module ``mod``, or None if it must be dynamic."""
        if target in REPLACE:
            return REPLACE[target]
        if target in self.starts["resident"] and not OVL_WINDOW[0] <= target < OVL_WINDOW[1]:
            return sym("resident", target)
        if OVL_WINDOW[0] <= target < OVL_WINDOW[1] and mod != "resident" and target in self.starts[mod]:
            return sym(mod, target)
        return None

    # --- per instruction --------------------------------------------------------------------------------
    def simple(self, ins: Insn) -> str:
        """C statement for a non-control-flow instruction."""
        ov = OVERRIDE.get((getattr(self, "cur_mod", ""), ins.pc))
        if ov is not None:
            return ov
        op, rs, rt, rd, sa, imm = ins.op, ins.rs, ins.rt, ins.rd, ins.sa, ins.imm
        s_, t_ = R(rs), R(rt)
        u16 = imm & 0xFFFF
        simm = f"(uint32_t){imm}" if imm >= 0 else f"(uint32_t)({imm})"

        def wr(dst: int, expr: str) -> str:
            return f"c->r[{dst}] = {expr};" if dst else f"(void)({expr});"

        if op == "nop":
            return ""
        if op == "sll":
            return wr(rd, f"{t_} << {sa}") if rd else ""
        if op == "srl":
            return wr(rd, f"{t_} >> {sa}") if rd else ""
        if op == "sra":
            return wr(rd, f"(uint32_t)((int32_t){t_} >> {sa})") if rd else ""
        if op == "sllv":
            return wr(rd, f"{t_} << ({s_} & 31)") if rd else ""
        if op == "srlv":
            return wr(rd, f"{t_} >> ({s_} & 31)") if rd else ""
        if op == "srav":
            return wr(rd, f"(uint32_t)((int32_t){t_} >> ({s_} & 31))") if rd else ""
        if op in ("add", "addu"):
            return wr(rd, f"{s_} + {t_}") if rd else ""
        if op in ("sub", "subu"):
            return wr(rd, f"{s_} - {t_}") if rd else ""
        if op == "and":
            return wr(rd, f"{s_} & {t_}") if rd else ""
        if op == "or":
            return wr(rd, f"{s_} | {t_}") if rd else ""
        if op == "xor":
            return wr(rd, f"{s_} ^ {t_}") if rd else ""
        if op == "nor":
            return wr(rd, f"~({s_} | {t_})") if rd else ""
        if op == "slt":
            return wr(rd, f"(int32_t){s_} < (int32_t){t_}") if rd else ""
        if op == "sltu":
            return wr(rd, f"{s_} < {t_}") if rd else ""
        if op in ("addi", "addiu"):
            return wr(rt, f"{s_} + {simm}") if rt else ""
        if op == "slti":
            return wr(rt, f"(int32_t){s_} < {imm}") if rt else ""
        if op == "sltiu":
            return wr(rt, f"{s_} < {simm}") if rt else ""
        if op == "andi":
            return wr(rt, f"{s_} & 0x{u16:x}u") if rt else ""
        if op == "ori":
            return wr(rt, f"{s_} | 0x{u16:x}u") if rt else ""
        if op == "xori":
            return wr(rt, f"{s_} ^ 0x{u16:x}u") if rt else ""
        if op == "lui":
            return wr(rt, f"0x{(u16 << 16):08x}u") if rt else ""
        if op == "mfhi":
            return wr(rd, "c->hi") if rd else ""
        if op == "mflo":
            return wr(rd, "c->lo") if rd else ""
        if op == "mthi":
            return f"c->hi = {s_};"
        if op == "mtlo":
            return f"c->lo = {s_};"
        if op == "mult":
            return (f"{{ int64_t p_ = (int64_t)(int32_t){s_} * (int32_t){t_}; "
                    f"c->lo = (uint32_t)p_; c->hi = (uint32_t)(p_ >> 32); }}")
        if op == "multu":
            return (f"{{ uint64_t p_ = (uint64_t){s_} * {t_}; "
                    f"c->lo = (uint32_t)p_; c->hi = (uint32_t)(p_ >> 32); }}")
        if op == "div":
            return f"op_div(c, {s_}, {t_});"
        if op == "divu":
            return f"op_divu(c, {s_}, {t_});"
        ea = f"{s_} + {simm}"
        if op == "lb":
            return wr(rt, f"(uint32_t)(int32_t)(int8_t)mem_r8({ea})")
        if op == "lbu":
            return wr(rt, f"mem_r8({ea})")
        if op == "lh":
            return wr(rt, f"(uint32_t)(int32_t)(int16_t)mem_r16({ea})")
        if op == "lhu":
            return wr(rt, f"mem_r16({ea})")
        if op == "lw":
            return wr(rt, f"mem_r32({ea})")
        if op == "lwl":
            return wr(rt, f"op_lwl({ea}, {t_})")
        if op == "lwr":
            return wr(rt, f"op_lwr({ea}, {t_})")
        if op == "sb":
            return f"mem_w8({ea}, (uint8_t){t_});"
        if op == "sh":
            return f"mem_w16({ea}, (uint16_t){t_});"
        if op == "sw":
            return f"mem_w32({ea}, {t_});"
        if op == "swl":
            return f"op_swl({ea}, {t_});"
        if op == "swr":
            return f"op_swr({ea}, {t_});"
        if op == "lwc2":
            return f"gte_write_data(c, {rt}, mem_r32({ea}));"
        if op == "swc2":
            return f"mem_w32({ea}, gte_read_data(c, {rt}));"
        if op == "mfc2":
            return wr(rt, f"gte_read_data(c, {rd})")
        if op == "cfc2":
            return wr(rt, f"gte_read_ctrl(c, {rd})")
        if op == "mtc2":
            return f"gte_write_data(c, {rd}, {t_});"
        if op == "ctc2":
            return f"gte_write_ctrl(c, {rd}, {t_});"
        if ins.extra.get("gte"):
            return f"gte_cmd(c, 0x{ins.word:08x}u); /* {op} */"
        if op == "mfc0":
            return wr(rt, f"cop0_read(c, {rd})")
        if op == "mtc0":
            return f"cop0_write(c, {rd}, {t_});"
        if op == "rfe":
            return "cop0_rfe(c);"
        if op == "syscall":
            return f"rt_syscall(c, 0x{ins.pc:08x}u);"
        if op == "break":
            return f"rt_break(c, 0x{ins.pc:08x}u);"
        return f"rt_invalid(c, 0x{ins.pc:08x}u, 0x{ins.word:08x}u);"

    def cond(self, ins: Insn) -> str:
        s_, t_ = R(ins.rs), R(ins.rt)
        return {
            "beq": f"{s_} == {t_}", "bne": f"{s_} != {t_}",
            "blez": f"(int32_t){s_} <= 0", "bgtz": f"(int32_t){s_} > 0",
            "bltz": f"(int32_t){s_} < 0", "bgez": f"(int32_t){s_} >= 0",
            "bltzal": f"(int32_t){s_} < 0", "bgezal": f"(int32_t){s_} >= 0",
            "b": "1",
        }[ins.op]

    # --- calls / jumps ----------------------------------------------------------------------------------
    def call(self, mod: str, target: int, pc: int) -> str:
        s = self.resolve(mod, target)
        ra = f"c->r[31] = 0x{pc + 8:08x}u; "
        if s:
            return f"{ra}{s}(c);"
        if mod == "resident" and OVL_WINDOW[0] <= target < OVL_WINDOW[1]:
            self.stats.dyn_calls += 1
            return f"{ra}rt_call_ovl(c, 0x{target:08x}u);"
        self.stats.unknown_targets.add((mod, target))
        self.stats.dyn_calls += 1
        return f"{ra}rt_call(c, 0x{target:08x}u);"

    def tail(self, mod: str, target: int) -> str:
        s = self.resolve(mod, target)
        if s:
            return f"{s}(c); return;"
        self.stats.dyn_jumps += 1
        if not (mod == "resident" and OVL_WINDOW[0] <= target < OVL_WINDOW[1]):
            self.stats.unknown_targets.add((mod, target))
        return f"rt_jump(c, 0x{target:08x}u); return;"

    # --- function ---------------------------------------------------------------------------------------
    def function(self, m: Module, start: int, end: int, falls_through: bool) -> str:
        im = m.image
        name = sym(m.name, start)
        if start in REPLACE:
            return ""
        if start in WRAP:
            name += "_impl"
        insns = {pc: decode(im.word(pc), pc) for pc in range(start, end, 4)}
        labels_in = sorted(a for a in m.labels if start <= a < end)
        targets = set(labels_in)
        for ins in insns.values():
            if ins.kind in (K_BRANCH, K_JUMP) and ins.target is not None and start <= ins.target < end:
                targets.add(ins.target)
        # restrict: c->r[] is only reached through c, so the compiler may keep registers across memory stores
        out = [f"void {name}(PsxCpu *restrict c)", "{", f"    TRACE(0x{start:08x}u);"]
        self.cur_mod = m.name
        pc = start
        while pc < end:
            ins = insns[pc]
            if pc in targets:
                out.append(f"L_{pc:08x}:;")
            hook = HOOKS.get((m.name, pc))
            if hook:
                out.append(f"    {hook}")
            self.stats.insns += 1
            if not ins.has_delay_slot:
                st = self.simple(ins)
                if st:
                    out.append(f"    {st}")
                pc += 4
                continue
            ds = insns.get(pc + 4) or decode(im.word(pc + 4), pc + 4)
            if pc + 4 in targets:
                out.append(f"    /* WARNING: branch target 0x{pc + 4:08x} is a delay slot */")
            if ds.has_delay_slot:
                self.stats.ds_branch += 1
                out.append(f"    /* WARNING: control transfer in delay slot at 0x{pc + 4:08x} */")
            dss = self.simple(ds) if not ds.has_delay_slot else ""
            k = ins.kind
            if k == K_BRANCH or (k == K_CALL and ins.op in ("bltzal", "bgezal")):
                tgt = ins.target
                inside = start <= tgt < end
                link = ins.op in ("bltzal", "bgezal")
                back = "POLL(c); " if tgt <= pc else ""
                go = (f"{back}goto L_{tgt:08x};" if inside and not link else
                      (self.call(m.name, tgt, pc) if link else self.tail(m.name, tgt)))
                if ins.op == "b":
                    out.append(f"    {dss}")
                    out.append(f"    {go}")
                elif link:
                    out.append(f"    {{ int t_ = {self.cond(ins)}; c->r[31] = 0x{pc + 8:08x}u; {dss} "
                               f"if (t_) {{ {go} }} }}")
                else:
                    out.append(f"    {{ int t_ = {self.cond(ins)}; {dss} if (t_) {{ {go} }} }}")
            elif k == K_JUMP:
                tgt = ins.target
                out.append(f"    {dss}")
                if start <= tgt < end and tgt not in self.starts[m.name]:
                    out.append(f"    {'POLL(c); ' if tgt <= pc else ''}goto L_{tgt:08x};")
                else:
                    out.append(f"    {self.tail(m.name, tgt)}")
            elif k == K_CALL:  # jal
                out.append(f"    {dss}")
                out.append(f"    {self.call(m.name, ins.target, pc)}")
            elif k == K_JR:
                if ins.rs == 31:
                    out.append(f"    {dss}")
                    out.append("    return;")
                else:
                    out.append(f"    {{ uint32_t j_ = {R(ins.rs)}; {dss}")
                    if labels_in:
                        out.append("      switch (j_) {")
                        for lab in labels_in:
                            out.append(f"        case 0x{lab:08x}u: goto L_{lab:08x};")
                        out.append("      }")
                    self.stats.dyn_jumps += 1
                    out.append("      rt_jump(c, j_); return; }")
            elif k == K_JALR:
                self.stats.dyn_calls += 1
                out.append(f"    {{ uint32_t j_ = {R(ins.rs)}; {dss} c->r[{ins.rd}] = 0x{pc + 8:08x}u; "
                           f"rt_call(c, j_); }}")
            pc += 8
        if falls_through:
            out.append(f"    {self.tail(m.name, end)}")
        out.append("}")
        self.stats.funcs += 1
        return "\n".join(out)

    def module(self, m: Module) -> str:
        parts = [f"/* Generated by gt2psp recomp from module {m.name}. Do not edit. */",
                 '#include "gt2rt.h"', '#include "funcs.h"', ""]
        for f in m.funcs:
            src = self.function(m, f.start, f.end, f.falls_through)
            if src:
                parts.append(src)
                parts.append("")
        return "\n".join(parts)


def prototypes(mods: list[Module]) -> str:
    lines = ["/* Generated. Prototypes of all recompiled functions. */", "#pragma once", '#include "gt2rt.h"', ""]
    for m in mods:
        for f in m.funcs:
            if f.start in REPLACE:
                continue
            s = sym(m.name, f.start)
            lines.append(f"void {s}(PsxCpu *c);")
            if f.start in WRAP:
                lines.append(f"void {s}_impl(PsxCpu *c);")
    for s in sorted(set(REPLACE.values())):
        lines.append(f"void {s}(PsxCpu *c);")
    return "\n".join(lines) + "\n"


def tables(mods: list[Module]) -> str:
    """Address -> function tables, one per module, sorted for binary search."""
    out = ["/* Generated. Dispatch tables. */", '#include "funcs.h"', ""]
    idx = []
    for m in mods:
        tn = f"tab_{m.name}"
        out.append(f"static const FuncEntry {tn}[] = {{")
        for f in sorted(m.funcs, key=lambda f: f.start):
            s = REPLACE.get(f.start) or sym(m.name, f.start)
            out.append(f"    {{0x{f.start:08x}u, {s}}},")
        out.append("};")
        idx.append(f'    {{"{m.name}", {tn}, {len(m.funcs)}}},')
    out.append("const ModuleTable g_modules[] = {")
    out += idx
    out.append("};")
    out.append(f"const int g_num_modules = {len(mods)};")
    return "\n".join(out) + "\n"


def generate(mods: list[Module], outdir: Path) -> Stats:
    outdir.mkdir(parents=True, exist_ok=True)
    tr = Translator(mods)
    for m in mods:
        (outdir / f"{m.name}.c").write_text(tr.module(m), newline="\n")
    (outdir / "funcs.h").write_text(prototypes(mods), newline="\n")
    (outdir / "tables.c").write_text(tables(mods), newline="\n")
    return tr.stats
