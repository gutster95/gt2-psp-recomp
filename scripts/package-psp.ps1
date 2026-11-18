# Assemble a folder ready to copy onto a real PSP memory stick:
#   build\psp\PSP\GAME\GT2PSP\{EBOOT.PBP, GT2.PAK}
# GT2.PAK comes from `python -m gt2psp pack <cue>` (converted once on the PC from the user's disc).
# Game files are hard links (no extra disk space; copying the folder copies the real data). build\ is git-ignored.
$proj = Split-Path -Parent $PSScriptRoot
$g = Join-Path $proj "build\psp\PSP\GAME\GT2PSP"
New-Item -ItemType Directory -Force $g | Out-Null
foreach ($old in "GT2.BIN", "SCUS_944.88") { Remove-Item (Join-Path $g $old) -ErrorAction SilentlyContinue }
Copy-Item (Join-Path $proj "build\gt2psp\EBOOT.PBP") $g -Force
$links = @{
    "GT2.PAK"     = Join-Path $proj "build\GT2.PAK"
}
foreach ($name in $links.Keys) {
    $dst = Join-Path $g $name
    if (Test-Path $dst) { Remove-Item $dst -Force }
    New-Item -ItemType HardLink -Path $dst -Target $links[$name] | Out-Null
}
Get-ChildItem $g | Select-Object Name, Length
