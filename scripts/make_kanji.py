"""KANJI.BIN: a stand-in for the PS1 BIOS ROM's kanji font (Japanese GT2), built from a free 16x16 JIS X 0208 bitmap
font (X.Org jis-misc jiskan16.bdf). The glyphs are put where the game's own JIS -> address code expects them
(scripts/kanji_map.py runs that code), 30 bytes per glyph: 15 rows of 2 bytes, first byte = left 8 pixels, most
significant bit = leftmost pixel. The runtime answers reads of the BIOS ROM range ROM_LO..ROM_HI from this file.

    python scripts/make_kanji.py <SCPS_101.16> <jiskan16.bdf> <KANJI.BIN>
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kanji_map import BASE, glyph_map  # noqa: E402

ROM_LO = 0xBFC64000          # lowest glyph address the game computes (rows 8+ use base - 0x2000)
ROM_HI = 0xBFC80000          # end of the 512 KB BIOS ROM
ROWS = 15                    # the ROM glyphs are 16x15


def read_bdf(path: Path) -> dict[int, list[int]]:
    """ENCODING (JIS code) -> 16 rows of 16 pixels (bit 15 = leftmost)."""
    out: dict[int, list[int]] = {}
    code, rows, inside = None, [], False
    for line in path.read_text(encoding="latin-1").splitlines():
        if line.startswith("ENCODING"):
            code = int(line.split()[1])
        elif line == "BITMAP":
            inside, rows = True, []
        elif line == "ENDCHAR":
            if code is not None:
                out[code] = (rows + [0] * 16)[:16]
            inside, code = False, None
        elif inside:
            v = int(line, 16)
            rows.append(v << 8 if len(line) <= 2 else v)
    return out


def build(exe: Path, bdf: Path) -> bytes:
    m = glyph_map(exe)
    font = read_bdf(bdf)
    rom = bytearray(ROM_HI - ROM_LO)
    placed = 0
    for code, off in m.items():
        g = font.get(code)
        if not g:
            continue
        a = BASE + off - ROM_LO
        for y in range(ROWS):
            row = g[y]
            rom[a + 2 * y] = row >> 8
            rom[a + 2 * y + 1] = row & 0xFF
        placed += 1
    print(f"KANJI.BIN: {placed} of {len(m)} glyph slots filled from {bdf.name}")
    return bytes(rom)


if __name__ == "__main__":
    Path(sys.argv[3]).write_bytes(build(Path(sys.argv[1]), Path(sys.argv[2])))
