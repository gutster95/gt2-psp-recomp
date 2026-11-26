# Stage the EBOOT + converted game data into PPSSPP's portable memstick, run for N seconds, screenshot, show log.
# Needs build\gt2psp\EBOOT.PBP (scripts/build.sh) and build\GT2.PAK (python -m gt2psp pack <cue>).
param([int]$Seconds = 15, [string]$Shot = "")
$proj = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$g = Join-Path $proj "tools\ppsspp\memstick\PSP\GAME\GT2PSP"
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item (Join-Path $proj "build\gt2psp\EBOOT.PBP") $g
Remove-Item "$g\SCUS_944.88" -ErrorAction SilentlyContinue
if (-not (Test-Path "$g\GT2.PAK")) {
    New-Item -ItemType HardLink -Path "$g\GT2.PAK" -Target (Join-Path $proj "build\GT2.PAK") | Out-Null
}
Remove-Item "$g\gt2log.txt" -ErrorAction SilentlyContinue
$p = Start-Process (Join-Path $proj "tools\ppsspp\PPSSPPWindows64.exe") -ArgumentList "`"$g\EBOOT.PBP`"" -PassThru
Start-Sleep $Seconds
if ($Shot) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $gr = [System.Drawing.Graphics]::FromImage($bmp)
    $gr.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save($Shot)
}
Stop-Process $p -ErrorAction SilentlyContinue
Start-Sleep 1
Get-Content "$g\gt2log.txt"
