"""Command line entry point: ``python -m gt2psp <command>`` (run with ``src`` on PYTHONPATH)."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from .disc import Disc, sha1_file


def cmd_extract(args: argparse.Namespace) -> None:
    disc = Disc.from_cue(Path(args.cue))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    print(f"image:   {disc.path.name} ({disc.sector_count} sectors)")
    print(f"volume:  {disc.volume_id()}")
    index = []
    for e in disc.walk():
        index.append({"path": e.path, "lba": e.lba, "size": e.size, "dir": e.is_dir, "form2": e.form2})
        if e.is_dir:
            continue
        disc.extract_file(e, out / "files" / e.path)
        print(f"  {e.lba:7d} {e.size:10d} {'F2' if e.form2 else '  '} {e.path}")
    (out / "index.json").write_text(json.dumps(index, indent=1))
    disc.close()


def cmd_pack(args: argparse.Namespace) -> None:
    from .pack import build_pack, default_skip
    disc = Disc.from_cue(Path(args.cue))
    info = build_pack(disc, Path(args.out), default_skip(disc.walk()))
    disc.close()
    print(f"{args.out}: {info['runs']} runs, {info['sectors']} sectors, {info['bytes'] / 1e6:.1f} MB")


def cmd_hash(args: argparse.Namespace) -> None:
    disc = Disc.from_cue(Path(args.cue))
    print(f"{sha1_file(disc.path)}  {disc.path.name}")


def _space(args: argparse.Namespace):
    from .analysis import Space
    from .image import load_overlays, load_psx_exe
    exe = load_psx_exe(Path(args.files) / "SCUS_944.88")
    if args.ovl is None:
        return exe, None, Space([exe])
    ovl = load_overlays(Path(args.files) / "GT2.OVL")[args.ovl]
    return exe, ovl, Space([ovl, exe])


def cmd_disasm(args: argparse.Namespace) -> None:
    from .mips import decode
    _exe, _ovl, space = _space(args)
    a = int(args.addr, 16)
    for _ in range(args.count):
        r = space.insn(a)
        if r is None:
            break
        im, ins = r
        print(f"{im.name:5s} {a:08x}: {ins.word:08x}  {ins.text()}")
        a += 4


def cmd_analyze(args: argparse.Namespace) -> None:
    from .analysis import bios_name, discover, summarize
    exe, ovl, space = _space(args)
    entries = [exe.entry] if ovl is None else [int(x, 16) for x in args.entry]
    funcs = discover(space, entries, limit_image=None if ovl is None else ovl.name)
    s = summarize(funcs)
    print(f"functions: {s['functions']}  computed jumps: {s['computed_jumps']}  "
          f"computed calls: {s['computed_calls']}  GTE functions: {s['gte_funcs']}")
    for a, b in s["bios"]:
        print(f"  bios stub {a:08x} {bios_name(*b)}")
    for (name, addr, rw), c in sorted(s["io"].items(), key=lambda x: x[0][1]):
        print(f"  io {addr:08x} {name:22s} {rw} x{c}")
    for a, e in s["errors"][:50]:
        print(f"  error in {a:08x}: {e[0]:08x} {e[1]}")
    if args.list:
        for a in sorted(funcs):
            f = funcs[a]
            print(f"  fn {a:08x}-{f.end:08x} {f.image} calls={len(f.calls)} callers={len(f.callers)}")


def cmd_functions(args: argparse.Namespace) -> None:
    from .program import build, to_json
    mods = build(Path(args.files))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(to_json(mods))
    for m in mods:
        print(f"{m.name:9s} funcs={len(m.funcs):5d} fallthrough={sum(f.falls_through for f in m.funcs):3d} "
              f"labels={len(m.labels):3d} text=" + ", ".join(f"{a:08x}-{b:08x}" for a, b in m.text))
    print(f"total {sum(len(m.funcs) for m in mods)} functions -> {out}")


def cmd_recomp(args: argparse.Namespace) -> None:
    from .program import build, to_json
    from .recomp import generate
    mods = build(Path(args.files))
    out = Path(args.out)
    st = generate(mods, out)
    (out / "functions.json").write_text(to_json(mods))
    print(f"functions: {st.funcs}  instructions: {st.insns}  dynamic calls: {st.dyn_calls}  "
          f"dynamic jumps: {st.dyn_jumps}  branches in delay slots: {st.ds_branch}")
    unk = sorted(st.unknown_targets)
    print(f"static targets that are not function starts: {len(unk)}")
    for mod, t in unk[:40]:
        print(f"  {mod:9s} {t:08x}")


def main() -> None:
    p = argparse.ArgumentParser(prog="gt2psp")
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract", help="extract the ISO9660 file system of a .cue/.bin image")
    e.add_argument("cue")
    e.add_argument("--out", default="extracted")
    e.set_defaults(func=cmd_extract)
    k = sub.add_parser("pack", help="convert the disc image into the PSP data pack GT2.PAK")
    k.add_argument("cue")
    k.add_argument("--out", default="build/GT2.PAK")
    k.set_defaults(func=cmd_pack)
    h = sub.add_parser("hash", help="print the SHA-1 of the image's data track")
    h.add_argument("cue")
    h.set_defaults(func=cmd_hash)
    f = sub.add_parser("functions", help="partition all code (resident, boot, overlays) into functions")
    f.add_argument("--files", default="extracted/files")
    f.add_argument("--out", default="generated/functions.json")
    f.set_defaults(func=cmd_functions)
    r = sub.add_parser("recomp", help="translate all modules to C")
    r.add_argument("--files", default="extracted/files")
    r.add_argument("--out", default="generated")
    r.set_defaults(func=cmd_recomp)
    for name, fn, hlp in (("disasm", cmd_disasm, "disassemble main EXE or an overlay"),
                          ("analyze", cmd_analyze, "function discovery, BIOS stubs, I/O accesses")):
        c = sub.add_parser(name, help=hlp)
        c.add_argument("--files", default="extracted/files")
        c.add_argument("--ovl", type=int, default=None, help="overlay index in GT2.OVL (0-5)")
        if name == "disasm":
            c.add_argument("addr")
            c.add_argument("-n", "--count", type=int, default=64)
        else:
            c.add_argument("--entry", nargs="*", default=["80010000"])
            c.add_argument("--list", action="store_true")
        c.set_defaults(func=fn)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
