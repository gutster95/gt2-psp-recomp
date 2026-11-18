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
* Indirect jumps (`jr`/`jalr` through registers, jump tables) go through an address â†’ function lookup.
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
  * Widescreen: gte.c scales projected X by 3/4, which squeezes the whole 16:9 field of view into the PS1 screen
    width. All of the game's visibility tests work in PS1 screen space, so they automatically cover 16:9 - no
    culling code needs patching (a first attempt kept 4:3 in PS1 space and lost geometry at the edges to dozens of
    screen-space culls). In the hi-res pass, 3D primitives are stretched over the full 480 pixels, 2D primitives
    (HUD, menus) are mapped 4:3 into the centre 363 pixels. 3D = at least two vertices equal a screen coordinate
    the GTE produced within the last few vblanks (hash set fed by RTPS/RTPT, like PGXP-style tagging).
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
  * `settings.txt` next to the EBOOT toggles widescreen, hires, pgxp, filter, hud_anchor, near_clip (0/1).
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

## Reference material

* `Recomps/GT2-PC-Port-Mod-Kit-0.4.3.1` (local only, not in this repo): a PC port for disc v1.0 that ships
  compiled WebAssembly modules and extensive explainer pages. Used as a reference for game structure only;
  our target disc is Rev 2, which it does not support.
