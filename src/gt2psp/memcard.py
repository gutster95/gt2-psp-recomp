"""PS1 memory card files -> the raw 128 KB card image the PSP runtime imports (IMPORT.MCD next to the EBOOT,
runtime/src/card.c).

Read formats (detected by content, not by the file name):
  whole cards   raw 131072 bytes (.mcr .mcd .mc .srm .ps .ddf .vm1 ...), DexDrive .gme (3904-byte header),
                Connectix VGS .mem/.vgs (64-byte header), PSP PS1 classic card .vmp (128-byte header)
  single saves  .mcs/.ps1 (one directory frame + blocks), Action Replay / Xploder .psx/.mcb/.pda (54-byte header),
                PS3 export .psv (132-byte header) - one or more are put on a freshly formatted card
Card layout: nocash psx-spx "Memory Card Data Format" (frame 0 header, frames 1-15 directory, 15 blocks of 8 KB).
"""

from __future__ import annotations

import unicodedata
from dataclasses import dataclass
from pathlib import Path

FRAME = 128
BLOCK = 8192
CARD = 128 * 1024

# GT2's save files (the game's own product code + name; found in the EXEs of each disc). Both discs of an edition
# use the same file. Editions: profiles.EDITIONS.
GT2_SAVES = {
    "BASCUS-94455GAME": ("usa", "plus_usa"),
    "BASCUS-94455REPLAY": ("usa", "plus_usa"),
    "BESCES-02380GAME": ("eur", "plus_eur"),          # also GT2 Plus Beta v6.1 Europe
    "BESCES-02380REPLAY": ("eur", "plus_eur"),
    "BESCES-02380GAME7C": ("plus_eur",),              # GT2 Plus Beta 7 Europe
    "BESCES-02380REPL7C": ("plus_eur",),
    "BISCPS-10116GAME": ("jpn",),
    "BISCPS-10116REPLAY": ("jpn",),
}


@dataclass
class Save:
    name: str          # file name in the directory, e.g. BASCUS-94455GAME
    blocks: int
    title: str         # from the save's title frame (Shift-JIS), best effort


def _checksum(f: bytearray) -> None:
    x = 0
    for b in f[:127]:
        x ^= b
    f[127] = x


def formatted() -> bytearray:
    """A freshly formatted card (same as the runtime's format())."""
    img = bytearray(CARD)
    img[0:2] = b"MC"
    for i in range(16):
        f = memoryview(img)[i * FRAME:(i + 1) * FRAME]
        if i:
            f[0] = 0xA0
            f[8] = f[9] = 0xFF
    for i in range(16, 36):
        o = i * FRAME
        img[o:o + 4] = b"\xff" * 4
        img[o + 8] = img[o + 9] = 0xFF
    for i in range(36):
        f = bytearray(img[i * FRAME:(i + 1) * FRAME])
        _checksum(f)
        img[i * FRAME:(i + 1) * FRAME] = f
    img[63 * FRAME:64 * FRAME] = img[0:FRAME]
    return img


def _title(data: bytes) -> str:
    """Title of a save from its first block (title frame 'SC', 64 bytes Shift-JIS at offset 4)."""
    if data[:2] != b"SC":
        return ""
    raw = data[4:68].split(b"\0", 1)[0]
    try:
        return unicodedata.normalize("NFKC", raw.decode("shift_jis", "replace")).strip()   # full-width -> ASCII
    except LookupError:
        return ""


def saves(img: bytes) -> list[Save]:
    """The files on a card: the directory frames that start a file (0x51), with their block chains."""
    out = []
    for i in range(1, 16):
        f = img[i * FRAME:(i + 1) * FRAME]
        if f[0] != 0x51:
            continue
        name = f[10:31].split(b"\0", 1)[0].decode("ascii", "replace")
        n, blk, seen = 1, int.from_bytes(f[8:10], "little"), {i - 1}
        while blk != 0xFFFF and 0 <= blk < 15 and blk not in seen:
            seen.add(blk)
            n += 1
            blk = int.from_bytes(img[(blk + 1) * FRAME + 8:(blk + 1) * FRAME + 10], "little")
        out.append(Save(name, n, _title(img[i * BLOCK:i * BLOCK + 128])))
    return out


def _single(name: bytes, data: bytes) -> tuple[str, bytes]:
    name = name.split(b"\0", 1)[0]
    if not name or len(data) < BLOCK or data[:2] != b"SC":
        raise ValueError("not a PS1 save file")
    n = len(data) // BLOCK
    return name.decode("ascii", "replace"), data[:n * BLOCK]


def parse(raw: bytes) -> tuple[str, bytearray | list[tuple[str, bytes]]]:
    """(format description, a whole card image) or (format, [(file name, blocks)]) for a single save."""
    if len(raw) == CARD and raw[:2] == b"MC":
        return "raw memory card", bytearray(raw)
    if raw[:11] == b"123-456-STD" and len(raw) >= 3904 + CARD and raw[3904:3906] == b"MC":
        return "DexDrive card (.gme)", bytearray(raw[3904:3904 + CARD])
    if raw[:4] == b"VgsM" and len(raw) >= 64 + CARD and raw[64:66] == b"MC":
        return "Connectix VGS card", bytearray(raw[64:64 + CARD])
    if raw[:4] == b"\0PMV" and len(raw) >= 0x80 + CARD and raw[0x80:0x82] == b"MC":
        return "PSP PS1 classic card (.vmp)", bytearray(raw[0x80:0x80 + CARD])
    if raw[:4] == b"\0VSP" and len(raw) >= 0x84 + BLOCK:
        return "PS3 single save (.psv)", [_single(raw[0x64:0x84], raw[0x84:])]
    if len(raw) >= FRAME + BLOCK and raw[0] == 0x51 and raw[FRAME:FRAME + 2] == b"SC":
        return "single save (.mcs)", [_single(raw[10:31], raw[FRAME:])]
    if len(raw) >= 54 + BLOCK and raw[54:56] == b"SC" and (len(raw) - 54) % BLOCK == 0:
        return "Action Replay / Xploder save", [_single(raw[0:21], raw[54:])]
    # a raw card with a few bytes of padding (some tools pad the file)
    if CARD <= len(raw) <= CARD + 4096 and raw[:2] == b"MC":
        return "raw memory card", bytearray(raw[:CARD])
    raise ValueError("not a PS1 memory card or save file this converter knows")


def add(img: bytearray, name: str, data: bytes) -> None:
    """Put one file (whole 8 KB blocks) on the card; a file of the same name is replaced."""
    for s in range(1, 16):                       # remove an existing file of that name
        f = img[s * FRAME:(s + 1) * FRAME]
        if f[0] == 0x51 and f[10:31].split(b"\0", 1)[0].decode("ascii", "replace") == name:
            blk = s - 1
            while blk != 0xFFFF and 0 <= blk < 15:
                o = (blk + 1) * FRAME
                nxt = int.from_bytes(img[o + 8:o + 10], "little")
                fr = bytearray(FRAME)
                fr[0] = 0xA0
                fr[8] = fr[9] = 0xFF
                _checksum(fr)
                img[o:o + FRAME] = fr
                blk = nxt
    n = len(data) // BLOCK
    free = [b for b in range(15) if img[(b + 1) * FRAME] in (0xA0, 0xA1, 0xA2, 0xA3)]
    if len(free) < n:
        raise ValueError(f"{name}: {n} blocks needed, {len(free)} free on the card")
    use = free[:n]
    for k, b in enumerate(use):
        fr = bytearray(FRAME)
        fr[0] = 0x51 if k == 0 else (0x53 if k == n - 1 else 0x52)
        if k == 0:
            fr[4:8] = (n * BLOCK).to_bytes(4, "little")
            fr[10:10 + min(20, len(name))] = name.encode("ascii", "replace")[:20]
        nxt = use[k + 1] if k + 1 < n else 0xFFFF
        fr[8:10] = nxt.to_bytes(2, "little")
        _checksum(fr)
        img[(b + 1) * FRAME:(b + 2) * FRAME] = fr
        img[(b + 1) * BLOCK:(b + 2) * BLOCK] = data[k * BLOCK:(k + 1) * BLOCK]


def load(paths: list[Path]) -> tuple[bytearray, list[str]]:
    """A card image from one whole card or from one or more single saves. Returns (image, format notes)."""
    img: bytearray | None = None
    notes = []
    singles = []
    for p in paths:
        fmt, res = parse(Path(p).read_bytes())
        notes.append(f"{Path(p).name}: {fmt}")
        if isinstance(res, bytearray):
            if img is not None or len(paths) > 1:
                raise ValueError("pick either one whole memory card or single save files, not several cards")
            img = res
        else:
            singles += res
    if img is None:
        img = formatted()
    for name, data in singles:
        add(img, name, data)
    return img, notes


def gt2_editions(img: bytes) -> tuple[list[Save], list[str]]:
    """GT2 saves on the card and the editions (profiles.EDITIONS keys) that can use them."""
    found = [s for s in saves(img) if s.name in GT2_SAVES]
    eds: list[str] = []
    for s in found:
        for e in GT2_SAVES[s.name]:
            if e not in eds:
                eds.append(e)
    return found, eds
