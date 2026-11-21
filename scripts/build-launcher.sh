#!/bin/bash
# Build the launcher EBOOT (runtime/launcher) inside GT2PSPDev. Output: build/launcher/EBOOT.PBP
set -euo pipefail
. /etc/profile.d/pspdev.sh
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
B=/root/gt2launcher
mkdir -p "$B"
cd "$B"
psp-cmake "$PROJ/runtime/launcher" >/dev/null
make > make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
mkdir -p "$PROJ/build/launcher"
cp EBOOT.PBP "$PROJ/build/launcher/"
ls -l "$PROJ/build/launcher/EBOOT.PBP"
