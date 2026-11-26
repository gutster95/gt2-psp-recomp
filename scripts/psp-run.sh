#!/bin/bash
# Run the current build on a real PSP through PSPLink (Git Bash on Windows).
# Needs: PSP connected by USB with PSPLink running (libusb-win32 driver for "PSP Type B").
# Host root (host0:/) = build/psp/PSP/GAME/GT2PSP (GT2.PAK + gt2psp.prx); the game's log is written there live.
#   scripts/psp-run.sh [seconds] [input-script-file]
# Leaves the game running; prints the log's statistics at the end. "scripts/psp-run.sh 0" only resets the PSP.
set -u
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$PROJ/tools/downloads/pspsh-windows-3.2.1/x/psplinkusb-windows"
ROOT="$PROJ/build/psp/PSP/GAME/GT2PSP"
SECS="${1:-60}"
INPUT="${2:-}"

if ! tasklist | grep -qi usbhostfs_pc; then
    powershell -NoProfile -Command "Start-Process -WindowStyle Hidden -WorkingDirectory '$(cygpath -w "$ROOT")' -FilePath '$(cygpath -w "$TOOLS/usbhostfs_pc.exe")' -ArgumentList '-n','.'" || exit 1
    sleep 3
fi
sh_() { (cd "$TOOLS" && timeout 15 ./pspsh.exe -e "$1" 2>&1); }

# back to a clean PSPLink (ends a running game), then wait until the shell answers again
sh_ "reset" >/dev/null
for i in $(seq 1 30); do
    sleep 1
    sh_ "ls host0:/" 2>/dev/null | grep -q "GT2.PAK" && break
done
[ "$SECS" = 0 ] && exit 0

cp "$PROJ/${GT2_PRX_DIR:-build/gt2psp}/gt2psp.prx" "$ROOT/"
rm -f "$ROOT/input.txt"
[ -n "$INPUT" ] && cp "$INPUT" "$ROOT/input.txt"
rm -f "$ROOT/gt2log.txt"
sh_ "./gt2psp.prx" | head -3
sleep "$SECS"
rm -f "$PROJ/build/psp-last-gt2log.txt"
cp "$ROOT/gt2log.txt" "$PROJ/build/psp-last-gt2log.txt" || echo "psp-run: no gt2log.txt - the game did not start (PSPLink?)" >&2
grep -E "gpu: vblank|^profile|game code|FATAL|syncs by reason|overlay load" "$ROOT/gt2log.txt" | tail -40
# race frame rate: display lists per second over the stats lines of the attract race (prims > 20000)
awk '/^gpu: vblank/ { for (i = 1; i <= NF; i++) { if ($i == "took") ms = $(i+1); if ($i == "display" && $(i+1) == "lists,") dl = $(i-1); if ($i == "prims") pr = $(i-1) }
     if (pr > 20000) { tms += ms; tdl += dl; n++ } }
     END { if (n) printf "RACE: %.1f fps over %d samples\n", tdl * 1000 / tms, n }' "$ROOT/gt2log.txt"
