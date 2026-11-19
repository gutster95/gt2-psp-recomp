"""Gran Turismo 2 (US, BASCUS-94455GAME) career save in a raw 128 KB PS1 memory card image: summary and "max" edit.

Offsets from the GT2SaveEditor documentation (github.com/zyzalfors/GT2SaveEditor): save data starts 512 bytes into
the save's first 8 KB block (after the title/icon header); CRC-32 over the block start up to data offset 31900.
  python scripts/gt2save.py card.mcd                  summary
  python scripts/gt2save.py card.mcd --max out.mcd    all licenses gold, all events won, money 9,999,999
"""
import collections, sys, zlib

LICENSES = {"S": 5145, "IA": 6785, "IB": 8425, "IC": 10065, "A": 11705, "B": 13345}


def find(d):
    for blk in range(1, 16):
        h = d[blk * 128:(blk + 1) * 128]
        if h[0] == 0x51 and h[10:30].startswith(b"BASCUS-94455GAME"):
            return blk * 8192
    raise SystemExit("no GT2 career save on this card")


def crc(d, b):
    return zlib.crc32(bytes(d[b:b + 512 + 31900])) & 0xFFFFFFFF


def summary(d):
    b = find(d)
    s = b + 512
    u = lambda o, n: int.from_bytes(d[s + o:s + o + n], "little")
    print(f"days {u(248, 4)} races {u(256, 4)} wins {u(260, 4)} prize {u(276, 4)} money {u(31880, 4)} "
          f"cars {u(15476, 1)} ending {u(533, 1)}")
    print(f"crc stored {u(31900, 4):08x} calc {crc(d, b):08x}")
    print("licenses (2 bronze 3 silver 4 gold):",
          {n: dict(collections.Counter(d[s + o + 164 * i] for i in range(10))) for n, o in LICENSES.items()})
    ev = [x for i in range(124) for x in (d[s + 280 + i] & 15, d[s + 280 + i] >> 4)]
    print("event placements (0 none, 1 first):", dict(collections.Counter(ev)))


def maxed(d):
    b = find(d)
    s = b + 512
    for o in LICENSES.values():
        for i in range(10):
            d[s + o + 164 * i] = 4
    for i in range(124):
        d[s + 280 + i] = 0x11
    d[s + 31880:s + 31884] = (9_999_999).to_bytes(4, "little")
    d[s + 31900:s + 31904] = crc(d, b).to_bytes(4, "little")


if __name__ == "__main__":
    data = bytearray(open(sys.argv[1], "rb").read())
    assert len(data) == 131072 and data[:2] == b"MC", "not a raw 128 KB memory card image"
    summary(data)
    if len(sys.argv) == 4 and sys.argv[2] == "--max":
        maxed(data)
        open(sys.argv[3], "wb").write(data)
        print("written", sys.argv[3])
        summary(data)
