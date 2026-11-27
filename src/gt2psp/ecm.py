"""ECM decoder (Neill Corlett's "Error Code Modeler" format, .bin.ecm): restores the original .bin image.

ECM drops the parts of each CD sector that can be recomputed (sync, address, EDC, ECC) and stores what is left.
Stream: "ECM\\0", then records: a type/count header (type in the low 2 bits of the first byte, count in a variable-
length number), then the record data; type 0 raw bytes, 1 Mode 1 (2352), 2 Mode 2 Form 1 (2336), 3 Mode 2 Form 2
(2336). Ends with count 0xFFFFFFFF and the EDC of the whole output.

    python -m gt2psp unecm <in.bin.ecm> [<out.bin>]     (writes a .cue next to it: one MODE2/2352 track)
"""

from __future__ import annotations

import struct
from pathlib import Path


def _tables():
    ecc_f = [0] * 256
    ecc_b = [0] * 256
    edc = [0] * 256
    for i in range(256):
        j = (i << 1) ^ (0x11D if i & 0x80 else 0)
        ecc_f[i] = j & 0xFF
        ecc_b[i ^ j & 0xFF] = i
        e = i
        for _ in range(8):
            e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
        edc[i] = e
    return ecc_f, ecc_b, edc


ECC_F, ECC_B, EDC = _tables()


def edc_compute(edc: int, data: bytes | bytearray | memoryview) -> int:
    t = EDC
    for b in data:
        edc = (edc >> 8) ^ t[(edc ^ b) & 0xFF]
    return edc


def _ecc_pq(sector: bytearray, major_count: int, minor_count: int, major_mult: int, minor_inc: int, dest: int) -> None:
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            temp = sector[0xC + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= temp
            b ^= temp
            a = ECC_F[a]
        a = ECC_B[ECC_F[a] ^ b]
        sector[dest + major] = a
        sector[dest + major + major_count] = a ^ b


def _ecc_generate(sector: bytearray, zero_address: bool) -> None:
    saved = bytes(sector[0xC:0x10])
    if zero_address:
        sector[0xC:0x10] = b"\0\0\0\0"
    _ecc_pq(sector, 86, 24, 2, 86, 0x81C)
    _ecc_pq(sector, 52, 43, 86, 88, 0x8C8)
    if zero_address:
        sector[0xC:0x10] = saved


SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def decode(src: Path, dst: Path, progress=None) -> None:
    data = Path(src).read_bytes()
    if data[:4] != b"ECM\0":
        raise ValueError(f"{src}: not an ECM file")
    pos, out_edc = 4, 0
    total = len(data)
    with open(dst, "wb") as out:
        while True:
            c = data[pos]; pos += 1
            typ, num, bits = c & 3, (c >> 2) & 0x1F, 5
            while c & 0x80:
                c = data[pos]; pos += 1
                num |= (c & 0x7F) << bits
                bits += 7
            if num == 0xFFFFFFFF:
                break
            num += 1
            if num >= 0x80000000:
                raise ValueError("corrupt ECM record")
            if typ == 0:
                chunk = data[pos:pos + num]; pos += num
                out.write(chunk)
                out_edc = edc_compute(out_edc, chunk)
                continue
            buf = bytearray()
            for _ in range(num):
                sec = bytearray(2352)
                if typ == 1:       # whole Mode 1 sector: sync, address, data, EDC, ECC
                    sec[0:12] = SYNC
                    sec[0xF] = 1
                    sec[0xC:0xF] = data[pos:pos + 3]; pos += 3
                    sec[0x10:0x810] = data[pos:pos + 0x800]; pos += 0x800
                    struct.pack_into("<I", sec, 0x810, edc_compute(0, sec[0:0x810]))
                    _ecc_generate(sec, False)
                    buf += sec
                    continue
                # Mode 2: a 2336-byte block from the subheader on (the sync and address in front of it are stored
                # as raw bytes); ECC of Form 1 is computed with a zero address
                if typ == 2:
                    sec[0x14:0x818] = data[pos:pos + 0x804]; pos += 0x804
                    sec[0x10:0x14] = sec[0x14:0x18]
                    struct.pack_into("<I", sec, 0x818, edc_compute(0, sec[0x10:0x818]))
                    _ecc_generate(sec, True)
                else:
                    sec[0x14:0x92C] = data[pos:pos + 0x918]; pos += 0x918
                    sec[0x10:0x14] = sec[0x14:0x18]
                    struct.pack_into("<I", sec, 0x92C, edc_compute(0, sec[0x10:0x92C]))
                buf += sec[0x10:0x930]
            out.write(buf)
            out_edc = edc_compute(out_edc, buf)
            if progress:
                progress(pos / total)
        stored = struct.unpack_from("<I", data, pos)[0]
        if stored != out_edc:
            raise ValueError(f"ECM checksum mismatch ({out_edc:08x}, expected {stored:08x}): damaged file")


def cmd(args) -> None:
    src = Path(args.ecm)
    dst = Path(args.out) if args.out else src.with_suffix("") if src.suffix.lower() == ".ecm" else src.with_suffix(".bin")
    decode(src, dst)
    cue = dst.with_suffix(".cue")
    if not cue.exists():
        cue.write_text(f'FILE "{dst.name}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    print(f"{dst} ({dst.stat().st_size} bytes), {cue.name}")
