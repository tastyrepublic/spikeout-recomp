# Draws a 3D frame dump (docs/frame-dump.md) with the patched Supermodel that tools\supermodel\build.ps1 makes in
# -Work\run: its picture (BASE.png, from its .bmp) and the models it drew (BASE-models.txt, as frame3d --models).
#   tools\supermodel\draw.ps1 -Dump FILE.m3d -Out BASE [-Res 496,384] [-Roms spikeofe.zip] [-Work DIR]
param([Parameter(Mandatory)][string]$Dump, [Parameter(Mandatory)][string]$Out, [string]$Res = '496,384',
      [string]$Roms = 'Z:\Emu\Sega Supermodel 3\ROMs\spikeofe.zip', [string]$Work = 'Z:\Emu\supermodel-build')
$run = "$Work\run"
if (-not (Test-Path "$run\supermodel.exe")) { "no patched Supermodel in ${run}: run tools\supermodel\build.ps1 first"; exit 1 }
$bmp = "$Out.bmp"
Remove-Item $bmp, "$Out.png", "$Out-models.txt" -ErrorAction SilentlyContinue
$env:SUPERMODEL_DRAW_3D = (Resolve-Path $Dump).Path
$env:SUPERMODEL_DRAW_3D_OUT = $bmp
$env:SUPERMODEL_MODELS_OUT = "$Out-models.txt"
$p = Start-Process "$run\supermodel.exe" -ArgumentList @("`"$Roms`"", "-res=$Res", '-window', '-no-gpu-thread', '-no-throttle') `
    -WorkingDirectory $run -RedirectStandardOutput "$Out-supermodel.txt" -RedirectStandardError "$Out-supermodel.err" -PassThru -NoNewWindow
$null = $p.Handle
if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; "STOPPED: Supermodel ran over 2 minutes"; exit 1 }
Remove-Item Env:SUPERMODEL_DRAW_3D, Env:SUPERMODEL_DRAW_3D_OUT, Env:SUPERMODEL_MODELS_OUT -ErrorAction SilentlyContinue
if (-not (Test-Path $bmp)) { Get-Content "$Out-supermodel.txt" -ErrorAction SilentlyContinue | Select-Object -Last 5; "no picture"; exit 1 }
Add-Type -AssemblyName System.Drawing
$i = [System.Drawing.Image]::FromFile((Resolve-Path $bmp).Path); $i.Save("$Out.png", [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose()
Remove-Item $bmp
"Supermodel: $Out.png, $(@(Get-Content "$Out-models.txt" -ErrorAction SilentlyContinue).Count) models"
