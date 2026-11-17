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


def cmd_hash(args: argparse.Namespace) -> None:
    disc = Disc.from_cue(Path(args.cue))
    print(f"{sha1_file(disc.path)}  {disc.path.name}")


def main() -> None:
    p = argparse.ArgumentParser(prog="gt2psp")
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract", help="extract the ISO9660 file system of a .cue/.bin image")
    e.add_argument("cue")
    e.add_argument("--out", default="extracted")
    e.set_defaults(func=cmd_extract)
    h = sub.add_parser("hash", help="print the SHA-1 of the image's data track")
    h.add_argument("cue")
    h.set_defaults(func=cmd_hash)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
