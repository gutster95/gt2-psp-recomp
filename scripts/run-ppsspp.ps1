# Stage the EBOOT + user's game files into PPSSPP's portable memstick, run for N seconds, screenshot, show log.
param([int]$Seconds = 15, [string]$Shot = "")
$proj = Split-Path -Parent $PSScriptRoot
$g = Join-Path $proj "tools\ppsspp\memstick\PSP\GAME\GT2PSP"
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item (Join-Path $proj "build\gt2psp\EBOOT.PBP") $g
foreach ($f in "SCUS_944.88", "GT2.OVL", "SYSTEM.CNF") {
    if (-not (Test-Path "$g\$f")) { Copy-Item (Join-Path $proj "extracted\files\$f") $g }
}
if (-not (Test-Path "$g\GT2.VOL")) {
    New-Item -ItemType HardLink -Path "$g\GT2.VOL" -Target (Join-Path $proj "extracted\files\GT2.VOL") | Out-Null
}
if (-not (Test-Path "$g\MUSIC.DAT")) {
    New-Item -ItemType HardLink -Path "$g\MUSIC.DAT" -Target (Join-Path $proj "extracted\files\MUSIC.DAT") | Out-Null
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
