# Assemble a folder ready to copy onto a real PSP memory stick:
#   build\psp\PSP\GAME\GT2PSP\{EBOOT.PBP, GT2SIM.PRX, GT2ARC.PRX, GT2.PAK, GT2A.PAK}
# EBOOT.PBP is the launcher (start menu, scripts/build-launcher.sh), GT2SIM.PRX the Simulation Mode runtime
# (scripts/pgo.sh build -> build\gt2psp), GT2ARC.PRX the Arcade Mode runtime (GT2_GAME=arc scripts/build.sh ->
# build\arc). The packs come from `python -m gt2psp pack <cue>` (converted once on the PC from the user's discs).
# A disc whose PRX or pack is missing is left out (the launcher then starts the other one directly).
# Packs are hard links (no extra disk space; copying the folder copies the real data). build\ is git-ignored.
$proj = Split-Path -Parent $PSScriptRoot
$g = Join-Path $proj "build\psp\PSP\GAME\GT2PSP"
New-Item -ItemType Directory -Force $g | Out-Null
foreach ($old in "GT2.BIN", "SCUS_944.88") { Remove-Item (Join-Path $g $old) -ErrorAction SilentlyContinue }
Copy-Item (Join-Path $proj "build\launcher\EBOOT.PBP") $g -Force
$prx = @{ "GT2SIM.PRX" = "build\gt2psp\gt2psp.prx"; "GT2ARC.PRX" = "build\arc\gt2psp.prx" }
foreach ($name in $prx.Keys) {
    $src = Join-Path $proj $prx[$name]
    if (Test-Path $src) { Copy-Item $src (Join-Path $g $name) -Force }
}
$packs = @{ "GT2.PAK" = "build\GT2.PAK"; "GT2A.PAK" = "build\GT2A.PAK" }
foreach ($name in $packs.Keys) {
    $src = Join-Path $proj $packs[$name]
    $dst = Join-Path $g $name
    if (-not (Test-Path $src)) { continue }
    if (Test-Path $dst) { Remove-Item $dst -Force }
    New-Item -ItemType HardLink -Path $dst -Target $src | Out-Null
}
Get-ChildItem $g -File | Where-Object { $_.Name -in "EBOOT.PBP", "GT2SIM.PRX", "GT2ARC.PRX", "GT2.PAK", "GT2A.PAK" } |
    Select-Object Name, Length
