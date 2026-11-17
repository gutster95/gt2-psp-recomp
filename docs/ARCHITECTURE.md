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

## Reference material

* `Recomps/GT2-PC-Port-Mod-Kit-0.4.3.1` (local only, not in this repo): a PC port for disc v1.0 that ships
  compiled WebAssembly modules and extensive explainer pages. Used as a reference for game structure only;
  our target disc is Rev 2, which it does not support.
