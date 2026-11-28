Post for r/PSP (also fits r/emulation and r/granturismo). Title, images and body below.

---

**Title:** I recompiled Gran Turismo 2 into a native PSP game - 16:9, a 60 fps mode, fast loading, PSP saves (USA, Europe, Japan, GT2 Plus)

**Images** (from build/shots/reddit, in this order):
1. race-shelby.png
2. race-michelin.png
3. race-chase.png
4. race-3rd.png
5. gtmode-worldmap.png

**Body:**

For the last few weeks I've been working on a static recompilation of Gran Turismo 2 for the PSP. It's not running
in an emulator: the PS1 code is translated to C and compiled as a normal PSP program, and the PS1 hardware (GPU,
geometry coprocessor, sound chip, CD drive, memory card) is rewritten on top of the PSP's own hardware.

**What you get compared to the PSP's PS1 emulator (POPS)**

- Real 16:9. The camera sees more, nothing is stretched, the race HUD keeps its shape.
- A Performance mode that targets 60 fps, next to a locked 30 fps Quality mode. Game speed is tied to real time,
  so it doesn't speed up or slow down with the frame rate.
- Loading without a CD drive. GT mode comes up in about a second, a save loads instantly.
- Saves go to normal PSP save data in the XMB.
- Both discs: Simulation Mode and Arcade Mode, with a little start menu if you have both.
- In-game settings menu (L + R + SELECT): mirror and tire smoke, opponent detail, FPS counter, button layout.
- Optional racing buttons on the USA versions (X gas, square brake, circle handbrake, triangle reverse).

**Supported versions**

- USA: Simulation Mode **SCUS-94488 Rev 2**, Arcade Mode **SCUS-94455**
- Europe: Gran Turismo Mode **SCES-12380**, Arcade Mode **SCES-02380**
- Japan: Gran Turismo **SCPS-10117 Rev 1**, Arcade Mode **SCPS-10116**
- GT2 Plus Beta v6.1 (fan mod) on the USA and European discs

One disc is enough. Every version gets its own PSP folder and its own save data, so they can sit side by side. The
European versions run at 50 Hz like on a PAL PlayStation (Quality 25 fps, Performance up to 50 fps), so game speed
and lap times are the same as on a PAL console. The Japanese versions use the PS1 BIOS's kanji font, which the PSP
doesn't have - the converter fills it in from a free JIS bitmap font, so a few texts look slightly different.

**No game files are shared**

You run a converter on your PC (Windows, macOS, Linux - double-click, it opens in your browser), pick your own
.cue/.bin (European .bin.ecm works too), and it builds the PSP folder from your disc. With the PSP connected over
USB it installs straight onto the memory stick; on an update it only copies what changed, your saves and settings
stay. Discs are recognised by their game code, so other dumps of a supported version or a GT2 Plus disc you
patched yourself work too. You need a PSP with custom firmware.

**About 60 fps**

It's a target, not a guarantee. The PS1 had a geometry coprocessor that the PSP doesn't have, so all of that math
runs in software. I moved the graphics conversion to the PSP's second CPU (the Media Engine) and that got races to
a solid 30 and to 60 in lighter scenes. A full grid on a busy track still needs 20-30 ms of CPU time per frame on
the main CPU, so Performance mode floats between 30 and 60 there. If that bothers you, Quality mode is a locked 30.

**Why not just use POPS?**

Fair question - the built-in emulator runs GT2 fine. But an emulator can only run the game as it is. Widescreen
with a wider view, a 60 fps mode with fixed game speed, a HUD at PSP resolution, no CD loading - that needs the
game's code to be something you can change. Also, PS1 and PSP share almost the same CPU instruction set, which
makes this kind of recompilation possible at all, and I wanted to see how far it goes.

**Known issues**

- The Arcade intro movie plays at a lower frame rate (the PS1 decoded it in hardware, here it's software).
- Performance mode drops below 60 fps in heavy races.
- The Japanese versions are tested less than the USA and European ones; GT2 Plus Europe (Gran Turismo Mode) is
  recognised but not tested on a real dump yet.

**Download and instructions**

https://github.com/gutster95/gt2-psp-recomp/releases/latest

Tested on a PSP-3000. Feedback welcome, especially from PSP-1000/2000 owners - I couldn't test those, so I'd like
to know if it runs.

**Disclaimer:** This project was heavily supported by AI. Especially the optimization parts couldn't have been
done by me alone.

---

Before posting:
- The latest GitHub release must contain the Europe / Japan / GT2 Plus support, otherwise `releases/latest`
  still points at the USA-only version.
- If the first post is already up, post this as an update rather than editing the old one.
- PSP-1000 (32 MB RAM): untested - don't promise anything there.
