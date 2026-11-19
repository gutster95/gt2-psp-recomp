"""Interprocedural register flow for the recompiler's calling convention (see docs/ARCHITECTURE.md).

Generated functions keep the PS1 registers in C locals and exchange them with callers/callees through ``c->r[]``.
Writing every local back before each call and reloading afterwards dominated the cost of calls, so the translator
uses three per-function register sets (bit masks, bit n = register n), computed to a fixpoint over the call graph:

* ``IN``   - registers whose ``c->r[]`` value at entry the function (or anything it calls) may read: live-in
             registers of a backward liveness analysis whose call sites use the callee's ``IN``.
* ``CLOB`` - registers the function (or anything it calls) may change in ``c->r[]``: its return write-back, the
             write-backs before its calls, and its callees' ``CLOB``.
* ``D``    - "dirty" registers of a function: those whose local may differ from ``c->r[]`` at a call site (written
             locally, or changed in ``c->r[]`` by an earlier callee). Before a call, ``D & IN(callee)`` is written
             back; after it, ``CLOB(callee)`` is reloaded (caller-saved registers only).

Assumptions (the MIPS ABI, which the game's compiled code and the PsyQ libraries follow):

* After a call the caller-saved registers (at, v0/v1, a0-a3, t0-t9) hold no value the caller relies on except
  what the callee returns; ra holds the return address.
* A store of a callee-saved register (s0-s7, fp, ra) to the stack frame only saves it for restoring later, and the
  restored value only matters in the local (callers keep callee-saved registers in their own locals). Such stores
  do not make the register live-in.
* Computed calls/jumps, syscalls, BIOS and setjmp/longjmp may read and change every register (``ALL``).
"""

from __future__ import annotations

from .mips import K_BRANCH, K_CALL, K_JALR, K_JR, K_JUMP, K_TRAP, Insn

ALL = 0xFFFFFFFE
CALLEE_SAVED = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30}
CALLER_SAVED_MASK = sum(1 << n for n in list(range(1, 16)) + [24, 25])
RETURN_MASK = (1 << 2) | (1 << 3) | (1 << 29) | (1 << 31)
_SAVE_REGS = {16, 17, 18, 19, 20, 21, 22, 23, 30, 31}

_ALU3 = {"add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu", "sllv", "srlv", "srav"}
_SHIFT = {"sll", "srl", "sra"}
_ALUI = {"addi", "addiu", "slti", "sltiu", "andi", "ori", "xori"}
_LOAD = {"lb", "lbu", "lh", "lhu", "lw"}
_STORE = {"sb", "sh", "sw", "swl", "swr"}


def b(*regs: int) -> int:
    m = 0
    for r in regs:
        if r:
            m |= 1 << r
    return m


def use_def(ins: Insn) -> tuple[int, int]:
    """Registers read and written by a non-control-flow instruction (unknown ops: read everything)."""
    op, rs, rt, rd = ins.op, ins.rs, ins.rt, ins.rd
    if op == "nop" or ins.extra.get("gte") or op == "rfe":
        return 0, 0
    if op in _ALU3:
        return b(rs, rt), b(rd)
    if op in _SHIFT:
        return b(rt), b(rd)
    if op in _ALUI:
        return b(rs), b(rt)
    if op == "lui":
        return 0, b(rt)
    if op in ("mfhi", "mflo"):
        return 0, b(rd)
    if op in ("mthi", "mtlo"):
        return b(rs), 0
    if op in ("mult", "multu", "div", "divu"):
        return b(rs, rt), 0
    if op in _LOAD:
        return b(rs), b(rt)
    if op in ("lwl", "lwr"):
        return b(rs, rt), b(rt)
    if op in _STORE:
        if op == "sw" and rs == 29 and rt in _SAVE_REGS:      # prologue save of a callee-saved register
            return b(rs), 0
        return b(rs, rt), 0
    if op in ("lwc2", "swc2"):
        return b(rs), 0
    if op in ("mfc2", "cfc2", "mfc0"):
        return 0, b(rt)
    if op in ("mtc2", "ctc2", "mtc0"):
        return b(rt), 0
    return ALL, 0


def cond_use(ins: Insn) -> int:
    if ins.op in ("beq", "bne"):
        return b(ins.rs, ins.rt)
    if ins.op == "b":
        return 0
    return b(ins.rs)


class FuncFlow:
    """Liveness nodes of one function: each node is one instruction or a control transfer with its delay slot."""

    def __init__(self, name: str, insns: dict[int, Insn], start: int, end: int, labels_in: list[int],
                 falls_through: bool, resolve, extra_use: dict[int, int]):
        self.name = name
        self.start = start
        self.nodes: list[tuple] = []    # (pc, kind, use, def, successor pcs, callee-or-None, tail/call flags)
        self.callees: list[str | None] = []   # every call site / tail call (None = dynamic)
        self.tails: list[str | None] = []
        pc = start
        inside = lambda t: start <= t < end
        while pc < end:
            ins = insns[pc]
            xu = extra_use.get(pc, 0)
            if not ins.has_delay_slot:
                if ins.kind == K_TRAP or ins.op == "invalid":
                    self.nodes.append((pc, "dyn", ALL, 0, [], None))
                    self.callees.append(None)
                else:
                    u, d = use_def(ins)
                    self.nodes.append((pc, "seq", u | xu, d, [pc + 4], None))
                pc += 4
                continue
            ds = insns.get(pc + 4)
            du, dd = use_def(ds) if ds is not None and not ds.has_delay_slot else (ALL, 0)
            xu |= extra_use.get(pc + 4, 0)
            k, t = ins.kind, ins.target
            nxt = pc + 8
            if k == K_BRANCH:
                u = cond_use(ins) | du | xu
                succ = [] if ins.op == "b" else [nxt]
                if inside(t):
                    self.nodes.append((pc, "br", u, dd, succ + [t], None))
                else:
                    s = resolve(t)
                    self.tails.append(s)
                    self.nodes.append((pc, "brtail", u, dd, succ, s))
            elif k == K_CALL:
                s = resolve(t)
                self.callees.append(s)
                u = (cond_use(ins) if ins.op != "jal" else 0) | du | xu
                kind = "call" if ins.op == "jal" else "condcall"
                self.nodes.append((pc, kind, u, dd | b(31), [nxt], s))
            elif k == K_JUMP:
                if inside(t):
                    self.nodes.append((pc, "br", du | xu, dd, [t], None))
                else:
                    s = resolve(t)
                    self.tails.append(s)
                    self.nodes.append((pc, "brtail", du | xu, dd, [], s))
            elif k == K_JR:
                if ins.rs == 31:
                    self.nodes.append((pc, "ret", du | xu, dd, [], None))
                elif labels_in:          # jump table: the known case labels (no fallback assumed)
                    self.nodes.append((pc, "br", b(ins.rs) | du | xu, dd, list(labels_in), None))
                else:                    # computed tail call
                    self.tails.append(None)
                    self.nodes.append((pc, "dyn", ALL, 0, [], None))
            elif k == K_JALR:
                self.callees.append(None)
                self.nodes.append((pc, "dyn", ALL, 0, [], None))
            pc += 8
        if falls_through:
            s = resolve(end)
            self.tails.append(s)
            self.nodes.append((end, "brtail", 0, 0, [], s))
        self.live = {n[0]: 0 for n in self.nodes}

    def live_in(self, IN: dict[str, int]) -> int:
        get = lambda s: ALL if s is None else IN.get(s, ALL)
        live = self.live
        changed = True
        while changed:
            changed = False
            for pc, kind, u, d, succ, callee in reversed(self.nodes):
                out = 0
                for s in succ:
                    out |= live.get(s, 0)
                if kind == "dyn":
                    v = ALL
                elif kind == "brtail":
                    v = u | ((out | get(callee)) & ~d)
                elif kind == "call":
                    v = u | ((((out & ~(CALLER_SAVED_MASK | b(31))) | get(callee)) & ~b(31)) & ~(d & ~b(31)))
                elif kind == "condcall":
                    v = u | ((out | (get(callee) & ~b(31))) & ~(d & ~b(31)))
                else:
                    v = u | (out & ~d)
                if v != live[pc]:
                    live[pc] = v
                    changed = True
        return live[self.start] if self.start in live else 0


def solve(flows: dict[str, FuncFlow], written: dict[str, int], max_iter: int = 100):
    """Fixpoint of IN, CLOB and D over all functions. ``written``: registers each function assigns."""
    IN = {n: 0 for n in flows}
    CLOB = {n: 0 for n in flows}
    D = {n: 0 for n in flows}
    get = lambda tab, s: ALL if s is None or s not in tab else tab[s]
    for _ in range(max_iter):
        changed = False
        for n, f in flows.items():
            i = f.live_in(IN) & ALL
            sites = f.callees + f.tails
            cl = 0
            for s in sites:
                cl |= get(CLOB, s)
            d = written[n] | cl
            need = 0
            for s in f.callees:
                need |= get(IN, s)
            for s in f.tails:
                need |= get(IN, s) | RETURN_MASK
            c = (written[n] & RETURN_MASK) | (d & need) | cl
            if i != IN[n] or c != CLOB[n] or d != D[n]:
                IN[n], CLOB[n], D[n] = i, c, d
                changed = True
        if not changed:
            return IN, CLOB, D
    raise RuntimeError("register flow did not converge")
