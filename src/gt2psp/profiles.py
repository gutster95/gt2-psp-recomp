"""Per-disc constants of the two GT2 discs (USA): Simulation Mode Rev 2 (SCUS-94488) and Arcade Mode (SCUS-94455).

Both executables are built from the same sources; the Arcade disc's code sits a few hundred bytes lower (resident
code -0x90 at the start, -0xF0 from the middle on). The Arcade addresses were derived from the Simulation ones with
code signatures (sigmap.py, 2026-11-21). ``use(name)`` switches program.py and recomp.py to a disc.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace


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
    kind: str = ""            # "sim" / "arc": which runtime (GT2SIM.PRX / GT2ARC.PRX, GT2.PAK / GT2A.PAK); "" = name
    # runtime constants (generated game.h): the memory card task and its context (bios.c card fast path), the race
    # frame interval byte (io.c, RAM offset) and one instruction that builds the race display struct's address
    # (derive.py follows the interval through it)
    card_task: int = 0
    card_ctx: int = 0
    interval: int = 0
    interval_ref: tuple[str, int] | None = None

    def __post_init__(self) -> None:
        self.kind = self.kind or self.name


SIM = Profile(
    name="sim", exe="SCUS_944.88", prefix="",
    boot_text=(0x80010000, 0x80011DF8), resident_text=(0x8005D600, 0x8008E020),
    overlay_entry_table=0x80091174, setjmp=0x8007AD58, longjmp=0x8007AD90, ovl_loader=0x8005DAD8,
    ovl_jump=None,            # 0x8005DA7C: the fix would merge 6 functions; the tested Simulation code stays as it is
    # nine 2-instruction memory card event callbacks (`j 0x8007eaac; li a1, bit`) registered via OpenEvent
    extra_starts={"resident": {0x8007EA68 + 8 * i for i in range(9)}},
    card_task=0x8007EACC, card_ctx=0x801F06E0, interval=0x1D5864, interval_ref=("ovl0", 0x80013844),
)

ARC = Profile(
    name="arc", exe="SCUS_944.55", prefix="a",
    boot_text=(0x80010000, 0x80011DF0), resident_text=(0x8005D570, 0x8008DF30),
    overlay_entry_table=0x80090EB8, setjmp=0x8007AC68, longjmp=0x8007ACA0, ovl_loader=0x8005DA48,
    ovl_jump=0x8005D9EC,
    extra_starts={"resident": {0x8007E978 + 8 * i for i in range(9)}},
    card_task=0x8007E9DC, card_ctx=0x801F00D0, interval=0x1D52C4, interval_ref=("ovl0", 0x80013844),
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

# GT2 Plus (Beta v6.1) fan mod, USA discs. Simulation: the EXE and overlays 0-3, 5 are bit-identical to Rev 2 (ovl4
# changes one constant and a string table), so the Simulation profile applies unchanged - recompiled from the Plus
# disc's own files. Arcade: another build of the EXE (resident code +0x50, overlays relinked); everything below was
# derived from ARC with `python -m gt2psp derive` (2026-11-27, 0 problems: every site found once, every patch site's
# instruction and the code around it identical).
SIM_PLUS = replace(SIM, name="sim_plus", kind="sim")
ARC_PLUS = replace(
    ARC, name="arc_plus", kind="arc",
    boot_text=(0x80010000, 0x80011DF0), resident_text=(0x8005D5C0, 0x8008DF80),
    overlay_entry_table=0x80090F58, setjmp=0x8007ACB8, longjmp=0x8007ACF0, ovl_loader=0x8005DA98,
    ovl_jump=0x8005DA3C,
    extra_starts={"resident": {0x8007E9C8 + 8 * i for i in range(9)}},
    extra_resume={"ovl2": {0x80014230}, "resident": {0x80080A04, 0x80080A24}},
    resume_inside={"resident": {0x800833A0, 0x800833B8}},
    text_exclude={"ovl2": [(0x80050A5C, 0x80050B0C)]},
    card_task=0x8007EA2C, card_ctx=0x801F04A0, interval=0x1D5634, interval_ref=("ovl0", 0x80013844),
    patch_map={
        ("ovl0", 0x800100D0): 0x800100D0,
        ("ovl0", 0x800100D4): 0x800100D4,
        ("ovl0", 0x8001435C): 0x8001435C,
        ("ovl0", 0x800168C8): 0x800168C8,
        ("ovl0", 0x80018E90): 0x80018E90,
        ("ovl0", 0x80018FB8): 0x80018FB8,
        ("ovl0", 0x80019098): 0x80019098,
        ("ovl0", 0x800191BC): 0x800191BC,
        ("ovl0", 0x8001929C): 0x8001929C,
        ("ovl0", 0x800193C8): 0x800193C8,
        ("ovl0", 0x800194BC): 0x800194BC,
        ("ovl0", 0x800195EC): 0x800195EC,
        ("ovl0", 0x80019644): 0x80019644,
        ("ovl0", 0x80020418): 0x80020418,
        ("ovl0", 0x80020428): 0x80020428,
        ("ovl0", 0x80029550): 0x80029548,
        ("ovl0", 0x800295EC): 0x800295E4,
        ("ovl0", 0x800295F0): 0x800295E8,
        ("ovl0", 0x800497C8): 0x80049734,
        ("ovl0", 0x80049F48): 0x80049EB4,
        ("ovl0", 0x80049F50): 0x80049EBC,
        ("ovl0", 0x8004C194): 0x8004C100,
        ("ovl0", 0x8004C19C): 0x8004C108,
        ("ovl0", 0x8004E064): 0x8004DFD0,
        ("ovl0", 0x80050C14): 0x80050B80,
        ("ovl0", 0x80050C1C): 0x80050B88,
        ("ovl0", 0x80058070): 0x8005802C,
        ("ovl0", 0x80058078): 0x80058034,
        ("ovl0", 0x80058924): 0x800588E0,
        ("ovl0", 0x8005892C): 0x800588E8,
        ("ovl0", 0x80059A08): 0x800599C4,
        ("ovl2", 0x8001536C): 0x80015350,
        ("ovl2", 0x8001E578): 0x8001E55C,
        ("ovl2", 0x8001E580): 0x8001E564,
        ("ovl2", 0x800201C4): 0x800201A8,
        ("ovl2", 0x800201CC): 0x800201B0,
        ("ovl4", 0x8001CDF4): 0x8001CDF4,
        ("ovl4", 0x8001CDFC): 0x8001CDFC,
        ("ovl4", 0x8001CE04): 0x8001CE04,
        ("resident", 0x8007D23C): 0x8007D19C,
        ("resident", 0x8007D288): 0x8007D1E8,
        ("resident", 0x80080B88): 0x80080AE8,
        ("resident", 0x80080B94): 0x80080AF4,
        ("resident", 0x8008C338): 0x8008C298,
        ("resident", 0x8008C4A8): 0x8008C408
    },
)

PROFILES = {p.name: p for p in (SIM, ARC, SIM_PLUS, ARC_PLUS)}


@dataclass
class Edition:
    """A set of discs that belong together on the PSP: one PSP/GAME folder with its own launcher and save data."""
    name: str
    folder: str               # ms0:/PSP/GAME/<folder>
    title: str                # XMB title and save data title
    save_game: str            # save data game name, 9 characters (folder ms0:/PSP/SAVEDATA/<save_game>00CARD1)


EDITIONS = {e.name: e for e in (
    Edition("usa", "GT2PSP", "Gran Turismo 2", "GT2PSP000"),
    Edition("plus_usa", "GT2PLUS", "Gran Turismo 2 Plus", "GT2PLUS00"),
)}


@dataclass
class Disc:
    sha1: str                 # of the whole .bin (single track, MODE2/2352)
    profile: str              # PROFILES key
    edition: str              # EDITIONS key
    title: str                # shown in the converter


DISCS = {d.sha1: d for d in (
    Disc("c4813703d2ad440ed36670c1ac2c1a66c9dc673d", "sim", "usa", "Gran Turismo 2 (USA) Simulation Mode, Rev 2"),
    Disc("a617b948e23c198a4d0eb521b874d0f563987d98", "arc", "usa", "Gran Turismo 2 (USA) Arcade Mode"),
    Disc("6b36e3569e16547efd8d6cc65ca41774e2d21dd4", "sim_plus", "plus_usa",
         "Gran Turismo 2 Plus Beta v6.1 (USA) Simulation Mode"),
    Disc("75d36eebc5b3fb5e767b860173a43e9682d96b21", "arc_plus", "plus_usa",
         "Gran Turismo 2 Plus Beta v6.1 (USA) Arcade Mode"),
)}
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


def edition_of(profile: str) -> Edition:
    eds = {d.edition for d in DISCS.values() if d.profile == profile}
    if len(eds) != 1:
        raise KeyError(f"profile {profile} belongs to {len(eds)} editions")
    return EDITIONS[eds.pop()]
