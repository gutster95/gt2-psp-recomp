# CLAUDE.md

Static recompiler: Gran Turismo 2 (PS1, USA Simulation Mode **Rev 2**) â†’ native PSP homebrew (EBOOT.PBP).
Repo: https://github.com/gutster95/gt2-psp-recomp (private).

## Rules

* All code, comments, docs and commit messages in **English**. The user talks German â€” answer in German.
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
wsl -d GT2PSPDev -u root --cd "<proj>" -- bash scripts/build.sh      # -> build/gt2psp/EBOOT.PBP
powershell -ExecutionPolicy Bypass -File scripts/run-ppsspp.ps1 -Seconds 40 -Shot <png>   # stages + runs + log
```
(PYTHONPATH=src for the python calls.) The runtime needs `SCUS_944.88` and `GT2.BIN` (raw disc image) next to
the EBOOT; run-ppsspp.ps1 stages them (hard links) into `tools/ppsspp/memstick/PSP/GAME/GT2PSP/`.
Runtime log: `gt2log.txt` there (watchdog "alive" lines every 2 s with the last called functions).

Pitfalls: in Git-Bash heredocs `
` inside C strings gets mangled — edit C files with the Edit tool.
PowerShell `Set-Content -Encoding utf8` writes a BOM. CMake globs: build.sh re-runs cmake every time.

## Status (2026-11-18)

* Toolchain: WSL2 distro `GT2PSPDev` (`tools/wsl`), pspdev in `/opt/pspdev`, psp-gcc 15.2. PPSSPP 1.20.4
  portable in `tools/ppsspp` (memstick inside, no `installed.txt`).
* **Milestone 1 reached**: legal notice and title screen render (`build/milestone1-title.png`).
* **Beyond**: GTE implemented; with `input.txt` (start/cross) the game enters overlay 0 and renders the 3D
  attract-mode race replay (`build/milestone2-race-replay.png`). ~6 fps: rasterizer 72 %, game code ~46 ms/frame.
* Not implemented yet: SPU audio, XA music, MDEC (movies), memory card (reported absent), GE rendering,
  analog pad / DualShock, real-time pacing (game runs in slow motion when frames take too long).

## Next steps

1. GE renderer: draw PS1 primitives with the PSP GE (keep the software rasterizer as reference/fallback;
   PS1 VRAM image must stay coherent for CPU readback, VRAM copies and texture uploads).
2. Speed up generated code (registers in locals, sp-relative fast memory path, POLL budget) — see DEVLOG.
3. SPU emulation + PSP audio; XA-ADPCM decoding of MUSIC.DAT streams (cdrom.c filters them now).
4. MDEC for movies; memory card file on the memory stick; DualShock analog for steering.
