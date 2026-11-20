#!/bin/bash
# Profile-guided build of the EBOOT (Git Bash on Windows, WSL distro GT2PSPDev).
#   scripts/pgo.sh profile   instrumented build, attract race on the real PSP through PSPLink (autoexit after 170 s
#                            writes the counters to host0:/pgo), profile copied to build/pgo-data/
#   scripts/pgo.sh build     optimized build with that profile -> build/gt2psp/EBOOT.PBP (+ prx, elf)
# The .gcda file names contain the object paths, so both steps use the same WSL build dir (/root/gt2pgo) and flags.
# A stale profile is fine: functions whose code changed lose their profile (warning suppressed), the rest keep it -
# re-profile after bigger changes. The generated code is built with -O2 here (cold parts end up optimized for size).
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
W="$(cygpath -w "$PROJ")"
ROOT="$PROJ/build/psp/PSP/GAME/GT2PSP"
GEN="-O2 -DGT2_NO_TRACE -DGT2_GTE_OUTLINE"
wslrun() { MSYS_NO_PATHCONV=1 wsl -d GT2PSPDev -u root --cd "$W" -- "$@"; }

case "${1:-}" in
profile)
    wslrun env GT2_BUILD_DIR=/root/gt2pgo GT2_OUT=build/pgogen GEN_EXTRA="$GEN" \
        ALL_EXTRA="-fprofile-generate=host0:/pgo -fprofile-update=single -DGT2_PGO_GEN" bash scripts/build.sh
    mkdir -p "$ROOT/pgo"
    find "$ROOT/pgo" -maxdepth 1 -name '*.gcda' -delete
    cp "$ROOT/settings.txt" "$PROJ/build/settings.pgo.bak"
    printf "widescreen=1\nautoexit=170\nme_gpu=0\n" > "$ROOT/settings.txt"   # __gcov_dump hangs after the ME ran
    GT2_PRX_DIR=build/pgogen bash "$PROJ/scripts/psp-run.sh" 200 > /dev/null 2>&1 || true
    cp "$PROJ/build/settings.pgo.bak" "$ROOT/settings.txt"
    grep -q "profile data written" "$PROJ/build/psp-last-gt2log.txt" || { echo "no profile written (see build/psp-last-gt2log.txt)"; exit 1; }
    mkdir -p "$PROJ/build/pgo-data"
    find "$PROJ/build/pgo-data" -maxdepth 1 -name '*.gcda' -delete
    cp "$ROOT"/pgo/*.gcda "$PROJ/build/pgo-data/"
    echo "profile: $(ls "$PROJ/build/pgo-data" | wc -l) files in build/pgo-data"
    ;;
build)
    test -n "$(ls "$PROJ/build/pgo-data"/*.gcda 2>/dev/null)" || { echo "no profile: run scripts/pgo.sh profile"; exit 1; }
    wslrun bash -c 'rm -rf /root/pgo-data && mkdir -p /root/pgo-data && cp build/pgo-data/*.gcda /root/pgo-data/'
    wslrun env GT2_BUILD_DIR=/root/gt2pgo GT2_OUT=build/gt2psp GEN_EXTRA="$GEN" \
        ALL_EXTRA="-fprofile-use=/root/pgo-data -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch -Wno-error=coverage-mismatch" \
        bash scripts/build.sh
    ;;
*)
    echo "usage: scripts/pgo.sh profile|build"; exit 1;;
esac
