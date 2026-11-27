"""Derive a disc profile for another build of GT2 from a known one (profiles.py), by code signatures (sigmap.py).

    python -m gt2psp derive --ref arc --ref-files extracted-arcade/files --files build/plus-arc/files

Every code address of the reference profile (text ranges, setjmp/longjmp, overlay loader, extra starts, resume
points, the game patch sites, the runtime's card task) is looked up in the new disc's code: the masked instruction
signature around the address must match exactly once (near the expected place first, then anywhere in the module).
Data addresses (overlay entry table, race frame interval byte, card task context) are read from the instructions
that use them. The result is printed as a Profile(...) for profiles.py; anything not found is reported, never
guessed. Patch sites must also have the bit-identical original instruction (the patch replaces it).
"""

from __future__ import annotations

import struct
from pathlib import Path

from . import sigmap
from .image import Image, load_overlays, load_psx_exe
from .profiles import PROFILES, Profile


def _clipper_sites() -> set[tuple[str, int]]:
    from . import recomp
    return {("ovl0", b) for _k, b, *_ in recomp._TRACK_LOOPS}


CLIPPER_SITES = _clipper_sites()


def reads_registers(key: tuple[str, int]) -> bool:
    """Does the Simulation patch code at this site read a CPU register (beyond the one an override assigns)?"""
    import re
    from . import recomp
    if key in recomp.SIM_HOOKS:
        return True
    code = recomp.SIM_OVERRIDE.get(key, "")
    m = re.match(r"\s*c->r\[(\d+)\]\s*=", code)
    rest = code[m.end():] if m else code
    return "c->r[" in rest


class Code:
    """The code images of one disc: EXE (boot + resident) and the six overlays."""

    def __init__(self, files: Path, exe: str) -> None:
        self.exe = load_psx_exe(files / exe)
        self.ovl = {o.name: o for o in load_overlays(files / "GT2.OVL")}

    def image(self, mod: str) -> Image:
        return self.exe if mod in ("boot", "resident") else self.ovl[mod]


def find_exe(files: Path) -> str:
    cnf = (files / "SYSTEM.CNF").read_text(errors="replace")
    for line in cnf.splitlines():
        if line.strip().upper().startswith("BOOT"):
            return line.split("\\")[-1].split(";")[0].strip()
    raise SystemExit("SYSTEM.CNF names no boot executable")


class Mapper:
    def __init__(self, src: Code, dst: Code) -> None:
        self.src, self.dst = src, dst
        self.delta: dict[str, int] = {}     # last offset found per module (search hint)
        self.problems: list[str] = []

    def map(self, mod: str, a: int, what: str = "", exact: bool = False) -> int | None:
        """The address in the new disc for code address a of module mod; None (and a problem) when not unique.
        exact: the instruction word at the address must be bit-identical too (patch sites)."""
        s, d = self.src.image(mod), self.dst.image(mod)
        hint = a + self.delta.get(mod, 0)
        for back in (0, 4, 8, 16, 32, 64):
            start = a - 4 * back
            if start < s.base:
                continue
            for n in (back + 8, back + 16, back + 32):
                if start + 4 * n > s.end:
                    continue
                for near in (hint - 4 * back, None):
                    hits = sigmap.find(s, start, d, n, near=near, window=0x2000) if near is not None \
                        else sigmap.find(s, start, d, n)
                    if len(hits) == 1:
                        r = hits[0] + 4 * back
                        if exact and d.word(r) != s.word(a):
                            self.problems.append(f"{what or mod} {a:08x} -> {r:08x}: instruction differs "
                                                 f"({s.word(a):08x} / {d.word(r):08x})")
                            return None
                        self.delta[mod] = r - a
                        return r
                    if len(hits) > 1 and near is not None:
                        break     # ambiguous even close by: a longer signature
        self.problems.append(f"{what or mod} {a:08x}: no unique match")
        return None


def lui_addiu(img: Image, a: int, reg: int | None = None, span: int = 16) -> int | None:
    """The 32-bit constant built by a lui/addiu (or lui/load-store offset) pair starting at a."""
    w = img.word(a)
    if w >> 26 != 0x0F:
        return None
    rt, hi = (w >> 16) & 31, (w & 0xFFFF) << 16
    for i in range(1, span):
        x = img.word(a + 4 * i)
        op, rs = x >> 26, (x >> 21) & 31
        if rs == rt and (op == 0x09 or 0x20 <= op <= 0x2E):
            lo = x & 0xFFFF
            return (hi + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
    return None


def derive(ref: Profile, src_files: Path, files: Path, name: str, prefix: str) -> tuple[str, list[str]]:
    src = Code(src_files, ref.exe)
    exe = find_exe(files)
    dst = Code(files, exe)
    m = Mapper(src, dst)
    out: dict[str, object] = {}

    # resident code starts at the EXE entry point on every known build (CRT start)
    entry_src, entry_dst = load_entry(src_files / ref.exe), load_entry(files / exe)
    if entry_src != ref.resident_text[0]:
        m.problems.append(f"reference entry {entry_src:08x} is not the resident start {ref.resident_text[0]:08x}")
    m.delta["resident"] = entry_dst - entry_src
    res_end = m.map("resident", ref.resident_text[1] - 4, "resident text end")
    # the boot code (and its strings) ends where the embedded notice.tim.gz starts, on every known disc
    def gzip_at(e: Image) -> int | None:
        i = e.data.find(b"\x1f\x8b\x08", 0, 0x4000)
        return e.base + i if i >= 0 else None
    if gzip_at(src.exe) != ref.boot_text[1]:
        m.problems.append("boot text end: the reference's does not end at its gzip image")
    out["boot_text"] = (ref.boot_text[0], gzip_at(dst.exe))
    out["resident_text"] = (entry_dst, res_end + 4 if res_end else None)
    out["setjmp"] = m.map("resident", ref.setjmp, "setjmp")
    out["longjmp"] = m.map("resident", ref.longjmp, "longjmp")
    out["ovl_loader"] = m.map("resident", ref.ovl_loader, "overlay loader")
    out["ovl_jump"] = m.map("resident", ref.ovl_jump, "overlay jump") if ref.ovl_jump else None

    # overlay entry table: the loader addresses it with lui/addiu; take the same instruction pair on both discs
    out["overlay_entry_table"] = data_ref(m, "resident", ref.resident_text[0], ref.overlay_entry_table,
                                          "overlay table", span=ref.resident_text[1] - ref.resident_text[0])

    def map_set(d: dict[str, set[int]], what: str) -> dict[str, set[int]]:
        r: dict[str, set[int]] = {}
        for mod, addrs in d.items():
            r[mod] = {x for x in (m.map(mod, a, f"{what} {mod}") for a in sorted(addrs)) if x is not None}
        return r

    out["extra_starts"] = map_set(ref.extra_starts, "extra start")
    out["extra_resume"] = map_set(ref.extra_resume, "extra resume")
    out["resume_inside"] = map_set(ref.resume_inside, "resume inside")
    out["text_exclude"] = {mod: [(m.map(mod, lo, "text exclude"), m.map(mod, hi - 4, "text exclude end") + 4)
                                 for lo, hi in rs] for mod, rs in ref.text_exclude.items()}

    # game patch sites: Simulation address -> this disc (through the reference's own map when it has one)
    if ref.patch_map:
        sites = dict(ref.patch_map)
    else:                    # the Simulation disc itself: its patch sites are the recompiler's HOOKS/OVERRIDE keys
        from . import recomp
        sites = {k: k[1] for k in list(recomp.SIM_HOOKS) + list(recomp.SIM_OVERRIDE)}
    pm = {}
    for key in sorted(sites):
        mod = key[0]
        a = sites[key]
        r = m.map(mod, a, f"patch site {mod}", exact=True)
        if r is not None:
            # a patch that reads registers relies on the code around it: that must match too (the near-plane
            # clipper loops start 0x74 before their branch). An override that only sets the register its
            # instruction sets (`c->r[2] = g_ws_menus ? ...`) needs just the identical instruction.
            lo = 0x74 if (mod, key[1]) in CLIPPER_SITES else 32
            si, di = src.image(mod), dst.image(mod)
            if not reads_registers(key):
                pm[key] = r
            elif any(sigmap._mask(si.word(a + o)) != sigmap._mask(di.word(r + o)) for o in range(-lo, 36, 4)):
                m.problems.append(f"patch site {mod} {a:08x} -> {r:08x}: surrounding code differs")
            else:
                pm[key] = r
    out["patch_map"] = pm

    # runtime constants (runtime game.h): card task + its context, race frame interval byte
    rt = {}
    if ref.card_task:
        ct = m.map("resident", ref.card_task, "card task", exact=False)
        if ct is not None:
            rt["card_task"] = ct
            rt["card_ctx"] = lui_addiu(dst.exe, ct)
            if lui_addiu(src.exe, ref.card_task) != ref.card_ctx:
                m.problems.append("card task: the reference does not build its context with lui/addiu")
    if ref.interval_ref:
        mod, a = ref.interval_ref
        r = m.map(mod, a, "interval reference")
        if r is not None:
            v_src, v_dst = lui_addiu(src.image(mod), a), lui_addiu(dst.image(mod), r)
            if v_src is None or v_dst is None:
                m.problems.append("interval reference is not a lui/addiu pair")
            else:
                rt["interval"] = (ref.interval + v_dst - v_src) & 0x1FFFFF
                rt["interval_ref"] = (mod, r)
    out.update(rt)

    lines = [f"{name.upper().replace('-', '_')} = Profile(",
             f'    name="{name}", exe="{exe}", prefix="{prefix}",']
    for k, v in out.items():
        lines.append(f"    {k}={fmt(v)},")
    lines.append(")")
    return "\n".join(lines), m.problems


def data_ref(m: Mapper, mod: str, func: int, value: int, what: str, span: int) -> int | None:
    """A data address the reference uses inside [func, func+span): find the lui/addiu pairs building it, map one
    to the new disc and read the value there (all pairs that map must agree)."""
    s = m.src.image(mod)
    found = set()
    for a in range(func, func + span - 64, 4):
        if lui_addiu(s, a) == value:
            r = m.map(mod, a, what)
            if r is not None:
                found.add(lui_addiu(m.dst.image(mod), r))
    if len(found) == 1:
        return found.pop()
    if found:
        m.problems.append(f"{what}: references disagree ({', '.join(f'{v:08x}' for v in found)})")
        return None
    m.problems.append(f"{what}: {value:08x} not built by a lui/addiu pair in {func:08x}+{span:#x}")
    return None


def load_entry(path: Path) -> int:
    return struct.unpack_from("<I", path.read_bytes(), 0x10)[0]


def fmt(v) -> str:
    if v is None:
        return "None  # NOT FOUND"
    if isinstance(v, int):
        return f"0x{v:08X}" if v > 0xFFFF else f"0x{v:X}"
    if isinstance(v, tuple):
        return "(" + ", ".join(fmt(x) if not isinstance(x, str) else f'"{x}"' for x in v) + ")"
    if isinstance(v, list):
        return "[" + ", ".join(fmt(x) for x in v) + "]"
    if isinstance(v, set):
        return "{" + ", ".join(fmt(x) for x in sorted(v)) + "}" if v else "set()"
    if isinstance(v, dict):
        items = []
        for k, x in sorted(v.items()):
            kk = f'"{k}"' if isinstance(k, str) else "(" + f'"{k[0]}", {fmt(k[1])}' + ")"
            items.append(f"\n        {kk}: {fmt(x)}")
        return "{" + ",".join(items) + "\n    }" if items else "{}"
    return repr(v)


def cmd(args) -> None:
    ref = PROFILES[args.ref]
    text, problems = derive(ref, Path(args.ref_files), Path(args.files), args.name, args.prefix)
    print(text)
    print(f"# {len(problems)} problems")
    for p in problems:
        print("#  ", p)
