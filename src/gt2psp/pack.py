"""Convert the user's disc image into the PSP data pack ``GT2.PAK`` (built once on the PC).

The pack holds only what the game reads, in a compact, directly addressable layout:

* Form 1 sectors (all game data) are stored as their 2048 data bytes; runs of sectors with an identical XA
  subheader share one table entry.
* Form 2 sectors (XA-ADPCM music streams in MUSIC.DAT) keep their 2336 bytes (subheader + data + EDC), one table
  entry per run; the subheader of every sector is inside the stored bytes.
* Unused sectors are dropped: the 27 MB padding file FAULTY.PSX.

Format (little endian)::

    char  magic[8]  = "GT2PAK1\\0"
    u32   run_count
    u32   total_sectors        (highest LBA + 1)
    run[run_count]: u32 lba, u32 count, u32 offset, u8 subheader[4], u16 stride, u16 reserved
    data

The runtime maps an LBA to (run, offset + (lba - run.lba) * stride) with a binary search.
"""

from __future__ import annotations

import shutil
import struct
from pathlib import Path

from .disc import RAW_SECTOR, Disc

MAGIC = b"GT2PAK1\0"
RUN = struct.Struct("<III4sHH")
HEADER = struct.Struct("<8sII")
FORM2 = 0x20


def build_pack(disc: Disc, out: Path, skip: list[tuple[int, int]]) -> dict:
    """Write ``out``; ``skip`` lists LBA ranges [start, end) that are left out."""
    runs: list[list] = []          # [lba, count, subheader, stride, chunks]
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    data = open(tmp, "wb")
    end = disc.sector_count
    lba = 0
    while lba < end:
        sk = next((e for s, e in skip if s <= lba < e), None)
        if sk is not None:
            lba = sk
            continue
        raw = disc.raw(lba)
        sub = raw[16:20]
        if sub[2] & FORM2:
            stride, payload, key = 2336, raw[16:16 + 2336], b"\xff\xff\xff\xff"
        else:
            stride, payload, key = 2048, raw[24:24 + 2048], sub
        last = runs[-1] if runs else None
        if last and last[0] + last[1] == lba and last[3] == stride and last[2] == key:
            last[1] += 1
        else:
            runs.append([lba, 1, key, stride])
        data.write(payload)
        lba += 1
    data.close()
    head = HEADER.size + RUN.size * len(runs)
    with open(out, "wb") as f:
        f.write(HEADER.pack(MAGIC, len(runs), end))
        off = head
        for r in runs:
            f.write(RUN.pack(r[0], r[1], off, r[2], r[3], 0))
            off += r[1] * r[3]
        with open(tmp, "rb") as d:
            shutil.copyfileobj(d, f, 1 << 20)
    tmp.unlink()
    return {"runs": len(runs), "sectors": sum(r[1] for r in runs), "bytes": off}


def default_skip(entries) -> list[tuple[int, int]]:
    """FAULTY.PSX is pure padding (never read by the game)."""
    out = []
    for e in entries:
        if e.path.upper() == "FAULTY.PSX":
            out.append((e.lba, e.lba + (e.size + 2047) // 2048))
    return out


def raw_size(disc: Disc) -> int:
    return disc.sector_count * RAW_SECTOR
