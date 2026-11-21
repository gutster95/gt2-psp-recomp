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
    patch_map: dict[tuple[str, int], int] = field(default_factory=dict)   # Simulation patch site -> this disc
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
    # The Simulation disc's game patches (recomp.py HOOKS/OVERRIDE: frame timing, frame flip for 60 fps, 16:9 view
    # windows and near-plane clipper loops, 60 fps effects, car and track LOD) at their Arcade addresses. Generated
    # 2026-11-21 with sigmap.py (unique signature match; the 8 clipper loops at -0x74, whole loop windows identical);
    # every site's original instruction word was checked to be bit-identical on both discs.
    patch_map={
        ("ovl0", 0x800100D0): 0x800100D0,
        ("ovl0", 0x800100D4): 0x800100D4,
        ("ovl0", 0x8001435C): 0x8001435C,
        ("ovl0", 0x800168C8): 0x80016854,
        ("ovl0", 0x80018E90): 0x80018E1C,
        ("ovl0", 0x80018FB8): 0x80018F44,
        ("ovl0", 0x80019098): 0x80019024,
        ("ovl0", 0x800191BC): 0x80019148,
        ("ovl0", 0x8001929C): 0x80019228,
        ("ovl0", 0x800193C8): 0x80019354,
        ("ovl0", 0x800194BC): 0x80019448,
        ("ovl0", 0x800195EC): 0x80019578,
        ("ovl0", 0x80019644): 0x800195D0,
        ("ovl0", 0x80020418): 0x800203A4,
        ("ovl0", 0x80020428): 0x800203B4,
        ("ovl0", 0x80029550): 0x800294FC,
        ("ovl0", 0x800295EC): 0x80029598,
        ("ovl0", 0x800295F0): 0x8002959C,
        ("ovl0", 0x800497C8): 0x800496E8,
        ("ovl0", 0x80049F48): 0x80049E68,
        ("ovl0", 0x80049F50): 0x80049E70,
        ("ovl0", 0x8004C194): 0x8004C0B4,
        ("ovl0", 0x8004C19C): 0x8004C0BC,
        ("ovl0", 0x8004E064): 0x8004DF84,
        ("ovl0", 0x80050C14): 0x80050B34,
        ("ovl0", 0x80050C1C): 0x80050B3C,
        ("ovl0", 0x80058070): 0x80057FE0,
        ("ovl0", 0x80058078): 0x80057FE8,
        ("ovl0", 0x80058924): 0x80058894,
        ("ovl0", 0x8005892C): 0x8005889C,
        ("ovl0", 0x80059A08): 0x80059978,
        ("ovl2", 0x8001536C): 0x80015350,
        ("ovl2", 0x8001E578): 0x8001E55C,
        ("ovl2", 0x8001E580): 0x8001E564,
        ("ovl2", 0x800201C4): 0x800201A8,
        ("ovl2", 0x800201CC): 0x800201B0,
        ("ovl4", 0x8001CDF4): 0x8001CDF4,
        ("ovl4", 0x8001CDFC): 0x8001CDFC,
        ("ovl4", 0x8001CE04): 0x8001CE04,
        ("resident", 0x8007D23C): 0x8007D14C,
        ("resident", 0x8007D288): 0x8007D198,
        ("resident", 0x80080B88): 0x80080A98,
        ("resident", 0x80080B94): 0x80080AA4,
        ("resident", 0x8008C338): 0x8008C248,
        ("resident", 0x8008C4A8): 0x8008C3B8,
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
