#!/bin/bash
# Disassemble one compiled function of the last build: disfn.sh <symbol> [lines]
. /etc/profile.d/pspdev.sh
cd /root/gt2build
psp-objdump -d gt2psp | awk -v f="<$1>:" '$0 ~ f {p=1} p && /^$/ {exit} p' | head -"${2:-80}"
