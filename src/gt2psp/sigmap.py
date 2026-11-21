"""Find code of one GT2 disc in the other: map an address of the Simulation disc's code to the Arcade disc.

Both discs are built from the same sources; code moved by a few hundred bytes. A function is found by a signature of
its first instructions with everything address-dependent masked: jump targets (j/jal), and the immediates of lui,
addiu/ori/load/store (they often build or use data addresses). Used to derive the per-disc constants (disc.py
profiles) and to port runtime hooks.
"""

from __future__ import annotations

import struct

from .image import Image


def _mask(w: int) -> int:
    op = w >> 26
    if op in (2, 3):                              # j / jal: target
        return w & 0xFC000000
    if op in (0x0F, 0x09, 0x0D) or 0x20 <= op <= 0x2E or op in (0x32, 0x3A):   # lui addiu ori loads stores lwc2/swc2
        return w & 0xFFFF0000
    if op in (1, 4, 5, 6, 7):                     # branches: offsets are position independent, keep
        return w
    return w


def signature(im: Image, addr: int, n: int = 12) -> tuple[int, ...]:
    return tuple(_mask(im.word(addr + 4 * i)) for i in range(n))


def find(src: Image, addr: int, dst: Image, n: int = 12, near: int | None = None,
         window: int = 0x10000) -> list[int]:
    """All addresses in dst whose masked n-instruction signature equals the one at src:addr.
    near: search only dst addresses within +-window of it."""
    sig = signature(src, addr, n)
    lo, hi = dst.base, dst.end - 4 * n
    if near is not None:
        lo, hi = max(lo, near - window), min(hi, near + window)
    words = [_mask(struct.unpack_from("<I", dst.data, o)[0]) for o in range(0, len(dst.data) - 3, 4)]
    out = []
    first = sig[0]
    for a in range(lo & ~3, hi, 4):
        i = (a - dst.base) >> 2
        if words[i] == first and tuple(words[i:i + n]) == sig:
            out.append(a)
    return out
