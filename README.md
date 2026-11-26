# GT2 PSP

Gran Turismo 2 running natively on the PSP. Not in an emulator: the game's MIPS code is recompiled to C and
built as a regular PSP program, with the PS1 hardware (GPU, GTE, SPU, CD drive, memory card) rewritten for the
PSP.

You bring your own discs. This repository has no game code and no game data, and neither has any release. The
converter turns your discs into a PSP folder on your own computer.

## What you need

- A PSP with custom firmware (tested on a PSP-3000; PPSSPP works too, with its software renderer)
- Your own Gran Turismo 2 discs as .cue/.bin, **USA versions**:
  - Simulation Mode (Disc 2), **SCUS-94488, Rev 2**
  - Arcade Mode (Disc 1), **SCUS-94455**

  One disc is enough. With both, the PSP shows a small menu where you pick the mode.
- Python 3 on your computer (Windows: from python.org, tick "Add python.exe to PATH"; Linux and macOS usually
  have it)
- About 3 GB of free disk space while converting

## Converting

1. Download this repository (green "Code" button, "Download ZIP") and unpack it.
2. Put your .cue and .bin files into the `input` folder.
3. Start the converter:
   - Windows: double-click `convert.bat`
   - macOS: double-click `convert.command`
   - Linux: run `./convert.sh`
4. Your browser opens. Click **Check discs**, then **Start conversion**.
5. When it's done, copy `output/PSP/GAME/GT2PSP` to your memory stick as `PSP/GAME/GT2PSP` and start it from the
   XMB.

The first run downloads a PSP compiler (about 150 MB, checked against a fixed checksum). After that a conversion
takes a few minutes. Everything happens locally; nothing is uploaded anywhere.

The converter checks the .bin against the checksums of the known good dumps. Other versions (PAL, Japanese, Rev 0/1)
won't work - the recompiler is built around the exact code of these two discs.

## Features

- **16:9.** The 3D view is really wider, not stretched. The race HUD stays at its original proportions.
- **Two picture modes**, picked in the start menu:
  - Quality: a locked 30 fps, everything on.
  - Performance: aims for 60 fps. Mirror and tire smoke are off by default and opponent cars use their low-detail
    models (both can be switched back on).
- **Game speed stays right at any frame rate.** If a frame takes longer, the game time catches up instead of the
  game slowing down.
- **Short loading times.** The game reads its files from a pre-converted data pack on the memory stick instead
  of an emulated CD drive. GT mode loads in about a second.
- **Saves are PSP saves.** The memory card is stored as normal PSP save data, visible in the XMB. Loading a save
  takes a fraction of a second.
- **Reverb** like on the PS1 in Quality mode (off in Performance mode, where every millisecond goes to the frame
  rate; `reverb=0`/`reverb=1` in settings.txt overrides it).
- **Analog controls**, plus optional racing buttons (X gas, square brake, circle handbrake, triangle reverse) and
  L/R shifting.
- **In-game settings menu** with L + R + SELECT: mirror and tire smoke, opponent detail, FPS counter, button layout.
- **Arcade Mode** including its intro movie.

## Why 60 fps is a target and not a promise

GT2 was made for 30 fps on a PS1, and the PSP is not a lot faster than it needs to be for this.

The GTE, the PS1's geometry coprocessor, has no equivalent on the PSP. Every vertex the game transforms goes
through software that does what the GTE did. Graphics are split between the two PSP CPUs: the Media Engine (the
PSP's second CPU, normally used for audio and video) converts the PS1's drawing commands for the PSP's GPU, while
the main CPU runs the game and the geometry.

In light scenes that fits into 16.7 ms and you get 60 fps. In a full field on a detailed track the main CPU needs
20-30 ms per frame, and no amount of tuning on our side closes that gap. So Performance mode runs as fast as the
scene allows, between 30 and 60, and the game logic keeps real-time speed. If you want a steady picture, use
Quality mode.

## Why a recompilation when the PSP can already run PS1 games?

The PSP's built-in PS1 emulator runs GT2, and it runs it well. If you just want to play GT2 on a PSP, that's the
easy way.

An emulator runs the game the way it was. A native port can change it. 16:9 with a wider field of view, a 60 fps
mode with the game speed fixed for it, the race HUD drawn at the PSP's resolution, loading without a CD drive,
saves as PSP saves, a second CPU doing graphics work - all of that needs the game's code to be under our control.

And honestly, part of the reason is that it's an interesting problem. PS1 and PSP use nearly the same CPU
instruction set, so a static recompiler is possible, but the hardware around it is completely different.

## Known issues

- The Arcade Mode intro movie plays at about 10 fps (the PS1 decoded it in hardware; here it's software).
- Performance mode drops below 60 fps in heavy races (see above).
- Only the USA discs are supported.

## How it works

```
your .bin/.cue -> extract -> recompile (MIPS -> C) -> pack the data -> psp-gcc + runtime -> PSP folder
```

| PS1                  | PSP                                                                    |
|----------------------|------------------------------------------------------------------------|
| R3000A CPU code      | recompiled to C (`src/gt2psp/recomp.py`), compiled with psp-gcc        |
| GTE (geometry)       | C implementation in the runtime                                        |
| GPU                  | PSP GE display lists, conversion running on the Media Engine           |
| SPU, CD-XA audio     | software mixer, PSP audio thread                                       |
| MDEC (movies)        | software decoder                                                       |
| CD-ROM               | reads from the data pack                                               |
| Memory card          | PSP save data                                                          |
| BIOS                 | the calls the game uses, reimplemented                                 |

More in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and the development log in [docs/DEVLOG.md](docs/DEVLOG.md).

## Building from source (developers)

The converter does all of this, but by hand it is:

```
PYTHONPATH=src python -m gt2psp extract "<sim cue>" --out extracted
PYTHONPATH=src python -m gt2psp recomp
PYTHONPATH=src python -m gt2psp pack "<sim cue>"
python scripts/build_psp.py --pspdev <toolchain> --game sim --launcher
```

The toolchains for Windows and Linux x86_64 are our own builds of [pspdev](https://github.com/pspdev/pspdev)
(`scripts/wintc/build.sh`, releases `toolchain-win-v1` and `toolchain-linux-v1`), so they run without anything
else installed. Their output is byte-identical to the official Linux toolchain. Older Linux systems (glibc before
2.38), ARM Linux and macOS use the official pspdev release.

## Credits

- [pspdev](https://github.com/pspdev) for the PSP toolchain and SDK
- [psp-media-engine-custom-core](https://github.com/mcidclan) (MIT) for running code on the Media Engine
- Silent's GT2 widescreen cheat, which the 16:9 patch is based on
- PCSX-Redux, used as the reference for how the game should look

## Legal

Unofficial fan project, not affiliated with or endorsed by Sony Interactive Entertainment or Polyphony Digital.
Gran Turismo is a trademark of Sony Interactive Entertainment. This project contains no code or data from the
game. You need your own original discs.
