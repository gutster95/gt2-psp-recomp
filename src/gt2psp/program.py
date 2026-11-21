"""The whole GT2 program as seen by the recompiler: resident main executable + 6 overlays, partitioned into functions.

Memory map (derived in docs/DEVLOG.md):

* ``0x80010000-0x80011df8`` boot code of SCUS_944.88, followed by two embedded gzip TIM images; the whole range up
  to ``0x8005d600`` is overwritten by overlays once the first one is loaded.
* ``0x8005d600-0x8008e020`` resident text (CRT start, overlay loader, libraries), then resident data.
* Overlays ``ovl0``-``ovl5`` from GT2.OVL, each loaded at ``0x80010000``, entered via the table at ``0x80091174``.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .analysis import Region, code_pointers_in, find_jump_tables, partition, text_ranges
from .image import Image, PsxExe, load_overlays, load_psx_exe
from .mips import K_BRANCH, K_JR, K_JUMP, decode

BOOT_TEXT = (0x80010000, 0x80011DF8)      # boot code + strings, up to the embedded notice.tim.gz
RESIDENT_TEXT = (0x8005D600, 0x8008E020)
OVERLAY_ENTRY_TABLE = 0x80091174
NUM_OVERLAYS = 6
SETJMP = 0x8007AD58

# Function entries the heuristics cannot see: code addresses only ever passed at run time. Resident 0x8007ea68+8n:
# nine 2-instruction stubs (`j 0x8007eaac; li a1, bit`) registered as memory card event callbacks via OpenEvent.
EXTRA_STARTS = {"resident": {0x8007EA68 + 8 * i for i in range(9)}}
LONGJMP = 0x8007AD90
EXE = "SCUS_944.88"
EXTRA_RESUME: dict[str, set[int]] = {}   # resume points after jalr calls (profiles.py)
SWITCH_STARTS = False     # switch cases outside the function of their jr become function starts (profiles.py)
OVL_JUMP = None           # load overlay a0, then longjmp into it with a1 (profiles.py; None: not handled)
TEXT_EXCLUDE: dict[str, list[tuple[int, int]]] = {}


def use_profile(p) -> None:
    """Switch to a disc (profiles.py): the constants above are the Simulation disc's defaults."""
    global BOOT_TEXT, RESIDENT_TEXT, OVERLAY_ENTRY_TABLE, SETJMP, LONGJMP, EXTRA_STARTS, EXE, TEXT_EXCLUDE
    global OVL_JUMP, EXTRA_RESUME, SWITCH_STARTS
    BOOT_TEXT, RESIDENT_TEXT, OVERLAY_ENTRY_TABLE = p.boot_text, p.resident_text, p.overlay_entry_table
    SETJMP, LONGJMP, EXTRA_STARTS, EXE = p.setjmp, p.longjmp, p.extra_starts, p.exe
    TEXT_EXCLUDE = p.text_exclude
    OVL_JUMP = p.ovl_jump
    EXTRA_RESUME, SWITCH_STARTS = p.extra_resume, p.switch_starts


@dataclass
class Func:
    start: int
    end: int
    strong: bool
    falls_through: bool = False   # last instruction is not an unconditional transfer: continues into next function


@dataclass
class Module:
    name: str
    image: Image
    text: list[tuple[int, int]]
    funcs: list[Func] = field(default_factory=list)
    labels: set[int] = field(default_factory=set)   # code pointers that are not function starts (jump tables)
    entry: int | None = None
    resume: set[int] = field(default_factory=set)
    jtables: dict[int, list[int]] = field(default_factory=dict)


def _jal_targets(im: Image, text: list[tuple[int, int]]) -> set[int]:
    out = set()
    for s, e in text:
        for a in range(s, e, 4):
            w = im.word(a)
            if w >> 26 == 3:
                out.add(((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
    return out


def _ends_function(im: Image, a: int, lo: int) -> bool:
    """Does the code right before ``a`` end with an unconditional transfer + delay slot (nop padding skipped)?"""
    b = a - 4
    while b >= lo and im.word(b) == 0 and b - 4 >= lo and im.word(b - 4) == 0:
        b -= 4
    b -= 4  # instruction before the delay slot
    if b < lo:
        return True
    ins = decode(im.word(b), b)
    return ins.kind in (K_JR, K_JUMP) or bool(ins.extra.get("always"))


def _terminal(ins) -> bool:
    return ins.kind in (K_JR, K_JUMP) or bool(ins.extra.get("always"))


def _ends_terminal(im: Image, r: Region) -> bool:
    """Last real instruction pair of the region is `<unconditional transfer>; <delay slot>`; nop padding allowed."""
    e = r.end
    while e - 4 >= r.start and im.word(e - 4) == 0:
        e -= 4
    for a in (e - 4, e - 8):  # delay slot was a stripped nop / delay slot is a real instruction
        if a >= r.start and _terminal(decode(im.word(a), a)):
            return True
    return False


def build(files: Path) -> list[Module]:
    """Partition all modules; then add *resume points* and partition again.

    A resume point is the return address of a ``jal`` whose callee can (transitively) call ``setjmp``. After a
    ``longjmp`` the runtime continues execution at the restored ``ra`` and then keeps "returning" to ``r31``;
    each of those addresses must therefore be the start of a recompiled function.
    """
    mods = _build(files, {})
    extra = resume_points(mods)
    for _ in range(8):
        mods = _build(files, extra)
        cross = cross_targets(mods)
        if not cross:
            break
        for k, v in cross.items():
            extra.setdefault(k, set()).update(v)
    return mods


def cross_targets(mods: list["Module"]) -> dict[str, set[int]]:
    """Branch/jump targets that land inside a *different* function of the same module (not at its start)."""
    import bisect
    out: dict[str, set[int]] = {}
    for m in mods:
        funcs = sorted(m.funcs, key=lambda f: f.start)
        starts = [f.start for f in funcs]
        for f in funcs:
            for a in range(f.start, f.end, 4):
                ins = decode(m.image.word(a), a)
                if ins.kind not in (K_BRANCH, K_JUMP) or ins.target is None:
                    continue
                t = ins.target
                if f.start <= t < f.end or t in starts:
                    continue
                i = bisect.bisect_right(starts, t) - 1
                if i >= 0 and t < funcs[i].end:
                    out.setdefault(m.name, set()).add(t)
        if SWITCH_STARTS:
            # switch cases outside the function of their `jr` (a resume point split the switch): the dispatch falls
            # back to rt_jump, so they must be function starts too
            for jr, cases in m.jtables.items():
                i = bisect.bisect_right(starts, jr) - 1
                if i < 0 or not funcs[i].start <= jr < funcs[i].end or not _bounded_switch(m.image, jr):
                    continue
                for t in cases:
                    if not funcs[i].start <= t < funcs[i].end and t not in starts:
                        out.setdefault(m.name, set()).add(t)
    return out


def _bounded_switch(im: Image, jr: int, window: int = 16) -> bool:
    """A real switch: `sltiu` bound check within the window before the `jr` (find_jump_tables also reports pointer
    tables without one; their 'cases' are guesses)."""
    return any(decode(im.word(a), a).op == "sltiu" for a in range(max(im.base, jr - 4 * window), jr, 4))


def resume_points(mods: list["Module"]) -> dict[str, set[int]]:
    import bisect
    calls = []  # (module, function start, jal pc, target)
    for m in mods:
        starts = [f.start for f in m.funcs]
        for s_, e_ in m.text:
            for a in range(s_, e_, 4):
                w = m.image.word(a)
                if w >> 26 == 3:
                    t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                    fs = starts[bisect.bisect_right(starts, a) - 1]
                    calls.append((m.name, fs, a, t))
    reach = {SETJMP}
    changed = True
    while changed:
        changed = False
        for _mn, fs, _a, t in calls:
            if t in reach and fs not in reach:
                reach.add(fs)
                changed = True
    out: dict[str, set[int]] = {}
    for mn, _fs, a, t in calls:
        if t in reach:
            out.setdefault(mn, set()).add(a + 8)
    # calls through function pointers that reach setjmp (not visible statically; found at run time, profiles.py).
    # Not every jalr: a split inside a loop turns the loop into a chain of tail calls (stack overflow).
    for mn, addrs in EXTRA_RESUME.items():
        out.setdefault(mn, set()).update(addrs)
    return out


def _is_prologue(im: Image, a: int) -> bool:
    """`addiu sp, sp, -N` at a: the start of a stack frame (second entry points into functions look like this)."""
    ins = decode(im.word(a), a)
    return ins.op == "addiu" and ins.rs == 29 and ins.rt == 29 and ins.imm < 0


def _inside_loop(im: Image, a: int, hi: int, window: int = 0x2000) -> bool:
    """Is there a backward branch from >= a to < a before the next `jr ra` (a split at a would cut a loop)?"""
    pc = a
    while pc < min(hi, a + window):
        w = im.word(pc)
        ins = decode(w, pc)
        if ins.kind in (K_BRANCH, K_JUMP) and ins.target is not None and a - window <= ins.target < a:
            return True
        if w == 0x03E00008:
            return False
        pc += 4
    return False


def _code_constants(im: Image, text: list[tuple[int, int]]) -> set[int]:
    """32-bit constants built with `lui rX, hi` followed (within 8 instructions) by `addiu/ori rY, rX, lo`."""
    out = set()
    for s, e in text:
        lui: dict[int, tuple[int, int]] = {}
        for a in range(s, e, 4):
            ins = decode(im.word(a), a)
            if ins.op == "lui":
                lui[ins.rt] = (a, (ins.imm << 16) & 0xFFFFFFFF)
            elif ins.op in ("addiu", "ori") and ins.rs in lui and a - lui[ins.rs][0] <= 32:
                base = lui[ins.rs][1]
                v = (base + ins.imm) & 0xFFFFFFFF if ins.op == "addiu" else base | ins.imm
                if 0x80010000 <= v < 0x80200000 and not v & 3:
                    out.add(v)
    return out


def _overlay_jump_targets(im: Image, text: list[tuple[int, int]]) -> set[int]:
    """Addresses passed in a1 to the "load overlay a0 and longjmp to a1" function (OVL_JUMP): code of the overlay
    being loaded, not of the calling module (Arcade disc: ovl1 passes ovl0's 0x80011f64, which in ovl1 lies in the
    middle of a switch)."""
    out = set()
    if OVL_JUMP is None:
        return out
    for s, e in text:
        for a in range(s + 8, e, 4):
            w = im.word(a)
            if w >> 26 != 3 or (((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) != OVL_JUMP:
                continue
            hi = lo = None
            for b in range(a - 4, max(s, a - 32) - 4, -4):        # a1 = lui/addiu within the 8 instructions before
                ins = decode(im.word(b), b)
                if ins.op == "addiu" and ins.rt == 5 and ins.rs == 5 and lo is None:
                    lo = ins.imm
                elif ins.op == "lui" and ins.rt == 5:
                    hi = (ins.imm << 16) & 0xFFFFFFFF
                    break
            if hi is not None and lo is not None:
                out.add((hi + lo) & 0xFFFFFFFF)
    return out


def _extend_to_targets(m: "Module", targets: set[int], max_gap: int = 0x400) -> None:
    """A call target just before a text range (block-granular detection started too late): extend the range
    backwards when every word from the target up to the range start decodes as a valid instruction."""
    rngs = [list(r) for r in m.text]
    for t in sorted(targets):
        if not m.image.contains(t) or t & 3 or any(a <= t < b for a, b in rngs):
            continue
        for r in rngs:
            if 0 < r[0] - t <= max_gap:
                if all(decode(m.image.word(a), a).op != "invalid" for a in range(t, r[0], 4)):
                    r[0] = t
                break
    m.text = [tuple(r) for r in rngs]


def _build(files: Path, extra: dict[str, set[int]]) -> list[Module]:
    exe = load_psx_exe(Path(files) / EXE)
    ovls = load_overlays(Path(files) / "GT2.OVL")
    boot = Module("boot", exe, text_ranges(exe, start=BOOT_TEXT[0], end=BOOT_TEXT[1]), entry=None)
    res = Module("resident", exe, text_ranges(exe, start=RESIDENT_TEXT[0], end=RESIDENT_TEXT[1]), entry=exe.entry)
    mods = [res, boot]
    for i, o in enumerate(ovls):
        m = Module(o.name, o, [r for r in text_ranges(o) if tuple(r) not in TEXT_EXCLUDE.get(o.name, [])])
        m.entry = exe.word(OVERLAY_ENTRY_TABLE + 4 * i)
        mods.append(m)

    jal = {m.name: _jal_targets(m.image, m.text) for m in mods}
    everyone = set().union(*jal.values())
    for m in mods:
        _extend_to_targets(m, everyone if m.name == "resident" else jal[m.name] | jal["resident"])
    jal = {m.name: _jal_targets(m.image, m.text) for m in mods}
    everyone = set().union(*jal.values())
    # address constants built in code: own module, plus resident targets from any module
    consts: dict[str, set[int]] = {}
    for m in mods:
        cs = _code_constants(m.image, m.text) - _overlay_jump_targets(m.image, m.text)
        consts.setdefault(m.name, set()).update(cs)
        consts.setdefault("resident", set()).update(a for a in cs if a >= RESIDENT_TEXT[0])

    def in_text(m: Module, a: int) -> bool:
        return any(s <= a < e for s, e in m.text)

    for m in mods:
        lo, hi = m.text[0][0], m.text[-1][1]
        if m.name == "resident":
            strong = {t for t in everyone if in_text(m, t)}
            ptrs = set()
            for other in mods:
                if other.name == "boot":
                    continue
                ptrs |= code_pointers_in(other.image, lo, hi, m.text)
        elif m.name == "boot":
            strong = {t for t in jal["boot"] | jal["resident"] if in_text(m, t)}
            ptrs = code_pointers_in(m.image, lo, hi, m.text)
            ptrs = {p for p in ptrs if p < BOOT_TEXT[1]}
        else:
            strong = {t for t in jal[m.name] if in_text(m, t)}
            ptrs = code_pointers_in(m.image, lo, hi, m.text)
        if m.entry is not None and in_text(m, m.entry):
            strong.add(m.entry)
        m.resume = {a for a in extra.get(m.name, ()) if in_text(m, a)}
        strong |= {a for a in EXTRA_STARTS.get(m.name, ()) if in_text(m, a)}
        strong |= m.resume
        m.jtables = find_jump_tables(m.image, m.text)
        case_labels = {t for ts in m.jtables.values() for t in ts}
        # a pointer table to the extra entries looks like a jump table; they are functions
        case_labels -= EXTRA_STARTS.get(m.name, set())
        # Every code pointer that is not a jump-table case label is a function entry (possibly a second entry
        # into a function: splitting is always safe, control crossing the split becomes a tail call).
        ptrs |= {p for p in consts.get(m.name, set()) if in_text(m, p)}
        fptrs = {p for p in ptrs - case_labels
                 if p in strong or _ends_function(m.image, p, lo) or _is_prologue(m.image, p)
                 or not _inside_loop(m.image, p, hi)}
        m.labels = (ptrs - fptrs) | case_labels
        strong |= fptrs
        strong -= case_labels - {t for t in strong if t in jal[m.name] or t in m.resume}
        for r in partition(m.image, m.text, strong, m.jtables):
            m.funcs.append(Func(r.start, r.end, r.strong, falls_through=not _ends_terminal(m.image, r)))
    return mods


def to_json(mods: list[Module]) -> str:
    out = []
    for m in mods:
        out.append({
            "name": m.name,
            "base": m.image.base,
            "size": len(m.image.data),
            "entry": m.entry,
            "text": m.text,
            "labels": sorted(m.labels),
            "resume": sorted(m.resume),
            "funcs": [asdict(f) for f in m.funcs],
        })
    return json.dumps(out, indent=0)
