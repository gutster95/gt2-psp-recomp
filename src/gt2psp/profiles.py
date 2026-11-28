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
    # data/memprof/*.txt (memory profiles of Simulation runs, keyed by function address) apply: only to discs with
    # exactly the Simulation Rev 2 code; elsewhere they would mark other functions as safe
    memprof: bool = False
    # PAL build: the game counts 20 ms per vblank (measured: the lap clock advances 6.0 s per 300 vblanks on the EU
    # disc, 5.0 s on the USA discs), so the runtime delivers 50 Hz vblanks (game.h GT2_VBLANK_US)
    pal: bool = False

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
    memprof=True,
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

# Europe (PAL, En/Fr/De/Es/It): Disc 2 "Gran Turismo Mode" SCES-12380 and Disc 1 Arcade SCES-02380. Both EXEs share
# one layout (resident 0x8005D640). Derived 2026-11-27 with `gt2psp derive` from SIM and ARC (0 problems; the three
# ovl4 GT mode view constants differ only in a register of the code before them, their overrides read none). The
# GT2 Plus Arcade (Europe) disc has the same code as the original EU Arcade disc: profile eu_arc.
EU_SIM = replace(
    SIM, name="eu_sim", kind="sim", exe="SCES_123.80", memprof=False, pal=True,
    boot_text=(0x80010000, 0x80011DD8),
    resident_text=(0x8005D640, 0x8008E074),
    setjmp=0x8007AD9C,
    longjmp=0x8007ADD4,
    ovl_loader=0x8005DB18,
    overlay_entry_table=0x800911B4,
    extra_starts={
        "resident": {0x8007EAB0, 0x8007EAB8, 0x8007EAC0, 0x8007EAC8, 0x8007EAD0, 0x8007EAD8, 0x8007EAE0, 0x8007EAE8, 0x8007EAF0}
    },
    extra_resume={},
    resume_inside={},
    text_exclude={},
    patch_map={
        ("ovl0", 0x800100D0): 0x800100D0,
        ("ovl0", 0x800100D4): 0x800100D4,
        ("ovl0", 0x8001435C): 0x8001435C,
        ("ovl0", 0x800168C8): 0x800168C4,
        ("ovl0", 0x80018E90): 0x80018E8C,
        ("ovl0", 0x80018FB8): 0x80018FB4,
        ("ovl0", 0x80019098): 0x80019094,
        ("ovl0", 0x800191BC): 0x800191B8,
        ("ovl0", 0x8001929C): 0x80019298,
        ("ovl0", 0x800193C8): 0x800193C4,
        ("ovl0", 0x800194BC): 0x800194B8,
        ("ovl0", 0x800195EC): 0x800195E8,
        ("ovl0", 0x80019644): 0x80019640,
        ("ovl0", 0x80020418): 0x80020414,
        ("ovl0", 0x80020428): 0x80020424,
        ("ovl0", 0x80029550): 0x8002954C,
        ("ovl0", 0x800295EC): 0x800295E8,
        ("ovl0", 0x800295F0): 0x800295EC,
        ("ovl0", 0x800497C8): 0x80049820,
        ("ovl0", 0x80049F48): 0x80049FA0,
        ("ovl0", 0x80049F50): 0x80049FA8,
        ("ovl0", 0x8004C194): 0x8004C1EC,
        ("ovl0", 0x8004C19C): 0x8004C1F4,
        ("ovl0", 0x8004E064): 0x8004E084,
        ("ovl0", 0x80050C14): 0x80050C48,
        ("ovl0", 0x80050C1C): 0x80050C50,
        ("ovl0", 0x80058070): 0x800580AC,
        ("ovl0", 0x80058078): 0x800580B4,
        ("ovl0", 0x80058924): 0x80058960,
        ("ovl0", 0x8005892C): 0x80058968,
        ("ovl0", 0x80059A08): 0x80059A50,
        ("ovl2", 0x8001536C): 0x8001536C,
        ("ovl2", 0x8001E578): 0x8001E524,
        ("ovl2", 0x8001E580): 0x8001E52C,
        ("ovl2", 0x800201C4): 0x80020170,
        ("ovl2", 0x800201CC): 0x80020178,
        ("ovl4", 0x8001CDF4): 0x8001CD68,
        ("ovl4", 0x8001CDFC): 0x8001CD70,
        ("ovl4", 0x8001CE04): 0x8001CD78,
        ("resident", 0x8007D23C): 0x8007D284,
        ("resident", 0x8007D288): 0x8007D2D0,
        ("resident", 0x80080B88): 0x80080BD0,
        ("resident", 0x80080B94): 0x80080BDC,
        ("resident", 0x8008C338): 0x8008C38C,
        ("resident", 0x8008C4A8): 0x8008C4FC
    },
    card_task=0x8007EB14,
    card_ctx=0x801F0710,
    interval=0x001D5894,
    interval_ref=("ovl0", 0x80013844),
)
EU_ARC = replace(
    ARC, name="eu_arc", kind="arc", exe="SCES_023.80", pal=True,
    boot_text=(0x80010000, 0x80011DD0),
    resident_text=(0x8005D640, 0x8008E074),
    setjmp=0x8007AD9C,
    longjmp=0x8007ADD4,
    ovl_loader=0x8005DB18,
    ovl_jump=0x8005DABC,
    overlay_entry_table=0x800911B4,
    extra_starts={
        "resident": {0x8007EAB0, 0x8007EAB8, 0x8007EAC0, 0x8007EAC8, 0x8007EAD0, 0x8007EAD8, 0x8007EAE0, 0x8007EAE8, 0x8007EAF0}
    },
    extra_resume={
        "ovl2": {0x800141F4},
        "resident": {0x80080AEC, 0x80080B0C}
    },
    resume_inside={
        "resident": {0x80083494, 0x800834AC}
    },
    text_exclude={
        "ovl2": [(0x80050980, 0x80050A30)]
    },
    patch_map={
        ("ovl0", 0x800100D0): 0x800100D0,
        ("ovl0", 0x800100D4): 0x800100D4,
        ("ovl0", 0x8001435C): 0x8001435C,
        ("ovl0", 0x800168C8): 0x800168C4,
        ("ovl0", 0x80018E90): 0x80018E8C,
        ("ovl0", 0x80018FB8): 0x80018FB4,
        ("ovl0", 0x80019098): 0x80019094,
        ("ovl0", 0x800191BC): 0x800191B8,
        ("ovl0", 0x8001929C): 0x80019298,
        ("ovl0", 0x800193C8): 0x800193C4,
        ("ovl0", 0x800194BC): 0x800194B8,
        ("ovl0", 0x800195EC): 0x800195E8,
        ("ovl0", 0x80019644): 0x80019640,
        ("ovl0", 0x80020418): 0x80020414,
        ("ovl0", 0x80020428): 0x80020424,
        ("ovl0", 0x80029550): 0x8002954C,
        ("ovl0", 0x800295EC): 0x800295E8,
        ("ovl0", 0x800295F0): 0x800295EC,
        ("ovl0", 0x800497C8): 0x80049820,
        ("ovl0", 0x80049F48): 0x80049FA0,
        ("ovl0", 0x80049F50): 0x80049FA8,
        ("ovl0", 0x8004C194): 0x8004C1EC,
        ("ovl0", 0x8004C19C): 0x8004C1F4,
        ("ovl0", 0x8004E064): 0x8004E084,
        ("ovl0", 0x80050C14): 0x80050C48,
        ("ovl0", 0x80050C1C): 0x80050C50,
        ("ovl0", 0x80058070): 0x800580AC,
        ("ovl0", 0x80058078): 0x800580B4,
        ("ovl0", 0x80058924): 0x80058960,
        ("ovl0", 0x8005892C): 0x80058968,
        ("ovl0", 0x80059A08): 0x80059A50,
        ("ovl2", 0x8001536C): 0x8001536C,
        ("ovl2", 0x8001E578): 0x8001E524,
        ("ovl2", 0x8001E580): 0x8001E52C,
        ("ovl2", 0x800201C4): 0x80020170,
        ("ovl2", 0x800201CC): 0x80020178,
        ("ovl4", 0x8001CDF4): 0x8001CD68,
        ("ovl4", 0x8001CDFC): 0x8001CD70,
        ("ovl4", 0x8001CE04): 0x8001CD78,
        ("resident", 0x8007D23C): 0x8007D284,
        ("resident", 0x8007D288): 0x8007D2D0,
        ("resident", 0x80080B88): 0x80080BD0,
        ("resident", 0x80080B94): 0x80080BDC,
        ("resident", 0x8008C338): 0x8008C38C,
        ("resident", 0x8008C4A8): 0x8008C4FC
    },
    card_task=0x8007EB14,
    card_ctx=0x801F0710,
    interval=0x001D5894,
    interval_ref=("ovl0", 0x80013844),
)

# Japan (NTSC): Disc 2 "Gran Turismo" SCPS-10117 Rev 1 and Disc 1 Arcade SCPS-10116. Derived 2026-11-27 with
# `gt2psp derive` from SIM and ARC, 0 problems (Arcade: resident text end and the ovl2 data table found by the raw
# fallback - pointers moved by the table's own distance).
JP_SIM = replace(
    SIM, name="jp_sim", kind="sim", exe="SCPS_101.17", memprof=False,
    boot_text=(0x80010000, 0x80011DF4),
    resident_text=(0x8005D4D0, 0x8008DF18),
    setjmp=0x8007AC48,
    longjmp=0x8007AC80,
    ovl_loader=0x8005D9A8,
    overlay_entry_table=0x80090FDC,
    extra_starts={
        "resident": {0x8007E958, 0x8007E960, 0x8007E968, 0x8007E970, 0x8007E978, 0x8007E980, 0x8007E988, 0x8007E990, 0x8007E998}
    },
    extra_resume={},
    resume_inside={},
    text_exclude={},
    patch_map={
        ("ovl0", 0x800100D0): 0x800100D0,
        ("ovl0", 0x800100D4): 0x800100D4,
        ("ovl0", 0x8001435C): 0x80014354,
        ("ovl0", 0x800168C8): 0x800168C0,
        ("ovl0", 0x80018E90): 0x80018E88,
        ("ovl0", 0x80018FB8): 0x80018FB0,
        ("ovl0", 0x80019098): 0x80019090,
        ("ovl0", 0x800191BC): 0x800191B4,
        ("ovl0", 0x8001929C): 0x80019294,
        ("ovl0", 0x800193C8): 0x800193C0,
        ("ovl0", 0x800194BC): 0x800194B4,
        ("ovl0", 0x800195EC): 0x800195E4,
        ("ovl0", 0x80019644): 0x8001963C,
        ("ovl0", 0x80020418): 0x80020410,
        ("ovl0", 0x80020428): 0x80020420,
        ("ovl0", 0x80029550): 0x80029570,
        ("ovl0", 0x800295EC): 0x8002960C,
        ("ovl0", 0x800295F0): 0x80029610,
        ("ovl0", 0x800497C8): 0x80049724,
        ("ovl0", 0x80049F48): 0x80049EA4,
        ("ovl0", 0x80049F50): 0x80049EAC,
        ("ovl0", 0x8004C194): 0x8004C0F0,
        ("ovl0", 0x8004C19C): 0x8004C0F8,
        ("ovl0", 0x8004E064): 0x8004DF64,
        ("ovl0", 0x80050C14): 0x80050B14,
        ("ovl0", 0x80050C1C): 0x80050B1C,
        ("ovl0", 0x80058070): 0x80057F40,
        ("ovl0", 0x80058078): 0x80057F48,
        ("ovl0", 0x80058924): 0x800587F4,
        ("ovl0", 0x8005892C): 0x800587FC,
        ("ovl0", 0x80059A08): 0x800598D8,
        ("ovl2", 0x8001536C): 0x8001536C,
        ("ovl2", 0x8001E578): 0x8001E3A0,
        ("ovl2", 0x8001E580): 0x8001E3A8,
        ("ovl2", 0x800201C4): 0x8001FFBC,
        ("ovl2", 0x800201CC): 0x8001FFC4,
        ("ovl4", 0x8001CDF4): 0x8001C9D4,
        ("ovl4", 0x8001CDFC): 0x8001C9DC,
        ("ovl4", 0x8001CE04): 0x8001C9E4,
        ("resident", 0x8007D23C): 0x8007D12C,
        ("resident", 0x8007D288): 0x8007D178,
        ("resident", 0x80080B88): 0x80080A78,
        ("resident", 0x80080B94): 0x80080A84,
        ("resident", 0x8008C338): 0x8008C230,
        ("resident", 0x8008C4A8): 0x8008C3A0
    },
    card_task=0x8007E9BC,
    card_ctx=0x801F0540,
    interval=0x001D56C4,
    interval_ref=("ovl0", 0x8001383C),
)
JP_ARC = replace(
    ARC, name="jp_arc", kind="arc", exe="SCPS_101.16",
    boot_text=(0x80010000, 0x80011DD0),
    resident_text=(0x8005D210, 0x8008DB30),
    setjmp=0x8007A878,
    longjmp=0x8007A8B0,
    ovl_loader=0x8005D6E8,
    ovl_jump=0x8005D68C,
    overlay_entry_table=0x800900DC,
    extra_starts={
        "resident": {0x8007E584, 0x8007E58C, 0x8007E594, 0x8007E59C, 0x8007E5A4, 0x8007E5AC, 0x8007E5B4, 0x8007E5BC, 0x8007E5C4}
    },
    extra_resume={
        "ovl2": {0x80014234},
        "resident": {0x800805AC, 0x800805CC}
    },
    resume_inside={
        "resident": {0x80082F50, 0x80082F68}
    },
    text_exclude={
        "ovl2": [(0x8004E480, 0x8004EB98)]
    },
    patch_map={
        ("ovl0", 0x800100D0): 0x800100D0,
        ("ovl0", 0x800100D4): 0x800100D4,
        ("ovl0", 0x8001435C): 0x80014324,
        ("ovl0", 0x800168C8): 0x800167EC,
        ("ovl0", 0x80018E90): 0x80018D9C,
        ("ovl0", 0x80018FB8): 0x80018EC4,
        ("ovl0", 0x80019098): 0x80018FA4,
        ("ovl0", 0x800191BC): 0x800190C8,
        ("ovl0", 0x8001929C): 0x800191A8,
        ("ovl0", 0x800193C8): 0x800192D4,
        ("ovl0", 0x800194BC): 0x800193C8,
        ("ovl0", 0x800195EC): 0x800194F8,
        ("ovl0", 0x80019644): 0x80019550,
        ("ovl0", 0x80020418): 0x80020324,
        ("ovl0", 0x80020428): 0x80020334,
        ("ovl0", 0x80029550): 0x800294A0,
        ("ovl0", 0x800295EC): 0x8002953C,
        ("ovl0", 0x800295F0): 0x80029540,
        ("ovl0", 0x800497C8): 0x80049718,
        ("ovl0", 0x80049F48): 0x80049E54,
        ("ovl0", 0x80049F50): 0x80049E5C,
        ("ovl0", 0x8004C194): 0x8004C0A0,
        ("ovl0", 0x8004C19C): 0x8004C0A8,
        ("ovl0", 0x8004E064): 0x8004DC90,
        ("ovl0", 0x80050C14): 0x80050804,
        ("ovl0", 0x80050C1C): 0x8005080C,
        ("ovl0", 0x80058070): 0x80057C80,
        ("ovl0", 0x80058078): 0x80057C88,
        ("ovl0", 0x80058924): 0x80058534,
        ("ovl0", 0x8005892C): 0x8005853C,
        ("ovl0", 0x80059A08): 0x80059618,
        ("ovl2", 0x8001536C): 0x80015340,
        ("ovl2", 0x8001E578): 0x8001E374,
        ("ovl2", 0x8001E580): 0x8001E37C,
        ("ovl2", 0x800201C4): 0x8001FF90,
        ("ovl2", 0x800201CC): 0x8001FF98,
        ("ovl4", 0x8001CDF4): 0x8001C9D4,
        ("ovl4", 0x8001CDFC): 0x8001C9DC,
        ("ovl4", 0x8001CE04): 0x8001C9E4,
        ("resident", 0x8007D23C): 0x8007CD58,
        ("resident", 0x8007D288): 0x8007CDA4,
        ("resident", 0x80080B88): 0x80080690,
        ("resident", 0x80080B94): 0x8008069C,
        ("resident", 0x8008C338): 0x8008BE48,
        ("resident", 0x8008C4A8): 0x8008BFB8
    },
    card_task=0x8007E5E8,
    card_ctx=0x801EEA20,
    interval=0x001D5CB4,
    interval_ref=("ovl0", 0x8001380C),
)

PROFILES = {p.name: p for p in (SIM, ARC, SIM_PLUS, ARC_PLUS, EU_SIM, EU_ARC, JP_SIM, JP_ARC)}


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
    Edition("eur", "GT2PSPEU", "Gran Turismo 2 (Europe)", "GT2PSPEU0"),
    Edition("plus_eur", "GT2PLUSEU", "Gran Turismo 2 Plus (Europe)", "GT2PLUSE0"),
    Edition("jpn", "GT2PSPJP", "Gran Turismo 2 (Japan)", "GT2PSPJP0"),
)}


@dataclass
class Disc:
    sha1: str                 # of the whole .bin (single track, MODE2/2352) of the dump the profile was made from
    profile: str              # PROFILES key
    edition: str              # EDITIONS key
    title: str                # shown in the converter
    code: str = ""            # code id (code_id): SHA-1 over the main EXE + GT2.OVL - what the profile depends on


DISCS = {d.sha1: d for d in (
    Disc("c4813703d2ad440ed36670c1ac2c1a66c9dc673d", "sim", "usa", "Gran Turismo 2 (USA) Simulation Mode, Rev 2",
         code="bdb49477ff865301a6a50efd9032241917f8c909"),
    Disc("a617b948e23c198a4d0eb521b874d0f563987d98", "arc", "usa", "Gran Turismo 2 (USA) Arcade Mode",
         code="cf2314fc372deb732a9d9557007ab61d53b4efa9"),
    Disc("6b36e3569e16547efd8d6cc65ca41774e2d21dd4", "sim_plus", "plus_usa",
         "Gran Turismo 2 Plus Beta v6.1 (USA) Simulation Mode", code="b77d78fe46e98832515514df6b848f28ac36fc48"),
    Disc("75d36eebc5b3fb5e767b860173a43e9682d96b21", "arc_plus", "plus_usa",
         "Gran Turismo 2 Plus Beta v6.1 (USA) Arcade Mode", code="69b7c99456c0da00d88291f2f158b13b234b1960"),
    Disc("c63a53bcf5ac6e49547a0c95b05174deff172021", "eu_sim", "eur",
         "Gran Turismo 2 (Europe) Disc 2 Gran Turismo Mode", code="c93222b0d559d9e8da09a373b5c3bb646189878f"),
    Disc("a04cec5a45c4b3e4667e08df895f0018b83cb2d2", "eu_arc", "eur", "Gran Turismo 2 (Europe) Disc 1 Arcade Mode",
         code="9fc6ec55c03004a77b59c88917c058247889694d"),
    Disc("c43b1daf666dfd5ee7a0a9cc5cb0b30ed0d1de41", "eu_arc", "plus_eur",
         "Gran Turismo 2 Plus Beta v6.1 (Europe) Arcade Mode", code="9fc6ec55c03004a77b59c88917c058247889694d"),
    # GT2 Plus Beta 7, patched onto the original discs: same layout as the known code (derive: identical profiles,
    # 0 problems - USA Simulation = sim/sim_plus, USA Arcade = arc_plus, Europe = eu_sim/eu_arc); the EXE and
    # overlays differ in place (a x*1000/1014 scaling nop'ed out, data)
    Disc("b5728472684ef64131f242ce91bf45abf9d427d0", "sim_plus", "plus_usa",
         "Gran Turismo 2 Plus Beta 7 (USA) Simulation Mode", code="f2c8eb00ba630b532aa6eaa30fa80eeeac4b3433"),
    Disc("319fedd941b2e8955ef2fe4b35d816c6798b3206", "arc_plus", "plus_usa",
         "Gran Turismo 2 Plus Beta 7 (USA) Arcade Mode", code="b366f2c9d58385ffe704a1097e35659ce03f8121"),
    Disc("b59058dd85d5eda7781dab4b4977397f6ab10a8e", "eu_sim", "plus_eur",
         "Gran Turismo 2 Plus Beta 7 (Europe) Gran Turismo Mode", code="127ba130916db7fbfcc26ac23d219ef4f56b0ec9"),
    Disc("260a1f48a43fab43d769c18612b49683c79561ac", "eu_arc", "plus_eur",
         "Gran Turismo 2 Plus Beta 7 (Europe) Arcade Mode", code="3cc62cd16ed8350845b8d12656d485b50df408da"),
    Disc("1e362a9edc6aea22fc628390fefeefacf793cc4e", "jp_sim", "jpn", "Gran Turismo 2 (Japan) Disc 2 Gran Turismo, Rev 1",
         code="258fe65175cdf6b465c22f3ec70760992bc279a2"),
    Disc("d98c0143c902768eeea85765cefc74063e75c972", "jp_arc", "jpn", "Gran Turismo 2 (Japan) Disc 1 Arcade Mode",
         code="d2e79d7bc8cf2b4043e3645c7b1ceba790db2441"),
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


# GT2 Plus is a data mod: a Plus disc whose code matches an original disc uses that disc's profile with the Plus edition
PLUS_EDITION = {"usa": "plus_usa", "eur": "plus_eur"}
BASE_EDITION = {v: k for k, v in PLUS_EDITION.items()}


def code_id(disc) -> tuple[str, bool]:
    """(code id, is GT2 Plus) of a disc image (disc.Disc). The code id hashes only what the recompiled code and the
    profile depend on - the main executable and GT2.OVL - so self-patched or differently dumped images (other EDC/ECC,
    padding, data mods) are recognised by their game code. GT2 Plus discs carry the volume id GRANTURISMO2PLUS."""
    import hashlib
    import re
    files = {e.path.upper(): e for e in disc.walk() if not e.is_dir}
    exe = sorted(p for p in files if re.fullmatch(r"S[A-Z]{3}_\d{3}\.\d{2}", p))
    h = hashlib.sha1()
    for p in exe + ["GT2.OVL"]:
        if p in files:
            h.update(disc.read(files[p].lba, files[p].size))
    return h.hexdigest(), "PLUS" in disc.volume_id().upper()


def identify(disc) -> Disc | None:
    """The profile and edition of a disc image by its game code (code_id); None when the code is unknown."""
    code, plus = code_id(disc)
    same = [d for d in DISCS.values() if d.code == code]
    if not same:
        return None
    for d in same:
        if d.edition.startswith("plus") == plus:
            return d
    d = same[0]
    ed = (PLUS_EDITION if plus else BASE_EDITION).get(d.edition)
    if not ed:
        return None
    kind = "Simulation Mode" if PROFILES[d.profile].kind == "sim" else "Arcade Mode"
    return Disc("", d.profile, ed, f"{EDITIONS[ed].title} {kind} (game code of: {d.title})", code)


def edition_of(profile: str, edition: str | None = None) -> Edition:
    """The edition a build of this profile is for: given, or the only one the profile has (DISCS)."""
    eds = {d.edition for d in DISCS.values() if d.profile == profile}
    if edition and edition not in eds:
        # identify(): a Plus disc with an original disc's code (or the other way round)
        eds |= {PLUS_EDITION[e] for e in eds if e in PLUS_EDITION} | {BASE_EDITION[e] for e in eds if e in BASE_EDITION}
    if edition:
        if edition not in eds:
            raise KeyError(f"profile {profile} is not part of edition {edition} ({', '.join(sorted(eds))})")
        return EDITIONS[edition]
    if len(eds) != 1:
        raise KeyError(f"profile {profile} belongs to {len(eds)} editions: pass --edition ({', '.join(sorted(eds))})")
    return EDITIONS[eds.pop()]
