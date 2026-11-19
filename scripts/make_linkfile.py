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
hot = "".join(f"    *(.text.{n} .text.{n}.*)\n" for n in names)    # .part/.constprop clones too
out = tmpl.replace(anchor, anchor + "    /* hot code first (data/hot_functions.txt, scripts/make_linkfile.py) */\n" + hot, 1)
(proj / "runtime/ld/linkfile.prx").write_text(out, newline="\n")
print(f"linkfile.prx: {len(names)} hot functions")
