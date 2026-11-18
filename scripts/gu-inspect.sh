#!/bin/bash
# Disassemble selected pspgu functions (to see what each GU call costs).
. /etc/profile.d/pspdev.sh
L=/opt/pspdev/psp/sdk/lib/libpspgu.a
for f in sceGuDrawArray sceGuEnable sceGuTexImage sceGuScissor; do
    echo "== $f"
    psp-objdump -dr "$L" | awk -v f="<$f>:" '$0 ~ f {p=1} p && /^$/ {exit} p' | grep -E "jal|R_MIPS_26|jr" | head -12
done
echo "== sendCommandi / stall"
psp-objdump -dr "$L" | grep -E "^[0-9a-f]+ <.*(endCommand|tall).*>:" | head
