"""Read a PlayStation CD image (.cue/.bin, MODE2/2352) and extract its ISO9660 file system.

Only the data track is supported, which is all GT2 uses. Sectors are raw 2352-byte frames:

    12 bytes sync | 4 bytes header | 8 bytes XA subheader | 2048 bytes user data (Form 1) | 280 bytes EDC/ECC

XA Form 2 sectors (streamed audio/video, flagged in the subheader) carry 2324 bytes of user data; ISO9660 records
the file size as if every sector held 2048 bytes, so such files are extracted with ``raw=True`` to keep the full
sectors intact for the runtime's streaming code.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path

RAW_SECTOR = 2352
USER_OFFSET = 24  # sync(12) + header(4) + subheader(8)
USER_SIZE = 2048
SUBMODE_FORM2 = 0x20


@dataclass
class DirEntry:
    path: str
    lba: int
    size: int
    is_dir: bool
    form2: bool  # first sector of the extent is an XA Form 2 sector


class Disc:
    def __init__(self, bin_path: Path):
        self.path = Path(bin_path)
        self._f = open(self.path, "rb")
        self.sector_count = self.path.stat().st_size // RAW_SECTOR

    @classmethod
    def from_cue(cls, cue_path: Path) -> "Disc":
        cue_path = Path(cue_path)
        for line in cue_path.read_text(errors="replace").splitlines():
            line = line.strip()
            if line.upper().startswith("FILE"):
                name = line.split('"')[1]
                return cls(cue_path.parent / name)
        raise ValueError(f"no FILE line in {cue_path}")

    def close(self) -> None:
        self._f.close()

    def raw(self, lba: int) -> bytes:
        self._f.seek(lba * RAW_SECTOR)
        data = self._f.read(RAW_SECTOR)
        if len(data) != RAW_SECTOR:
            raise EOFError(f"sector {lba} beyond end of image")
        return data

    def submode(self, lba: int) -> int:
        return self.raw(lba)[18]

    def read(self, lba: int, size: int) -> bytes:
        """Read ``size`` bytes of Form 1 user data starting at ``lba``."""
        out = bytearray()
        while len(out) < size:
            out += self.raw(lba)[USER_OFFSET:USER_OFFSET + USER_SIZE]
            lba += 1
        return bytes(out[:size])

    # ---------------------------------------------------------------- ISO9660

    def volume_id(self) -> str:
        pvd = self.read(16, USER_SIZE)
        if pvd[1:6] != b"CD001":
            raise ValueError("no ISO9660 primary volume descriptor at sector 16")
        return pvd[40:72].decode("ascii", "replace").strip()

    def walk(self) -> list[DirEntry]:
        pvd = self.read(16, USER_SIZE)
        root = pvd[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        entries: list[DirEntry] = []
        self._walk_dir(lba, size, "", entries)
        return entries

    def _walk_dir(self, lba: int, size: int, prefix: str, out: list[DirEntry]) -> None:
        data = self.read(lba, size)
        pos = 0
        while pos < len(data):
            length = data[pos]
            if length == 0:  # records never span sectors: skip to next sector
                pos = (pos // USER_SIZE + 1) * USER_SIZE
                continue
            rec = data[pos:pos + length]
            pos += length
            name_len = rec[32]
            raw_name = rec[33:33 + name_len]
            if raw_name in (b"\x00", b"\x01"):
                continue
            name = raw_name.decode("ascii", "replace").split(";")[0]
            e_lba = struct.unpack_from("<I", rec, 2)[0]
            e_size = struct.unpack_from("<I", rec, 10)[0]
            is_dir = bool(rec[25] & 0x02)
            path = f"{prefix}{name}"
            form2 = (not is_dir) and e_size > 0 and bool(self.submode(e_lba) & SUBMODE_FORM2)
            out.append(DirEntry(path, e_lba, e_size, is_dir, form2))
            if is_dir:
                self._walk_dir(e_lba, e_size, path + "/", out)

    def extract_file(self, entry: DirEntry, dest: Path) -> None:
        dest.parent.mkdir(parents=True, exist_ok=True)
        sectors = (entry.size + USER_SIZE - 1) // USER_SIZE
        with open(dest, "wb") as f:
            if entry.form2:
                # Keep raw sectors; the runtime decides how to interpret each one (XA audio, STR video, data).
                for i in range(sectors):
                    f.write(self.raw(entry.lba + i))
            else:
                f.write(self.read(entry.lba, entry.size))


def sha1_file(path: Path) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
