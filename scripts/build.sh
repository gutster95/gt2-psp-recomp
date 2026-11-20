#!/bin/bash
# Build the game EBOOT inside GT2PSPDev. Needs `python -m gt2psp recomp` to have been run (generated/).
# Output: build/gt2psp/EBOOT.PBP
set -euo pipefail
. /etc/profile.d/pspdev.sh
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
B=${GT2_BUILD_DIR:-/root/gt2build}    # Linux FS: much faster than /mnt/c
mkdir -p "$B"
cd "$B"
psp-cmake -DGEN_EXTRA="${GEN_EXTRA--Os -DGT2_NO_TRACE -DGT2_GTE_OUTLINE}" -DRT_EXTRA="${RT_EXTRA-}" -DALL_EXTRA="${ALL_EXTRA-}" "$PROJ/runtime" >/dev/null   # re-run: picks up new source files
make -j"$(nproc)" > make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
grep -E "warning" make.log | head -20 || true
test -f EBOOT.PBP
OUT="$PROJ/${GT2_OUT:-build/gt2psp}"
mkdir -p "$OUT"
cp EBOOT.PBP gt2psp.prx gt2psp "$OUT/"   # prx + elf: PSPLink / psp-gdb
ls -l "$OUT/EBOOT.PBP"
