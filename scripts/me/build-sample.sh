#!/bin/bash
# Build one sample of tools/downloads/me-custom-core (MIT, mcidclan) against the library sources, inside WSL,
# without installing anything into /opt/pspdev. Output: build/me/<sample> (run: copy mecore.prx next to the EBOOT folder root of usbhostfs, pspsh "./mecore.prx")/{EBOOT.PBP,mecore.prx}
set -e
[ -f /etc/profile.d/pspdev.sh ] && . /etc/profile.d/pspdev.sh   # WSL setup; the pspdev Docker image sets PSPDEV/PATH itself
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${GT2_WORK:-/root}"
case "$WORK" in *" "*) echo "GT2_WORK must not contain spaces: $WORK"; exit 1;; esac
[ -n "${PSPDEV:-}" ] && export PATH="$PSPDEV/bin:$PATH"
SAMPLE=${1:-poc}
rm -rf $WORK/me-src $WORK/meinc $WORK/mepoc
ME_SRC="${ME_SRC:-$PROJ/third_party/me-custom-core}"   # MIT, vendored (third_party/)
cp -r "$ME_SRC" $WORK/me-src
mkdir -p $WORK/meinc && ln -s $WORK/me-src $WORK/meinc/me-core-mapper
# own samples: scripts/me/<sample>/main.cpp
if [ -f "$PROJ/scripts/me/$SAMPLE/main.cpp" ]; then mkdir -p $WORK/me-src/samples/$SAMPLE; cp "$PROJ/scripts/me/$SAMPLE/main.cpp" $WORK/me-src/samples/$SAMPLE/; fi
# psp-build-exports -s writes kcall.S itself; the redirect in the upstream CMakeLists left it empty
sed -i.bak 's#COMMAND psp-build-exports -s ${CMAKE_SOURCE_DIR}/kernel/exports.exp > kcall.S#COMMAND psp-build-exports -s ${CMAKE_CURRENT_SOURCE_DIR}/kernel/exports.exp#' $WORK/me-src/CMakeLists.txt
grep -n "psp-build-exports -s" $WORK/me-src/CMakeLists.txt
mkdir -p $WORK/mepoc/src $WORK/mepoc/build
cat > $WORK/mepoc/src/CMakeLists.txt <<CM
cmake_minimum_required(VERSION 3.10)
set(CMAKE_TOOLCHAIN_FILE "\$ENV{PSPDEV}/psp/share/pspdev.cmake" CACHE PATH "toolchain file")
project(mepoc C CXX ASM)
add_subdirectory($WORK/me-src mecorelib)
add_executable(mecore $WORK/me-src/samples/$SAMPLE/main.cpp)
target_include_directories(mecore PRIVATE $WORK/meinc)
target_compile_options(mecore PRIVATE -Os -G0 -Wall -fno-pic \$<\$<COMPILE_LANGUAGE:CXX>:-std=c++11 -fno-rtti -fno-exceptions>)
target_link_libraries(mecore me-core me-core-lib psppower pspge pspdisplay pspctrl pspdebug)
create_pbp_file(TARGET mecore TITLE "Me Core Demo" BUILD_PRX)
CM
cd $WORK/mepoc/build
which xxd > /dev/null || { echo "xxd missing"; exit 1; }
psp-cmake ../src > /dev/null
make 2>&1 | grep -E "error|Error|Built target" | head -20
mkdir -p "$PROJ/build/me/$SAMPLE"
cp mecore.prx EBOOT.PBP "$PROJ/build/me/$SAMPLE/"
ls -l "$PROJ/build/me/$SAMPLE"
