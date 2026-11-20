#!/bin/bash
# Build one sample of tools/downloads/me-custom-core (MIT, mcidclan) against the library sources, inside WSL,
# without installing anything into /opt/pspdev. Output: build/me/<sample> (run: copy mecore.prx next to the EBOOT folder root of usbhostfs, pspsh "./mecore.prx")/{EBOOT.PBP,mecore.prx}
set -e
. /etc/profile.d/pspdev.sh
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
SAMPLE=${1:-poc}
rm -rf /root/me-src /root/meinc /root/mepoc
cp -r "$PROJ/tools/downloads/me-custom-core" /root/me-src
mkdir -p /root/meinc && ln -s /root/me-src /root/meinc/me-core-mapper
# psp-build-exports -s writes kcall.S itself; the redirect in the upstream CMakeLists left it empty
sed -i 's#COMMAND psp-build-exports -s ${CMAKE_SOURCE_DIR}/kernel/exports.exp > kcall.S#COMMAND psp-build-exports -s ${CMAKE_CURRENT_SOURCE_DIR}/kernel/exports.exp#' /root/me-src/CMakeLists.txt
grep -n "psp-build-exports -s" /root/me-src/CMakeLists.txt
mkdir -p /root/mepoc/src /root/mepoc/build
cat > /root/mepoc/src/CMakeLists.txt <<CM
cmake_minimum_required(VERSION 3.10)
set(CMAKE_TOOLCHAIN_FILE "\$ENV{PSPDEV}/psp/share/pspdev.cmake" CACHE PATH "toolchain file")
project(mepoc C CXX ASM)
add_subdirectory(/root/me-src mecorelib)
add_executable(mecore /root/me-src/samples/$SAMPLE/main.cpp)
target_include_directories(mecore PRIVATE /root/meinc)
target_compile_options(mecore PRIVATE -Os -G0 -Wall -fno-pic \$<\$<COMPILE_LANGUAGE:CXX>:-std=c++11 -fno-rtti -fno-exceptions>)
target_link_libraries(mecore me-core me-core-lib psppower pspge pspdisplay pspctrl pspdebug)
create_pbp_file(TARGET mecore TITLE "Me Core Demo" BUILD_PRX)
CM
cd /root/mepoc/build
which xxd > /dev/null || { echo "xxd missing"; exit 1; }
psp-cmake ../src > /dev/null
make 2>&1 | grep -E "error|Error|Built target" | head -20
mkdir -p "$PROJ/build/me/$SAMPLE"
cp mecore.prx EBOOT.PBP "$PROJ/build/me/$SAMPLE/"
ls -l "$PROJ/build/me/$SAMPLE"
