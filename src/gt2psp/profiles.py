"""Per-disc constants of the two GT2 discs (USA): Simulation Mode Rev 2 (SCUS-94488) and Arcade Mode (SCUS-94455).

Both executables are built from the same sources; the Arcade disc's code sits a few hundred bytes lower (resident
code -0x90 at the start, -0xF0 from the middle on). The Arcade addresses were derived from the Simulation ones with
code signatures (sigmap.py, 2026-11-21). ``use(name)`` switches program.py and recomp.py to a disc.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Profile:
    name: str                 # "sim" / "arc"
    exe: str                  # main executable on the disc
    prefix: str               # symbol prefix of the generated code ("" / "a")
    boot_text: tuple[int, int]
    resident_text: tuple[int, int]
    overlay_entry_table: int
    setjmp: int
    longjmp: int
    ovl_loader: int           # overlay loader (wrapped by the runtime)
    ovl_jump: int | None      # load overlay a0 and longjmp to a1 (program.py _overlay_jump_targets); None: off
    extra_starts: dict[str, set[int]] = field(default_factory=dict)
    extra_resume: dict[str, set[int]] = field(default_factory=dict)   # resume points after jalr (found at run time)
    switch_starts: bool = False  # program.py: switch cases split off their jr become starts (Simulation: off, tested)
    resume_inside: dict[str, set[int]] = field(default_factory=dict)   # resume points inside loops (entry switch)
    resume_jalr: bool = False   # recomp.py: every jalr return address is an in-place resume point (task switches)
    resume_full_return: bool = False   # recomp.py: resume points return their callee-saved registers (task switches)
    game_patches: bool = True  # recomp.py HOOKS/OVERRIDE of the Simulation disc (widescreen, 60 fps, LOD, ...)
    hooks: dict[tuple[str, int], str] = field(default_factory=dict)   # disc-specific runtime hooks
    text_exclude: dict[str, list[tuple[int, int]]] = field(default_factory=dict)   # data the text detection took


SIM = Profile(
    name="sim", exe="SCUS_944.88", prefix="",
    boot_text=(0x80010000, 0x80011DF8), resident_text=(0x8005D600, 0x8008E020),
    overlay_entry_table=0x80091174, setjmp=0x8007AD58, longjmp=0x8007AD90, ovl_loader=0x8005DAD8,
    ovl_jump=None,            # 0x8005DA7C: the fix would merge 6 functions; the tested Simulation code stays as it is
    # nine 2-instruction memory card event callbacks (`j 0x8007eaac; li a1, bit`) registered via OpenEvent
    extra_starts={"resident": {0x8007EA68 + 8 * i for i in range(9)}},
)

ARC = Profile(
    name="arc", exe="SCUS_944.55", prefix="a",
    boot_text=(0x80010000, 0x80011DF0), resident_text=(0x8005D570, 0x8008DF30),
    overlay_entry_table=0x80090EB8, setjmp=0x8007AC68, longjmp=0x8007ACA0, ovl_loader=0x8005DA48,
    ovl_jump=0x8005D9EC,
    extra_starts={"resident": {0x8007E978 + 8 * i for i in range(9)}},
    game_patches=False,
    # jalr calls that can end in a task switch (setjmp/longjmp): ovl2 0x80014228 (menu task callback), resident
    # 0x800809ac/0x800809cc (virtual calls in the frame flip)
    extra_resume={"ovl2": {0x80014230}, "resident": {0x800809B4, 0x800809D4}},
    switch_starts=True,
    # the task scheduler's loop calls the tasks (jalr 0x80083348/0x80083360): a split there would turn the loop into
    # a chain of tail calls, so the function is resumed in place (recomp.py entry switch on c->resume_pc)
    resume_inside={"resident": {0x80083350, 0x80083368}},
    resume_jalr=True,
    resume_full_return=True,   # the Arcade menus switch tasks with setjmp/longjmp (ovl2, scheduler 0x80083a1c)
    text_exclude={"ovl2": [(0x80050800, 0x800508B0)]},   # a table of byte offsets (decodes as jalr zero, lb ...)
    hooks={
        # frame timing statistics, as on the Simulation disc (libetc VSync and GT2's vblank counter wait)
        ("resident", 0x8008C248): "rt_vsync_enter((int32_t)c->r[4] >= 0 && c->r[4] != 1);",
        ("resident", 0x8008C3B8): "rt_vsync_leave();",
        ("resident", 0x8007D14C): "rt_vsync_enter((int32_t)c->r[4] > 0);",
        ("resident", 0x8007D198): "rt_vsync_leave();",
    },
)

PROFILES = {"sim": SIM, "arc": ARC}
CURRENT = SIM


def use(name: str) -> Profile:
    """Switch the recompiler (program.py, recomp.py) to a disc."""
    global CURRENT
    p = PROFILES[name]
    CURRENT = p
    from . import program, recomp
    program.use_profile(p)
    recomp.use_profile(p)
    return p
