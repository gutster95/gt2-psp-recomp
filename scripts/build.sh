#!/bin/bash
# Build the game EBOOT inside GT2PSPDev. Needs `python -m gt2psp recomp` to have been run (generated/).
# Output: build/gt2psp/EBOOT.PBP
set -euo pipefail
. /etc/profile.d/pspdev.sh
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
B=/root/gt2build                      # Linux FS: much faster than /mnt/c
mkdir -p "$B"
cd "$B"
psp-cmake -DGEN_EXTRA="${GEN_EXTRA:-}" "$PROJ/runtime" >/dev/null   # re-run: picks up new source files
make -j"$(nproc)" > make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
grep -E "warning" make.log | head -20 || true
test -f EBOOT.PBP
mkdir -p "$PROJ/build/gt2psp"
cp EBOOT.PBP gt2psp.prx gt2psp "$PROJ/build/gt2psp/"   # prx + elf: PSPLink / psp-gdb
ls -l "$PROJ/build/gt2psp/EBOOT.PBP"
