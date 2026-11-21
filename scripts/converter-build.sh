#!/bin/bash
# Compile step of the converter (needs PSPDEV; works in the WSL dev
# setup and on Linux/macOS with the pspdev release; replaced by scripts/build_psp.py, docs/RELEASE-PLAN.md). Expects the recompiled code in generated/ (Simulation) and/or generated/arcade/ (Arcade), which the converter
# produced from the user's own discs. Output: build/release/{EBOOT.PBP, GT2SIM.PRX, GT2ARC.PRX}
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJ"
OUT="$PROJ/build/release"
rm -rf "$OUT" && mkdir -p "$OUT"
echo "== linker script (hot functions first)"
python3 scripts/make_linkfile.py
echo "== Media Engine library"
bash scripts/me/build-sample.sh poc > /dev/null
FLAGS="-O2 -DGT2_NO_TRACE -DGT2_GTE_OUTLINE"
if [ -f generated/tables.c ]; then
    echo "== Simulation Mode runtime (this takes a few minutes)"
    GT2_GAME=sim GT2_BUILD_DIR=/root/gt2rel-sim GT2_OUT=build/rel-sim GEN_EXTRA="$FLAGS" bash scripts/build.sh
    cp build/rel-sim/gt2psp.prx "$OUT/GT2SIM.PRX"
fi
if [ -f generated/arcade/tables.c ]; then
    echo "== Arcade Mode runtime (this takes a few minutes)"
    GT2_GAME=arc GT2_BUILD_DIR=/root/gt2rel-arc GT2_OUT=build/rel-arc GEN_EXTRA="$FLAGS" bash scripts/build.sh
    cp build/rel-arc/gt2psp.prx "$OUT/GT2ARC.PRX"
fi
echo "== launcher"
bash scripts/build-launcher.sh
cp build/launcher/EBOOT.PBP "$OUT/EBOOT.PBP"
ls -l "$OUT"
echo "== done"
