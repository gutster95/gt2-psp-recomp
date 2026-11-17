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

from .analysis import Region, code_pointers_in, partition, text_ranges
from .image import Image, PsxExe, load_overlays, load_psx_exe
from .mips import K_BRANCH, K_JR, K_JUMP, decode

BOOT_TEXT = (0x80010000, 0x80011DF8)      # boot code + strings, up to the embedded notice.tim.gz
RESIDENT_TEXT = (0x8005D600, 0x8008E020)
OVERLAY_ENTRY_TABLE = 0x80091174
NUM_OVERLAYS = 6


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
    exe = load_psx_exe(Path(files) / "SCUS_944.88")
    ovls = load_overlays(Path(files) / "GT2.OVL")
    boot = Module("boot", exe, text_ranges(exe, start=BOOT_TEXT[0], end=BOOT_TEXT[1]), entry=None)
    res = Module("resident", exe, text_ranges(exe, start=RESIDENT_TEXT[0], end=RESIDENT_TEXT[1]), entry=exe.entry)
    mods = [res, boot]
    for i, o in enumerate(ovls):
        m = Module(o.name, o, text_ranges(o))
        m.entry = exe.word(OVERLAY_ENTRY_TABLE + 4 * i)
        mods.append(m)

    jal = {m.name: _jal_targets(m.image, m.text) for m in mods}
    everyone = set().union(*jal.values())

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
        fptrs = {p for p in ptrs if p in strong or _ends_function(m.image, p, lo)}
        m.labels = ptrs - fptrs
        strong |= fptrs
        for r in partition(m.image, m.text, strong):
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
            "funcs": [asdict(f) for f in m.funcs],
        })
    return json.dumps(out, indent=0)
