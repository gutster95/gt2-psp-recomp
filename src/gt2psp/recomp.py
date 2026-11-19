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

import re

import bisect
from dataclasses import dataclass, field
from pathlib import Path

from .mips import (K_BRANCH, K_CALL, K_JALR, K_JR, K_JUMP, K_TRAP, Insn, decode)
from .program import LONGJMP, SETJMP, Module
from .regflow import ALL, CALLEE_SAVED, RETURN_MASK, FuncFlow, solve

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


# 16:9, after Silent's (CookiePLMonster) GT2 "Widescreen Hack 2.0" cheat template for NTSC-U 1.2: every 3D screen
# sets up its view window (the screen-space half-width the projection maps the camera's field of view to) with
# constants. Scaling the window by 4/3 makes the game itself render the 16:9 field of view squeezed into the 320
# pixel framebuffer (anamorphic); the presentation stretches it to the PSP's 16:9 screen. No GTE change.
# Races use g_widescreen (presented 16:9 while the race HUD layer is seen, ge.c ge_wide_now); the menu screens with
# 3D views only with settings.txt widescreen_menus (g_ws_menus), which presents every screen 16:9.
def _ws_li(mod: str, pc: int, reg: int, orig: int, cond: str = "g_ws_menus") -> None:
    """`addiu reg, zero, orig` -> orig * 4/3 with widescreen (truncated like the cheat generator)."""
    wide = int(orig * 4 / 3)
    OVERRIDE[(mod, pc)] = f"c->r[{reg}] = {cond} ? (uint32_t)({wide}) : (uint32_t)({orig});"


# Race (call to the view setup 0x8007b320 with a1/a2 = centre -+ half-width) and its rear view mirror.
OVERRIDE[("ovl0", 0x800100D0)] = "c->r[5] = c->r[6] + (g_widescreen ? (uint32_t)-213 : (uint32_t)-160);"
OVERRIDE[("ovl0", 0x800100D4)] = "c->r[6] = c->r[6] + (g_widescreen ? 213u : 160u);"
_ws_li("ovl0", 0x800295EC, 5, -60, "g_widescreen")
_ws_li("ovl0", 0x800295F0, 6, 60, "g_widescreen")
# Post-race / bonus / trophy screens: 0x80049780(view, width, height) stores width at +196 and derives the window
# from it. Only the window may widen, +196 keeps the original width: the caller leaves it in g_ws_keepw (Silent's
# cheat stores the height argument there instead, which slightly resizes other screens - not needed here).
OVERRIDE[("ovl0", 0x800497C8)] = ("mem_w16(c->r[4] + 196u, g_ws_keepw ? g_ws_keepw : c->r[5]); "
                                  "g_ws_keepw = 0;")
for _pc, _w in ((0x80049F48, 200), (0x8004C194, 200), (0x80058070, 200), (0x80058924, 200),
                (0x8004E064, 352), (0x80059A08, 352)):
    OVERRIDE[("ovl0", _pc)] = (f"c->r[5] = g_ws_menus ? {int(_w * 4 / 3)}u : {_w}u; "
                               f"g_ws_keepw = g_ws_menus ? {_w} : 0;")
for _pc in (0x80049F50, 0x8004C19C, 0x80058078, 0x8005892C):   # `move a2, a1`: the height stays 200
    OVERRIDE[("ovl0", _pc)] = "c->r[6] = g_ws_menus ? 200u : c->r[5];"
_ws_li("ovl0", 0x80050C14, 2, -176)                            # results screen
_ws_li("ovl0", 0x80050C1C, 2, 176)
# Overlay 2: car selection (1P / 2P battle) and the pre-race screen.
_ws_li("ovl2", 0x8001E578, 2, -128)
_ws_li("ovl2", 0x8001E580, 2, 128)
_ws_li("ovl2", 0x800201C4, 2, -105)
_ws_li("ovl2", 0x800201CC, 2, 105)
_ws_li("ovl2", 0x8001536C, 13, 320)
# Overlay 4 (GT mode screens): these three view constants are divided by 4/3 instead.
for _pc, _v in ((0x8001CDF4, 179), (0x8001CDFC, -50), (0x8001CE04, 800)):
    OVERRIDE[("ovl4", _pc)] = f"c->r[2] = g_ws_menus ? (uint32_t)({int(_v * 3 / 4)}) : (uint32_t)({_v});"


# Runtime functions that take the CPU but never touch c->r[] (GTE, coprocessor 0, division, interrupt polling,
# the widescreen clipper): calls to them need no register write-back (see registers_to_locals).
_NO_FLUSH = {"POLL", "gte_cmd", "gte_cmd_nf", "gte_read_data", "gte_write_data", "gte_read_ctrl", "gte_write_ctrl",
             "op_div", "op_divu", "cop0_read", "cop0_write", "cop0_rfe", "wide_clip_track"}
_CALLEE_SAVED = CALLEE_SAVED
_REG = re.compile(r"c->r\[(\d+)\]")
_WRITE = re.compile(r"c->r\[(\d+)\] = ")
_CALL = re.compile(r"\b([A-Za-z_]\w*)\(c[,)]")


def _regs(mask: int) -> list[int]:
    return [n for n in range(1, 32) if mask >> n & 1]


def text_regs(body: list[str]) -> tuple[int, int]:
    """Registers referenced / assigned in the raw (c->r[] based) body of a function."""
    text = "\n".join(body)
    used = sum(1 << int(n) for n in set(_REG.findall(text)) if n != "0")
    written = sum(1 << int(n) for n in set(_WRITE.findall(text)) if n != "0")
    return used, written


def registers_to_locals(body: list[str], dirty: int = ALL, info: dict | None = None) -> list[str]:
    """Keep the PS1 registers of one function in C locals.

    The compiler cannot keep c->r[] in machine registers: every memory access has an out-of-line I/O path that
    might (as far as it knows) modify *c, so it stores and reloads the registers around every load and store. With
    locals, registers are written back to c->r[] only right before a call that takes the CPU (other recompiled
    functions, dispatch, BIOS, hooks) or a return, and reloaded after such a call. Interrupt polling needs neither:
    interrupt handlers save and restore the whole CPU state and do not depend on the interrupted registers.

    Which registers are written back and reloaded at a call comes from the interprocedural register flow
    (regflow.py, ``info``: symbol -> (IN, CLOB)): before a call the dirty registers the callee may read, after it the
    caller-saved registers the callee may change. Unknown callees (dispatch, BIOS, setjmp) get all registers.
    """
    info = info or {}
    used_m, written_m = text_regs(body)
    get = lambda name: info.get(name, (ALL, ALL))
    # callee-saved registers that an earlier callee may have changed in c->r[] and a later one reads: keep a local
    # (loaded at entry) so the original value can be written back
    need = 0
    for line in body:
        for m in _CALL.finditer(line):
            if m.group(1) not in _NO_FLUSH:
                need |= get(m.group(1))[0]
    extra = dirty & need & ~used_m & sum(1 << n for n in _CALLEE_SAVED)
    locs = used_m | extra
    if not locs:
        return body
    dirty &= locs

    def flush(mask: int) -> str:
        return " ".join(f"c->r[{n}] = r{n};" for n in _regs(mask & dirty))

    def reload(mask: int) -> str:
        return " ".join(f"r{n} = c->r[{n}];" for n in _regs(mask & locs) if n not in _CALLEE_SAVED)

    # A plain return (jr ra) only has to hand back what the caller can rely on: v0/v1, sp and ra. Callee-saved
    # registers were restored by the function itself (their values equal c->r[] from the entry), argument and
    # temporary registers are undefined after a call.
    ret_flush = " ".join(f"c->r[{n}] = r{n};" for n in _regs(written_m & RETURN_MASK))

    def wrap_calls(line: str) -> str:
        out, i = [], 0
        while True:
            m = _CALL.search(line, i)
            if not m:
                out.append(line[i:])
                return "".join(out)
            name = m.group(1)
            if name in _NO_FLUSH:
                out.append(line[i:m.end()])
                i = m.end()
                continue
            # find the end of the call expression and its ';'
            depth, j = 0, m.start(1) + len(name)
            while j < len(line):
                if line[j] == "(":
                    depth += 1
                elif line[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            k = j + 1
            assert line[k:k + 1] == ";", line
            call = line[m.start():k + 1]
            rest = line[k + 1:]
            out.append(line[i:m.start()])
            cin, cclob = get(name)
            if rest.lstrip().startswith("return;"):          # tail call
                out.append(f"{{ {flush(cin | RETURN_MASK)} {call} return/**/; }}")
                i = k + 1 + rest.index("return;") + len("return;")
            else:
                out.append(f"{{ {flush(cin)} {call} {reload(cclob)} }}")
                i = k + 1

    res = []
    for line in body:
        line = wrap_calls(_REG.sub(lambda m: "r" + m.group(1) if m.group(1) != "0" else "0u", line))
        # remaining plain returns (jr ra): write back first
        if "return;" in line:
            line = line.replace("return;", f"{{ {ret_flush} return; }}")
        res.append(line)
    decl = "    uint32_t " + ", ".join(f"r{n} = c->r[{n}]" for n in _regs(locs)) + ";"
    return [decl] + res


_STACK_OPS = {"lb", "lbu", "lh", "lhu", "lw", "sb", "sh", "sw"}


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
    gte_nf: int = 0
    unknown_targets: set = field(default_factory=set)


class Translator:
    def __init__(self, mods: list[Module]):
        self.mods = {m.name: m for m in mods}
        self.starts = {m.name: {f.start for f in m.funcs} for m in mods}
        self.sorted_starts = {m.name: sorted(self.starts[m.name]) for m in mods}
        self.stats = Stats()
        self.mod_index = {m.name: i for i, m in enumerate(mods)}
        self.fast_funcs = set()
        self.fast_mem = False
        from .flagflow import analyze
        self.flag_dead_all = analyze(mods, lambda mod: (lambda t: self.resolve(mod, t)))

    def load_memprof(self, paths: list[Path]) -> None:
        """Memory profiles (memprof.txt of GT2_MEMPROF runs, "<id> <flags>"): functions that accessed memory but
        never I/O (flag 2) get unchecked RAM/scratchpad accessors. Functions never seen keep the checked ones."""
        flags: dict[int, int] = {}
        for p in paths:
            for line in Path(p).read_text().split(chr(10)):
                if line.strip() and not line.startswith("#"):
                    a, f = line.split()
                    flags[int(a, 16)] = flags.get(int(a, 16), 0) | int(f)
        self.fast_funcs = {a for a, f in flags.items() if not f & 6}
        low = sorted(a for a, f in flags.items() if f & 4)
        if low:
            print(f"memprof: {len(low)} functions access RAM 0x000-0x3FF (shared with the scratchpad):",
                  " ".join(f"{a:08x}" for a in low[:20]))
        print(f"memprof: {len(self.fast_funcs)} functions with unchecked memory accesses, "
              f"{sum(1 for f in flags.values() if f & 2)} with I/O")

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
            if dst == 29 and not (op in ("addi", "addiu") and rs == 29):
                return f"c->r[29] = {expr}; rt_check_sp(c->r[29]);"
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
        st = self.memory(op, rs, rt, s_, t_, ea, wr)
        if st is not None and op in _STACK_OPS and (rs == 29 or self.fast_mem):
            # unchecked RAM/scratchpad access in base + offset form (gt2rt.h ram_*o): stack accesses (sp always
            # points into main RAM, checked where sp is set from anything but sp + constant) and all accesses of
            # functions the memory profile never saw touching I/O
            st = st.replace("mem_", "ram_", 1).replace(f"({ea}", f"o({s_}, {imm}", 1)
        if st is not None:
            return st
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
            nf = "_nf" if ins.pc in getattr(self, "flag_dead", ()) else ""
            return f"gte_cmd{nf}(c, 0x{ins.word:08x}u); /* {op} */"
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

    def memory(self, op, rs, rt, s_, t_, ea, wr):
        """C statement for a CPU load/store, or None."""
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
        return None

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
            return None
        if start in WRAP:
            name += "_impl"
        insns = {pc: decode(im.word(pc), pc) for pc in range(start, end, 4)}
        labels_in = sorted(a for a in m.labels if start <= a < end)
        targets = set(labels_in)
        for ins in insns.values():
            if ins.kind in (K_BRANCH, K_JUMP) and ins.target is not None and start <= ins.target < end:
                targets.add(ins.target)
        # restrict: c->r[] is only reached through c, so the compiler may keep registers across memory stores
        # fn_: function id for the memory profile build (module index << 24 | address), unused otherwise
        fid = (self.mod_index[m.name] << 24) | (start & 0xFFFFFF)
        out = [f"void {name}(PsxCpu *restrict c)", "{",
               f"    TRACE(0x{start:08x}u); const uint32_t fn_ = 0x{fid:08x}u; (void)fn_;"]
        self.fast_mem = fid in self.fast_funcs
        self.cur_mod = m.name
        self.flag_dead = self.flag_dead_all.get(sym(m.name, start), set())
        self.stats.gte_nf += len(self.flag_dead)
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
        self.stats.funcs += 1
        # register flow of this function (regflow.py); hooks read the registers they mention
        extra_use = {}
        for (hm, hpc), code in HOOKS.items():
            if hm == m.name and start <= hpc < end:
                extra_use[hpc] = sum(1 << int(n) for n in _REG.findall(code) if n != "0")
        key = sym(m.name, start)
        flow = FuncFlow(key, insns, start, end, labels_in, falls_through, lambda t: self.resolve(m.name, t),
                        extra_use)
        return key, out, flow

    def module(self, m: Module) -> list:
        """Raw functions of a module: (symbol, lines with c->r[] registers, register flow)."""
        return [r for f in m.funcs if (r := self.function(m, f.start, f.end, f.falls_through))]


def finalize(m: Module, raw: list, IN: dict, CLOB: dict, D: dict) -> str:
    info = {k: (IN[k], CLOB[k]) for k in IN}
    parts = [f"/* Generated by gt2psp recomp from module {m.name}. Do not edit. */",
             '#include "gt2rt.h"', '#include "funcs.h"', ""]
    for key, out, _flow in raw:
        parts.append("\n".join(out[:3] + registers_to_locals(out[3:], D[key], info) + ["}"]))
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


def WRAP_KEYS(mods: list[Module]) -> list[str]:
    return [sym(m.name, f.start) for m in mods for f in m.funcs if f.start in WRAP]


def generate(mods: list[Module], outdir: Path, memprof: list[Path] | None = None) -> Stats:
    outdir.mkdir(parents=True, exist_ok=True)
    tr = Translator(mods)
    if memprof:
        tr.load_memprof(memprof)
    raw = {m.name: tr.module(m) for m in mods}
    flows, written = {}, {}
    for m in mods:
        for key, out, flow in raw[m.name]:
            flows[key] = flow
            written[key] = text_regs(out[3:])[1]
    IN, CLOB, D = solve(flows, written)
    for m in mods:                          # runtime wrappers (WRAP) read a0 themselves
        for f in m.funcs:
            if f.start in WRAP:
                IN[sym(m.name, f.start)] |= 1 << 4
    for m in mods:
        (outdir / f"{m.name}.c").write_text(finalize(m, raw[m.name], IN, CLOB, D), newline="\n")
    (outdir / "funcs.h").write_text(prototypes(mods), newline="\n")
    (outdir / "tables.c").write_text(tables(mods), newline="\n")
    return tr.stats
