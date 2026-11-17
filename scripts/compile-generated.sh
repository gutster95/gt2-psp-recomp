#!/bin/bash
# Compile-check the recompiled C code with psp-gcc (inside GT2PSPDev). Usage: compile-generated.sh [files...]
set -euo pipefail
. /etc/profile.d/pspdev.sh
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/gt2gen   # Linux FS: much faster than /mnt/c
mkdir -p "$OUT"
FILES=("$@")
[ ${#FILES[@]} -eq 0 ] && FILES=("$PROJ"/generated/*.c)
for f in "${FILES[@]}"; do
    b=$(basename "$f" .c)
    ( /usr/bin/time -f "$b: %es %MKB" psp-gcc -O2 -G0 -fno-strict-aliasing -w -c "$f" \
        -I"$PROJ/runtime/include" -I"$PROJ/generated" -o "$OUT/$b.o" && psp-size "$OUT/$b.o" | tail -1 ) &
done
wait
