#!/bin/bash
# Build the toolchain smoke-test EBOOT inside GT2PSPDev.
set -euo pipefail
. /etc/profile.d/pspdev.sh
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
B="$PROJ/build/hello"
mkdir -p "$B"
cd "$B"
psp-cmake "$PROJ/runtime/hello" >/dev/null
make
ls -l EBOOT.PBP
