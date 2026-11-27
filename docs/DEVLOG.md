# Development log

## 2026-11-17 — Project start

* Goal fixed: static recompiler for GT2 Simulation Mode **Rev 2** → PSP (CFW), milestone 1 = title screen in PPSSPP.
* Analysed the local reference PC port (UGT2R 0.4.3): it accepts only disc v1.0 and ships no source code (game
  code as `.wasm` modules on a closed "PSX Runtime"). Decision: write our own MIPS → C recompiler working directly
  on the Rev 2 executable and overlays; use the PC port only as documentation.
* Toolchain: pspdev has no native Windows build. We use a project-local WSL distro (Ubuntu 24.04) whose virtual
  disk lives in `tools/wsl`, with pspdev v20261101 inside it. WSL1/WSL2 were not enabled on the dev machine;
  enabling the Windows feature (`wsl --install --no-distribution`, admin + reboot) is required once.
* Disc extractor (`python -m gt2psp extract <cue>`) works. Rev 2 image: 294154 sectors, volume `GRANTURISMO2`.

  | File          | LBA    | Size        | Notes                                   |
  |---------------|--------|-------------|-----------------------------------------|
  | SYSTEM.CNF    | 23     | 68          | boot config                             |
  | SCUS_944.88   | 24     | 628,736     | main PS-X EXE                           |
  | GT2.OVL       | 331    | 289,752     | code overlays                           |
  | GT2.VOL       | 473    | 488,241,152 | game data volume                        |
  | MUSIC.DAT     | 238872 | 85,262,336  | XA Form 2 streamed music (kept raw)     |
  | FAULTY.PSX    | 280504 | 27,648,000  | filler / padding                        |

## 2026-11-17 — WSL blocked, first binary analysis

* `wsl --import` fails with `HCS_E_HYPERV_NOT_INSTALLED`: AMD SVM is disabled in the BIOS
  (`VirtualizationFirmwareEnabled = False`, board ASUS ROG STRIX B550-F). User is enabling SVM Mode.
* SCUS_944.88 (PS-X EXE): entry `pc = 0x8005d600`, text loaded at `0x80010000`, size `0x99000`
  (ends `0x800a9000`), `gp`/`sp` not set in the header (SYSTEM.CNF: STACK = 801fff00, TCB 4, EVENT 10).
* GT2.OVL layout: 12-word header = 6 × (offset, size) pairs, members 4-byte aligned. Each member is a
  plain **gzip** stream (mtime Dec 1999) holding raw MIPS code without a header.

  | # | packed  | unpacked | load addr (inferred) |
  |---|---------|----------|----------------------|
  | 0 | 0x23545 | 0x4d5f8  | 0x80010000           |
  | 1 | 0xad2d  | 0x3c8c4  | 0x80010000           |
  | 2 | 0xd08d  | 0x43544  | 0x80010000           |
  | 3 | 0x144b  | 0x2cec   | 0x80010000           |
  | 4 | 0x963d  | 0x42a74  | 0x80010000           |
  | 5 | 0xe12   | 0x20e0   | 0x80010000           |

  Load address inferred by voting (jal target − prologue offset): every overlay maps to `0x80010000`,
  i.e. overlays overwrite the *start* of the main EXE's text. The low part of SCUS_944.88 is therefore
  presumably boot/init code that is discarded once an overlay is loaded; the resident kernel lives
  above the largest overlay end (≥ 0x8005d5f8). To verify: find the overlay loader in the main EXE.
  Consequence for the recompiler: code at the same address exists in up to 7 variants → functions must be
  keyed by (module, address) and indirect calls dispatched through the currently loaded module.

## 2026-11-17 — Toolchain working

* SVM enabled; `wsl --import GT2PSPDev tools/wsl ...` succeeded (WSL2).
* `scripts/wsl-setup.sh` (run as root once) unpacks pspdev to `/opt/pspdev`, writes `/etc/profile.d/pspdev.sh`
  and installs the host libraries the prebuilt toolchain needs (libmpc3, libmpfr6, libisl23, ... — without them
  `cc1` fails with `libmpc.so.3: cannot open shared object file`). `psp-gcc (GCC) 15.2.0`.
* `runtime/hello` + `scripts/build-hello.sh`: debug-screen hello world → `build/hello/EBOOT.PBP` (PRX-based).
  Boots in PPSSPP 1.20.4 (portable, `tools/ppsspp`) and exits cleanly via `sceKernelExitGame()`.
* Build from Windows: `wsl -d GT2PSPDev --cd "<proj>" -- bash scripts/build-hello.sh`.

## 2026-11-17 — Program structure and function partitioning

* New tooling: `mips.py` (R3000A + GTE decoder), `image.py` (PS-X EXE / GT2.OVL loaders), `analysis.py`
  (recursive descent, BIOS stub + I/O detection, text-range detection, linear-sweep partitioning),
  `program.py` (whole-program model). CLI: `disasm <addr> [--ovl N]`, `analyze [--ovl N --entry ..]`,
  `functions` (→ `generated/functions.json`).
* **SCUS_944.88 layout** (text `0x80010000-0x800a9000`):
  * `0x80010000-0x80011d30` boot code (own ISO9660 reader: strings `CD001`, `gt2.ovl`, `gt2.vol`, `music.dat`,
    `stream.dat`, `/carobj`, `/carlogo`, `/carwheel`, `/engine`, `/crsmap`; C++ RTTI `14VendorBootLogo`).
  * `0x80011df8` gzip `notice.tim` (0x50220 bytes unpacked), `0x8003322c` gzip `logo-scea.tim` (0x28040).
  * zeros up to `0x8005d600`; this whole low part is overwritten once an overlay is loaded.
  * `0x8005d600-0x8008e018` resident text (CRT, overlay loader, PsyQ libs, BIOS stubs at `0x8008c9xx`,
    `0x8008dfxx`), resident data from `0x8008e020`. BSS cleared by CRT: `0x801d93b0-0x801f0d60`.
* **Startup**: `0x8005d600` (PsyQ `__main`-style CRT, saves regs, clears BSS) → `0x8005d6e0` main →
  `0x80085a98`, boot `0x80010e14` (lib init: GPU/SPU/CD/pad, notice + logo), `0x8005d9f0`, `0x8005da3c(1)`.
* **Overlay switching**: `0x8005da3c(n)` looks up the entry point in the table at `0x80091174`, stores up to 4
  args at `0x801d945c`, `0x8005dad8` loads overlay `n` (from a RAM copy if present, else CD into `0x800a8d5c`),
  gunzips it to `0x80010000` (`0x80082fac`), `FlushCache`, clears the heap `0x800a8d5c-0x801d93b0`, then
  `longjmp(0x801d942c, entry)`. `0x8005d9f0` is the matching `setjmp` dispatcher which calls `entry(a0..a3)`.
  So every overlay switch unwinds the stack. Entries: ovl0 `0x80012254`, ovl1 `0x80011384`, ovl2 `0x80011750`,
  ovl3 `0x80012c00`, ovl4 `0x80013628`, ovl5 `0x800114b8`.
* Function partitioning (strong starts = all `jal` targets + code pointers in data that follow a function end;
  weak starts = after `jr ra`, dropped when a branch crosses them): 3664 functions in total
  (resident 1313, boot 47, ovl0 935, ovl1 411, ovl2 427, ovl3 16, ovl4 460, ovl5 55). A handful of
  functions fall through into the next one (shared epilogues / second entry points) → recompiler must emit a
  tail call to the following function.

## 2026-11-17 — Recompiler: whole program translates and compiles

* `setjmp` = `0x8007ad58`, `longjmp` = `0x8007ad90` (12-word buffer: ra, sp, fp, s0-s7, gp).
  The overlay switch `longjmp`s into the frame of `0x8005d9f0`, which has already *returned* at that point
  (it only works on the PS1 because the stack memory is still intact) → see "Unwind & resume" in ARCHITECTURE.md.
* Partitioning improvements:
  * switch **jump tables** detected (`sltiu N` bound, `lui/addiu` base, `lw`, `jr`); their entries are code
    labels, not function starts (before this, every `case` became a function: 3664 → ~2840 functions);
  * text ranges are extended backwards to call targets just before them (code directly after rodata);
  * *resume points* (return addresses of calls that can reach `setjmp`) and any branch target landing inside a
    different function become extra function starts; iterated until stable.
* `recomp.py`: MIPS → C (one C function per PS1 function, `goto` labels, statically resolved delay slots,
  switch over jump-table labels for `jr reg`, tail calls, `rt_call`/`rt_call_ovl`/`rt_jump` for dynamic targets).
  Result: **2853 functions, 162,676 instructions, 0 unresolved static targets**, 245 computed calls,
  165 computed jumps. `python -m gt2psp recomp` → `generated/*.c` (~8 MB of C).
* `runtime/include/gt2rt.h`: CPU struct (`PsxCpu` — `R3000` is a predefined macro in psp-gcc), inline memory
  access (RAM mirrors, scratchpad fast path, I/O via functions), lwl/lwr/swl/swr, div semantics.
* `scripts/compile-generated.sh`: all modules compile with `psp-gcc -O2` without errors.
  Object sizes: resident 1.0 MB, ovl0 1.45 MB, ovl1 0.33 MB, ovl2 0.45 MB, ovl3 63 KB, ovl4 0.38 MB,
  ovl5 60 KB, boot 75 KB → ~3.8 MB of PSP code in total.

## 2026-11-18 — Runtime: recompiled GT2 boots, reads the disc, switches overlays, runs its frame loop

* `runtime/src/`: `main.c` (loads SCUS_944.88 data into PS1 RAM, CPU init), `core.c` (dispatcher, `setjmp`/
  `longjmp` unwind & resume, overlay hook, interrupt delivery from `POLL()` points, watchdog log), `bios.c`
  (A0/B0/C0 HLE, syscalls, COP0), `io.c` (I/O register file, IRQ controller, DMA incl. OTC + GPU linked list,
  timers), `cdrom.c` (CD controller at register level backed by the user's `GT2.BIN`), `sio.c` (digital pad on
  port 1 from the PSP buttons), `gpu.c` (GP0/GP1 parsing, VRAM transfers, presentation; no rasterizer yet),
  `gte.c` (stub), `log.c` (`gt2log.txt` next to the EBOOT). `scripts/build.sh`, `scripts/run-ppsspp.ps1`.
* Game files go next to the EBOOT: `SCUS_944.88`, `GT2.BIN` (the user's raw disc image).
  PPSSPP runs portable from `tools/ppsspp` (memstick in `tools/ppsspp/memstick`).
* Findings while bringing it up:
  * `R3000` is predefined by psp-gcc → CPU struct is `PsxCpu`.
  * The BIOS enters the `HookEntryInt` hook like `longjmp(hook, 1)`: `v0` must be 1, otherwise libetc's
    dispatcher (`8008be74`, right after a `setjmp`) treats it as the initial return and skips the handler.
  * `ReturnFromException` ends the whole interrupt (chains + hook); a `longjmp` out of an interrupt resets
    the runtime's interrupt state.
  * libcd reads the 12-byte sector header and the data with two separate `CdGetSector` DMAs; the request
    register (BFRD) loads the buffered sector only when the data FIFO is empty (Mednafen semantics).
  * GT2's pad driver talks to SIO0 directly from a `SysEnqIntRP(2)` VBlank handler (func1 `80087668`,
    func2 `800876d0`).
  * Library identification: VSync `8008c338`, libetc IRQ dispatcher `8008bee4`, CdGetSector DMA `8008a488`,
    memory card state machine `8007eacc`, GPU DrawSync callback `8007d2d4`.
* Result in PPSSPP: CRT → boot code (CD_init, SPU init, MDEC init, pad, memory card) → GT2.OVL read from the
  disc by the game's own ISO9660 reader → gunzip → `longjmp` → **overlay 1 running its frame loop** (GPU DMA,
  VSync, ~11 primitives/frame, display mode 368x480i). Screen still black: primitives are not rasterized yet.

## 2026-11-18 — First picture: GT2's legal notice rendered by recompiled code

* `runtime/src/raster.c`: software rasterizer into the emulated VRAM (triangles via edge functions with
  incremental 16.16 attributes, quads as two triangles, sprites/rectangles, lines/polylines; 4/8/15-bit
  textures with CLUT, texture window, modulation, 4 semi-transparency modes, mask bits; draw area/offset).
* Presentation: the PS1 display area is scaled (nearest) to a 4:3 363x272 window centred on the PSP screen.
* Debug: VRAM snapshots (`vram300/900/1500.bin`) + `scripts/vram2png.py`.
* Result: the legal notice (640x480i, textured sprites over a dimmed photo) displays correctly in PPSSPP.
* Performance: a software-rasterized 640x480 screen with several full-screen layers runs at only a few fps
  on PSP-class CPU time (PPSSPP paces the emulated CPU). Decision: keep the software rasterizer as the
  accurate reference; add fast paths / move hot primitives to the GE later (see ARCHITECTURE.md).

## 2026-11-18 — Milestone 1: title screen

* Sprite fast paths in the rasterizer (pre-modulated CLUT per primitive, row fills for flat rectangles) make
  the notice fade run at playable speed; the game then loads overlay 1 and shows the **GT2 title screen**
  ("Start Game", "Replay Theater", Polyphony Digital logo) — all game logic is recompiled code.
* Title screen frame: 1 clear rect, 4 textured 256x256 background sprites, 1 flat triangle (menu cursor),
  5 textured menu sprites with varying modulation (fade/highlight).

## 2026-11-18 — GTE, scripted input, first 3D: attract-mode race replay renders

* `runtime/src/gte.c`: full GTE (RTPS/RTPT with the UNR division table, NCLIP, OP, DPCS/DPCT, INTPL, MVMVA incl.
  the garbage matrix, NCS/NCT/NCCS/NCCT/NCDS/NCDT, CC/CDP, SQR, DCPL, AVSZ3/4, GPF/GPL; register read/write
  quirks, FLAG bits). Simplification: MAC1-3 overflow checked on the final sum only.
* Scripted input for tests: `input.txt` next to the EBOOT, lines `<vblank> <button> <duration>`.
* Partitioning: code pointers found in data or built with `lui`+`addiu/ori` become function entries if they
  follow a function end, start a stack frame (`addiu sp, sp, -N` — second entry points such as `80068310`,
  called through a pointer from ovl0), or are not inside a loop. Splitting inside loops turned loops into
  tail-call chains and crashed. 2910 functions now.
* Bug: SysEnqIntRP chain handlers ran with `sp = 0x801FFC00`, i.e. inside the game's own stack — the VBlank
  pad handler overwrote C++ objects on the stack (crash in a virtual call `obj->vtable[3]` with a vtable
  pointing into code). Interrupt handlers now use a stack in the unused kernel RAM (`0x8000E000`).
* Result: title screen → Start → overlay 0 (main game, entered at `80011f64`) → 320x240 double-buffered 3D:
  the attract-mode **race replay** (Shelby GT350 '66, lap/position/time HUD) renders correctly.

## 2026-11-18 — Performance baseline (PPSSPP, PSP-paced CPU)

Measured in the 3D race replay (320x240, ~1170 primitives per frame), `gpu:` log line every 120 vblanks:

| per game frame                        | time     |
|---------------------------------------|----------|
| total                                 | ~165 ms (~6 fps) |
| software rasterizer (display lists)   | ~118 ms (72 %) |
| recompiled game code + runtime rest   | ~46 ms   |

The emulated vblank clock advances from `POLL()`, so the game runs in slow motion rather than dropping frames.
Conclusion: a GE-based renderer is the top priority for speed; the recompiled code itself also needs work
(target < 33 ms) — candidates: keep CPU registers in locals instead of `c->r[]`, cheaper memory access for
known-RAM bases (sp/gp), fewer POLL checks, `-O3`/profile-guided hot paths.

## 2026-11-18 - Direction: native port; data pack GT2.PAK replaces the disc image

User feedback: shipping the raw disc image next to the EBOOT and emulating PS1 hardware is not acceptable - the
goal is a native port where the PC converts code and assets once and the PSP (target: PSP-3000) just runs it.
Plan: (1) data pack instead of BIN, (2) GE renderer with PSP-native textures, (3) native audio with music
pre-converted on the PC, (4) movies pre-converted, (5) saves as memory stick files.

Step 1 done:
* `src/gt2psp/pack.py`, `python -m gt2psp pack <cue>` -> `build/GT2.PAK` (12 runs, 587 MB).
* `cdrom.c` reads from the pack (sceIo, 128 KB read-ahead); `main.c` loads SCUS_944.88 from the pack (LBA 24).
* Bug found on the way: a newly delivered sector emptied the data FIFO while CdGetSector (`8008a488`) was waiting
  for it (BFRD written, then polling status bit 6) -> endless loop inside the CD interrupt. Sector buffer and
  data FIFO are now separate as on hardware; the FIFO is only loaded by BFRD.
* Data sectors every 1.5 ms instead of 6.7 ms. 100 us worked but kept the libcd interrupt handler busy for
  seconds (it drains all pending sectors with interrupts blocked), stalling VSync.
* CPU clock set to 333 MHz at startup (PSP default is 222 MHz).
* Verified in PPSSPP: boot -> title -> overlay 0 -> 3D race replay, PSP folder = EBOOT.PBP + GT2.PAK only.

## 2026-11-18 - GE renderer: race replay at full 30 fps

* `runtime/src/ge.c`: PS1 primitives on the PSP GE (design in ARCHITECTURE.md). PS1 VRAM moved into eDRAM.
* Debugging: nothing reached VRAM at first. A startup self-test (draw sprite, read back) showed that raw
  scissor commands were overridden by pspgu's own scissor state; sceGuScissor (width/height semantics in this
  pspsdk, verified by the self-test) fixed it.
* PPSSPP's hardware renderers keep GE output on the host GPU (no write-back to emulated VRAM), so VRAM dumps
  were black and texture copies from rendered areas would fail there. Tests now use PPSSPP's software renderer.
* 640-wide display modes are presented in 256-texel strips (GE texture width limit 512).
* Result (PPSSPP, hardware renderer): race replay 60 display lists per 120 vblanks = the game's 30 fps,
  title 60 fps; 0 software fallbacks, 38 texture page copies in total. Screens: `build/milestone3-ge-race.png`.

## 2026-11-18 - Native 480x272 rendering and 16:9 widescreen

* Displayed frames are rendered a second time at PSP resolution straight into the display buffers (design in
  ARCHITECTURE.md); 1x VRAM stays exact. Title screen now native-resolution 4:3 pillarbox
  (`build/milestone4-hires-title.png`); race replay fills 16:9 (`build/milestone4-widescreen-race.png`).
* Widescreen = GTE hack: RTPS/RTPT projected X * 3/4. HUD stays 4:3 in the centre.
* Bug fixed on the way: state-cache sentinel -1 collided with "blend off", leaving additive blending enabled
  from the previous list (blown-out title).
* Known issue: near ground polygons at the 16:9 edges are missing (black steps): the game discards polygons
  that lie outside the 4:3 screen. The cull/subdivision code still has to be found and widened.
* PPSSPP (software GE) cost: ~45 instead of 60 lists per 120 vblanks in the race; real PSP numbers pending.

## 2026-11-18 - 16:9 without missing geometry

* First approach (3D X * 3/4, PS1 frame mapped 4:3 into the centre) lost near ground at the 16:9 edges: the edge
  areas were outside PS1 x 0..320 and the game culls in screen space in many places.
* Measured with a GTE error histogram: ~12% of RTPTs fail (vertex near/behind camera); 90% of them in overlay 0's
  scenery renderer 0x80018d1c. Added a recompiler patch mechanism and a near-plane clipper for its 8 loops - it
  works (~900 polygons recovered per minute) but was not the cause of the black edges.
* Fix: keep X * 3/4 but stretch 3D over the full width in the hi-res pass, so 16:9 = PS1 0..320 and every game
  visibility test is right. 2D/3D are told apart by tagging GTE output coordinates. Result:
  `build/milestone5-widescreen-race.png` - full 16:9, no gaps, HUD undistorted.

## 2026-11-18 - Widescreen: widen the game's view window (world-space culling)

* Research: the established GT2 widescreen fix is Silent's (CookiePLMonster) "16:9 Widescreen Hack 2.0"
  (DuckStation cheat, github.com/CookiePLMonster/Console-Cheat-Codes, PS1/Gran Turismo 2). It patches the viewport
  extents the game hands to its view setup, not the projection. For NTSC-U 1.2 race/replay: overlay 0
  `0x800100D0/D4` `addiu a1/a2, a2, -160/+160` before `jal 0x8007b320`, rear view mirror `0x800295EC/F0` (+-60).
  Scaled by 4/3 (+-213, +-80) the game's world-space culling covers 16:9.
* Implemented as recompiler OVERRIDEs (active when `g_widescreen`), on top of the GTE X*3/4 squeeze + 3D stretch.
  Objects at the 16:9 edges no longer pop in/out. Menu screens (car selection, GT mode) have further viewport
  values in Silent's cheat - to be ported when those screens are reached.

## 2026-11-18 - Visual quality: perspective-correct, sub-pixel 3D and texture filtering

* Research: no GT2 texture pack exists (GT3 on PS2 only); DuckStation packs would not fit anyway. Anti-aliasing on
  the GE only via supersampling, which needs more than the 2 MB eDRAM (PSP-3000 has 4 MB, access unclear).
* Implemented PGXP-style rendering in the hi-res pass (design in ARCHITECTURE.md) + bilinear filtering of opaque
  3D textures. Before/after: `build/cmp-2100.png`, `build/cmp-3900.png` - road and walls no longer warp/jitter.
* A self-test confirms the GE 3D path covers exactly the same pixels as through mode for extreme w.
* Filtering transparent textures (trees) produced dark seams: now skipped for CLUTs with transparent entries.
* Black garbage polygons appeared in some frames with the near-plane clipper enabled; it is now off by default.
* PPSSPP (software GE) speed is the same with and without PGXP/filtering; real PSP numbers still pending.
* `settings.txt` added for toggling all enhancements without rebuilding.

## 2026-11-18 - Performance round: from ~13 to ~28 fps (PPSSPP, heavy replay scenes)

Measured with a new sampling profiler (prof.c, enabled by profile.txt; 1 kHz thread, runtime phases + hottest
recompiled functions). Frames per 120 vblanks in the heavy part of the replay (30 fps = 60):

| step | frames/120 vb | finding |
|------|---------------|---------|
| baseline | 23-32 | GTE 51%, GE build 26%, game 20% |
| GTE 32x32 multiplies | 32-45 | 64-bit products/division/float conversion were libgcc calls |
| direct GE draw cmds, precomputed mappings | 41-47 | sceGuDrawArray calls the kernel (stall update) every draw |
| forced inlining (mem/GTE regs), restrict, static RAM | 46-51 | mem_r32 was an out-of-line call per access |
| inline GTE fast paths RTPS/RTPT/NCLIP/MVMVA | 46-59 | ~16k GTE ops per frame; decoding folded at compile time |
| DMA packets in place, raw GE state cmds, vertex arena | 52-60 | per-word GP0 calls, pspgu call overhead |
| registers in locals (recompiler) | 50-60 | c->r[] was stored/reloaded around every memory access |

The last step barely shows in PPSSPP (its timing does not model loads/stores) but removes most memory traffic of
the generated code on real hardware. Remaining profile: game code ~65% (incl. inlined GTE, ~4% VSync wait),
GE build ~25%, IRQ 3%. Not yet done: TRACE removal in release builds, POLL budget tuning, cheaper hi-res pass.

## 2026-11-18 - 60 fps dropped; target stays 30 fps

Tried Silent's GT2 "60 FPS" cheat (frame interval 0x801D5864 2 -> 1 plus three effect patches in overlay 0). It
works technically (~40 fps in PPSSPP), but the game advances its simulation by a fixed step per frame, so anything
below a steady 60 fps runs in slow motion (measured ~2/3 speed). Decision with the user: removed again, the target
is a stable 30 fps.

## 2026-11-18 - Controller (DualShock) and memory card

* Controller input never worked before: libpad clears IRQ7, waits for RX, reads the byte and only then waits for
  the /ACK interrupt - the emulation raised it at transmit time, libpad timed out after the first byte of every
  poll. Now the ACK interrupt is raised when the response byte is read. The "race replay" seen so far was GT2's
  attract demo; with input the game now reaches GT mode (world map, My Home, Game Status).
* DualShock (SCPH-1200) emulation: analog mode by default (ID 0x73, settings.txt analog=0 for digital), config
  commands 0x43-0x4D (GT2's libpad uses config mode), PSP stick = left stick with dead zone, SELECT+L/R = L2/R2.
* Memory card: GT2 uses only the BIOS raw sector functions (_card_info A0:AB, _card_read B0:4F, _card_write
  B0:4E, _new_card via write sector 63 with NULL) and its own file handling. card.c emulates them on a 128 KB
  .mcd image (GT2CARD1.MCD next to the EBOOT, created formatted), results are delivered as SwCARD/HwCARD events
  whose callbacks run in interrupt context shortly after the call (bios.c event table: OpenEvent/EnableEvent/
  DeliverEvent with callback mode). The callbacks are 2-instruction stubs at 0x8007ea68+8n that the partitioner
  had taken for a jump table: program.py EXTRA_STARTS.
* GT2 detects the card and reads its directory at boot. An in-game save has not been exercised yet (the save
  menu was not found by scripted navigation).
* CD fix: the "overrun" (deliver the next sector if the game has not taken the previous one within 13 ms) let a
  new sector replace an untaken one when frames were slow, which desynchronised libcd's interrupt handler (hang in
  CdGetSector in overlay 4). Removed; a new read/pause/stop discards an untaken sector instead.

## 2026-11-18 - Memory card as PSP save data

* User wish: keep things PSP-native. The card image is now a PSP save through sceUtilitySavedata (savedata.c):
  ms0:/PSP/SAVEDATA/GT2PSP00000CARD1/ with MEMCARD.BIN (encrypted by the system), PARAM.SFO (title "Gran Turismo 2
  (PSP)", "Memory Card 1", detail text) and ICON0.PNG (own artwork, scripts/make_save_icon.py -> save_icon.h).
  Visible, copyable and deletable in the XMB under Saved Data.
* Auto modes only (no PSP dialog; GT2's own card screens are the UI). Load blocking at startup; saving in the
  background from the vblank tick once the game has not written a sector for 1 s (snapshot of the image).
* A GT2CARD1.MCD from the previous build is taken over once. Verified in PPSSPP: save result 0, reload gives the
  identical image (checksum logged on save and load).

## 2026-11-18 - Audio: SPU and CD-XA music

* spu.c: SPU at register level (psx-spx): 24 ADPCM voices from 512 KB sound RAM (DMA4 and data FIFO), ADSR
  envelopes, pitch + pitch modulation, noise, voice/main volumes, key on/off latched and applied per block, ENDX,
  current-envelope reads. Not emulated: reverb (dry output), volume sweeps (fixed), SPU IRQ.
* CD-XA: cdrom.c passes XA-ADPCM sectors (mode bit 6, filter file/channel) to spu_xa_sector, which decodes them
  (4/8 bit, mono/stereo, 37.8/18.9 kHz) into a ring buffer; the mixer resamples to 44.1 kHz and adds it with the
  SPU CD volume. GT2 streams its music this way (MUSIC.DAT, stereo 37.8 kHz, channel interleave).
* Output: PSP thread, 1024-sample blocks, sceAudioOutputBlocking at 44.1 kHz.
* Debug: audiodump.txt -> audio.raw (first 3 min). Check: output is structured (lag-1 autocorrelation ~0.98),
  ~0.1% of samples clip.
* Cost (PPSSPP): heavy replay scenes 37-50 frames/120 vblanks with audio vs 50-60 without, after moving the mixer
  to a voice-by-voice block loop (first version: 31-45). settings.txt audio=0 turns sound off for comparisons.

## 2026-11-18 - Audio fix: music dropouts ("constant hiss")

* User: constant hissing. Measured separately (debug dump now SPU L/R + XA L/R): SPU spectrum normal; XA decoding
  matches an independent Python decoder and has a normal music spectrum - but the XA output had a gap every ~90 ms
  (368 gaps of 15-40 ms in 30 s): constant crackling.
* Cause: the CD scheduled each XA sector one period after the delivery instead of after the previous due time, so
  the stream drifted behind real time, and the mixer played without any buffer. Fix: fixed 75/150 Hz sector clock
  for XA reads, a ~100 ms jitter buffer in the mixer, linear interpolation in the 37.8 -> 44.1 kHz resampler.
  Result: 0 gaps.

## 2026-11-18 - Audio cost

* Mixer CPU measured in the audio thread (logged every 5 s): ~25-29% with 15-19 voices at 44.1 kHz in PPSSPP.
  Chunked envelope and a decoded-block cache (looping engine samples) did not change it measurably; the cost is
  per output sample and voice (interpolation, gains, pitch counter with up to 4x pitch).
* SPU voices are now mixed at 22.05 kHz into a PSP SRC channel (hardware resampling to 44.1 kHz), the XA music runs
  at 44.1 kHz on a separate PSP channel (hardware-mixed). Mixer ~20%; heavy replay scenes 36-57 frames/120 vb.
  Real PSP numbers pending.

## 2026-11-18 - Movies (MDEC) and the CD layer: findings

* Movies: the Simulation disc has none. No STR/video sectors; overlay 5 is the shared `psxMovieLoop` player (movies
  24/25/26 of the disc's movie table); forcing it at boot makes it stream from LBA 50880, which on this disc is plain
  GT2.VOL data. The game never loads overlay 5 on this disc (the opening movie is on the Arcade disc).
* MDEC is otherwise only used by overlay 2 (Arcade menu, not reachable from the Simulation disc). Added `mdec.c`
  anyway (cmd 1/2/3, lazy macroblock decoding, DMA0/DMA1, deferred DMA1): only the two table uploads at boot run
  through it on this disc, decoding itself is untested.
* CD layer: measured every read (`cd: read of N data sectors took X ms`). Loads read 30-280 sectors, 0.05-0.4 s at
  the 1.5 ms sector spacing. 0.75 ms halves that and works; 0.1 ms breaks the game (a runaway read of 55k sectors at
  boot, the GT map is never reached) - libcd's read logic depends on sector timing. A libcd-level read path would
  save only these fractions of a second per load, so it is postponed until load times on the real PSP are known.

## 2026-11-18 - Widescreen in menus, software fallback check

* Navigated by script to the dealer car view (East City -> Mazda -> Demio), license test B-1 and the attract race.
  The dealer screen (3D car + 2D menu) was torn apart by the HUD anchoring (breadcrumb bar, buttons and price shifted
  to the edges in pieces). Anchoring now only runs in overlay 0 (races, incl. license tests); menus keep their 2D
  layout in the 4:3 middle. Race and license HUD still anchored; the dealer car renders correctly.
* Software fallbacks (15-bit textures, texture windows): 0 in ~10 M primitives over title, map, menus, dealer,
  license test and replay. 24-bit display mode only occurs in the (unused) movie overlay. Nothing to do for now.

## 2026-11-18 - First tests on the real PSP-3000

* Missing on hardware, invisible in PPSSPP: HOME exit callback (now registered); the log lost everything after the
  first lines on power-off (FAT size only updated on close - the log is reopened every second now); the
  perspective-correct 3D pass drew nothing (GE transform mode with viewport scale 1 draws 0 pixels on hardware; the
  startup probe now picks a variant with scale 256); drawing into the still scanned-out display buffer (half-drawn
  frames, black band streaks - now waits for the swap vblank); unaligned 32-bit stores trap (BMP header);
  CLUT syncs per primitive on the GT map (palette next to a frame buffer in the same 64x256 page - now bounding box).
* PSPLink 3.2.1 (tools/downloads) + libusb-win32 driver: scripts/psp-run.sh resets the PSP, starts gt2psp.prx from
  host0: (= build/psp/PSP/GAME/GT2PSP, the log arrives live) and prints the race frame rate. pspsh `scrshot` takes
  screenshots. sceKernelExitGame (autoexit setting) left PSPLink unresponsive - do not use with PSPLink.
* Race speed on hardware (attract replay): 11.5 fps at the start. Game code ~60%, GE preparation ~30%, audio mixer
  ~20% (separate thread). Steps: generated code with -Os and without trace 12.0; the 1x pass of frame primitives
  stayed on for the whole session after a stale title frame matched a texture read - reset on display mode change
  13.3; cached vertex arena with writeback before stall-address updates 13.5.

## 2026-11-18 - Correct picture and 22 fps on the PSP-3000

* The coarse/blocky areas on hardware came from GE transform mode (the perspective-correct widescreen pass):
  geometry dropped or distorted on the real GE, exact in PPSSPP. Verified against PCSX-Redux (OpenBIOS + the
  original disc, Lua screenshots every 30 vsyncs: build/ref/shots.lua) - the new reference for the PS1 picture.
  Widescreen, PGXP and hi-res frames are now off by default (settings.txt can enable them).
* Default is native 1x rendering into PS1 VRAM, scaled to the screen: double-buffered in PSP buffers 0/1, and the
  blit is skipped while nothing was written into the displayed area (it ran every vblank with a GE sync: 45% wait).
* Race (attract replay) on hardware: 18.3 fps (1x) -> 20.2 (no 3D-vertex lookups) -> 22.0 (batched ADSR envelope,
  mixer 15-20% -> 8-12% CPU) -> 22.2 (returns flush only v0/v1/sp/ra). Measured without gain: -O2 for the generated
  code (15.8 fps vs 18.3 with -Os: the 16 KB I-cache favours small code), out-of-line scratchpad/IO accessors
  (17.8), no POLL in storing loops (22.2, reverted).
* FPS counter top right (settings fps=0). PSPLink workflow: scripts/psp-run.sh.

## 2026-11-19 - Faster generated code: register flow, unchecked RAM accesses (22.2 -> 27.0 fps on the PSP)

* Profile with function trace (PSP, attract race): game code ~60%, spread over many functions; the generated
  machine code was ~10x the size of the PS1 code (6.4 MB text for 650 KB of MIPS), with the 16 KB I-cache the main
  cost. Every memory access was ~6 instructions plus an inline scratchpad/I/O path and register spills around it.
* Interprocedural register flow (`regflow.py`): calls write back and reload only the registers the callee can
  read/change (hottest call: 11 instead of 29 stores, 5 instead of 18 loads). 22.2 -> 22.9 fps.
* Stack accesses (base sp) without checks: 23.7 fps.
* Scratchpad mapped onto RAM 0x000-0x3FF, memory profile (`-DGT2_MEMPROF`, data/memprof/) selects functions that
  never touch I/O: 826 functions use unchecked accessors, 57 touch I/O. Found on the way: libapi patches the BIOS
  exception handler through C0 table entry 6, which pointed to 0 (writes to RAM 0x28-0x38); two GT2 functions
  write RAM 0x14 through a null pointer (they keep checked accessors, the checked path redirects the low KB to a
  dummy area). Text 3.5 MB, attract race **27.0 fps** (on-screen counter 29), picture unchanged.

## 2026-11-19 - GPU path benchmark, memory system numbers, CLUT cache, 100% save import

* Frame-rate comparisons between builds turned out unreliable: unrelated edits moved the race result by up to
  8% (code/data layout). Tried: hot functions first in .text (own PRX linker script, `scripts/make_linkfile.py`,
  `data/hot_functions.txt`, all code with -ffunction-sections), 64 KB alignment of the big buffers - no measurable
  gain, kept for reproducible layouts. GTE RTPS/RTPT/MVMVA out of line for generated code (`GT2_GTE_OUTLINE`):
  the two biggest renderer functions shrank from 66 KB to 42 KB.
* Deterministic benchmark instead (`settings.txt gebench=<vblank>`, io.c): the first race display list after that
  vblank is replayed 100 times per stage, cut off at successive points of the primitive path. PSP, ~1600 prims:
  OT walk 1.0 ms, packet decode 3.2 ms, clipping 0.5 ms, texture setup 4.2 ms, draw preparation 1.9 ms, command
  emission 1.7 ms - 12.5 ms per frame, about 40% of the 33 ms budget.
* Memory system of the PSP-3000 (`membench=1`): cache hit ~14 ns, miss 130-170 ns, D-cache 16 KB; cached
  sequential stores 24 ns/word (write allocate), **uncached sequential stores 14 ns/word**.
* Texture setup: every frame re-converted ~55 CLUTs (the CLUT arena was reset at every GE sync) with uncached
  single stores. Now a persistent 32x4 CLUT cache keyed by VRAM page versions (cached writes + writeback): 55 -> 3
  conversions per frame. Plus a memo for "same texture as the previous primitive" (keeps the slot's LRU stamp -
  without it the hot page was evicted) and a direct (depth, page) -> slot lookup.
* pspgu writes the display list through an uncached pointer (sceGuStart ORs 0x40000000). A cached list with
  explicit writeback worked but is not faster given the store numbers above; reverted. An uncached vertex arena
  hung the PSP early in boot (or the USB link died at the same time) - to be retried.
* Side quest: a GT2 US Simulation v1.2 save (BASCUS-94455GAME, 93 cars, all licenses, ending seen, 218/248 events
  won; RetroPie save collection on GitHub) checked with `scripts/gt2save.py` (own reader, offsets and CRC-32 range
  from the GT2SaveEditor documentation), maxed (all license tests gold, all events won, 9,999,999 credits).
  New: `IMPORT.MCD` next to the EBOOT replaces the card once; the current card is written to `BACKUP.MCD` first.
  Verified in PPSSPP (My Home shows 632 days, 9,999,999 credits).

## 2026-11-19 - 16:9 research: Silent's viewport patch already squeezes the picture

* Silent's (CookiePLMonster) "Widescreen 2.0" for GT2 (DuckStation cheat, generator `PS1/Tools/gt2-widescreen-gen.py`
  in his Console-Cheat-Codes repo; NTSC-U 1.2 template `resource/gt2-ws/NTSC-U 1.2.cht`) patches the viewport data
  of each screen: car selection, pre-race, race (+-160 -> +-213 at ovl0 0x800100D0/D4, the call to 0x8007b320),
  rear view mirror (+-60 -> +-80), post-race/results/bonus screens, GT mode screens (B3/-32/320 divided by 4/3).
  DuckStation's own widescreen rendering stays off with it.
* PPSSPP, same replay frame with and without the race patch only (`widescreen=1 hires=0`, no GTE hack): the patch
  alone squeezes the whole 16:9 field of view into the 320-pixel framebuffer (anamorphic). The old widescreen path
  (patch + GTE X*3/4) therefore squeezed twice.
* Cost: race frames draw 5% more primitives on average (1343 -> 1413 per frame), up to 20% in places.

## 2026-11-19 - 16:9 done properly (stage A + B of the research)

* GTE X*3/4 removed. All view window constants of Silent's NTSC-U 1.2 template ported as recomp.py OVERRIDEs
  (addresses verified against our modules: race/mirror/post-race/results/bonus in ovl0, car selection + pre-race
  in ovl2, GT mode in ovl4). The cheat's "a1 -> t1" change in 0x80049780 (keeps +196 at the original width but
  slightly resizes other screens) is done cleanly instead: the patched callers pass the original width in
  `g_ws_keepw`, other callers are unaffected (and the bonus screen needs no free-space re-fit).
* Presentation stretches the anamorphic picture to 480x272 - but only in races: pure 2D menus would be stretched.
  Race detection: `walklog=<vblank>` (new debug setting, one summary line per GPU list) showed the race's ordering
  table: 8515 entries (ClearOTagR), four layers each starting with a drawing-area packet (E3/E4/E5), the HUD layer
  always linked from OT[3] = list start - 0x850C, in both buffers and all replay cameras. gpu_walk compares each
  empty OT entry against that address (no per-primitive cost).
* HUD (stage B): primitives of that layer are squeezed by 3/4 towards their anchor (left/centre/right by the
  element's centre, glyph runs share an anchor, elements wider than half the screen stay). Textured ones are
  filtered bilinearly at 1x - with nearest, every fourth texel column of the glyphs vanished ("0:10.477"
  unreadable); filtered they read cleanly.
* Menu screens with 3D views (car selection, post-race, GT mode) keep 4:3 by default; `widescreen_menus=1` turns
  their patches on and presents every screen 16:9 (emulator-with-cheat behaviour).
* PPSSPP: race replay 16:9 with a 4:3-shaped HUD at the screen edges, no missing geometry at the edges, 30 fps.

## 2026-11-19 - Real PSP: slow menu transitions, HOME quit hang, CLUT cache thrashing

The user reported the PSP "hanging" when going into a menu, and HOME -> quit hanging as well.
* **Loading took 15-20 s** (log: exactly 120 rt_poll calls per 2 s, i.e. one per vblank). 1x screens without a
  double buffer (loading screens, title menus) present with `sceDisplayWaitVblankStart`; the emulated vblank runs
  on its own 16683 us clock at the same rate as the PSP's display, so an unlucky phase stays locked and every
  emulated vblank blocked the game for almost a whole frame. CD sectors are handed out per poll, so loading crawled
  at ~60 sectors/s. Fix: after such a wait, the emulated vblank re-phases itself to come 1.2 ms before the next real
  one (`g_vblank_sync_us`, ge.c/core.c), and the wait is skipped when the display is already in vblank. PSP: title
  menus 42000 polls per 2 s (was 120), loading into GT mode ~4.5 s (was ~20 s).
* **HOME quit hang**: quitting now goes through `quit_game()` (main.c): the audio threads leave their loops and
  release their channels (SRC + XA), the game thread parks in rt_poll, the log is closed with a bounded semaphore
  wait, then `sceKernelExitGame`. Verified on the PSP with `autoexit` (both audio threads stopped, PSPLink back).
* **CLUT cache thrashing** (GT mode map 23 ms per frame, 40 GE syncs per frame): GT2 keeps palettes in rows
  480-511, inside the same 64x256 VRAM pages as the frame buffers, so the page version changed with every primitive
  and every CLUT use converted again into a new way until the set was full -> sync. CLUT validity now uses versions
  of 64x32 blocks (`g_blk_ver`), the cache grew to 64 sets x 8 ways. Map: 13 ms per frame, 0 CLUT syncs.
* Race (attract, PSP-3000, no profiler): 16:9 25.0 fps, 4:3 25.7 fps.
* The memory stick still had `PROFILE.TXT` (1 kHz sampling profiler at top priority) - removed.

## 2026-11-19 - GTE fast paths: HI/LO chains, branchless flags, FLAG liveness

Profiling the outlined GTE calls separately (phase markers) showed RTPS 12%, RTPT 6%, MVMVA 7% of a race frame.
Microbenchmark (`settings.txt membench=2`, also a bit-exact self-test of the new cores against the reference code
over 200000 random register sets incl. extreme values - 0 mismatches):

| ns per command | before | sf=1 core | + branchless flags | without FLAG |
|---|---|---|---|---|
| RTPS | 1049 | 983 | 877 | 722 |
| RTPT | 3213 | 2440 | 2177 | 1764 |
| MVMVA | 631 | 597 | 560 | 520 |

* sf = 1 core (`gtef_dot3`): (T << 12 + S) >> 12 == T + (S >> 12); S from one mult/madd/madd chain in HI/LO (gcc
  interleaved the three int64 sums and shuffled HI/LO through registers). The 44-bit MAC overflow is exactly the
  signed overflow of the 32-bit sum.
* Saturations and their flag bits with min/max instead of branches.
* FLAG liveness (src/gt2psp/flagflow.py): RTPS/RTPT/MVMVA whose FLAG is never read get `gte_cmd_nf`. The sound
  whole-program analysis finds almost nothing (dynamic calls make every return live), so it is function-local
  (FLAG assumed dead across calls/returns): 66 of 202 commands. Checked at run time with `-DGT2_FLAG_POISON`
  (FLAG-free commands write an impossible FLAG, all 112 reads check it): 0 hits in attract race and menus.
* Race (attract, 16:9, PSP-3000): 25.0 -> 26.6-26.9 fps.

## 2026-11-19 - GPU path: deterministic frame benchmark, scratchpad state, vertex arena in eDRAM

* `settings.txt gedump=<vblank>` saves the first big display list after that vblank with PS1 RAM, VRAM and drawing
  state to `geframe.bin`; `gebenchfile=1` replays it at startup (100 passes per stage, each from an empty list, GE
  tail measured separately). Identical numbers run to run - the live gebench frame depended on timing, and the
  race fps differ by +-1.5 between builds from code layout alone.
* The CPU is the bottleneck (GE tail after the CPU work ~80 us). Costs behaved like cache misses everywhere: the
  16 KB 2-way data cache is swept by the streams of PS1 packets, vertices and display list, evicting the state read
  for every primitive. Measured on one heavy race frame (1806 primitives):
  * 15.4 ms (old method, list-full syncs inside) / 13.3 ms CPU (new method) at the start;
  * `touch()` with lazy page/block versions (dirty bits, bumped when read): touch 3 ms -> ~0.6 ms;
  * GE state cache `S`, drawing state `g_rs`, page versions/dirty bits/pending boxes, arena pointers and the
    texture memo in the PSP scratchpad (16 KB on-chip at 0x10000, never misses);
  * vertex arena in the free eDRAM tail (0x1CC000-0x1FFFFF, 208 KB, uncached writes, no writeback): 13.3 -> 12.45 ms.
    The display list there too was slower (80 KB left -> more mid-frame syncs), it stays in RAM.
  * OT walk: tight loop over empty entries (~17000 per race frame), about 0.4 ms less per frame.
* Race (attract, 16:9): 26.7 -> 27.4 (scratchpad, walk, lazy versions) -> 27.8 fps (eDRAM arena).
* Not worth it: `varena_uncached` (RAM, uncached), fewer stall-address updates (slower: the GE then starts late).

* Recompiler: lwc2/swc2 use the unchecked base + offset accessors in the same functions as normal loads/stores
  (stack base or memory-profile functions without I/O): 1007 GTE transfers. Race 27.8 -> 28.0-28.1 fps.

## 2026-11-19 - Real PSP play test: save data, screenshots, HUD anchors, display width

* **Save data never worked on the real PSP** (only PPSSPP was tested): every save answered 0x80110388 (bad
  parameter). A variant test on the PSP-3000 showed the cause: pspsdk's SceUtilitySavedataParam with the fields of
  firmware 2.00+ (1536 bytes, key/secureVersion left zero) is rejected; the parameter block without them (size up
  to utilityData) saves and loads. Import of the 100% card verified: save result 0, reload with the same checksum.
* **L+R+SELECT screenshot hung the whole PSP**: the VRAM dump was written straight from eDRAM (0x44000000) and the
  memory stick driver transfers by DMA from the source buffer. All VRAM file I/O now goes through a RAM buffer
  (also gedump/gebenchfile), the BMP is written in one call (row by row it took seconds over PSPLink), snapN takes
  the first free number, VRAM dumps only with snapvram.txt.
* **16:9 HUD anchors**: by the element's centre (40/60% limits) centred texts fell apart ("BRONZE PRIZE!") and the
  pause menu moved right. Now edge anchors only for elements entirely in the outer 30% of the drawing area,
  everything else is squeezed towards the centre.
* **Display width from GP1(06)**: the visible width is taken from the horizontal display range (the right edge of
  the 368-pixel menus showed a stripe of VRAM). To be confirmed on the PSP.
* **Sharper 4:3 screens** (`sharp=1`, default; `sharp=0` = old full-height scaling): 240 lines on screen with
  16-pixel bands, 320 wide. 240-line screens 1:1 vertically (240 -> 272 smeared every text line); GT2's 480-line
  interlaced menus (mode 0x64, 368x480) halved exactly - bilinear sampling of a half-height sprite averages each
  line pair 50/50 (was 480 -> 272). User: "wirkt schon viel schaerfer". Confirmed on the PSP: the display range crop
  removed the VRAM stripe at the right edge of the 368-pixel menus.
* Ideas for later (user): menus truly in 16:9 would need per-screen relayout (fixed 4:3 compositions); texture
  replacement with assets converted on the PC (hash lookup, hi-res 2D path) - biggest gain for the font.
* **Loading took 15-30 s again** (menu -> race): the same phase lock as in the afternoon, in the second vblank wait
  (scanout wait before drawing into the buffer shown before the last swap, 120 x ~15 ms per 2 s; new `waits:` log
  line). That wait now re-phases the emulated vblank too. PSP: race overlay 142 sectors 2385 -> 242 ms, scanout waits
  ~1 ms.
* 16:9 HUD: elements that touch or overlap within a frame form a group with one anchor (32 groups per frame; was
  only the previous glyph on the same line).
* **Black line above the minimap** (16:9): the minimap sits on a 96x96 textured sprite; squeezed HUD textures are
  filtered bilinearly, and the GE samples at the texel edge: every row was blended with the row above, at the top
  edge with the row outside the sprite. Filtered HUD elements now use float texture coordinates shifted down half a
  texel (each screen row hits a texel row centre). Insetting each element by half a texel instead made the small
  glyphs visibly thinner (user). Found with the new frame capture:
  L+R+SELECT also saves the next big display list (geframe.bin), `scripts/geframe.py` lists its primitives by
  position and layer.

## 2026-11-19 night - Frame pacing statistics, GTE profile, constant GTE variants

* New log lines every 120 vblanks: `pacing: lists after 1/2/3/4+ vblanks` (display lists by vblanks since the
  previous one; a race frame that misses 33.3 ms takes 3 vblanks) and `frame work` (time between waiting VSync calls,
  recomp.py hooks on libetc VSync 0x8008c338 - GT2's race loop does not call VSync, the histogram stays empty there).
  `build/pace.sh <log>` summarises the race windows.
* Baseline (attract race, 16:9, PSP-3000): 71.6% of the frames on time (2 vblanks), 27.6% take 3, 0.8% 4+; 26.2 fps.
  In heavy stretches 30 of 43 frames take 3 vblanks (~22 fps).
* Profile with the outlined GTE commands as their own phase (`RT_EXTRA=-DGT2_GTE_PHASE`, now also applies to gte.c):
  **GTE 21-25% of the race time** (~7 ms per frame), game code 30%, GE path ~33%, IRQ/poll 6%, GE sync wait 5%.
  Commands per second: MVMVA ~145k, RTPS ~100k, RTPT ~22k, DPCS ~8k (slow path), SQR ~2k.
* GTE variants for the 15 command words the generated code uses (gte_fast.h `gte_variant`, selected at compile time
  through `__builtin_constant_p`): the outlined MVMVA decoded mx/v/cv/sf/lm at run time. PSP: MVMVA 533 -> 323 ns,
  RTPS 741 -> 692 ns (its time is the HI/LO multiply chains and the UNR division, ~210 ns).
* Race: 3-vblank frames 27.6% -> 14.9%, 27.9 fps (one run each; the replay is deterministic but the timing is not).

## 2026-11-19 night - Race frame work, asynchronous presentation

* GT2's race loop waits in its own function for its vblank counter (0x8007d23c: `while (counter < a0)`, counter
  reset afterwards; found with `RT_EXTRA=-DGT2_SPIN_FIND`, which logs functions looping on rt_poll without calls).
  The frame work hook sits there now. A late frame does not wait for a third vblank, it just takes longer, so the
  vblank histogram understates the problem: **51% of the race frames needed more than 33 ms** of work (most 33-50 ms),
  although only 13% of the display lists came 3 vblanks apart.
* Asynchronous presentation (ge.c, `settings.txt async_present`, default on): the presentation list is kicked
  without waiting for the GE (~2 ms per race frame); the next list goes into a second list buffer and continues in
  a vertex ring; the display swap (and the fps digits) follow once `sceGeListSync(qid, 1)` reports the list done
  (rt_poll and before every sceGuStart, so the queue id cannot be reused). The finish callback of pspgu
  (sceGuSetCallback) never fired on the PSP-3000. Display buffer 2 is unused at 1x, so the vertex ring runs from
  there to the end of eDRAM (480 KB; a race frame needs up to 168 KB).
* PSP race: GE sync waits 97 ms per 2 s -> 0; frames over 33 ms 51.6% -> 45.6%, 28.5 fps.

## 2026-11-20 - Inline MVMVA variants, deterministic race work statistics

* `race work` log line: average frame work and frames over 33.4 ms per 300 race frames, counted from the race overlay
  load (`build/racework.sh <label>` runs the attract race 150 s on the PSP and sums the blocks). Blocks still differ
  by up to 4 ms between runs of the same build (frame alignment / replay timing), the average over ~2700 frames is the
  usable number.
* MVMVA variants inline in the generated code (~60 instructions each after folding; `GT2_GTE_MV_OUTLINE` for the
  outlined ones): race frames 300-2999 average 34.5 -> 33.4 ms.
* GE frame benchmark, finer stages (1428 primitives, CPU us per pass): walk 671, + dispatch (stage 15, new) 1391,
  + decode incl. rects/lines (stage 2 now stops them too) 3066, + clip 3769, + texture setup 6808, + draw state/
  emit 10153, full 10164. `list_uncached=1` (display list through the uncached mirror): no difference. The runtime
  with -Os instead of -O2: 12.6 ms (slower).

## 2026-11-20 - 16:9 race HUD at PSP resolution, code layout is worth 10%

* **Race HUD drawn at 480x272** (ge.c `hud_record`/`hud_draw`, `settings.txt hud_defer`, default on with 16:9 and
  1x presentation): squeezed into the 320-pixel PS1 frame every fourth texel column of the HUD was lost ("She'by",
  "Rep'ay", "Re?ord", no digits on the tachometer). The race HUD layer is now recorded per PS1 frame buffer (float
  x squeezed towards the element's anchor, texture page/CLUT, drawing area) and drawn after the presentation blit
  straight into the display buffer: 1 texel = 1.125 x 1.133 pixels, nearest sampling, nothing lost. Cost ~170
  primitives / 0.6 ms per frame, the same as drawing them into VRAM before.
* **The race frame work depends on the code layout by +-10%**: the same EBOOT gives the same `race work` average to
  +-0.1 ms in repeated runs, but builds with identical logic measured 33.5 / 35.6 / 38.1 ms. The 16 KB 2-way
  instruction cache: the hot runtime block (GE path, GTE, rt_poll) and the hot game functions evict each other
  depending on their relative position. Fixes:
  * `data/hot_functions.txt` updated (GTE variants `gte_rtps_k_nf`... were outside the hot block; game functions by
    the current PSP profile) and library members can be listed (`libpspgu.a:sceGuDrawArray.o`, memcpy, memset).
  * `pad=<bytes>` entries insert a gap; a gap before the game functions, measured: 0 35.6, 1 KB 34.9, **2 KB 33.6**,
    3 KB 37.1, 4 KB 33.7, 5 KB 33.9, 6 KB 35.0, 7 KB 35.2 ms. 2 KB kept.
  * Every change to a function in the hot block shifts the layout: re-measure with `build/racework.sh` (and redo the
    gap search) after such changes. The overlay renderers are 27-36 KB each, so a contiguous hot block that fits the
    cache is impossible at function granularity - profile-guided block layout would be the robust fix.

## 2026-11-20 - Profile-guided optimization

* `scripts/pgo.sh profile` builds an instrumented EBOOT (`-fprofile-generate=host0:/pgo`, libgcov links fine),
  runs the attract race on the PSP through PSPLink and lets `autoexit` call `__gcov_dump()` (main.c quit_game,
  `GT2_PGO_GEN`): 27 .gcda files, copied to `build/pgo-data/` (git-ignored). The instrumented build runs at ~82 ms
  per race frame, fine for counting. `scripts/pgo.sh build` builds with `-fprofile-use` in the same WSL build dir
  (`/root/gt2pgo`: the .gcda names contain the object paths) into `build/gt2psp`.
* With the profile the generated code is built -O2 instead of -Os (cold blocks are optimized for size anyway,
  hot/cold splitting into .text.unlikely). make_linkfile.py also places `.text.hot.<fn>` sections of listed functions.
* Race work, frames 300-2999 of the attract replay: non-PGO 34.3-36 ms; PGO -Os 32.6; **PGO -O2 31.2-33.5** (four runs
  of comparable builds, mean ~32.4); frames over 33.4 ms 1189 -> ~800-950 of 2700.
* Measurement noise: the same EBOOT gave 33.2 and 31.7 ms in two runs (an earlier pair agreed to 0.1 ms) - one run
  per variant cannot separate +-1 ms effects. Load addresses are identical between PSPLink launches (logged at
  startup now), so the spread comes from timing, not from the memory layout. The gap search for the PGO build
  (0 33.6, 1 KB 31.2, 3 KB 33.9, 4 KB 33.7 ms) picked 1 KB, partly within that noise.
* Git Bash rewrites `/root/...` in environment variables for wsl.exe into `C:/Program Files/Git/root/...`:
  `MSYS_NO_PATHCONV=1` and a Windows path for `--cd` (scripts/pgo.sh does both).

## 2026-11-20 - Night summary; Media Engine research

* Attract race on the PSP-3000 (16:9), start of the night -> now: display lists 3 vblanks apart 27.6% -> 3.8%, race
  fps (pacing) 26.2 -> 29.4; frame work over 33.4 ms 51.6% -> ~28% (incl. load spikes); `race work` average
  (frames 300-2999) ~35 -> ~32.4 ms. Steps: GTE constant variants + inline MVMVA, asynchronous presentation, PGO with
  -O2 generated code, hot code layout. GE frame benchmark (1428 prims) unchanged at ~10 ms CPU.
* Menus, GT mode and a license test: 0 primitives through the software fallback (no 15-bit textures / texture
  windows seen), so nothing to move to the GE there. CD data sectors already come every 1.5 ms (~9x the PS1 drive);
  the memory card load at boot is 36 sectors within the first 2 s.
* Media Engine: mcidclan/psp-media-engine-custom-core (MIT, built with psp-gcc 15.2) boots custom ME code through a
  small kernel PRX (kcall: runs a function in kernel mode), copies the ME image to 0xBFC00000 and toggles
  0xBC10004C; shared data through uncached memory (0x40000000 mirror), a hardware mutex at 0xBC100048 (main CPU sets
  bit 0, ME bit 1), main->ME soft interrupt 0xBC100044; the ME makes no syscalls; samples include gu-shared-list (GE
  lists built on the ME). Downloaded to tools/downloads/me-custom-core and reviewed, **not built or run**: building
  and starting third-party kernel code on the PSP unattended was blocked by the session's safety check - needs the
  user's go-ahead. Offloading the GPU->GE path (~10 ms/frame) would also need the stall-address updates on the main
  CPU, the VRAM-access syncs as ME handshakes and all ge.c state in shared memory: several days of work.

## 2026-11-20 - Media Engine test on the PSP-3000 (user approved)

* `scripts/me/build-sample.sh <sample>` builds a sample of tools/downloads/me-custom-core (mcidclan, MIT) in WSL
  without installing into /opt/pspdev. Upstream build bug: `psp-build-exports -s` writes kcall.S itself, the
  `> kcall.S` redirect in its CMakeLists left the file empty (undefined kinit/kcall_2) - patched in the copy.
* The poc sample, started through PSPLink: the kernel PRX (kcall.prx, written next to the module) loaded, the ME
  booted custom code and wrote to shared uncached memory: table id 2 (`ME_CORE_T2_IMG_TABLE`, slim+), ME stack top
  0x803FFFF8 (4 MB local eDRAM), bus clock enable 0x0F read from the ME. The module exited cleanly, PSPLink kept
  answering - no hang, no power cycle needed.
* Next: benchmark from the ME (main RAM reads through the uncached mirror, compute loop speed) to size an offload
  of the GPU->GE path or of GTE work.

## 2026-11-20 - Media Engine benchmark (scripts/me/bench, PSP-3000, 333/166 MHz)

Same kernels on the ME (command loop polling uncached shared words) and on the main CPU, times in us:

| kernel | ME | main CPU |
|---|---|---|
| ALU chain 10M | 227288 | 227908 |
| mult/madd/madd (GTE dot3) 10M | 681483 | 683318 |
| RAM read cached 1 MB x8 | 39337 | 42335 |
| RAM read 16 KB x512 (cache hits) | 21501 | 21598 |
| RAM read uncached 1 MB x8 | 209457 | 301911 |
| RAM write uncached 1 MB x8 | 30971 | 56703 |
| RAM write cached 1 MB x8 | 45128 | 50379 |
| random reads RAM 2 MB, 1M | 169688 | 172882 |
| ME local eDRAM write / read 1 MB x8 | 33306 / 28909 | - |
| ME local eDRAM random 1 MB, 1M | 99134 | - |
| GE eDRAM (VRAM) read 1 MB x8 | 57201 | 157451 |

* The ME is a full second Allegrex at the same 333 MHz: identical compute speed, same cached RAM speed, faster
  uncached RAM and much faster VRAM reads; its local eDRAM halves the cost of random reads (170 -> 99 ns).
* No measurable contention: the main CPU's RAM reads (1 MB x16, 84.6 ms) and ALU loop were unchanged while the ME
  streamed RAM at full speed.
* Coherency: the ME's data cache is separate. Cheap per-frame handoff: main CPU `sceKernelDcacheWritebackAll`
  (16 KB cache), ME invalidates its own data cache before reading PS1 RAM; results through uncached memory.
* Consequence: the GPU->GE path (~10 ms per heavy race frame) can run in parallel with the game code on the ME;
  the frame would take max(game + GTE ~22 ms, GE path ~10 ms) instead of their sum. Constraints for the design:
  the ME makes no syscalls (no sceGeListUpdateStallAddr / sceGuSync / sceGuDrawArray kick: the list is built on
  the ME and enqueued/stalled by the main CPU), DrawSync / GPUSTAT and every CPU VRAM access (LoadImage,
  StoreImage, readback) must wait for the ME, and the ordering-table buffer must not be reused by the game before
  the ME has walked it.

## 2026-11-20 - GPU emulation on the Media Engine: races at a locked 30 fps

* The whole PS1 GPU path (gpu.c, ge.c, raster.c: DMA list walk, GP0/GP1, GE list building, texture/CLUT caches,
  presentation) runs on the Media Engine (runtime/src/me.c, `settings.txt me_gpu`, default on, falls back to the main
  CPU if the ME cannot be started). The main CPU posts GP0/GP1 writes, DMA2 transfers and vblanks into an uncached
  command ring; DMA2 stays busy until the ME has finished, so the game's DrawSync waits like on the PS1. A service
  thread enqueues the GE lists the ME finished (sceGeListEnQueue/Sync) and swaps the display, a vblank thread
  publishes the vcount; logf_/now_us are ME-aware (log ring, published time).
* Found on the way (probes in scripts/me/probe):
  * the ME's address 0x10000 is its own local eDRAM: ge.c's "scratchpad" state works unchanged, copied at start;
  * the hardware timer 0xBC600000 reads 0 on the ME;
  * **the ME cannot reach the GE's eDRAM** (0x04/0x44/0x84/0xA4000000 all read 0, writes are lost; mcidclan's
    samples also draw only into main RAM): vertex ring in main RAM (uncached), and the CPU side of the GPU works on a
    shadow of the PS1 VRAM in RAM - CPU writes are copied into VRAM by GE copies in the list (ge_push), regions the
    GE drew are copied back before the CPU reads them (ge_pull: CLUTs, VRAM->CPU transfers, software primitives);
  * the ME's data cache is not coherent: lists, vertices and CLUTs are written uncached (cached CLUTs stayed in the
    ME's cache - all textured HUD elements were transparent).
* Data isolation: ge.c/gpu.c/raster.c/libpspgu data and bss (incl. pspgu's .sbss/.sdata) in their own 64-byte aligned
  blocks (linkfile.prx.in); main-CPU-written statistics and vblank code moved to gpuvbl.c, gpu_walk into gpu.c,
  g_phase in its own cache line, g_vblank_sync_us into core.c.
* PSP-3000, attract race 16:9: display lists 2 vblanks apart **100%** (was 71.6% at the start of the night), 30.0 fps,
  ~100-140k primitives per 2 s; menus, GT mode and machine test work; HUD correct.
* 16:9 HUD: lines starting in the left 10% (or ending in the right 10%) of the drawing area are anchored to that edge
  ("Replay" and the car name sit at the left edge as in the 4:3 original), and HUD groups form only within one kind
  (textured sprites / lines and polygons). The minimap now keeps its 4:3 distance to the "Replay" text (user's choice).

## 2026-11-20 - Headroom with the GPU path on the Media Engine

New log lines: `idle:` (time the game spends in its frame wait 0x8007d23c / waiting VSync per 2 s - the real
headroom), `me: N walks avg/max` (ordering-table walk, post -> completion seen by the main CPU) and the game's waits
for the ME (DrawSync polling DMA2 while busy). The `race work` frame histogram merges two frames whenever the game
calls its wait with a count <= 0 (about every 4th frame, maxima ~65 ms), so it overstates; the idle time does not.

Attract race 16:9, PSP-3000 (pacing 100% at 2 vblanks, 30.0 fps):
* main CPU idle: average 25% of the time (~8 ms per frame), worst 2-s window 7.7% (~2.5 ms per frame);
* ME walk: average 15.5 ms, worst 33 ms (incl. waiting behind the previous presentation in the ring) - the ME is
  the tighter side in heavy stretches; its path is slower than on the main CPU (~10 ms) because lists, vertices,
  CLUTs and the VRAM shadow are written uncached to main RAM;
* the game waited for the ME 0-2 times per 2 s, for < 1 ms.

## 2026-11-20 - 60 fps experiment again (settings.txt fps60=1, off by default)

Silent's 60 FPS cheat restored as an option (frame interval 0x801D5864 2 -> 1 at every vblank, the three
"interval < 2" effect switches in overlay 0 overridden so tire smoke and the mirror stay on). With the GPU path on
the Media Engine:
* **Machine test (one car): a steady 60 fps** - 100% of the display lists one vblank apart after the start, game
  speed 100%. Both CPUs are at their limit: the main CPU idles ~0 ms per 2 s, the ME needs ~16.5 ms per frame and the
  game waits ~3 ms per frame for it (DrawSync).
* Attract race (6 cars): 80% one vblank, 20% two -> 50 fps, i.e. the simulation runs at 84% speed (slow motion); the
  main CPU is the limit (idle ~0, ME 14 ms). The attract replay also desyncs at 60 fps (the leading car stood at the
  start with 0 mph after 1:27) - replays are recorded per 30 fps frame.
* build/pace.sh now reports 1-vblank frames too (it ignored them).

## 2026-11-20 - Towards 60 fps: profiling with the ME active

* The profiler's phase word moved to the scratchpad end (0x13FFC): the ME runs the same code and its writes to the
  global g_phase garbled the main CPU's profile; at that address each CPU has its own word (ME: local eDRAM).
* `-DGT2_POLL_FN` (generated code + prof.c): the profiler attributes game-code samples to the function whose loop
  polled last - the TRACE ring only names the last function entered, which put a wait loop after an event callback
  (0x8007eaac, ~10%) on the callback.
* Main CPU profile, attract race 16:9 with the ME: GTE ~26% of the time (per frame ~4560 RTPS, ~1000 RTPT, ~360 DPCS
  on the slow path), interrupts/polling ~9%, waits ~22% (frame wait 0x8007d23c 7%, the loop after 0x8007eaac 10%,
  BIOS B0 calls 5%), the rest game code.
* PGO profiling hangs in __gcov_dump once the ME has run (the PSP then needed a power cycle): pgo.sh profiles with
  me_gpu=0 now (same game code; the GPU code profiled on the main CPU is the code the ME runs). quit_game is guarded
  against a second request and stops the ME first (me_stop).
* DPCS fast path (gte_fast.h gte_dpcs_fast, outlined gte_dpcs_o): ~360 per race frame went through the slow command
  switch. Bit-exact against gte_cmd_impl in the startup selftest (membench=2; sf/lm variants added), checked in
  PPSSPP while the PSP was unreachable: 0 mismatches in 200000 random register sets. Speed still to be measured.

## 2026-11-20 - 60 fps: where the main CPU goes, CD read-ahead, fixed data/code placement

* fps60 attract race with the ME (`-DGT2_POLL_FN` build): ~52 fps, 87% of the frames one vblank apart. The ME never
  makes the game wait (walk avg 12.2 ms) - the main CPU is the limit. The wait loop after 0x8007eaac seen at 30 fps
  is gone at 60 fps (no headroom left to wait in). Game code 90% (the heaviest: the overlay 0 mesh renderers
  0x800234f8 19%, 0x8002106c 10%, 0x80018d1c 6% - RTPS/MVMVA/NCLIP/DPCS per vertex and polygon), poll/IRQ 8%.
* rt_poll timing (GT2_POLL_FN): per 2 s cd 40-50 ms, IRQ handlers ~95 ms, ME service ~5 ms, vblank ~5 ms. The CD
  part was XA music streaming: one synchronous 128 KB pack read every ~0.4 s (~15 ms each). Now a prefetch thread
  reads ahead into a second buffer (cdrom.c): cd time from the memory stick 67 -> 32 ms per 2 s, no stalls left
  (`cd: 0 synchronous pack reads`). `pakpath.txt` lets PSPLink runs read the pack from ms0: (USB is not
  representative).
* Pitfall found on the way: fps60 pacing of the attract demo is not a benchmark - the demo desyncs at 60 fps, and
  every build drove a different race (52 vs 47 fps for builds that are equally fast at 30 fps, where the demo is
  deterministic: idle 661 vs 672 ms per 2 s).
* Layout: .rodata/.data now start at fixed 16 KB offsets and the generated code follows the hot block at a fixed
  offset (linkfile.prx.in, make_linkfile.py), so runtime edits no longer move game code and tables.

## 2026-11-20 - Quality / performance mode, clock overlay

* Overlay: the fps counter has a second line with the CPU clock, measured against the 1 MHz system timer (ARK's
  overclock above 333 MHz programs the PLL directly, scePowerGetCpuClockFrequencyInt keeps saying 333). The first
  version measured ~78 MHz: gcc had unrolled the nop loop into code that did not fit the I-cache; now all asm.
* User test on the PSP with ARK `cpuclock:403`: the clock is not steady at 403 and the frame rate barely changes -
  more CPU clock is not the way to 60 fps (the ME/bus do not scale with it, see the ME waits in a real race:
  walk ~20 ms per frame, the game waited ~0.4 s per 2 s).
* Performance mode (`performance=1` = `fps60=1`, `fps60_effects=0`, `lowlod=1`; default is the quality mode):
  * fps60_effects=0 keeps the game's own 60 fps trade-off (no rear view mirror - a second render of the scene -
    and no tire smoke).
  * lowlod=1: the other cars always at the lowest LOD. Found through Silent's "Full detail AI cars" cheat
    (tools/downloads/gt2-lod-cheat): the per-car LOD mode s2 in overlay 0 at 0x8001435C is 1 for the followed car,
    0 for the others (distance LOD), 3 = lowest; recomp.py OVERRIDE gives the others 3.

## 2026-11-20 - Dynamic 60 fps (no slow motion), real race analysis, settings menu, controls

* Settings menu (menu.c): L+R+SELECT pauses the game and opens it (pspdebug font drawn into the picture on screen,
  SPU muted); mode, 60 fps, mirror/smoke, opponent LOD, fps counter, L/R pair, 16:9 (after restart). Closing writes
  settings.txt.
* Controls: in races d-pad up/down also press R2/L2 (upshift/downshift, `dpad_shift`); `lr_shift` swaps the L/R pairs.
* 16:9 HUD: the tachometer needle (a line) moved to the screen edge when it pointed right - lines and textured
  elements now join a group when one lies inside the other.
* User race in performance mode (PSPLink log): main CPU fine (every frame < 20 ms, ~37% idle), the **Media Engine is
  the limit**: ~18 ms per frame (of that ~3 ms waiting for the GE), the game waits ~1 s per 2 s for it. With 3+ cars
  in view the frame rate fell to ~40 fps. ARK overclocking (measured ~389 MHz) only speeds up the main CPU.
* **fps60_dynamic** (default with fps60): the game simulates `interval` vblanks of time per frame (global byte
  0x801D5864); its flip (0x800809b0) waits on a copy in a stack struct. Hooks before/after the wait: the copy becomes 1
  (no frame is stretched), the global byte the vblanks the frame really took (1/2, more carried over). Checked with
  the lap clock in the attract race: fixed step 24.30 s game time in 30.15 s (80.6%, slow motion), dynamic 30.11 s in
  30.15 s (99.9%). A first try that patched struct+32 (copied from the interval) had no effect. The three
  "interval < 2" effect switches are pinned (mirror/smoke stay off or on as chosen), so they do not flicker.

## 2026-11-20 - Media Engine profiling

* The ME has no running timer and its COP0 Count does not advance either. ME profiles: `-DGT2_ME_PROF` (the ME
  writes its phase to an uncached word, the main CPU's profiler thread samples it) and `-DGT2_ME_FPROF` with
  `RT_EXTRA=-finstrument-functions` (function level: enter/exit hooks keep a call stack in the ME's local memory;
  `build/mesyms.sh` symbolizes). New PHASE_SCOPE markers on the GPU path (only active in these builds).
* Attract race, performance mode: ME busy ~58% (per frame ~12.7 ms): walk/packets 13%, primitive decode 13%,
  draw emit 15%, touch 6%, texture setup 5%; the function profile is flat (ge_polygon, submit, draw, touch, passes,
  clip_box, ...). The GE itself needs only ~6 ms per frame (service thread: enqueue -> sync).
* Tried: batching consecutive primitives with the same state into one draw call (37% of the draws merged): no GE
  gain (GE not the limit), the ME got no faster (quads as triangle lists = more vertex data). Removed.
* CPU->VRAM transfers: per word through gpu_gp0 and a division per pixel (vram_put) -> whole packets with column/row
  counters (xfer_put), also for the ME's block DMA. ~6% of the ME time in the function profile.
* In the attract demo at 60 fps the main CPU is the limit again (frame work 20-25 ms, partly the extra simulation
  ticks of fps60_dynamic: 72% one-vblank frames with the fixed step, 59% with the dynamic one); in the user's own
  race it was the ME. ME run-to-run noise is +-1.5 ms per frame.
* Track LOD (`track_lod`, performance mode 2): the scenery renderer uses a segment's full model below distance class 3
  and the simple one above, replays always the full one (Silent's draw distance cheat). The limit is a setting now,
  in replays too. Attract replay at 30 fps: primitives -3.5%, ME ~-0.9 ms (within the noise); track_lod=1 gives no
  more. Visually unchanged.

## 2026-11-20 - Stable frame rates: automatic 30/60 lock

* User race in performance mode with the release build: here the **main CPU** was the limit (frame work 20-30 ms,
  ~3900 polygons per frame vs ~2400 in the attract demo). Race profile (GT2_POLL_FN): almost all of it is the game's
  3D renderers (car meshes 0x800234f8 ~19%, other mesh/scenery renderers 5-7% each), i.e. GTE emulation per vertex;
  physics is not significant.
* GTE micro-benchmark (membench=2, ~389 MHz): RTPS ~760-980 ns, RTPT ~2-2.5 us, MVMVA 350-600 ns, the UNR divide
  ~230 ns. Tried the projection divide on the FPU (float): slower (311 vs 253 ns in the benchmark) - reverted.
* **fps60_auto** (default on): instead of alternating 1 and 2 vblanks (judder) the game is locked to 30 fps when it
  misses 60 in 3 of the last 32 frames, and goes back to 60 once every frame of a 2 s window was ready within 15.5 ms
  (work measured from the end of one flip wait to the start of the next, incl. the wait for the ME). The game time
  step follows real time in both (lap clock 30.108 s in 30.14 s). Attract demo (heavy): locks to a steady 30.
  Settings menu: "60 fps lock: auto / always 60".
* Media Engine: submit wait measured (time the ME waits for the service thread to take the next list).


## 2026-11-20 - Hitches every 2 s (log writing), race buttons, faster loads

* User report: short "loading" hitches in both modes, even alone in front. The memory stick log showed exactly one
  frame over 50 ms (max 53-65 ms) in every 2 s window, in menus too: the statistics block (dozens of lines, more
  since the ME profiling work) was written in the game thread with an fflush per line and a close/reopen per
  second. The log now goes through a 64 KB ring buffer; a low-priority thread ("logw", priority 0x48) appends it
  about once a second (and on the fatal/quit path synchronously). Same scenes afterwards: max 5-14 ms.
* CD sector spacing 1.5 ms -> 0.75 ms (tested working on 2026-11-18): loads read about twice as fast
  (280 sectors 432 -> 224 ms). A 35-sector read that took 1 s before is gone too.
* Race buttons (`race_buttons=1`, menu entry): the game's setup on the port is circle gas, triangle brake, X
  handbrake, square reverse; in races the face buttons are turned to X gas, square brake, circle handbrake,
  triangle reverse. Only while a race is drawn (ge_in_race), so menus keep X = confirm; the in-race pause menu
  confirms with circle then.
* Load phases are logged now (`load: N ms, sectors, memory stick reads`, `load commands: ...`, `card access: ...`).
  Entering GT mode (701 sectors) took 2.0 s, of which memory stick reads 0.2 s and sector spacing 0.5 s; GT2
  pauses after every read (29 Pause commands per load) and the emulated drive answered each after 30 ms (SeekL
  20 ms). Pause/SeekL now answer after 2 ms: GT mode 1.1 s, boot load 0.97 -> 0.69 s. Sector spacing below
  0.75 ms still hangs the game (0.2 and 0.4 ms tested on the PSP), so 0.75 ms stays (`cd_period_us` for tests).


## 2026-11-21 - Broken race HUD text on the ME path, 60 -> 30 fps switch, log writer

* User report: race HUD text "pixelated", performance mode at double speed, no way back from 60 to 30 fps.
* HUD: the glyphs were wrong (the clock showed "2:22.222" instead of "0:00.000"), not a scaling problem. Same
  build with `me_gpu=0`: clean. The ME received CPU->VRAM block DMAs (LoadImage) as a RAM address and read the
  words when it got to the command; GT2 builds each HUD string's glyphs in one buffer and reuses it at once for the
  next string, so a lagging ME uploaded the next string's pixels. Timing-dependent (how far the ME is behind), so
  it showed in the 16:19 screenshots of yesterday's build too, only at other moments. me.c now copies the words
  into a 256 KB snapshot ring at DMA time (CMD_BLOCK_SNAP), as the PS1 DMA reads them then; when the ring wraps,
  me_join first. Checked on the PSP: six race screenshots over 10 s, all clean, in quality and performance mode.
  The CD timing changes of 2026-11-20 were suspected first and ruled out (all combinations tested,
  `cd_pause_us` / `cd_seek_us` settings added for that); the screenshots only differed because faster loads
  start the race earlier.
* 60 -> 30 fps (settings menu, or performance -> quality): fps60_dynamic sets the display struct's wait byte to 1
  and the game's step byte (0x801D5864) per frame. Switching off only restored the step byte to 2, the wait byte
  stayed 1: 60 frames per second with 30 fps time steps = double speed, and 30 fps never came back. The managed
  wait byte is remembered (g_dyn_wait) and set back to 2 when fps60 goes off. Test: `fps60_flip1..4=<vblank>`
  toggles fps60 like the menu; attract race 30 -> 60 -> 30 -> 60 -> 30 switches cleanly (pacing 0/60 at 30 fps).
* Log writer: at 60 fps the game thread never sleeps and the writer (priority 0x48) starved - the log stopped
  ~10 s into races. Now 0x1F (above the game thread; it sleeps on its semaphore and writes once a second). On host0
  (PSPLink) the file is no longer closed/reopened per drain (raced with pspsh screenshots).
* `cd_period_us` was read as 0/1 (missing from the numeric settings list) - fixed.


## 2026-11-21 - Faster loads: CD command latency, sectors without a clock, memory card fast path, ME snapshot fix

* Where a load's time goes (GT mode entry, command timeline): ~52 small reads of 6 commands each (Setloc, Setmode,
  ReadN, Getstat x2, GetlocL, Pause), ~3 ms per command - the per-command delays, not the sectors. ACK 600 ->
  100 us: 1043 -> 686 ms; Pause/Seek 2 -> 0.3 ms, first sector 1 -> 0.2 ms: 536 ms (PPSSPP). Below 100 us no gain.
* Sector spacing: below 0.75 ms the game hung or read runaway (55605 sectors). Cause: libcd's interrupt handler
  loops while CD interrupts are pending; a sector that arrives while it runs keeps it inside, and the Pause after
  the last sector comes from the game's main code, which never runs. Data sectors are now held back while the game
  is in an interrupt handler (rt_in_irq) and otherwise come at once (cd_period_us 0). Boot load 527 -> 136 ms.
  (The 2026-11-20 test "0.2/0.4 ms hang" was not one: cd_period_us was parsed as 0/1, i.e. 1 us.)
* PSP freeze found on the way: the ME upload snapshot ring (2026-11-21 morning) called me_join() when it wrapped -
  also inside the CD interrupt handler, where waiting for the whole ME queue could hang (in_irq 1, almost no
  polls; seen with fast loads). The ring is now 1 MB in two halves; entering a half waits only for the commands
  that read it (long done). 3 runs at 0 ms spacing with the ME: all reach the race.
* Memory card: GT2 reads one 128-byte sector per frame from its card task (0x8007EACC), 254 sectors = 4.2 s when
  GT mode loads the save. The task now runs again right after each read result (bios.c, interrupt context like
  its vblank call; EXE check on the code bytes): 105 ms, identical save state (632 days, 9,999,999 Cr, same
  garage). Card result delay 2 -> 0.3 ms (card_event_us). Writes: unchanged (one per frame) until saving is
  verified - in PPSSPP no save prompt was reached by script (leaving GT mode does not ask); `card_fast=2`.
* Open: PSP verification of this build. One run of eight on the PSP (0.75 ms spacing, gating, new snapshot ring)
  froze the whole PSP during boot, PSPLink included - cause unknown. The final defaults are verified only in
  PPSSPP (me_gpu=0): boot, attract race, GT mode with the save loaded.


## 2026-11-21 - Arcade Mode disc (work in progress)

* Recompiler: disc profiles (src/gt2psp/profiles.py; `recomp --disc arc --files extracted-arcade/files --out
  generated/arcade`). The Arcade EXE (SCUS_944.55) is the same code ~0x90-0xF0 lower; its constants were derived with
  code signatures (sigmap.py: jump targets and address immediates masked). Arcade symbols carry the prefix "a"
  (af_/ab_/ao<N>_, a_g_modules). The Simulation output stays bit-identical (checked after every change).
* Arcade-only recompiler fixes (kept off for the tested Simulation code): a data table in ovl2 taken as code
  (text_exclude); constants passed to "load overlay a0 and longjmp to a1" (0x8005D9EC) belong to the loaded overlay,
  not the caller (ovl1 passed ovl0's 0x80011f64, which in ovl1 split a switch); resume point after the jalr at ovl2
  0x80014228; switch cases split off their jr become function starts. Making every jalr return a resume point hung
  the game (splits inside loops become tail-call chains).
* Runtime: one EBOOT with both discs. Start menu (main.c) when GT2.PAK and GT2A.PAK are present; settings.txt
  `game=sim|arc` skips it. Per game: module tables, entry, overlay loader wrapper, pack, card task (Arcade
  0x8007E9DC). Arcade runs without the Simulation game patches (16:9, 60 fps, LOD forced off; the settings menu
  does not save there). Pack tool: Form 1 runs shorter than 8 sectors (STR movie streams) stored raw: 9 runs
  instead of 163592.
* 24-bit display mode (GP1(08) bit 4) for the Arcade disc's movies: presentation converts the RGB888 display area
  into a 15-bit texture in RAM (ge.c). MDEC decoding (untested so far) works: intro movie correct.
* PPSSPP: intro movie, title, "Game Selection" menu and attract race run. Entering a race: `resume at 801ff9f0`
  after a longjmp (ovl2 menu code, never run on the Simulation disc) - open.
* XMB icon/background from the user's Images/ (scripts/make_eboot_images.py -> build/ICON0.PNG, PIC1.PNG).
* PSP: not yet tested with the dual build (PSPLink stopped answering).
* Arcade race entry, step by step with data (stack and register snapshots at setjmp/longjmp): the Arcade menus run
  as tasks switched with setjmp/longjmp (scheduler 0x80083a1c, task creation 0x80083ae4, own stacks). After a switch
  the runtime follows the return addresses on the PS1 stack, which exposed three recompiler assumptions:
  1. regflow.py: storing ra / s-registers to the stack "does not read them" - a caller left a stale ra in c->r[]
     (the stack held the outer caller's ra, the resumed chain skipped a function). Callers of functions that may
     switch tasks now write ra and s0-s7/fp back first.
  2. A plain return does not hand back callee-saved registers - wrong for a function resumed mid-way (its entry
     state came from the setjmp buffer). Resume points return them.
  3. Return addresses after calls through function pointers are not known resume points, and splitting functions
     there breaks loops (the scheduler loop). New: in-place resume - a function with resume points gets an entry
     switch on c->resume_pc (recomp.py RESUME_INSIDE, table g_resume_inside, core.c run_program). Arcade: every
     jalr return and every call of a function that may switch tasks (transitively: setjmp or a jalr), 1591 points.
  All Arcade-only (profiles.py flags); the Simulation output stays bit-identical (its overlay-loader longjmp goes
  to the top level, where none of this matters).
* PSP freezes: the whole PSP (PSPLink included) froze several times today - at the ME start, during boot, and in
  the Arcade intro movie (the ME stopped walking display lists, the game waited for DMA2). Points to the Media
  Engine path; to be investigated before a new build goes onto the user's memory stick.
* PPSSPP gives homebrew less memory than the PSP-3000 (stack 0x09FBFDF0 vs 0x0BEBFE10): with the larger Arcade code
  the EXE no longer loads there.
* Two programs instead of one (user's suggestion): runtime/CMakeLists.txt GT2_GAME=sim|arc builds one runtime per
  disc (GT2SIM.PRX / GT2ARC.PRX); EBOOT.PBP is a small launcher (runtime/launcher) with the start menu (last choice
  preselected, lastgame.txt) and the XMB images, which loads and starts the chosen PRX (like PSPLink does). The
  Simulation binary stays exactly the tested one (code layout moves performance by +-10%), no Arcade code in it.
  scripts/build-launcher.sh; `GT2_GAME=arc scripts/build.sh`.
* With the task-switch fixes the Arcade disc runs into a race in PPSSPP: menus, course selection (Tahiti Road),
  race start (overlays 3 and 0).
* Simulation patches ported to the Arcade disc: all 45 sites (6 runtime hooks: frame timing and the frame flip for
  60 fps; 8 near-plane clipper loops for 16:9; 31 instruction patches: 16:9 view windows, 60 fps effects, car and
  track LOD) found with sigmap.py (unique signature, clipper loops at -0x74 with whole loop windows identical),
  every original instruction word bit-identical on both discs; table in profiles.py (patch_map). The race frame
  interval byte moves with the race display struct: 0x801D5864 -> 0x801D52C4 (all 5 struct references -0x5A0).
  Arcade no longer forces 16:9 / 60 fps / LOD off; the settings menu saves there too.
* 30/60 fps is chosen in the launcher (Left/Right, written to settings.txt for both discs); the in-game settings menu
  only shows it. Switching 30 -> 60 fps during a race ran the game at double speed (the game sets up its timing
  from the frame interval when a race starts); 60 -> 30 was fixed earlier today.
* Arcade "slow motion" report, checked against PCSX-Redux (build/ref/arcshots.lua: START at vsync 1500 skips the
  intro, screenshots of the attract replay): the replay's camera cuts come at the same race times in the port
  (REPLAY 0:01.1, wall cam 0:06.1, front 0:10.7, side 0:12.5; port +6/+10/+12 s after REPLAY) - game logic speed
  correct at 30 fps in PPSSPP. Arcade race clock at forced 60 fps: 9.982 s in 10.087 s (99%).
  Diagnostics: settings fstep_log=1 (frame step state), snap_at1=<vblank>/snap_at2=<interval> (16 snapshots with
  real time in the log).
* Reference timeline of the Arcade disc boot (Redux): SCEA presents ~vsync 300, legal notice ~480-1080, Polyphony
  logo ~1380, then the long intro movie (still running at vsync 10000).
* Arcade movies (user: 5-6 fps): MDEC decode measured (mdec stats line): 3840 macroblocks per 2 s, 0.33 ms each =
  ~6.4 movie frames per second. IDCT now skips zero coefficients (pass 1 only for columns with one, pass 2 only over
  the rows pass 1 produced; scale/8 precomputed, exact truncating /0x2000) - verified bit-identical against the plain
  psx-spx loops (20000 random blocks incl. dense ones); chroma terms once per chroma sample. 0.14 ms per macroblock,
  ~10 frames/s in PPSSPP, now CPU-bound elsewhere (idle 60-80 ms of 2 s): profile on the PSP next.
* In-game menu cleaned up (user's list): heading with the mode, then mirror + smoke (both modes now: the three
  "interval < 2" tests follow fps60_effects), opponent car detail, FPS counter, L/R, race buttons. Performance mode
  defaults to "always 60" (fps60_auto 0); mirror/smoke and opponent detail default by mode, the menu's saved choice
  wins (the launcher resets them when the mode changes).
* Arcade movie on the PSP, step by step (profile: the game waited 766 ms per 2 s for the Media Engine):
  24-bit conversion only when an upload reached the displayed area (was every vblank), read as words (3 per 4
  pixels); a CPU -> VRAM upload syncs the GE only when it overlaps a shadow -> eDRAM copy still queued (was always:
  322 syncs per 2 s -> 5); word-wise upload copies; a snapshotted block DMA is complete at once (the data is copied,
  as on the PS1 the DMA reads it then) - ME waits 766 -> 0 ms. Movie 5-6 -> ~9.6 frames/s; now the main CPU limits
  (MDEC decode ~1 s per 2 s, the game's VLC decoding). Simulation checked on the PSP (race HUD, clock, GT mode).
* Log: the game thread sleeps 0.5 ms every 2 s so the writer (priority 0x48) gets to run in busy scenes.

## 2026-11-21 (evening) - release preparation

* Release decision (docs/RELEASE-PLAN.md): no game code or data in the release. The converter recompiles and
  compiles on the user's machine from their own discs; Windows gets our own pspdev build (no WSL, no Docker).
* Converter web app started: `convert.bat` / `convert.command` / `convert.sh` -> `converter/server.py` (Python
  standard library only) + `converter/web/index.html` (GT-style UI). Finds .cue/.bin in `input/`, verifies the
  .bin by SHA-1 (Simulation Rev 2 c4813703..., Arcade a617b948...), runs extract / recomp / pack per disc, compile
  step, writes `output/PSP/GAME/GT2PSP`. Missing: toolchain download and the bash-free build driver (plan steps 1-3).
* Media Engine library vendored as `third_party/me-custom-core` (MIT); build scripts take `GT2_WORK` (scratch dir,
  default /root) and `PSPDEV`, so they also run outside the dev WSL distro.

## 2026-11-26 - Windows toolchain and bash-free build (release plan steps 1-3)

* `scripts/build_psp.py`: the CMake builds as plain compiler calls from Python (parallel, incremental). Simulation PRX
  and launcher are byte-identical to the CMake output. Pitfall: `runtime/ld/linkfile.prx` places
  `*generated/*.c.obj` as one block, so the object files must keep CMake's names (`generated/<file>.c.obj`) and be
  passed relative with "/" (Windows paths with "\" do not match the pattern).
* `scripts/wintc/build.sh`: PSP toolchain for Windows, cross-built with mingw-w64 in WSL from the same pspdev commits
  as our Linux toolchain (binutils 2.44 Allegrex, gcc 15.2.0 Allegrex, pspsdk tools); the PSP-side files (newlib,
  libgcc, pspsdk libs, headers) are copied from the Linux toolchain. Native .exe files, only Windows system DLLs.
  Two traps on the way to identical output: (1) pspdev's libraries (libpthread etc.) contain LTO objects - without
  the LTO plugin ld silently links their fallback code (launcher +864 bytes); the plugin DLL is only built without
  `-static` in LDFLAGS (libtool builds no shared library otherwise). (2) The LTO data is zstd-compressed: lto1 needs
  zstd (`compiler does not support ZSTD LTO compression`) - static libzstd 1.5.6 built for the host.
* Converter: `converter/toolchain.py` + `toolchains.json` (pinned packages per platform, SHA-256 checked, unpacked
  into tools/pspdev-<platform>/); compile step = build_psp.py. Release launcher icon: own design
  (`scripts/make_release_icon.py` -> converter/ICON0.PNG).
* Linux test in a fresh Ubuntu 24.04 WSL distro (tools/wsl-test, "GT2Test": only python3/git/curl): repository
  cloned, both discs converted through the converter API. The official pspdev tarball failed (`libmpc.so.3`
  missing) - so Linux x86_64 gets its own toolchain too (`TC_HOST=linux scripts/wintc/build.sh`: only glibc needed,
  glibc >= 2.38). With it: both discs in 262 s, GT2SIM.PRX / GT2ARC.PRX / GT2.PAK byte-identical to the dev builds.
  converter/toolchain.py falls back to the official package on older glibc and names the missing packages.
* Toolchains published as prereleases toolchain-win-v1 / toolchain-linux-v1 (private repo for now; the URLs work
  once it is public). The zips carry licence texts and source commits (GPL).
* PGO vs plain -O2 on the PSP (attract race): 26.9 vs 27.9 ms average, 454 vs 380 frames over budget - the release
  build stays without PGO.
* README rewritten for the release; Reddit draft in docs/release/reddit-post.md.

## 2026-11-26 - SPU reverb

* GT2 enables the reverb (SPUCNT bit 7 set, `c081`). Implemented after psx-spx "SPU Reverb Formula" in spu.c: the
  mixer already runs at 22.05 kHz, the reverb's own rate, so the hardware's 39-tap resampling filters are skipped
  (one reverb step per output sample). Voices with their EON bit feed it; CD audio does not (own PSP channel).
* Cost on the PSP-3000 (attract race, `build/racework.sh`): reverb alone ~3.0% of the main CPU, constant (also in
  menus). Race work 27.98 ms / 389 frames over 33.4 ms without, 29.53 ms / 478 and 32.57 ms / 719 with (two runs).
  The mixer as a whole is ~11-12% CPU in races without reverb.
* Optimised: reverb at 11.025 kHz in a private work buffer at the same time scale (one entry per 4 bytes of sound RAM
  address - delays unchanged in seconds; input averaged per output pair, output interpolated back), inner loop cut
  into segments in which no tap wraps (no per-access wrap check). Voice loop: the pitch counter steps over several
  source samples at once inside an ADPCM block (engines: up to 8 per output sample), ADPCM position kept in a local
  (with -fno-strict-aliasing every mix-buffer store reloaded v->pos).
  Result (race, PSP-3000): reverb 3.1% -> 1.35%, voice loop 11.0% -> 10.1%; race work 28.12 ms (reverb on) vs 27.83 /
  27.93 ms (off), frames over budget 376/378 vs 381/393 - the reverb's real cost is in the noise.
* The mixer's own "% CPU" overstates its cost: it includes time in which higher-priority threads preempt it. With
  `audio=0` race work is 26.24 ms, so all audio together really costs ~1.6-1.9 ms per frame. Counters (log line
  "spu: voice loop"): ~400k voice samples/s, ~13k ADPCM blocks/s of which ~half come from the decode cache - the
  block decoder is ~1%, not the bottleneck.
* Default: reverb on in quality mode, off in performance mode (`g_reverb = -1`); `reverb=0/1` forces it.
  One PSPLink run hung after the memory card load (game thread stopped, audio thread alive); two repeats of the same
  settings ran normally. Reverb still not checked by ear on hardware.

## 2026-11-26 (evening) - looping display lists, release clean-up

* Screenshot session found a GT-mode glitch (car info page: screen full of green blocks, game at ~1 fps). Cause:
  GT2 sometimes rebuilds primitives right after DrawOTag without DrawSync - harmless on the PS1, whose DMA has read
  the list by then, but the Media Engine walks the list later and can find it half rebuilt: a packet linked to
  itself (every boot, vblank ~565, packet 1b15d0, drawn 253625 times = over 1 s) or a cycle through the ordering
  table (now and then in the garage, vblank ~6685). A PS1 would hang on such a list, so the game never means it:
  gpu_walk stops at a self-linked packet and after 32768 packets (a heavy frame has ~3700). Counter in the `ot:`
  log line ("looping lists cut short"). Screenshots were not the trigger (also without them).
  Diagnostics kept: `me: walk pending` (a walk older than 0.5 s, where the ME is), `dma2:`/`dma6:` lines when the
  game starts a list or clears the walked table while the ME still walks.
* Own mistake on the way: a diagnostic line inserted into gpu_walk's if/else chain sent every packet twice (icon
  and glyph fragments all over the menus) - found by bisecting builds (pre-reverb c512219 and 672862c clean).
* Rare boot hang (2 of ~20 PSPLink runs): stops inside meLibDefaultInit (no "me: table" line). Suspect: PSPLink's
  reset ends the game without stopping the ME, the next start finds it still running. Not seen from the XMB yet.
* `me_gpu=0` hangs on hardware in the intro (vblank ~600) - the main-CPU GPU path is not maintained any more.
* psp-run.sh deletes the previous log first (a run that did not start used to look like a fresh result).
* Clean-up for the release: developer tools moved to scripts/dev/, toolchain smoke test (runtime/hello) and
  build-exp.sh removed.

## 2026-11-27 - first user reports after the release

* Missing `input/` folder: the README says to put the .cue/.bin files there before starting the converter, but git
  does not keep empty folders, so the download had none (the server only created it on start). Now `input/README.txt`
  is tracked (`.gitignore`: `input/*` + `!input/README.txt`) and convert.bat/.sh/.command create `input/` first.
* Windows build stalled on a modal "bad image" dialog (0xc000012f) from psp-ar.exe: binutils loads every file in
  `lib/bfd-plugins` as a plugin DLL, and our Windows package shipped binutils' static `libdep.a` there. The
  converter now deletes non-DLL files there on its own install (fixes existing downloads without a new package),
  scripts/wintc/build.sh leaves them out of future packages, and server.py sets SetErrorMode
  (FAILCRITICALERRORS | NOOPENFILEERRORBOX), inherited by the toolchain processes, so no Windows dialog can block
  a conversion. Checked: psp-gcc/psp-ar/psp-nm work without the file.
* "No 16:9" report: not the disc (the converter only accepts the two known dumps, an unknown one never gets
  converted). g_widescreen defaulted to 0 and since 9aed1c0 neither the launcher nor the in-game menu sets it - the
  release has no settings.txt, so every fresh install ran 4:3 (the developer PSP still had `widescreen=1` from the
  old menu). Default is now 1; `widescreen=0` in settings.txt gives 4:3 (README).

## 2026-11-27 - GT2 Plus (Beta v6.1) checked

Fan mod discs (ISO/, zips from the user). Compared EXE + GT2.OVL code word by word against our discs
(extracted with `gt2psp extract` into build/plus-*):
* **Plus Simulation** (sha1 6b36e356...): SCUS_944.88 and overlays 0-3, 5 bit-identical to Rev 2. ovl4: one immediate
  (completion percentage fix, `addiu v1, v1, -29` -> `-27` at 0x80019DCC) and a string table. `recomp` on its files
  gives the same C except that one line; all other Plus content (cars, colours, textures) is in GT2.VOL/GT2.OVL data,
  which the pack takes from the disc anyway. Test: Rev 2 runtime + pack of the Plus disc in PPSSPP: title, attract
  race, GT mode garage with the existing save - all fine. Supporting it = converter accepts the hash (recompiles from
  the disc's files as for Rev 2); open: two Simulation discs at once, separate save data (a Plus save with new cars
  in the vanilla game).
* **Plus Arcade USA** (sha1 75d36eeb...): a different build of the Arcade EXE - entry 0x8005D570 -> 0x8005D5C0, nearly
  all jal targets +0x50 (ovl3/ovl5 differ only in jal targets), data moved too. Probably based on another Arcade
  revision. Needs its own disc profile (profiles.py constants, 45 patch sites via sigmap.py) and runtime constants
  (core.c module table/overlay loader, race frame interval byte).
* **Plus Arcade Europe** (SCES_023.80, sha1 c43b1daf...): PAL build, code moved by ~0x130 and much more changed data;
  same work as above plus PAL (50 Hz, 256 lines, language selection) - untested ground.

## 2026-11-27 - Conversion profiles per game version; GT2 Plus (USA) supported

* profiles.py: disc profiles `sim`, `arc`, `sim_plus`, `arc_plus`; `DISCS` (SHA-1 -> profile + edition + title) and
  `EDITIONS` (`usa` -> PSP/GAME/GT2PSP, save GT2PSP000; `plus_usa` -> GT2PLUS, save GT2PLUS00, title "Gran Turismo 2
  Plus"). The converter takes the dump table from there (server.py KNOWN removed).
* `recomp` writes `game.h` (runtime constants of the profile); core.c, io.c, bios.c, main.c, savedata.c use it
  instead of `#ifdef GT2_ARC_BUILD` / tables indexed by g_game. Vanilla sim + arc regenerated: all C files
  bit-identical, game.h values equal to the old hard-coded ones.
* New `python -m gt2psp derive` (derive.py): profile of another build from a known one by code signatures. Check:
  derive sim -> Arcade disc reproduces the hand-made ARC profile (all 45 patch sites, card task, interval byte,
  overlay table), 0 problems. arc -> GT2 Plus Arcade: 0 problems (resident +0x50, ovl0 sites partly at the
  Simulation addresses, ovl2 text_exclude +0x25C, interval byte 0x1D5634, card context 0x801F04A0).
* build_psp.py: `--game sim|arc[=<generated dir>]`, `--title` (XMB + launcher heading), one object dir per disc,
  objects rebuilt when a header (game.h) is newer. CMake: `GT2_GEN_DIR` (build.sh env). Launcher heading from
  `-DGT2_LAUNCHER_TITLE`.
* Tests (PPSSPP): Plus Simulation - title, attract race, GT mode garage; new save data GT2PLUS00 (no vanilla save
  found = separate). Plus Arcade (direct EBOOT, arc_plus) - intro movie, START -> menu, attract replay in 16:9 at
  30 fps (race work 22-26 ms). Release build through build_psp.py with the Windows toolchain: launcher + both PRXs.
