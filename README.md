# GT2 PSP Recomp

A static recompiler that brings **Gran Turismo 2 (USA, Simulation Mode, Rev 2)** from the PlayStation to the
**PlayStation Portable** as a native homebrew application (EBOOT.PBP) — no emulator involved.

> **No game data is included in this repository.** You need your own disc image. The pipeline extracts the
> game's code and data from *your* image on *your* machine and produces a PSP build locally.

## Status

Early development. Milestone 1: boot to the title screen in PPSSPP. See [docs/DEVLOG.md](docs/DEVLOG.md).

## How it works (short)

PS1 (MIPS R3000A) and PSP (MIPS Allegrex) share almost the same instruction set, which makes a static
recompilation comparatively tractable. The real work is translating the hardware:

| PS1 component        | PSP replacement                                   |
|----------------------|---------------------------------------------------|
| R3000A CPU code      | recompiled to C, compiled with `psp-gcc`           |
| GTE (geometry coproc)| C / VFPU implementation                            |
| GPU (2D rasteriser)  | translated to PSP GE display lists                 |
| SPU (sound)          | software mixer → PSP audio channels                |
| CD-ROM               | file reads from the Memory Stick                   |
| BIOS                 | high-level emulation of the used BIOS calls        |
| Pad                  | PSP buttons / analog nub                           |

Pipeline:

```
your .bin/.cue ──► extract ──► analyse/recompile (MIPS → C) ──► psp-gcc + runtime ──► EBOOT.PBP
```

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Targets

* Real PSP with custom firmware (development and testing in PPSSPP)
* 480×272 native output, 30 FPS target, PSP-adapted controls

## Legal

Gran Turismo is a trademark of Sony Interactive Entertainment. This is an unaffiliated fan project. It contains no
copyrighted game code or data.
