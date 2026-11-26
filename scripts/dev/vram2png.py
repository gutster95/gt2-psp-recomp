"""Convert a raw PS1 VRAM dump (1024x512, 15-bit BGR) written by the runtime into a PNG (no dependencies)."""
import struct
import sys
import zlib


def main(src: str, dst: str) -> None:
    raw = open(src, "rb").read()
    w, h = 1024, 512
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            p = struct.unpack_from("<H", raw, (y * w + x) * 2)[0]
            r, g, b = (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3
            row += bytes((r, g, b))
        rows.append(bytes(row))

    def chunk(t: bytes, d: bytes) -> bytes:
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
