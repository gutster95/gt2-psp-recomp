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
  mips.py            R3000A + GTE decoder
  image.py           PS-X EXE and GT2.OVL (gzip members) loaders
  analysis.py        text ranges, jump tables, partitioning, BIOS/IO detection
  program.py         whole program: modules resident/boot/ovl0-5, functions, resume points
  recomp.py          MIPS -> C translator
  __main__.py        CLI: extract, hash, disasm, analyze, functions, recomp
runtime/             PSP runtime (C): include/gt2rt.h (interface for generated code), src/*.c
  src/widescreen.c   game patch: near-plane clipping for the overlay 0 scenery renderer
  src/ge.c           GE renderer (PS1 VRAM in eDRAM, texture page cache, presentation)
  src/core.c         dispatcher, setjmp/longjmp unwind & resume, overlay hook, IRQ delivery, watchdog
  src/bios.c io.c cdrom.c sio.c gpu.c raster.c gte.c log.c files.c main.c
scripts/             wsl-setup.sh, build.sh, run-ppsspp.ps1, compile-generated.sh, vram2png.py
docs/                ARCHITECTURE.md, DEVLOG.md
ISO/ extracted/ generated/ build/ Recomps/ tools/   (all git-ignored)
```

## Workflow

```
python -m gt2psp extract "ISO/Gran Turismo 2 (USA) (Simulation Mode) (Rev 2).cue" --out extracted
python -m gt2psp recomp                      # -> generated/*.c (2853 functions)
python -m gt2psp pack "ISO/...cue"          # -> build/GT2.PAK (PSP data pack, converted once)
wsl -d GT2PSPDev -u root --cd "<proj>" -- bash scripts/build.sh      # -> build/gt2psp/EBOOT.PBP
powershell -ExecutionPolicy Bypass -File scripts/run-ppsspp.ps1 -Seconds 40 -Shot <png>   # stages + runs + log
powershell -ExecutionPolicy Bypass -File scripts/package-psp.ps1   # -> build/psp/PSP/GAME/GT2PSP for a real PSP
```
(PYTHONPATH=src for the python calls.) The PSP folder holds only `EBOOT.PBP` + `GT2.PAK` (the EXE is read from the
pack too); run-ppsspp.ps1 stages them (hard links) into `tools/ppsspp/memstick/PSP/GAME/GT2PSP/`.
Target hardware: PSP-3000 (the user tests on it manually). Goal: a native port - convert code AND assets on the PC,
no raw disc image, no hardware emulation where a native PSP implementation is possible.
Runtime log: `gt2log.txt` there (watchdog "alive" lines every 2 s with the last called functions).

Pitfalls: in Git-Bash heredocs `
` inside C strings gets mangled � edit C files with the Edit tool.
PowerShell `Set-Content -Encoding utf8` writes a BOM. CMake globs: build.sh re-runs cmake every time.

## Status (2026-11-18)

* Toolchain: WSL2 distro `GT2PSPDev` (`tools/wsl`), pspdev in `/opt/pspdev`, psp-gcc 15.2. PPSSPP 1.20.4
  portable in `tools/ppsspp` (memstick inside, no `installed.txt`). **PPSSPP must use its software renderer**
  (`SoftwareRenderer = True` in `memstick/PSP/SYSTEM/ppsspp.ini`): the hardware renderers do not write GE output
  back to emulated VRAM, which the port relies on (texture page copies, CPU readback). Real PSP: no issue.
* Milestone 1 (title screen) reached; 3D attract-mode race replay renders.
* **Data pack**: PSP folder = `EBOOT.PBP` + `GT2.PAK` (converted on the PC); no disc image.
* **GE renderer** (`runtime/src/ge.c`): all primitives in GT2's title and race run on the GE (0 software
  fallbacks); race at the game's full 30 fps in PPSSPP (was ~6 fps with the software rasterizer).
* **Hi-res 480x272 + 16:9** (ge.c `g_hires`, `g_widescreen`, GTE X*3/4, 3D/2D by GTE output tagging);
  game patches via recomp.py HOOKS (widescreen.c near-plane clipper).
* **Input + memory card**: DualShock analog emulation (libpad works now - before, all input was ignored and the
  "race replay" was the attract demo); memory card = GT2CARD1.MCD on the memory stick (card.c, BIOS events).
  Test input: input.txt (`<vblank> <button> <len>`), e.g. start/cross reaches the GT mode world map.
* Not implemented yet: SPU audio, XA music, MDEC (movies),
  15-bit textures / texture windows on the GE (software fallback).

## Next steps

1. Test on the PSP-3000 (user, manually) and fix what differs from PPSSPP.
2. Replace the CD-controller adapter by a libcd-level read path (no CD timing at all); speed up generated code.
3. SPU emulation + PSP audio; XA-ADPCM of MUSIC.DAT pre-converted on the PC.
4. MDEC movies pre-converted on the PC; saves as memory stick files; DualShock analog for steering.
5. Texture replacement / graphics options on top of the GE texture cache.
