"""Kanji ROM layout as the Japanese GT2 sees it: runs the game's own JIS -> glyph address function (0x80080024 on
the Japanese Arcade disc SCPS-10116, 0x8008040C on the Gran Turismo disc SCPS-10117; found by its first two
instructions) in a tiny MIPS interpreter for every JIS X 0208 code and prints / returns the
offset of each glyph in the BIOS kanji font (base = Krom2RawAdd(0x8140) = 0xBFC66000, 30 bytes per 16x15 glyph).
The PS1 BIOS ROM is not available on the PSP; scripts/make_kanji.py fills these offsets from a free bitmap font.

    python scripts/kanji_map.py <SCPS_101.16>        -> prints how many codes map, first/last offsets
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

BASE = 0xBFC66000
SIG = bytes.fromhex("dfde8424ff008530")   # addiu a0, a0, -0x2121; andi a1, a0, 0xff


def load_exe(path: Path) -> tuple[int, bytes]:
    d = path.read_bytes()
    return struct.unpack_from("<I", d, 0x18)[0], d[0x800:]


def s16(x: int) -> int:
    return x - 0x10000 if x & 0x8000 else x


def find(base: int, text: bytes) -> tuple[int, int]:
    """(function address, address of the variable with the font base = Krom2RawAdd(0x8140))."""
    i = text.find(SIG)
    if i < 0 or i % 4:
        raise ValueError("kanji function not found")
    lui, lw = struct.unpack_from("<II", text, i + 8)
    if lui >> 26 != 15 or lw >> 26 != 35:
        raise ValueError("unexpected code at the kanji function")
    return base + i, ((lui & 0xFFFF) << 16) + s16(lw & 0xFFFF)


def run(base: int, text: bytes, jis: int, func: int, var: int) -> int:
    """Address the game computes for one JIS code (a0 = JIS code, as the glyph code passes it: 0x2121..0x7E7E)."""
    mem: dict[int, int] = {var: BASE}
    r = [0] * 32
    r[4] = jis
    r[31] = 0xDEAD0000

    def lw(a: int) -> int:
        if a in mem:
            return mem[a]
        o = a - base
        return struct.unpack_from("<I", text, o)[0] if 0 <= o < len(text) - 3 else 0

    def lb(a: int, signed: bool) -> int:
        o = a - base
        v = text[o] if 0 <= o < len(text) else 0
        return v - 0x100 if signed and v & 0x80 else v

    def lh(a: int) -> int:
        o = a - base
        v = struct.unpack_from("<H", text, o)[0]
        return s16(v)

    pc, npc = func, func + 4
    for _ in range(4000):
        w = struct.unpack_from("<I", text, pc - base)[0]
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        imm, sh, fn = w & 0xFFFF, (w >> 6) & 31, w & 63
        target = None
        if op == 0:
            if fn == 0x00: r[rd] = (r[rt] << sh) & 0xFFFFFFFF                          # sll
            elif fn == 0x02: r[rd] = r[rt] >> sh                                       # srl
            elif fn == 0x03: r[rd] = (s32(r[rt]) >> sh) & 0xFFFFFFFF                   # sra
            elif fn == 0x08: target = r[rs]                                            # jr
            elif fn == 0x21: r[rd] = (r[rs] + r[rt]) & 0xFFFFFFFF                      # addu
            elif fn == 0x23: r[rd] = (r[rs] - r[rt]) & 0xFFFFFFFF                      # subu
            elif fn == 0x24: r[rd] = r[rs] & r[rt]
            elif fn == 0x25: r[rd] = r[rs] | r[rt]
            elif fn == 0x2A: r[rd] = int(s32(r[rs]) < s32(r[rt]))
            elif fn == 0x2B: r[rd] = int(r[rs] < r[rt])
            else: raise ValueError(f"op {w:08x} at {pc:08x}")
        elif op == 2: target = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)                # j
        elif op == 4: target = pc + 4 + (s16(imm) << 2) if r[rs] == r[rt] else None     # beq
        elif op == 5: target = pc + 4 + (s16(imm) << 2) if r[rs] != r[rt] else None     # bne
        elif op == 6: target = pc + 4 + (s16(imm) << 2) if s32(r[rs]) <= 0 else None    # blez
        elif op == 7: target = pc + 4 + (s16(imm) << 2) if s32(r[rs]) > 0 else None     # bgtz
        elif op == 9: r[rt] = (r[rs] + s16(imm)) & 0xFFFFFFFF                            # addiu
        elif op == 10: r[rt] = int(s32(r[rs]) < s16(imm))                                # slti
        elif op == 11: r[rt] = int(r[rs] < (s16(imm) & 0xFFFFFFFF))                      # sltiu
        elif op == 12: r[rt] = r[rs] & imm                                               # andi
        elif op == 13: r[rt] = r[rs] | imm                                               # ori
        elif op == 15: r[rt] = (imm << 16) & 0xFFFFFFFF                                 # lui
        elif op == 32: r[rt] = lb(r[rs] + s16(imm), True) & 0xFFFFFFFF                   # lb
        elif op == 33: r[rt] = lh(r[rs] + s16(imm)) & 0xFFFFFFFF                         # lh
        elif op == 35: r[rt] = lw((r[rs] + s16(imm)) & 0xFFFFFFFF)                       # lw
        elif op == 36: r[rt] = lb(r[rs] + s16(imm), False)                               # lbu
        else: raise ValueError(f"op {w:08x} at {pc:08x}")
        r[0] = 0
        pc, npc = npc, (target if target is not None else npc + 4)
        if pc == 0xDEAD0000:
            return r[2]
    raise RuntimeError(f"no return for {jis:04x}")


def s32(x: int) -> int:
    return x - 0x100000000 if x & 0x80000000 else x


def glyph_map(exe: Path) -> dict[int, int]:
    """JIS X 0208 code (0x2121..0x7E7E) -> offset of its glyph from the font base (only codes with their own glyph)."""
    base, text = load_exe(exe)
    func, var = find(base, text)
    out: dict[int, int] = {}
    for hi in range(0x21, 0x7F):
        for lo in range(0x21, 0x7F):
            a = run(base, text, hi << 8 | lo, func, var)
            if a != BASE or (hi, lo) == (0x21, 0x21):
                out[hi << 8 | lo] = a - BASE
    return out


if __name__ == "__main__":
    m = glyph_map(Path(sys.argv[1]))
    offs = sorted(set(m.values()))
    print(f"{len(m)} codes, {len(offs)} glyph slots, offsets 0x{offs[0]:x}..0x{offs[-1]:x}, "
          f"ROM end 0x{BASE + offs[-1] + 30:08x}")
    for code in (0x2121, 0x2122, 0x2330, 0x2421, 0x2521, 0x3021, 0x4F53, 0x5021):
        print(f"  {code:04x} -> {m.get(code, -1):#x}")
