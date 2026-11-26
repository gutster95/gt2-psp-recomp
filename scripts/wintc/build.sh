#!/bin/bash
# Build the PSP toolchain for Windows (native .exe, no MSYS2/Cygwin DLLs) by cross-compiling with mingw-w64 on
# Linux (the dev WSL distro or a CI runner). Release plan step 2 (docs/RELEASE-PLAN.md).
#
# Same sources as the Linux pspdev release we develop with (commits from /opt/pspdev/build.txt). Only the HOST
# programs are rebuilt (binutils, the gcc driver + compilers, the PSPSDK host tools); everything that runs on the PSP
# (newlib, libgcc, libstdc++, PSPSDK libraries, headers, linker scripts) is target code and is copied 1:1 from the
# Linux toolchain - it does not depend on the host OS.
#
# Usage (WSL, root):  bash scripts/wintc/build.sh        -> build/wintc/pspdev-win-x86_64.zip
# Needs: /opt/pspdev (the Linux toolchain, also used for gcc's configure checks), sources in
# tools/downloads/wintc-src/*.tar.gz (pinned commits, see SOURCES below).
set -euo pipefail
[ -f /etc/profile.d/pspdev.sh ] && . /etc/profile.d/pspdev.sh
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$PROJ/tools/downloads/wintc-src"
W="${WINTC_WORK:-/root/wintc}"
OUT="$W/pspdev"                       # staged Windows toolchain (becomes the zip's pspdev/ folder)
HOST=x86_64-w64-mingw32
J=$(nproc)
STEP="${1:-all}"

# SOURCES (pspdev GitHub, = /opt/pspdev/build.txt of pspdev release v20261101)
#   binutils-gdb a8b53fe2b5825fa86337623c743d21a19aeb0daf  (Allegrex 2.44 branch)
#   gcc          1a33997924916ff5a6f61b64179ff9c8921f46c6  (allegrex-v15.2.0)
#   pspsdk       c3da4314d783ac26323e5cdaf8163ac369c2089d
#   zstd         1.5.6 release tarball (sha256 8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1)

deps() {
    command -v $HOST-gcc >/dev/null && command -v flex >/dev/null && command -v makeinfo >/dev/null && return
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        mingw-w64 g++ bison flex texinfo m4 bzip2 xz-utils curl ca-certificates zip file
}

unpack() {   # unpack <name> -> $W/src/<name>
    [ -d "$W/src/$1" ] && return
    mkdir -p "$W/src/tmp"
    tar xzf "$SRC/$1.tar.gz" -C "$W/src/tmp"
    mv "$W/src/tmp/$1-"* "$W/src/$1"
}

binutils() {
    unpack binutils-gdb
    rm -rf "$W/b-binutils" && mkdir -p "$W/b-binutils" && cd "$W/b-binutils"
    "$W/src/binutils-gdb/configure" --quiet --build=x86_64-linux-gnu --host=$HOST --target=psp \
        --prefix="$OUT" --with-sysroot="$OUT/psp" --disable-initfini-array --disable-werror --disable-nls \
        --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --enable-plugins --without-zstd \
        --with-python=no LDFLAGS="-static -static-libgcc -static-libstdc++" > configure.log
    make -s -j$J all > make.log 2>&1 || { tail -40 make.log; exit 1; }
    make -s install-strip > install.log 2>&1 || { tail -40 install.log; exit 1; }
}

zstd() {   # static libzstd for the Windows host (gcc's lto1 must read the zstd-compressed LTO sections of pspdev's libs)
    unpack zstd
    make -s -C "$W/src/zstd/lib" clean > /dev/null
    make -s -C "$W/src/zstd/lib" -j$J libzstd.a CC=$HOST-gcc AR=$HOST-ar ZSTD_LEGACY_SUPPORT=0 > /dev/null
    mkdir -p "$W/zstd-win/include" "$W/zstd-win/lib"
    cp "$W/src/zstd/lib/libzstd.a" "$W/zstd-win/lib/"
    cp "$W/src/zstd/lib/zstd.h" "$W/src/zstd/lib/zstd_errors.h" "$W/src/zstd/lib/zdict.h" "$W/zstd-win/include/"
}

gcc_host() {
    [ -f "$W/zstd-win/lib/libzstd.a" ] || zstd
    unpack gcc
    if [ ! -d "$W/src/gcc/gmp" ]; then        # gmp/mpfr/mpc/isl built in-tree for the Windows host
        (cd "$W/src/gcc" && ./contrib/download_prerequisites > /dev/null)
    fi
    rm -rf "$W/b-gcc" && mkdir -p "$W/b-gcc" && cd "$W/b-gcc"
    # flags as the pspdev release (psp-gcc -v), plus the cross-host settings
    "$W/src/gcc/configure" --quiet --build=x86_64-linux-gnu --host=$HOST --target=psp --prefix="$OUT" \
        --with-sysroot="$OUT/psp" --with-native-system-header-dir=/include --enable-languages=c,c++ \
        --with-float=hard --with-newlib --disable-libssp --disable-multilib --enable-threads=posix --disable-tls \
        --disable-nls --with-zstd="$W/zstd-win" --disable-plugin \
        LDFLAGS="-static-libgcc -static-libstdc++" > configure.log   # no -static: libtool would build no DLL (lto plugin)
    # lto-plugin: pspdev's libraries contain LTO objects; without the plugin ld links their fallback code and the
    # result differs from the Linux build
    make -s -j$J all-gcc all-lto-plugin > make.log 2>&1 || { grep -E "error" make.log | head -30; tail -20 make.log; exit 1; }
    make -s install-strip-gcc install-lto-plugin > install.log 2>&1 || { tail -40 install.log; exit 1; }
    # (the gcc driver passes libexec/gcc/psp/15.2.0/liblto_plugin.dll to collect2/ld itself, as on Linux)
}

target_files() {   # PSP-side files from the Linux toolchain (identical for any host)
    mkdir -p "$OUT/lib/gcc/psp/15.2.0" "$OUT/psp"
    # everything under psp/ except psp/bin (binutils' tool dir: the Windows as/ld/ar built above)
    tar -C /opt/pspdev/psp --exclude=./bin -cf - . | tar -C "$OUT/psp" -xf -
    # libgcc, crt*.o, gcc's own target headers (install-gcc put the include dirs; take the Linux ones as the reference)
    cp -a /opt/pspdev/lib/gcc/psp/15.2.0/. "$OUT/lib/gcc/psp/15.2.0/"
}

sdk_tools() {   # PSPSDK host tools, compiled for Windows
    unpack pspsdk
    T="$W/src/pspsdk/tools"
    mkdir -p "$OUT/bin" "$W/b-tools"
    # config.h as autoconf would write it for a little-endian Windows host
    printf '#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MALLOC_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_FCNTL_H 1
#define HAVE_UNISTD_H 1
' > "$W/b-tools/config.h"
    for t in psp-prxgen psp-fixup-imports psp-build-exports mksfoex pack-pbp; do
        case $t in
            psp-prxgen) f="$T/psp-prxgen.c" ;;
            psp-fixup-imports) f="$T/psp-fixup-imports.c" ;;
            psp-build-exports) f="$T/psp-build-exports.c" ;;
            mksfoex) f="$T/mksfoex.c" ;;
            pack-pbp) f="$T/pack-pbp.c" ;;
        esac
        extra=""
        extra="$T/sha1.c"; grep -q getopt_long "$f" && extra="$extra $T/getopt_long.c"
        $HOST-gcc -O2 -s -static -DHAVE_CONFIG_H -I"$W/b-tools" -I"$T" -I"$W/src/pspsdk" -o "$OUT/bin/$t.exe" "$f" $extra 2> "$W/b-tools/$t.log" \
            || { echo "== $t"; head -30 "$W/b-tools/$t.log"; FAIL=1; }
    done
    [ -z "${FAIL:-}" ]
}

package() {
    cd "$W"
    # no Linux leftovers in the Windows bin dir
    find "$OUT/bin" -type f ! -name "*.exe" -delete
    cp /opt/pspdev/build.txt "$OUT/build-linux-reference.txt"
    {
        echo "PSP toolchain for Windows, built by scripts/wintc/build.sh (GT2 PSP project) with mingw-w64."
        echo "Sources: https://github.com/pspdev (binutils-gdb, gcc, pspsdk at the commits in build-linux-reference.txt)."
        echo "GCC and binutils are GPL-3.0; target libraries as in pspdev (newlib, pspsdk BSD)."
        echo "binutils-gdb a8b53fe2b5825fa86337623c743d21a19aeb0daf, gcc 1a33997924916ff5a6f61b64179ff9c8921f46c6,"
        echo "pspsdk c3da4314d783ac26323e5cdaf8163ac369c2089d, zstd 1.5.6 (https://github.com/facebook/zstd)."
        echo "Build script: scripts/wintc/build.sh in https://github.com/gutster95/gt2-psp-recomp"
        echo "Licence texts: licenses/."
    } > "$OUT/README-WINDOWS.txt"
    mkdir -p "$OUT/licenses"
    cp "$W/src/gcc/COPYING3" "$OUT/licenses/gcc-COPYING3"
    cp "$W/src/gcc/COPYING.RUNTIME" "$OUT/licenses/gcc-COPYING.RUNTIME"
    cp "$W/src/binutils-gdb/COPYING3" "$OUT/licenses/binutils-COPYING3"
    cp "$W/src/pspsdk/LICENSE" "$OUT/licenses/pspsdk-LICENSE"
    cp "$W/src/zstd/LICENSE" "$OUT/licenses/zstd-LICENSE"
    cp "$W/src/gcc/gmp/COPYING.LESSERv3" "$OUT/licenses/gmp-COPYING.LESSERv3" 2>/dev/null || true
    cp "$W/src/gcc/mpfr/COPYING.LESSER" "$OUT/licenses/mpfr-COPYING.LESSER" 2>/dev/null || true
    cp "$W/src/gcc/mpc/COPYING.LESSER" "$OUT/licenses/mpc-COPYING.LESSER" 2>/dev/null || true
    rm -f "$PROJ/build/wintc/pspdev-win-x86_64.zip"; mkdir -p "$PROJ/build/wintc"
    zip -qr9 "$PROJ/build/wintc/pspdev-win-x86_64.zip" pspdev
    ls -l "$PROJ/build/wintc/pspdev-win-x86_64.zip"
    sha256sum "$PROJ/build/wintc/pspdev-win-x86_64.zip"
}

mkdir -p "$W"
case "$STEP" in
    all) deps; binutils; gcc_host; target_files; sdk_tools; package ;;
    *) "$STEP" ;;
esac
echo "== $STEP done"
