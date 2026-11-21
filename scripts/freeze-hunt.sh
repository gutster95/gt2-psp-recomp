#!/bin/bash
# Freeze hunt (2026-11-21): runs the Simulation release on the PSP through PSPLink with today's changes switched on
# one at a time (settings.txt toggles), each config N times (attract demo + GT mode via build/input-gtmode.txt, pack
# from the memory stick). A run counts as frozen when the log stops early or PSPLink no longer answers afterwards;
# the hunt stops there (the PSP then needs a power cycle).   scripts/freeze-hunt.sh [runs per config] [first config]
cd "$(dirname "$0")/.."
N=${1:-3}
FIRST=${2:-0}
R=build/psp/PSP/GAME/GT2PSP
T=tools/downloads/pspsh-windows-3.2.1/x/psplinkusb-windows
O=build/freeze; mkdir -p $O
BASE="widescreen=1\nfps=1\n"
OFF="cd_legacy=1\ncard_fast=0\nme_snapshot=0\nlog_prio=72\n"
CONFIGS=(
  "all-off|${OFF}"
  "snapshot|cd_legacy=1\ncard_fast=0\nme_snapshot=1\nlog_prio=72\n"
  "logprio|cd_legacy=1\ncard_fast=0\nme_snapshot=0\nlog_prio=31\n"
  "cardfast|cd_legacy=1\ncard_fast=1\nme_snapshot=0\nlog_prio=72\n"
  "cdfast|cd_legacy=0\ncard_fast=0\nme_snapshot=0\nlog_prio=72\n"
  "all-on|"
)
alive() { (cd $T && timeout 8 ./pspsh.exe -e "ls host0:/" 2>&1 | grep -q Listing); }
echo "ms0:/PSP/GAME/GT2PSP/GT2.PAK" > $R/pakpath.txt
for ((i = FIRST; i < ${#CONFIGS[@]}; i++)); do
  name=${CONFIGS[$i]%%|*}; set=${CONFIGS[$i]#*|}
  for ((k = 1; k <= N; k++)); do
    alive || { echo "PSPLink not answering before $name run $k - power cycle the PSP"; exit 2; }
    printf "${BASE}${set}" > $R/settings.txt
    rm -f $R/gt2log.txt
    GT2_PRX_DIR=build/gt2psp timeout 220 bash scripts/psp-run.sh 100 build/input-gtmode.txt > /dev/null 2>&1
    cp $R/gt2log.txt $O/$name-$k.log 2>/dev/null
    vb=$(grep -oE "^gpu: vblank [0-9]+" $O/$name-$k.log | tail -1 | awk '{print $3}')
    sus=$(grep -m1 "^suspects" $O/$name-$k.log)
    if [ "${vb:-0}" -lt 5400 ] || ! alive; then
      echo "FREEZE: $name run $k (log up to vblank ${vb:-0}) [$sus]"
      rm -f $R/pakpath.txt; exit 1
    fi
    echo "ok: $name run $k (vblank $vb) [$sus]"
  done
done
rm -f $R/pakpath.txt
echo "no freeze in any config"
