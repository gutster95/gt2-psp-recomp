"""Generate runtime/ld/linkfile.prx: the PSPSDK PRX linker script with the hot functions of
data/hot_functions.txt placed first in .text, in list order (all code is compiled with -ffunction-sections).

Why: the PSP's instruction cache is 16 KB, 2-way set associative. Without a fixed order, any change anywhere moved
the hot code and changed which hot functions evicted each other (up to 8% frame-rate swings for unrelated edits).
"""
import sys
from pathlib import Path

proj = Path(__file__).resolve().parent.parent
names = [l.strip() for l in (proj / "data/hot_functions.txt").read_text().splitlines()
         if l.strip() and not l.startswith("#")]
tmpl = (proj / "runtime/ld/linkfile.prx.in").read_text()
anchor = "    _ftext = . ;\n"
assert anchor in tmpl
def entry(n: str) -> str:
    if n.startswith("pad="):                         # pad=<bytes>: gap (I-cache set alignment experiments)
        return f"    . += {int(n[4:], 0)};" + chr(10)
    if n.startswith("section="):                     # section=<input section pattern>, e.g. .text.hot.*
        return f"    *({n[8:]})" + chr(10)
    if ":" in n:                                     # library member, e.g. libpspgu.a:sceGuDrawArray.o
        return f"    *{n}(.text .text.*)\n"
    # .part/.constprop clones too; .text.hot.* with profile-guided optimization (cold parts stay out: .text.unlikely)
    return f"    *(.text.{n} .text.{n}.* .text.hot.{n} .text.hot.{n}.*)\n"


hot = "".join(entry(n) for n in names)
# Then the rest of the recompiled code, at a fixed offset within a 16 KB block: runtime edits (which used to come
# first) no longer shift the game's not-listed functions against the hot ones (~10% at 60 fps for a 1 KB edit in
# cdrom.c, DEVLOG 2026-11-20). The offset reproduces a fast build; change it only by measuring.
hot += "    . = ALIGN(0x4000) + 0x254;\n    *generated/*.c.obj(.text .text.*)\n"
out = tmpl.replace(anchor, anchor + "    /* hot code first (data/hot_functions.txt, scripts/make_linkfile.py) */\n" + hot, 1)
(proj / "runtime/ld/linkfile.prx").write_text(out, newline="\n")
print(f"linkfile.prx: {len(names)} hot functions")
