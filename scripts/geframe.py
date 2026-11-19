"""List the PS1 GPU primitives of a geframe.bin (runtime io.c gpu_dump: L+R+SELECT or settings.txt gedump=<vblank>).

    python scripts/geframe.py build/psp/PSP/GAME/GT2PSP/geframe.bin [--y0 Y --y1 Y] [--maxh H]

Walks the ordering table like the runtime (gpu_walk) and prints every primitive (with the drawing area and offset
in effect) whose bounding box touches the PS1 line range y0..y1 and is at most maxh pixels tall.
"""
import argparse
import struct

HDR = struct.Struct("<IIIi" + "iiii" + "ii" + "I" + "iiii" + "H2x" + "i")
RAM = 0x200000


def sx11(v):
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


def cmd_len(op):
    k = op >> 5
    if k == 1:
        verts = 4 if op & 8 else 3
        per = 1 + (1 if op & 4 else 0) + (1 if op & 0x10 else 0)
        return 1 + verts * per - (1 if op & 0x10 else 0)
    if k == 2:
        return -1 if op & 8 else (4 if op & 0x10 else 3)
    if k == 3:
        n = 2 + (1 if op & 4 else 0)
        return n + 1 if ((op >> 3) & 3) == 0 else n
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--y0", type=int, default=0)
    ap.add_argument("--y1", type=int, default=511)
    ap.add_argument("--maxh", type=int, default=1000)
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    h = HDR.unpack_from(data, 0)
    magic, madr, draw_mode, ovl = h[:4]
    ram = data[HDR.size:HDR.size + RAM]
    w32 = lambda addr: struct.unpack_from("<I", ram, addr & 0x1FFFFC)[0]
    print(f"madr {madr:06x} overlay {ovl}")
    off_x = off_y = 0
    clip = [0, 0, 1023, 511]
    adr, guard, slot, in_hud = madr, 0, 0, False
    while guard < 0x40000:
        hdr = w32(adr)
        in_hud = in_hud or adr == madr - 0x850C
        n = hdr >> 24
        p = adr + 4
        i = 0
        while i < n:
            c = w32(p + 4 * i)
            op = c >> 24
            ln = cmd_len(op)
            if op == 0xE3:
                clip[0], clip[1] = c & 0x3FF, (c >> 10) & 0x1FF
            elif op == 0xE4:
                clip[2], clip[3] = c & 0x3FF, (c >> 10) & 0x1FF
            elif op == 0xE5:
                off_x, off_y = sx11(c), sx11(c >> 11)
            if ln < 0:                                 # polyline: until terminator
                ln = 1
                while i + ln < n and (w32(p + 4 * (i + ln)) & 0xF000F000) != 0x50005000:
                    ln += 1
                ln += 1
            words = [w32(p + 4 * (i + k)) for k in range(min(ln, n - i))]
            if 0x20 <= op < 0x80:
                xs, ys = [], []
                if op < 0x40:
                    verts = 4 if op & 8 else 3
                    per = 1 + (1 if op & 4 else 0) + (1 if op & 0x10 else 0)
                    k = 1
                    for v in range(verts):
                        if v > 0 and op & 0x10:
                            k += 1
                        xy = words[k] if k < len(words) else 0
                        xs.append(sx11(xy) + off_x); ys.append(sx11(xy >> 16) + off_y)
                        k += 1 + (1 if op & 4 else 0)
                elif op < 0x60:
                    step = 2 if op & 0x10 else 1
                    for k in range(1, len(words), step):
                        if (words[k] & 0xF000F000) == 0x50005000:
                            break
                        xs.append(sx11(words[k]) + off_x); ys.append(sx11(words[k] >> 16) + off_y)
                else:
                    xy = words[1]
                    x0, y0 = sx11(xy) + off_x, sx11(xy >> 16) + off_y
                    sz = (op >> 3) & 3
                    k = 3 if op & 4 else 2
                    ww, hh = ((words[k] & 0x3FF, (words[k] >> 16) & 0x1FF) if sz == 0 else
                              {1: (1, 1), 2: (8, 8), 3: (16, 16)}[sz])
                    xs, ys = [x0, x0 + ww - 1], [y0, y0 + hh - 1]
                if xs and min(ys) <= a.y1 and max(ys) >= a.y0 and max(ys) - min(ys) + 1 <= a.maxh:
                    print(f"slot {slot:5d} @{adr:06x} op {op:02x} x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)} "
                          f"area {clip} off {off_x},{off_y} hud {int(in_hud)} "
                          + " ".join(f"{x:08x}" for x in words))
            i += max(ln, 1)
        if hdr & 0x800000:
            break
        adr = hdr & 0x1FFFFC
        guard += 1
        slot += 1


if __name__ == "__main__":
    main()
