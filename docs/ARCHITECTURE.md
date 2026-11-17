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

## Reference material

* `Recomps/GT2-PC-Port-Mod-Kit-0.4.3.1` (local only, not in this repo): a PC port for disc v1.0 that ships
  compiled WebAssembly modules and extensive explainer pages. Used as a reference for game structure only;
  our target disc is Rev 2, which it does not support.
