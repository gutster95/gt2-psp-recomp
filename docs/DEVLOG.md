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

## 2026-11-18 � Milestone 1: title screen

* Sprite fast paths in the rasterizer (pre-modulated CLUT per primitive, row fills for flat rectangles) make
  the notice fade run at playable speed; the game then loads overlay 1 and shows the **GT2 title screen**
  ("Start Game", "Replay Theater", Polyphony Digital logo) � all game logic is recompiled code.
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
