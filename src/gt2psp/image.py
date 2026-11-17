"""Code images: the PS-X EXE main executable and the gzip-packed overlays in GT2.OVL."""

from __future__ import annotations

import gzip
import struct
from dataclasses import dataclass
from pathlib import Path

OVERLAY_BASE = 0x80010000


@dataclass
class Image:
    name: str
    base: int
    data: bytes

    @property
    def end(self) -> int:
        return self.base + len(self.data)

    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.end

    def word(self, addr: int) -> int:
        o = addr - self.base
        return struct.unpack_from("<I", self.data, o)[0]


@dataclass
class PsxExe(Image):
    entry: int = 0
    gp: int = 0
    sp: int = 0
    bss_addr: int = 0
    bss_size: int = 0


def load_psx_exe(path: Path) -> PsxExe:
    raw = Path(path).read_bytes()
    if raw[:8] != b"PS-X EXE":
        raise ValueError(f"{path}: not a PS-X EXE")
    pc0, gp0, t_addr, t_size, _d_addr, _d_size, b_addr, b_size, s_addr, s_size = struct.unpack_from("<10I", raw, 0x10)
    data = raw[0x800:0x800 + t_size]
    return PsxExe(Path(path).name, t_addr, data, entry=pc0, gp=gp0, sp=s_addr + s_size,
                  bss_addr=b_addr, bss_size=b_size)


def load_overlays(path: Path) -> list[Image]:
    """GT2.OVL: header of 6 (offset, size) pairs, each member a gzip stream of raw code for 0x80010000."""
    raw = Path(path).read_bytes()
    out = []
    hdr = struct.unpack_from("<12I", raw, 0)
    for i in range(6):
        off, size = hdr[2 * i], hdr[2 * i + 1]
        out.append(Image(f"ovl{i}", OVERLAY_BASE, gzip.decompress(raw[off:off + size])))
    return out
