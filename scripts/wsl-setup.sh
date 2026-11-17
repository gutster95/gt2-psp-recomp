#!/bin/bash
# One-time setup of the project-local WSL distro (run as root inside GT2PSPDev).
# Usage (from Windows, project root):  wsl -d GT2PSPDev -u root -- bash scripts/wsl-setup.sh
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -x /opt/pspdev/bin/psp-gcc ]; then
    echo "Unpacking pspdev into /opt ..."
    tar xzf "$PROJ/tools/downloads/pspdev.tar.gz" -C /opt
fi

cat > /etc/profile.d/pspdev.sh <<'EOF'
export PSPDEV=/opt/pspdev
export PATH="$PSPDEV/bin:$PATH"
EOF

# Build dependencies used by psp-cmake / pack-pbp etc.
if ! command -v make >/dev/null || ! command -v cmake >/dev/null || ! ldconfig -p | grep -q libmpc.so.3; then
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        make cmake python3 libusb-0.1-4 libgpgme11t64 libarchive-tools fakeroot \
        libmpc3 libmpfr6 libgmp10 libisl23 zlib1g libzstd1 libreadline8t64 libncurses6
fi

. /etc/profile.d/pspdev.sh
psp-gcc --version | head -1
