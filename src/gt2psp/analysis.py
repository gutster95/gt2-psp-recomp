"""Static analysis of PS1 code images: function discovery, BIOS calls, hardware register accesses.

The analysis works on an *address space*: a list of images searched in order, so that an overlay can be analysed
with the resident part of the main executable behind it.

Function discovery is recursive descent from known entry points (EXE entry, ``jal`` targets, tail-call ``j``
targets). Within a function, a small forward constant propagation (reset at every block start) recovers
``lui``/``addiu``/``ori`` constants; this is enough to identify

* BIOS stubs (``li t2, 0xA0/0xB0/0xC0; jr t2; li t1, N``),
* accesses to the scratchpad and the I/O area (``0x1F800000`` - ``0x1F802FFF``),
* address constants pointing into code (function pointers, jump tables).
"""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass, field

from .image import Image
from .mips import (K_BRANCH, K_CALL, K_JALR, K_JR, K_JUMP, K_TRAP, LOADS, STORES, Insn, decode)

IO_NAMES = {
    0x1F801000: "EXP1_BASE", 0x1F801004: "EXP2_BASE", 0x1F801008: "EXP1_DELAY", 0x1F80100C: "EXP3_DELAY",
    0x1F801010: "BIOS_DELAY", 0x1F801014: "SPU_DELAY", 0x1F801018: "CDROM_DELAY", 0x1F80101C: "EXP2_DELAY",
    0x1F801020: "COM_DELAY", 0x1F801060: "RAM_SIZE",
    0x1F801040: "JOY_DATA", 0x1F801044: "JOY_STAT", 0x1F801048: "JOY_MODE", 0x1F80104A: "JOY_CTRL",
    0x1F80104E: "JOY_BAUD",
    0x1F801050: "SIO_DATA", 0x1F801054: "SIO_STAT", 0x1F801058: "SIO_MODE", 0x1F80105A: "SIO_CTRL",
    0x1F801070: "I_STAT", 0x1F801074: "I_MASK",
    0x1F8010F0: "DPCR", 0x1F8010F4: "DICR",
    0x1F801800: "CD_INDEX", 0x1F801801: "CD_REG1", 0x1F801802: "CD_REG2", 0x1F801803: "CD_REG3",
    0x1F801810: "GP0/GPUREAD", 0x1F801814: "GP1/GPUSTAT",
    0x1F801820: "MDEC0", 0x1F801824: "MDEC1",
    0x1F802041: "POST",
}
for _ch, _n in enumerate(["MDECin", "MDECout", "GPU", "CDROM", "SPU", "PIO", "OTC"]):
    _b = 0x1F801080 + _ch * 0x10
    IO_NAMES[_b] = f"DMA{_ch}_{_n}_MADR"
    IO_NAMES[_b + 4] = f"DMA{_ch}_{_n}_BCR"
    IO_NAMES[_b + 8] = f"DMA{_ch}_{_n}_CHCR"
for _t in range(3):
    _b = 0x1F801100 + _t * 0x10
    IO_NAMES[_b] = f"TIMER{_t}_COUNT"
    IO_NAMES[_b + 4] = f"TIMER{_t}_MODE"
    IO_NAMES[_b + 8] = f"TIMER{_t}_TARGET"


def io_name(addr: int) -> str:
    if addr in IO_NAMES:
        return IO_NAMES[addr]
    if 0x1F800000 <= addr < 0x1F800400:
        return "SCRATCHPAD"
    if 0x1F801C00 <= addr < 0x1F802000:
        return "SPU"
    return "IO?"


@dataclass
class Function:
    addr: int
    image: str
    end: int = 0
    calls: set = field(default_factory=set)        # direct jal targets
    tailcalls: set = field(default_factory=set)
    callers: set = field(default_factory=set)
    computed_jumps: list = field(default_factory=list)   # pcs of jr reg (not ra)
    computed_calls: list = field(default_factory=list)   # pcs of jalr
    bios: tuple | None = None                      # (table, number) if the function is a BIOS stub
    syscalls: list = field(default_factory=list)
    io: list = field(default_factory=list)         # (pc, op, addr)
    code_ptrs: set = field(default_factory=set)    # constants that point into code
    gte_ops: int = 0
    errors: list = field(default_factory=list)


class Space:
    def __init__(self, images: list[Image]):
        self.images = images

    def find(self, addr: int) -> Image | None:
        a = addr & 0x1FFFFFFF | 0x80000000 if (addr & 0xE0000000) in (0x00000000, 0xA0000000) else addr
        for im in self.images:
            if im.contains(a):
                return im
        return None

    def insn(self, addr: int) -> tuple[Image, Insn] | None:
        im = self.find(addr)
        if im is None or addr & 3:
            return None
        return im, decode(im.word(addr), addr)


MAX_INTRA_JUMP = 0x4000  # forward `j` within this distance is treated as intra-function


def _const_step(ins: Insn, regs: dict) -> None:
    """Update the known-constant register map for one instruction (very small subset)."""
    op = ins.op
    dst = None
    val = None
    if op == "lui":
        dst, val = ins.rt, (ins.imm << 16) & 0xFFFFFFFF
    elif op in ("addiu", "addi"):
        dst = ins.rt
        if ins.rs == 0:
            val = ins.imm & 0xFFFFFFFF
        elif ins.rs in regs:
            val = (regs[ins.rs] + ins.imm) & 0xFFFFFFFF
    elif op == "ori":
        dst = ins.rt
        if ins.rs == 0:
            val = ins.imm
        elif ins.rs in regs:
            val = regs[ins.rs] | ins.imm
    elif op in ("addu", "or") and (ins.rs == 0 or ins.rt == 0):
        dst = ins.rd
        src = ins.rt if ins.rs == 0 else ins.rs
        if src == 0:
            val = 0
        elif src in regs:
            val = regs[src]
    elif ins.fmt in ("d,s,t", "d,t,s", "d,t,sa", "d", "d,s"):
        dst = ins.rd
    elif ins.fmt in ("t,s,i", "t,s,u", "t,u") or op in LOADS or op in ("mfc0", "mfc2", "cfc2"):
        dst = ins.rt
    if ins.kind in (K_CALL, K_JALR):
        # caller-saved registers are clobbered by the callee (applied after the delay slot by caller)
        pass
    if dst is not None and dst != 0:
        if val is None:
            regs.pop(dst, None)
        else:
            regs[dst] = val


def analyse_function(space: Space, addr: int, known_funcs: set[int]) -> Function:
    hit = space.find(addr)
    fn = Function(addr, hit.name if hit else "?")
    visited: set[int] = set()
    work = [(addr, {})]
    last = addr
    while work:
        pc, regs = work.pop()
        regs = dict(regs)
        while True:
            if pc in visited:
                break
            r = space.insn(pc)
            if r is None:
                fn.errors.append((pc, "outside image"))
                break
            im, ins = r
            visited.add(pc)
            last = max(last, pc)
            if ins.op == "invalid":
                fn.errors.append((pc, f"invalid {ins.word:08x}"))
                break
            if ins.extra.get("gte"):
                fn.gte_ops += 1
            # memory access through a known base
            if (ins.op in LOADS or ins.op in STORES or ins.op in ("lwc2", "swc2")) and ins.rs in regs:
                ea = (regs[ins.rs] + ins.imm) & 0xFFFFFFFF
                pa = ea & 0x1FFFFFFF
                if 0x1F800000 <= pa < 0x1F803000:
                    fn.io.append((pc, ins.op, pa))
            if ins.kind == K_TRAP:
                if ins.op == "syscall":
                    fn.syscalls.append((pc, regs.get(4)))
                break_after = ins.op == "break"
            else:
                break_after = False
            if not ins.has_delay_slot:
                _const_step(ins, regs)
                for v in list(regs.values()):
                    if 0x80010000 <= v < 0x80200000 and space.find(v) and v != addr:
                        fn.code_ptrs.add(v)
                if break_after:
                    break
                pc += 4
                continue
            # control flow with delay slot: execute delay slot effects for const tracking
            ds = space.insn(pc + 4)
            if ds is None:
                fn.errors.append((pc + 4, "delay slot outside image"))
                break
            visited.add(pc + 4)
            last = max(last, pc + 4)
            regs_before_ds = dict(regs)
            _const_step(ds[1], regs)
            k = ins.kind
            if k == K_BRANCH:
                work.append((ins.target, regs))
                if ins.extra.get("always"):
                    break
                pc += 8
                continue
            if k == K_JUMP:
                t = ins.target
                if t in known_funcs or t < addr or t - pc > MAX_INTRA_JUMP or space.find(t) is None:
                    fn.tailcalls.add(t)
                    break
                pc = t
                continue
            if k == K_CALL:
                fn.calls.add(ins.target)
                for rr in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                    regs.pop(rr, None)
                pc += 8
                continue
            if k == K_JR:
                if ins.rs == 31:
                    break
                tv = regs_before_ds.get(ins.rs)
                if tv in (0xA0, 0xB0, 0xC0):
                    fn.bios = (tv, regs.get(9))
                else:
                    fn.computed_jumps.append(pc)
                break
            if k == K_JALR:
                fn.computed_calls.append(pc)
                for rr in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                    regs.pop(rr, None)
                pc += 8
                continue
    fn.end = last + 4
    return fn


def discover(space: Space, entries: list[int], limit_image: str | None = None) -> dict[int, Function]:
    """Recursive descent over call graph. Only functions inside ``limit_image`` (if given) are analysed;
    calls leaving it are recorded as external."""
    funcs: dict[int, Function] = {}
    known = set(entries)
    work = list(entries)
    while work:
        a = work.pop()
        if a in funcs:
            continue
        im = space.find(a)
        if im is None or (limit_image and im.name != limit_image):
            continue
        f = analyse_function(space, a, known)
        funcs[a] = f
        for t in f.calls | f.tailcalls:
            if t not in known:
                known.add(t)
                work.append(t)
    for f in funcs.values():
        for t in f.calls | f.tailcalls:
            if t in funcs:
                funcs[t].callers.add(f.addr)
    return funcs


# Known BIOS function names (subset, from nocash psx-spx)
BIOS_NAMES = {
    (0xA0, 0x00): "open", (0xA0, 0x01): "lseek", (0xA0, 0x02): "read", (0xA0, 0x03): "write",
    (0xA0, 0x04): "close", (0xA0, 0x13): "setjmp", (0xA0, 0x14): "longjmp", (0xA0, 0x17): "strcmp",
    (0xA0, 0x19): "strcpy", (0xA0, 0x1B): "strlen", (0xA0, 0x25): "toupper", (0xA0, 0x2A): "memcpy",
    (0xA0, 0x2B): "memset", (0xA0, 0x2F): "rand", (0xA0, 0x30): "srand", (0xA0, 0x33): "malloc",
    (0xA0, 0x34): "free", (0xA0, 0x39): "InitHeap", (0xA0, 0x3F): "printf", (0xA0, 0x44): "FlushCache",
    (0xA0, 0x49): "GPU_cw", (0xA0, 0x72): "CdRemove", (0xA0, 0x96): "AddCDROMDevice",
    (0xA0, 0x97): "AddMemCardDevice", (0xA0, 0x99): "AddDummyTtyDevice", (0xA0, 0x9F): "SetMemSize",
    (0xA0, 0xA0): "WarmBoot", (0xA0, 0xA2): "EnqueueCdIntr", (0xA0, 0xA3): "DequeueCdIntr",
    (0xA0, 0xAB): "_card_info", (0xA0, 0xAC): "_card_load",
    (0xB0, 0x00): "alloc_kernel_memory", (0xB0, 0x07): "DeliverEvent", (0xB0, 0x08): "OpenEvent",
    (0xB0, 0x09): "CloseEvent", (0xB0, 0x0A): "WaitEvent", (0xB0, 0x0B): "TestEvent",
    (0xB0, 0x0C): "EnableEvent", (0xB0, 0x0D): "DisableEvent", (0xB0, 0x0E): "OpenThread",
    (0xB0, 0x0F): "CloseThread", (0xB0, 0x10): "ChangeThread", (0xB0, 0x12): "InitPad",
    (0xB0, 0x13): "StartPad", (0xB0, 0x14): "StopPad", (0xB0, 0x15): "OutdatedPadInitAndStart",
    (0xB0, 0x16): "OutdatedPadGetButtons", (0xB0, 0x17): "ReturnFromException",
    (0xB0, 0x18): "ResetEntryInt", (0xB0, 0x19): "HookEntryInt", (0xB0, 0x20): "UnDeliverEvent",
    (0xB0, 0x32): "open", (0xB0, 0x33): "lseek", (0xB0, 0x34): "read", (0xB0, 0x35): "write",
    (0xB0, 0x36): "close", (0xB0, 0x3F): "puts", (0xB0, 0x42): "firstfile", (0xB0, 0x43): "nextfile",
    (0xB0, 0x44): "rename", (0xB0, 0x45): "erase", (0xB0, 0x47): "AddDevice", (0xB0, 0x4A): "InitCard",
    (0xB0, 0x4B): "StartCard", (0xB0, 0x4C): "StopCard", (0xB0, 0x4E): "write_card_sector",
    (0xB0, 0x4F): "read_card_sector", (0xB0, 0x50): "allow_new_card", (0xB0, 0x56): "GetC0Table",
    (0xB0, 0x57): "GetB0Table", (0xB0, 0x58): "get_bu_callback_port", (0xB0, 0x5B): "ChangeClearPad",
    (0xC0, 0x00): "EnqueueTimerAndVblankIrqs", (0xC0, 0x01): "EnqueueSyscallHandler",
    (0xC0, 0x02): "SysEnqIntRP", (0xC0, 0x03): "SysDeqIntRP", (0xC0, 0x07): "InstallExceptionHandlers",
    (0xC0, 0x08): "SysInitMemory", (0xC0, 0x0A): "ChangeClearRCnt", (0xC0, 0x12): "InstallDevices",
    (0xC0, 0x1C): "AdjustA0Table",
}


def bios_name(table: int, num: int | None) -> str:
    if num is None:
        return f"{table:02X}(?)"
    return f"{table:02X}:{num:02X} {BIOS_NAMES.get((table, num), '?')}"


def summarize(funcs: dict[int, Function]) -> dict:
    io = Counter()
    for f in funcs.values():
        for _pc, op, a in f.io:
            io[(io_name(a), a, "w" if op.startswith("s") else "r")] += 1
    return {
        "functions": len(funcs),
        "bios": sorted((f.addr, f.bios) for f in funcs.values() if f.bios),
        "io": io,
        "computed_jumps": sum(len(f.computed_jumps) for f in funcs.values()),
        "computed_calls": sum(len(f.computed_calls) for f in funcs.values()),
        "gte_funcs": sum(1 for f in funcs.values() if f.gte_ops),
        "errors": [(f.addr, e) for f in funcs.values() for e in f.errors],
        "syscalls": [(f.addr, s) for f in funcs.values() for s in f.syscalls],
    }


def callers_of(funcs: dict[int, Function]) -> dict[int, set]:
    d = defaultdict(set)
    for f in funcs.values():
        for t in f.calls:
            d[t].add(f.addr)
    return d


# ---------------------------------------------------------------------------------------------------------------
# Whole-image partitioning
# ---------------------------------------------------------------------------------------------------------------

@dataclass
class Region:
    start: int
    end: int
    strong: bool       # start is a direct call target or a code pointer found in data
    valid: bool = True # decodes as plausible code
    why: str = ""


def text_ranges(im: Image, gran: int = 0x100, start: int | None = None, end: int | None = None) -> list[tuple[int, int]]:
    """Runs of ``gran``-sized blocks without invalid opcodes (all-zero blocks are only kept inside code)."""
    a = start or im.base
    stop = end or im.end
    blocks = []
    while a < stop:
        n = min(gran, stop - a) // 4
        words = [im.word(a + 4 * i) for i in range(n)]
        bad = any(decode(w, a + 4 * i).op == "invalid" for i, w in enumerate(words))
        zero = not any(words)
        blocks.append((a, a + 4 * n, bad, zero))
        a += gran
    out: list[list[int]] = []
    for s, e, bad, zero in blocks:
        if bad:
            continue
        if out and out[-1][1] == s:
            out[-1][1] = e
        elif not zero:
            out.append([s, e])
    # Refine edges at instruction granularity: extend forward over code that was cut off at a block with an
    # invalid word (up to the last unconditional transfer before it), then trim trailing data after the last
    # unconditional transfer + delay slot. Runs without any `jr` are data that happens to decode.
    res = []
    for s, e in out:
        x = e
        while x < stop and decode(im.word(x), x).op != "invalid":
            x += 4
        e2 = _last_terminal_end(im, s, x)
        if e2 is None:
            continue
        if not any(im.word(a) & 0xFC1FFFFF == 0x00000008 for a in range(s, e2, 4)):
            continue
        if res and res[-1][1] >= s:
            res[-1] = (res[-1][0], max(e2, res[-1][1]))
        else:
            res.append((s, e2))
    return res


def _last_terminal_end(im: Image, s: int, e: int) -> int | None:
    """End address (after the delay slot) of the last unconditional transfer in [s, e)."""
    a = e - 8
    while a >= s:
        ins = decode(im.word(a), a)
        if ins.kind in (K_JR, K_JUMP) or ins.extra.get("always"):
            return a + 8
        a -= 4
    return None


def code_pointers_in(im: Image, lo: int, hi: int, text: list[tuple[int, int]]) -> set[int]:
    """All aligned words of ``im`` (anywhere) whose value is a 4-aligned address inside a text range."""
    out = set()
    data = im.data
    for o in range(0, len(data) - 3, 4):
        v = int.from_bytes(data[o:o + 4], "little")
        if lo <= v < hi and not v & 3:
            for s, e in text:
                if s <= v < e:
                    out.add(v)
                    break
    return out


def _is_boundary_after(im: Image, a: int) -> bool:
    """True if ``a`` directly follows `jr ra; <ds>` (optionally followed by nop padding)."""
    b = a - 8
    while b >= im.base and im.word(b + 4) == 0 and im.word(b) == 0:
        b -= 4
    return b >= im.base and im.word(b) == 0x03E00008


def partition(im: Image, text: list[tuple[int, int]], strong: set[int]) -> list[Region]:
    """Split text ranges into functions in one linear sweep.

    Candidate starts are the beginning of each text range, every strong start, and every address following
    ``jr ra; <delay slot>`` (+ nop padding). A weak candidate is dropped when an earlier branch of the current
    function targets it or something beyond it (the function continues past an early return).
    """
    regions: list[Region] = []
    for s, e in text:
        weak = set()
        for a in range(s, e, 4):
            if im.word(a) == 0x03E00008:  # jr ra
                n = a + 8
                while n < e and im.word(n) == 0:
                    n += 4
                if n < e:
                    weak.add(n)
        fs = s
        reach = s
        for pc in range(s, e, 4):
            if pc != fs and (pc in strong or (pc in weak and reach <= pc)):
                regions.append(Region(fs, pc, fs in strong))
                fs = pc
                reach = pc
            ins = decode(im.word(pc), pc)
            if ins.kind in (K_BRANCH, K_JUMP) and ins.target is not None and pc < ins.target < e:
                if ins.kind == K_JUMP and ins.target in strong:
                    continue  # tail call
                reach = max(reach, ins.target + 4)
        regions.append(Region(fs, e, fs in strong))
    return regions
