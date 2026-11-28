# GT2 PSP

Gran Turismo 2 running natively on the PSP. Not in an emulator: the game's MIPS code is recompiled to C and
built as a regular PSP program, with the PS1 hardware (GPU, GTE, SPU, CD drive, memory card) rewritten for the
PSP.

You bring your own discs. This repository has no game code and no game data, and neither has any release. The
converter turns your discs into a PSP folder on your own computer.

## What you need

- A PSP with custom firmware
- Your own Gran Turismo 2 discs as .cue/.bin

  Supported versions:
  - USA Simulation Mode (Disc 2), **SCUS-94488 Rev 2**
  - USA Arcade Mode (Disc 1), **SCUS-94455**
  - Europe Gran Turismo Mode (Disc 2), **SCES-12380**
  - Europe Arcade Mode (Disc 1), **SCES-02380**
  - Japan Gran Turismo (Disc 2), **SCPS-10117 Rev 1**
  - Japan Arcade Mode (Disc 1), **SCPS-10116**
  - GT2 Plus Beta v6.1 (fan mod)
  - GT2 Plus Beta 7 (fan mod)

  One disc is enough. With both discs of a version, the PSP shows a small menu where you pick the mode. European
  `.bin.ecm` images are unpacked automatically; the European discs run at 50 Hz like on a PAL PlayStation (races at
  25 fps). Every version gets its own PSP folder (`GT2PSP`, `GT2PSPEU`, `GT2PSPJP`, `GT2PLUS`, `GT2PLUSEU`) and its
  own save data, so they can sit side by side.
- Python 3 on your computer (Windows: from python.org, tick "Add python.exe to PATH"; Linux and macOS usually
  have it)
- About 3 GB of free disk space while converting

## Converting

1. Download this repository and unpack it.
2. Start the converter:
   - Windows: double-click `convert.bat`
   - macOS: double-click `convert.command`
   - Linux: run `./convert.sh`
3. Your browser opens. Under **Simulation Mode** and **Arcade Mode**, click **Choose...** and pick the disc image
   (`.cue`, `.bin` or `.bin.ecm`), or paste its path. One disc is enough. Each disc is checked at once.
4. To install straight onto your PSP: connect it by USB, start **USB Connection** on the PSP and pick its memory
   stick under **Your PSP** (or click **Find PSP**). If the game is already on the PSP, only the files that changed
   are copied - an update usually replaces just the start file and the game program, not the big data pack. Your
   save data and settings stay.
5. Click **Start conversion**. Without a PSP selected, the result is in `output/PSP/GAME` (e.g. `GT2PSP`): copy it
   to your memory stick under `PSP/GAME`, or press **Install on PSP only** later. Start the game from the XMB.
6. Optional: bring your save along (see below).

The first run downloads a PSP compiler (about 150 MB, checked against a fixed checksum); the Japanese version
also downloads a free 1 MB kanji bitmap font (see Features). After that a conversion takes a few minutes per disc.
Everything happens locally; nothing is uploaded anywhere.

The converter recognises a disc by its game code (the executable and GT2.OVL), not by the whole image: a GT2 Plus
disc you patched yourself, or another dump of a supported version, works as long as its game code is one the
converter knows. Builds with other code (USA Simulation Rev 0/1, other mods or versions) need their own conversion
profile and are rejected with "Unknown game code".

## Bringing your save along

Under **Your save (optional)** the converter takes a GT2 save from an emulator or from a real memory card (via
DexDrive or similar) and puts it on the PSP:

- whole memory cards: `.mcr`, `.mcd`, `.mc`, `.srm` (RetroArch, DuckStation, ePSXe, PCSX, ...), DexDrive `.gme`,
  Connectix `.mem`/`.vgs`, PSP `.vmp`
- single save files: `.mcs`/`.ps1`, Action Replay / Xploder `.psx`/`.mcb`, PS3 `.psv`

The converter shows which GT2 saves are on the card and which PSP folder they fit, and writes the card as
`IMPORT.MCD` into that folder (without a PSP selected: into `output/SAVE/<folder>`, to copy into `PSP/GAME/<folder>`
yourself). The next time the game starts it takes the card over. The card it had until then is kept as
`BACKUP.MCD` in the same folder, the import file is renamed to `IMPORT.MCD.done`. Without the converter:
`python -m gt2psp memcard <file> --out IMPORT.MCD` (with `src` on `PYTHONPATH`).

A save only works with the version it was made with: a USA save works in the USA versions (GT2 and GT2 Plus USA),
a European one in the European versions, a Japanese one in the Japanese version.

## Settings menu

Press **L + R + SELECT** at any time - title screen, menus or in a race - to open the settings menu. The game
pauses while it is open.

| Option              | What it does                                                                     |
|---------------------|----------------------------------------------------------------------------------|
| Mirror + smoke      | rear-view mirror and tire smoke (default: on in Quality, off in Performance)    |
| Opponent car detail | normal or low-detail models for the other cars (default: low in Performance)    |
| FPS / CPU counter   | frame rate and CPU load on screen                                                |
| L / R buttons       | PSP L/R act as the PS1's L1/R1 (views) or L2/R2 (shifting)                      |
| Race buttons        | USA versions only: X gas, square brake, circle handbrake, triangle reverse       |

Up/Down selects, X or Left/Right changes; O, START or L + R + SELECT again closes the menu. Every change takes
effect at once and is saved to `settings.txt` next to the EBOOT. The picture mode (Quality / Performance) is
picked in the start menu before the game starts.

## Features

- **16:9.** The 3D view is really wider, not stretched. The race HUD stays at its original proportions.
  On by default; `widescreen=0` in a settings.txt next to the EBOOT gives the original 4:3.
- **Two picture modes**, picked in the start menu:
  - Quality: a locked 30 fps (European versions: 25 fps), everything on.
  - Performance: aims for 60 fps (European versions: 50 fps). Mirror and tire smoke are off by default and opponent
    cars use their low-detail models (both can be switched back on).
- **European versions run at 50 Hz** like on a PAL PlayStation, so the game speed and lap times are right.
- **Game speed stays right at any frame rate.** If a frame takes longer, the game time catches up instead of the
  game slowing down.
- **Short loading times.** The game reads its files from a pre-converted data pack on the memory stick instead
  of an emulated CD drive. GT mode loads in about a second.
- **Saves are PSP saves.** The memory card is stored as normal PSP save data, visible in the XMB. Loading a save
  takes a fraction of a second.
- **Reverb** like on the PS1 in Quality mode (off in Performance mode, where every millisecond goes to the frame
  rate; `reverb=0`/`reverb=1` in settings.txt overrides it).
- **Analog controls**, L/R shifting, and on the USA versions optional racing buttons (X gas, square brake, circle
  handbrake, triangle reverse). The European and Japanese versions have gas and brake on X and square already.
- **In-game settings menu** with L + R + SELECT (see [Settings menu](#settings-menu)).
- **Bring your save along** from an emulator or a real memory card (see [Bringing your save along](#bringing-your-save-along)).
- **Arcade Mode** including its intro movie (with sound in sync).
- **Japanese kanji.** The Japanese versions draw some texts with the PS1 BIOS's kanji font, which the PSP does not
  have. The converter fills it in from the free X.Org JIS bitmap font, so those texts look slightly different from
  the original.
- **Several versions side by side**: USA, Europe, Japan and GT2 Plus each get their own PSP folder and save data.

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

- The Arcade Mode intro movie plays at a lower frame rate (the PS1 decoded it in hardware; here it's software).
- Performance mode drops below 60 fps in heavy races (see above).
- The Japanese versions are tested less than the USA and European ones.

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
| BIOS                 | the calls the game uses, reimplemented (kanji font from a free font)   |

Every supported disc has a conversion profile (`src/gt2psp/profiles.py`): where its code, overlays and patch
sites are. New versions are derived from a known one by code signatures (`python -m gt2psp derive`).

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
- PCSX-Redux, used as the reference for how the game should look and behave
- [X.Org jis-misc](https://gitlab.freedesktop.org/xorg/font/jis-misc) JIS 16-dot bitmap font for the Japanese kanji
  (downloaded by the converter, not part of this repository)

## Legal

Unofficial fan project, not affiliated with or endorsed by Sony Interactive Entertainment or Polyphony Digital.
Gran Turismo is a trademark of Sony Interactive Entertainment. This project contains no code or data from the
game. You need your own original discs.

## License

The code in this repository (recompiler, runtime, converter, scripts) is released under the [MIT License](LICENSE).
`third_party/me-custom-core` keeps its own MIT license. The license covers only this code - not Gran Turismo 2,
its code or data, and not the files the converter generates from your discs.
