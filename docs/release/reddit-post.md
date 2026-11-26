Post for r/PSP (also fits r/emulation and r/granturismo). Title, images and body below.

---

**Title:** I recompiled Gran Turismo 2 into a native PSP game - 16:9, a 60 fps mode, fast loading, PSP saves

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

What you get compared to playing it in the PSP's PS1 emulator:

- Real 16:9. The camera sees more, nothing is stretched, the race HUD keeps its shape.
- A Performance mode that targets 60 fps, next to a locked 30 fps Quality mode. Game speed is tied to real time,
  so it doesn't speed up or slow down with the frame rate.
- Loading without a CD drive. GT mode comes up in about a second, a save loads instantly.
- Saves go to normal PSP save data in the XMB.
- Both discs: Simulation Mode and Arcade Mode, with a little start menu if you have both.

**No game files are shared.** You run a converter on your PC (Windows, macOS, Linux - double-click, it opens in
your browser), point it at your own .cue/.bin, and it builds the PSP folder from your disc. It only accepts the USA
discs (Simulation Mode Rev 2 and Arcade Mode) because the recompiler is made for exactly that code. You need a PSP
with custom firmware.

**About 60 fps:** it's a target, not a guarantee. The PS1 had a geometry coprocessor that the PSP doesn't have, so
all of that math runs in software. I moved the graphics conversion to the PSP's second CPU (the Media Engine) and
that got races to a solid 30 and to 60 in lighter scenes. A full grid on a busy track still needs 20-30 ms of CPU
time per frame on the main CPU, so Performance mode floats between 30 and 60 there. If that bothers you, Quality
mode is a locked 30.

**"Why not just use POPS?"** Fair question - the built-in emulator runs GT2 fine. But an emulator can only run the
game as it is. Widescreen with a wider view, a 60 fps mode with fixed game speed, a HUD at PSP resolution, no CD
loading - that needs the game's code to be something you can change. Also, PS1 and PSP share almost the same CPU
instruction set, which makes this kind of recompilation possible at all, and I wanted to see how far it goes.

Known issues: the Arcade intro movie plays at about 10 fps, and only USA discs work.

Download and instructions: https://github.com/gutster95/gt2-psp-recomp/releases/latest

Tested on a PSP-3000. Feedback welcome, especially from PSP-1000/2000 owners - I couldn't test those, so I'd like
to know if it runs.

---

Before posting:
- The repository must be public and the release published, otherwise the link is dead.
- PSP-1000 (32 MB RAM): untested - don't promise anything there.
