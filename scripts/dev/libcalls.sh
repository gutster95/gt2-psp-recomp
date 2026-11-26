#!/bin/bash
# Count calls to libgcc helpers (64-bit arithmetic, soft float conversions) per object file of the last build,
# or list all call targets of one object: libcalls.sh [name-fragment]
. /etc/profile.d/pspdev.sh
cd /root/gt2build/CMakeFiles/gt2psp.dir
if [ -n "${1:-}" ]; then
    f=$(find . -name "*$1*.obj" | head -1)
    psp-objdump -dr "$f" | grep -oE "R_MIPS_26\s+\S+" | awk '{print $2}' | sort | uniq -c | sort -rn | head -30
    exit 0
fi
for o in $(find . -name "*.obj"); do
    n=$(psp-objdump -dr "$o" | grep -cE "R_MIPS_26\s+__(mul|div|mod|udiv|umod|ashr|ashl|lshr)di3|R_MIPS_26\s+__float|R_MIPS_26\s+__fix")
    [ "$n" -gt 0 ] && echo "$n $o"
done | sort -rn | head -20
