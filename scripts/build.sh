#!/bin/bash
# Build the game EBOOT (dev: inside GT2PSPDev; converter: Linux, macOS, the converter's WSL distro). Needs `python -m gt2psp recomp` to have been run (generated/).
# Output: build/gt2psp/EBOOT.PBP
set -euo pipefail
[ -f /etc/profile.d/pspdev.sh ] && . /etc/profile.d/pspdev.sh   # WSL setup; the pspdev Docker image sets PSPDEV/PATH itself
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
# GT2_WORK: scratch directory for builds (dev WSL distro: /root, on the Linux FS - much faster than /mnt/c; the
# converter on Linux/macOS: a folder the user can write)
WORK="${GT2_WORK:-/root}"
[ -n "${PSPDEV:-}" ] && export PATH="$PSPDEV/bin:$PATH"
# Media Engine library (once per setup): third_party/me-custom-core -> $WORK/mepoc/build/mecorelib
[ -f "$WORK/mepoc/build/mecorelib/libme-core-lib.a" ] || bash "$PROJ/scripts/me/build-sample.sh" poc > /dev/null
B=${GT2_BUILD_DIR:-$WORK/gt2build}
mkdir -p "$B"
cd "$B"
psp-cmake -DGT2_GAME="${GT2_GAME-sim}" -DGEN_EXTRA="${GEN_EXTRA--Os -DGT2_NO_TRACE -DGT2_GTE_OUTLINE}" -DRT_EXTRA="${RT_EXTRA-}" -DALL_EXTRA="${ALL_EXTRA-}"     -DME_LIB_DIR="$WORK/mepoc/build/mecorelib" -DME_INC_DIR="$WORK/meinc" "$PROJ/runtime" >/dev/null   # re-run: picks up new source files
make -j"$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" > make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
grep -E "warning" make.log | head -20 || true
test -f EBOOT.PBP
OUT="$PROJ/${GT2_OUT:-build/gt2psp}"
mkdir -p "$OUT"
cp EBOOT.PBP gt2psp.prx gt2psp "$OUT/"   # prx + elf: PSPLink / psp-gdb
ls -l "$OUT/EBOOT.PBP"
