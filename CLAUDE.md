# CLAUDE.md

Static recompiler: Gran Turismo 2 (PS1, USA Simulation Mode **Rev 2**) → native PSP homebrew (EBOOT.PBP).
Repo: https://github.com/gutster95/gt2-psp-recomp (private).

## Rules

* All code, comments, docs and commit messages in **English**. The user talks German — answer in German.
* Stay inside this project folder. Installing project-specific software is allowed (into `tools/`), nothing else.
* **Never commit game data.** `ISO/`, `extracted/`, `generated/`, `build/`, `Recomps/`, `tools/` are git-ignored;
  check `git status` before every commit.
* Keep `docs/DEVLOG.md` updated with every meaningful step; `docs/ARCHITECTURE.md` with every design decision.
* Downloaded files are untrusted: one directory per download under `tools/downloads/`, run Python with `-I`.

## Layout

```
src/gt2psp/          Python tooling (run: python -m gt2psp ... with src on PYTHONPATH)
  disc.py            .cue/.bin MODE2/2352 reader, ISO9660 walk, XA Form 2 files kept raw
  __main__.py        CLI: extract, hash
runtime/             (planned) PSP C runtime: memory, BIOS HLE, GTE, GPU->GE, SPU, CD, pad
docs/                ARCHITECTURE.md, DEVLOG.md
ISO/                 user's disc image (ignored)
Recomps/             reference PC port UGT2R 0.4.3 — v1.0 only, wasm modules, no source; docs only (ignored)
extracted/           output of `extract` (ignored)
tools/               downloads + project-local WSL distro (ignored)
```

Extract: `python -m gt2psp extract "ISO/Gran Turismo 2 (USA) (Simulation Mode) (Rev 2).cue" --out extracted`
Disc files: SYSTEM.CNF, SCUS_944.88 (main EXE), GT2.OVL (code overlays), GT2.VOL (data), MUSIC.DAT (XA), FAULTY.PSX (filler).

## Toolchain status

* pspdev has no native Windows build → project-local WSL distro `GT2PSPDev`, disk in `tools/wsl`.
* Downloads ready: `tools/downloads/ubuntu/rootfs.tar.gz` (Ubuntu 24.04 WSL rootfs),
  `tools/downloads/pspdev.tar.gz` (pspdev v20261101, ubuntu x86_64).
* User enabled WSL (`wsl --install --no-distribution`) on 2026-11-17; a reboot was pending.

## Next steps

1. `wsl --import GT2PSPDev "<proj>\tools\wsl" "<proj>\tools\downloads\ubuntu\rootfs.tar.gz" --version 2`
   (if it fails: BIOS virtualization SVM/VT-x probably off — tell the user). Unpack pspdev into the distro,
   set `PSPDEV`, verify `psp-gcc --version`, build a hello-world EBOOT.
2. Get PPSSPP (portable) into `tools/ppsspp` for testing.
3. Analyse SCUS_944.88 (PS-X EXE header, load address, entry) and GT2.OVL (overlay table/structure),
   find functions, BIOS calls, hardware register accesses.
4. Recompiler MIPS → C (`src/gt2psp/recomp/`) + PSP runtime skeleton. Milestone 1: title screen in PPSSPP.
