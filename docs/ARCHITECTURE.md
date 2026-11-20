# Architecture

This document describes the planned design. It is updated as decisions are made.

## Repository layout

```
src/gt2psp/        Python tooling: disc extraction, MIPS analysis, recompiler (MIPS -> C)
runtime/           C runtime for the PSP: memory, BIOS HLE, GTE, GPU->GE, SPU, CD, pad
docs/              architecture notes and development log
ISO/               (ignored) your disc image
extracted/         (ignored) files extracted from your image
generated/         (ignored) recompiled C code generated from your image
build/             (ignored) build output, EBOOT.PBP
tools/             (ignored) local toolchain (pspdev inside a project-local WSL distro)
```

## Recompilation model

* Each PS1 function found by analysis becomes one C function operating on a CPU context struct
  (`r[32]`, `hi`, `lo`, COP0, GTE registers).
* PS1 main RAM (2 MB) is a buffer; all KSEG0/KSEG1/KUSEG mirrors are masked to it. The scratchpad (1 KB at
  `0x1F800000`) and hardware registers (`0x1F801000+`) are dispatched to runtime handlers.
* Load delay slots and branch delay slots are resolved statically during translation.
* Indirect jumps (`jr`/`jalr` through registers, jump tables) go through an address → function lookup.
* GT2 loads code overlays at runtime. Every overlay is recompiled separately; the lookup is keyed by
  address *and* the currently loaded overlay.
* BIOS calls (`A0`/`B0`/`C0` tables, `syscall`) are replaced by high-level implementations.

## Code model (decided)

* The program consists of modules: `resident` (SCUS_944.88 `0x8005d600+`), `boot` (low part of SCUS_944.88,
  only valid until the first overlay load) and `ovl0`-`ovl5`, all overlays mapped at `0x80010000`.
* Each module is partitioned completely into functions (`python -m gt2psp functions`); every function becomes
  one C function. Calls whose target lies in `0x80010000-0x8005d600` are dispatched through the currently loaded
  module (runtime variable set by the HLE replacement of the overlay loader `0x8005dad8`).
* The overlay loader itself is replaced by a runtime function (no gunzip of code needed — the code is already
  compiled in; the RAM image is still filled so data inside the overlay is correct).
* `setjmp`/`longjmp` (`0x8007ad58`/`0x8007ad90`) are replaced by the runtime (**unwind & resume**):
  `hle_setjmp` writes the PS1 buffer like the original. `hle_longjmp` restores the registers from the PS1
  buffer, sets `v0`, then C-`longjmp`s to the runtime's top-level loop, which "returns" to `r31`: it calls the
  recompiled function starting at `r31`; when that returns (via `jr ra`), it continues at the new `r31`, until
  the sentinel return address of the program entry is reached. This requires that every such return address is a
  function start — the partitioner adds the return address of every `jal` whose callee can transitively reach
  `setjmp` (*resume points*). Splitting a function is always safe: control crossing the split becomes a tail call
  (compiled to a jump by GCC's sibling-call optimisation).
* Switch jump tables are detected statically; `jr reg` compiles to a C `switch` over the table's labels with a
  fallback to the dynamic dispatcher.

## Runtime (implemented)

* Hardware is emulated at register level where the libraries talk to it directly (interrupt controller, DMA,
  timers, CD-ROM controller, SIO0 pad, GPU command stream); BIOS calls are HLE'd. This keeps the original
  PsyQ library code (libcd, libetc, libgpu, libpad, libspu, libcard) running unmodified as recompiled code.
* Interrupts are delivered at `POLL()` points (every backward branch, budgeted) and run the BIOS chains
  (SysEnqIntRP) and the HookEntryInt hook on a saved copy of the CPU state.
* GPU (`gpu.c` front end, `ge.c` renderer, `raster.c` software reference):
  * The PS1 VRAM (1024x512x16) lives at the start of PSP eDRAM. PS1 15-bit pixels are bit-identical to GE 5551,
    so the GE renders directly into the PS1 VRAM image and the CPU uses the same memory (uncached mirror) for
    uploads, readback and software fallbacks. Display buffers follow at 0x100000/0x144000.
  * Primitives become GE draws in through mode (vertex = VRAM coordinate, offset 2048 allows negatives); drawing
    area = scissor; PS1 mask bit = GE stencil (5551 alpha); semi-transparency modes = fixed-factor blending;
    dithering = GE dither matrix; modulation uses the GE colour-doubling bit (tex*col*2/255 ~ tex*col/128).
  * Textures: 4/8-bit texture pages cannot be sampled in place (row pitch 4096/2048 texels > GE limit 2047), so
    the GE copies pages (block transfer, part of the command stream) into a 40-slot T4/T8 cache in RAM; per-page
    version counters (16x2 pages) detect stale copies after CPU or GE writes, which keeps render-to-texture
    correct. CLUTs are converted by the CPU to 8888 with an alpha code (0 transparent, 0xFF opaque, 0x70 STP);
    semi-transparent textured primitives are drawn twice (opaque texels, then STP texels blended).
  * Fills and VRAM copies run on the GE; CPU uploads/readbacks sync the GE first. 15-bit textures and texture
    windows still use the software rasterizer (after a sync).
  * Presentation: the GE scales the display area (4:3, 363x272, bilinear) into the back buffer, then flips.
  * Hi-res + 16:9 (`g_hires`, `g_widescreen`): a displayed PS1 framebuffer region becomes a "frame" bound to
    one of two PSP display buffers. Primitives drawn into a frame are drawn twice: 1x into PS1 VRAM (readback,
    render-to-texture stay exact) and mapped (float through-mode vertices) at 480x272 into the frame's display
    buffer; presenting a frame is a flip. Fills/copies/uploads/software-rasterized areas are mirrored.
  * 16:9 (`g_widescreen`, decided 2026-11-19 after the research in DEVLOG): no GTE change. Every 3D screen of GT2
    sets up its view window (screen half-width of the camera's field of view) from constants; recomp.py OVERRIDEs
    scale them by 4/3 (values from Silent's "Widescreen Hack 2.0" cheat template for NTSC-U 1.2). The game then
    renders the 16:9 field of view itself, squeezed into its 320-pixel framebuffer (anamorphic), and all of its
    screen-space visibility tests cover 16:9 automatically. The presentation stretches the picture to 480x272.
    Races only by default: the race's ordering table (8515 entries, four layers each starting with a drawing-area
    packet) has the HUD layer at OT[3] = list start - 0x850C; gpu_walk recognises it, `ge_wide_now()` presents
    16:9 while that layer was seen in the last 30 vblanks, everything else stays 4:3 (pure 2D menus would be
    stretched otherwise). `widescreen_menus=1` enables the patches for the menu screens with 3D views (car
    selection, post-race, GT mode) and presents every screen 16:9, like an emulator with the cheat.
  * 16:9 HUD (`g_hud_anchor`, default on): primitives of the race HUD layer are squeezed by 3/4 towards their
    anchor (left edge, centre or right edge of the drawing area, by the element's centre; text runs keep one
    anchor; elements wider than half the screen stay) before they are drawn, so the HUD keeps its 4:3 shape and
    its distance to the screen edges. Squeezed textured HUD primitives are filtered bilinearly at 1x (nearest
    would drop every fourth texel column of the glyphs).
  * Recompiler patches (`recomp.py` HOOKS/OVERRIDE): C code at specific game addresses. Overlay 0's scenery
    renderer (0x80018d1c, 8 loops with prebuilt packets) calls `wide_clip_track` (widescreen.c) for polygons the
    GTE rejects (vertex near/behind the camera): near-plane + guard-band clipping in view space, emitted as
    triangles into a packet ring in kernel RAM, linked into the same OT slot.
  * PGXP-style precision (`g_pgxp`): the GTE stores the sub-pixel screen position and view depth of every
    projected vertex in the 3D hash set. In the hi-res pass, 3D polygons whose vertices are all found are drawn in
    GE 3D mode with vertices (x*w, y*w, w) and a projection that moves w into the homogeneous coordinate (viewport
    scale 1): exact screen positions, perspective-correct texture/colour interpolation (no PS1 affine warping).
    ~90% of 3D polygons take this path. Opaque 3D textures without transparent CLUT entries are filtered
    bilinearly (`g_tex_filter`); transparent ones stay nearest (filtering would pull black into the edges).
  * `settings.txt` next to the EBOOT toggles widescreen, widescreen_menus, hires, pgxp, filter, hud_anchor,
    near_clip (0/1).
    The overlay 0 near-plane clipper (widescreen.c) is off by default: suspected of producing garbage polygons.
  * The state cache is invalidated at every list start (the GE keeps state across lists); "unknown" must not
    collide with a valid value (blend -1 = opaque caused additive accumulation on the title screen).
  * pspgu keeps its own scissor state and re-sends it: use sceGuScissor (startup check of its width semantics),
    never raw scissor commands.
* Game data: the PC tool `python -m gt2psp pack <cue>` converts the user's own disc once into `GT2.PAK`
  (never distributed): Form 1 sectors as 2048-byte data, Form 2 (XA music) as 2336 bytes, runs of sectors with
  a shared subheader, padding file FAULTY.PSX dropped (587 MB vs 692 MB). The runtime rebuilds raw sectors on
  demand (128 KB read-ahead with sceIo) and loads SCUS_944.88 from the pack, so the PSP folder is just
  `EBOOT.PBP` + `GT2.PAK`. The CD controller interface stays as the adapter between game and pack for now
  (GT2's own CD layer is driven by libcd interrupts); data sectors come every 1.5 ms (~4x the 2x drive rate;
  faster rates make the libcd interrupt handler loop for seconds with interrupts blocked) and only after the
  game has taken the previous one. Next: replace the read path at the libcd level so no CD timing remains.

## Performance design

* Generated functions keep PS1 registers in C locals (`registers_to_locals` in recomp.py) and write them back only
  before calls that take the CPU and before returns; memory accessors and GTE transfers are force-inlined
  (`GT2_INLINE`); PS1 RAM is a static array (constant base address).
* The GTE command word is a compile-time constant at each call site: `gte_fast.h` inlines RTPS/RTPT/NCLIP/MVMVA so
  the decoding folds away; all GTE arithmetic uses 32x32->64 multiplies (no libgcc 64-bit helpers).
* GE: draw calls bypass sceGuDrawArray (kernel stall update every 32 draws only), state changes are raw GE
  commands into pspgu's list, vertices come from an uncached arena; GPU DMA packets are executed in place.
* `profile.txt` next to the EBOOT starts the sampling profiler (log every 10 s).
* **Register flow across calls** (2026-11-19, `regflow.py`): an interprocedural fixpoint computes per function
  `IN` (registers whose `c->r[]` value the function or its callees may read: liveness with call sites using the
  callee's `IN`), `CLOB` (registers it may change in `c->r[]`) and `D` (dirty registers). Before a call only
  `D & IN(callee)` is written back, after it only `CLOB(callee)` (caller-saved) is reloaded. Assumes the MIPS ABI:
  caller-saved registers are dead after a call, prologue stores of s0-s8/ra to the stack only save them.
  Dynamic calls, BIOS and setjmp/longjmp use all registers.
* **Memory accessors**: the scratchpad lives in the first KB of `g_ram` (physical 0x000-0x3FF, BIOS RAM on the
  PS1), so `g_ram + (a & 0x1FFFFF)` reaches RAM and scratchpad with no check (`ram_*`). Used for every access
  with base register `sp` (`rt_check_sp` guards the rare places that set sp from anything but sp + constant) and
  for all accesses of functions that a memory profile saw accessing memory but never I/O
  (`data/memprof/*.txt`, recorded by a build with `-DGT2_MEMPROF`, ids `module << 24 | address`; read by
  `recomp` by default). Functions never profiled, touching I/O or the low KB of RAM keep the checked accessors:
  inline fast path for RAM above 0x400, out of line for the low KB (redirected to a dummy area - GT2 writes
  there through null pointers), scratchpad and I/O. The BIOS C0 table points libapi's exception handler patch
  into kernel RAM at 0x1028 (main.c). Effect on the PSP: text 6.4 -> 3.5 MB, attract race 22.2 -> 27.0 fps.
* **Code layout and PGO** (2026-11-20): the race frame time depends on the code layout by +-10% (16 KB 2-way
  instruction cache; the overlay renderers alone are 27-36 KB each). `data/hot_functions.txt` fixes the order of
  the hot runtime code (incl. library members such as `libpspgu.a:sceGuDrawArray.o`), a measured `pad=` gap, then
  the hottest game functions (scripts/make_linkfile.py). The release EBOOT is built profile-guided:
  `scripts/pgo.sh profile` (instrumented build, attract race on the PSP, counters to `build/pgo-data/`), then
  `scripts/pgo.sh build` (generated code -O2 with the profile: hot/cold block splitting, branch layout). Judge
  changes with `build/racework.sh` (deterministic `race work` averages, same EBOOT +-0.1 ms) and re-check the gap
  after changes to hot code.
  Since 2026-11-20 the rest of the recompiled code (`generated/*.c`) follows the hot block at a fixed offset within
  a 16 KB block, and `.rodata`/`.data` start at fixed 16 KB offsets (linkfile.prx.in): runtime edits no longer move
  the game code or the tables relative to each other and to the 64 KB aligned PS1 RAM. Do not judge performance by
  fps60 pacing in the attract demo: at 60 fps the demo desyncs, and tiny timing differences give a different race
  (and load). Use the 30 fps idle/work statistics, which are deterministic.
* **CD read-ahead** (cdrom.c, 2026-11-20): two 128 KB buffers over GT2.PAK; past the middle of one, a prefetch thread
  (own file handle, priority 0x14) loads the next 128 KB. Before, XA music streaming did a synchronous 128 KB read
  every ~0.4 s on the game thread (~15 ms, a dropped frame at 60 fps). `cd_prefetch=0` turns it off;
  `pakpath.txt` (tests via PSPLink) names another pack, e.g. `ms0:/PSP/GAME/GT2PSP/GT2.PAK` instead of USB host0.
* **GPU emulation on the Media Engine** (2026-11-20, runtime/src/me.c): the second 333 MHz core runs gpu.c/ge.c/raster.c
  in parallel with the game; the main CPU posts commands into an uncached ring and does the system calls (GE enqueue,
  display swap) in a service thread. The ME cannot address the GE's eDRAM: vertices in RAM, PS1 VRAM shadow in RAM
  with GE copies in both directions (ge_push/ge_pull). Data written by the ME lives in isolated 64-byte aligned blocks
  (linkfile.prx.in), the main CPU never writes it while the ME runs. Boot code: mcidclan's psp-media-engine-custom-core
  (MIT, tools/downloads/me-custom-core, built by scripts/me/build-sample.sh).
* **Asynchronous presentation**: the presentation list is kicked without waiting; swap when `sceGeListSync(qid, 1)`
  reports it done (rt_poll); second list buffer and a vertex ring in the unused display buffer 2 (see ge.c).
* **16:9 race HUD at PSP resolution**: recorded during the walk, drawn after the presentation blit into the display
  buffer (ge.c hud_record/hud_draw) - squeezed at 320 pixels it lost a quarter of its texel columns.
* Refreshing the memory profile (after partitioning changes): `recomp --no-memprof`, build with
  `scripts/build-exp.sh "-Os -DGT2_NO_TRACE -DGT2_MEMPROF"`, run (attract race, menus, license tests), copy
  `memprof.txt` from the EBOOT folder to `data/memprof/`.

## Real-hardware rendering decision (2026-11-18)

* Default rendering is native 1x: all primitives go into the PS1 VRAM in eDRAM (GE through mode), the displayed
  area is scaled into PSP buffer 0/1 (double-buffered, blit only when the displayed area changed). Hi-res frames,
  the old widescreen (GTE X*3/4, replaced 2026-11-19) and the perspective-correct pass (GE transform mode) stay optional via settings.txt: GE
  transform mode drops/distorts geometry on the real PSP, and the hi-res frame bookkeeping was the main source of
  hardware-only glitches.
* Picture reference: PCSX-Redux with OpenBIOS and the original disc (Lua screenshots), not PPSSPP - PPSSPP's GE is
  more forgiving than the hardware (transform-mode culling, cache coherency, display buffer timing).
* Code size beats instruction count on the Allegrex (16 KB I-cache): generated code is built with -Os.

## Reference material

* `Recomps/GT2-PC-Port-Mod-Kit-0.4.3.1` (local only, not in this repo): a PC port for disc v1.0 that ships
  compiled WebAssembly modules and extensive explainer pages. Used as a reference for game structure only;
  our target disc is Rev 2, which it does not support.

## MDEC

`runtime/src/mdec.c` decodes the run-length/DCT macroblock format in software behind the MDEC registers and DMA
channels 0/1 (input buffered per command, output decoded lazily one macroblock at a time; a DMA1 started before the
input is complete is finished when it is). The Simulation disc has no movies; see DEVLOG 2026-11-18.
