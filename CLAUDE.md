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
  src/card.c savedata.c  memory card: BIOS sector functions + events, stored as PSP save data
  src/spu.c          SPU + CD-XA decoding, PSP audio output thread
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
bash scripts/pgo.sh profile && bash scripts/pgo.sh build             # release: profile-guided (PSP via PSPLink) -> build/gt2psp
powershell -ExecutionPolicy Bypass -File scripts/run-ppsspp.ps1 -Seconds 40 -Shot <png>   # stages + runs + log
powershell -ExecutionPolicy Bypass -File scripts/package-psp.ps1   # -> build/psp/PSP/GAME/GT2PSP for a real PSP
```
(PYTHONPATH=src for the python calls.) The PSP folder holds only `EBOOT.PBP` + `GT2.PAK` (the EXE is read from the
pack too); run-ppsspp.ps1 stages them (hard links) into `tools/ppsspp/memstick/PSP/GAME/GT2PSP/`.
Target hardware: PSP-3000 (the user tests on it manually). Goal: a native port - convert code AND assets on the PC,
no raw disc image, no hardware emulation where a native PSP implementation is possible.
Runtime log: `gt2log.txt` there (watchdog "alive" lines every 2 s with the last called functions).

Pitfalls: in Git-Bash heredocs `
` inside C strings gets mangled — edit C files with the Edit tool.
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
* **16:9** (`widescreen=1`): view window patches after Silent's cheat (recomp.py), race HUD squeezed back to 4:3
  (ge.c hud_squeeze, race OT layer), menus stay 4:3 unless `widescreen_menus=1`. Hi-res 480x272 (`g_hires`) optional;
  game patches via recomp.py HOOKS (widescreen.c near-plane clipper).
* **Input + memory card**: DualShock analog emulation (libpad works now - before, all input was ignored and the
  "race replay" was the attract demo); memory card = PSP save data (XMB "Saved Data", savedata.c via sceUtilitySavedata; card.c BIOS events).
  Test input: input.txt (`<vblank> <button> <len>`), e.g. start/cross reaches the GT mode world map.
* **Audio**: SPU (24 voices, ADSR, no reverb) + CD-XA music, PSP audio thread (spu.c).
* MDEC: software decoder (`mdec.c`); the Simulation disc has no movies (they are on the Arcade disc).
* Not implemented yet: reverb,
  15-bit textures / texture windows on the GE (software fallback).

## Next steps (as of 2026-11-20)

Real PSP-3000, attract race 16:9: pacing ~29.4 fps, `race work` avg ~32.4 ms, ~28% of the frames still over 33.4 ms
(`build/racework.sh`, same EBOOT +-0.8 ms between runs; the code layout alone moves it +-10% - see ARCHITECTURE
"Code layout and PGO"). Release = `scripts/pgo.sh build` (profile in build/pgo-data).

0. **Done 2026-11-20: races at a locked 30 fps with the GPU path on the Media Engine** (me.c, `me_gpu=1` default).
   Same day: settings menu in game (L+R+SELECT, menu.c), quality (30 fps) / performance mode (`performance=1`: 60 fps,
   no mirror/tire smoke, low-LOD opponents, track_lod 2), fps60_dynamic (game time = real time at any frame rate),
   fps60_auto (locks to a steady 30 when 60 cannot be held). Full 60 fps in heavy races is out of reach: main CPU
   20-30 ms per frame there (GTE emulation in the game's mesh renderers), ME 12-18 ms. ME profiling builds:
   `-DGT2_ME_PROF`, `-DGT2_ME_FPROF` + `RT_EXTRA=-finstrument-functions` (DEVLOG).
1. **30 fps in races** (historical notes): GTE ~21% of the race time (RTPS ~100k/s, MVMVA ~145k/s, RTPT ~22k/s), GE path ~30%
   (benchmark ~10 ms per heavy frame), game code ~30%. Biggest lever: the GE path on the Media Engine
   (tools/downloads/me-custom-core reviewed, not run - needs the user's OK; multi-day). Smaller: RTPS/RTPT cost
   (~200 cycles per vertex, HI/LO chains + UNR division).
2. 100% save: `build/save/gt2-max.mcd` as `IMPORT.MCD` next to the EBOOT (saving works on hardware since b9bea88).
3. 16:9: race HUD now at PSP resolution (hud_defer); menus stay 4:3 unless `widescreen_menus=1`.
4. Picture reference: PCSX-Redux (tools/downloads/pcsx-redux-*, OpenBIOS, `build/ref/shots.lua`), never PPSSPP alone.
   PSPLink: tools/downloads/psplink-3.2.1 + pspsh-windows; `pspsh -e "scrshot ..."`. Ask before taking over the PSP
   while the user is playing. If PSPLink stops answering, the PSP needs a manual power cycle.
