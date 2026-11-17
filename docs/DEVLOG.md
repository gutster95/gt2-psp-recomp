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
