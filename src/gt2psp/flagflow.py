"""Whole-program liveness of the GTE FLAG register (cop2 control register 31).

FLAG is computed by every GTE command, but GT2 reads it (cfc2 $31) in few places. RTPS/RTPT/MVMVA whose FLAG result
is never read get a fast path without the flag logic (runtime gte_fast.h gte_cmd_nf, ~20% cheaper).

Backward liveness per function, with two summaries per function solved to a fixpoint over the whole program:
  entry_live[f]  FLAG may be read in f (or its callees) before a GTE command overwrites it
  ret_live[f]    FLAG may be read after f returns (some caller reads it after the call)
Calls continue to the return point unless the callee overwrites FLAG first; this is approximated conservatively as
"live after the call if the callee may read it at entry, or if it is live at the return point". Dynamic calls,
computed jumps and unknown targets count as reads; the return value of dynamically called functions is live if
any dynamic call site has FLAG live at its return point.
"""
from __future__ import annotations

from .mips import K_BRANCH, K_CALL, K_JALR, K_JR, K_JUMP, decode

NF_OPS = {"rtps", "rtpt", "mvmva"}

# successor kinds
_PC, _TRUE, _CALL, _RET, _TAIL, _DYNCALL, _DYNTAIL = range(7)


def _gen_kill(ins) -> tuple[bool, bool]:
    if ins.op == "cfc2" and ins.rd == 31:
        return True, False
    if ins.op == "ctc2" and ins.rd == 31:
        return False, True
    if ins.extra.get("gte"):
        return False, True
    return False, False


class _Func:
    __slots__ = ("key", "pcs", "gk", "succ", "gte", "live")

    def __init__(self, key):
        self.key = key


def _build(mod: str, im, start: int, end: int, falls_through: bool, resolve, key: str) -> _Func:
    insns = {pc: decode(im.word(pc), pc) for pc in range(start, end, 4)}
    ds_of = {pc + 4: pc for pc, ins in insns.items() if ins.has_delay_slot}
    f = _Func(key)
    f.pcs = sorted(insns)
    f.gk = {pc: _gen_kill(ins) for pc, ins in insns.items()}
    f.gte = [pc for pc, ins in insns.items()
             if ins.extra.get("gte") and ins.op in NF_OPS and pc not in ds_of]
    f.succ = {}
    for pc in f.pcs:
        b = ds_of.get(pc)
        if b is None:
            if insns[pc].has_delay_slot or pc + 4 < end:
                f.succ[pc] = [(_PC, pc + 4)] if pc + 4 < end else [(_TRUE,)]
            else:                                   # last instruction, no branch: falls into the next function
                g = resolve(end) if falls_through else None
                f.succ[pc] = [(_TAIL, g)] if g else [(_DYNTAIL,)]
            continue
        ins = insns[b]
        k, op, tgt = ins.kind, ins.op, ins.target
        after = b + 8
        if k == K_BRANCH and op not in ("bltzal", "bgezal"):
            out = []
            if start <= tgt < end:
                out.append((_PC, tgt))
            else:
                g = resolve(tgt)
                out.append((_TAIL, g) if g else (_DYNTAIL,))
            if op != "b":
                out.append((_PC, after) if after < end else (_TRUE,))
            f.succ[pc] = out
        elif k == K_JUMP:
            if start <= tgt < end:
                f.succ[pc] = [(_PC, tgt)]
            else:
                g = resolve(tgt)
                f.succ[pc] = [(_TAIL, g) if g else (_DYNTAIL,)]
        elif k == K_CALL or (k == K_BRANCH and op in ("bltzal", "bgezal")):
            g = resolve(tgt)
            ret = after if after < end else None
            if ret is None:
                f.succ[pc] = [(_TRUE,)]
            elif g is None:
                f.succ[pc] = [(_DYNCALL, ret)]
            else:
                f.succ[pc] = [(_CALL, g, ret)]
            if k == K_BRANCH:                        # conditional link: may also not call
                f.succ[pc].append((_PC, after) if after < end else (_TRUE,))
        elif k == K_JALR:
            ret = after if after < end else None
            f.succ[pc] = [(_DYNCALL, ret)] if ret is not None else [(_TRUE,)]
        elif k == K_JR:
            f.succ[pc] = [(_RET,)] if ins.rs == 31 else [(_DYNTAIL,)]   # switch or computed tail jump
        else:
            f.succ[pc] = [(_TRUE,)]
    f.live = {pc: False for pc in f.pcs}
    return f


def analyze(mods, resolve_for, local: bool = True) -> dict[str, set[int]]:
    """{function symbol: set of GTE command pcs whose FLAG is dead}. resolve_for(mod)(target) -> symbol or None.

    local=True (used): FLAG is assumed not to be live across calls and returns - GT2 reads it right after the GTE
    command in the same function. The sound whole-program variant (local=False) finds almost nothing: dynamic calls
    make every return live. The assumption is checked at run time by the GT2_FLAG_POISON build (gt2rt.h)."""
    from .recomp import sym
    funcs: dict[str, _Func] = {}
    for m in mods:
        res = resolve_for(m.name)
        for fn in m.funcs:
            key = sym(m.name, fn.start)
            funcs[key] = _build(m.name, m.image, fn.start, fn.end, fn.falls_through, res, key)
    reads = {k: False for k in funcs}       # FLAG may be read inside f (or callees) before being overwritten
    ret = {k: False for k in funcs}         # FLAG may be read after f returns
    dyn_ret = False
    phase = 1

    def val(f: _Func, s) -> bool:
        t = s[0]
        if t == _PC:
            return f.live[s[1]]
        if local:
            return t == _CALL and f.live[s[2]]
        if t == _CALL:
            return reads.get(s[1], True) or f.live[s[2]]
        if t == _RET:
            return phase == 2 and (ret[f.key] or dyn_ret)
        if t == _TAIL:
            return reads.get(s[1], True) or (phase == 2 and (ret[f.key] or dyn_ret))
        return True                                   # _TRUE, _DYNCALL, _DYNTAIL

    def solve_func(f: _Func) -> None:
        inner = True
        while inner:
            inner = False
            for pc in reversed(f.pcs):
                g, k = f.gk[pc]
                v = True if g else False if k else any(val(f, s) for s in f.succ[pc])
                if v != f.live[pc]:
                    f.live[pc] = v
                    inner = True

    rounds = 0
    changed = True
    while changed:                          # phase 1: reads[] (returns count as "not read")
        changed = False
        rounds += 1
        for f in funcs.values():
            solve_func(f)
            if f.pcs and f.live[f.pcs[0]] and not reads[f.key]:
                reads[f.key] = True
                changed = True
    phase = 2
    for f in funcs.values():
        f.live = {pc: False for pc in f.pcs}
    changed = True
    while changed:                          # phase 2: liveness with return liveness from the call sites
        changed = False
        rounds += 1
        for f in funcs.values():
            solve_func(f)
        for f in funcs.values():
            for pc, ss in f.succ.items():
                for s in ss:
                    if s[0] == _CALL and f.live[s[2]] and not ret.get(s[1], True):
                        ret[s[1]] = True
                        changed = True
                    elif s[0] == _TAIL and s[1] in ret and (ret[f.key] or dyn_ret) and not ret[s[1]]:
                        ret[s[1]] = True
                        changed = True
                    elif s[0] == _DYNCALL and f.live[s[1]] and not dyn_ret:
                        dyn_ret = True
                        changed = True
                    elif s[0] == _DYNTAIL and ret[f.key] and not dyn_ret:
                        dyn_ret = True                # the unknown target returns to f's caller
                        changed = True
    dead: dict[str, set[int]] = {}
    for f in funcs.values():
        d = {pc for pc in f.gte if not any(val(f, s) for s in f.succ[pc])}
        if d:
            dead[f.key] = d
    print(f"flagflow: {rounds} rounds, {sum(reads.values())} functions may read FLAG, "
          f"{sum(ret.values())} after return, dynamic returns {'live' if dyn_ret else 'dead'}, "
          f"{sum(len(d) for d in dead.values())} of {sum(len(f.gte) for f in funcs.values())} commands without FLAG")
    return dead

