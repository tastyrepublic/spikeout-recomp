# A report's stack addresses as function names: a crash report (crash-*.txt), a hang's hang-stacks.txt, or any text with
# lines like "spikeout.exe+0x91c36e" (Windows) or "spikeout(+0x1234)" (Linux). The release's program with its symbols
# comes from the draft release symbols-v<VERSION> (.github/workflows/release.yml keeps it there, never in the downloads),
# downloaded once into -Cache. Each address becomes the function it falls in, and how far into it.
#   tools/symbolize.ps1 -File hang-stacks.txt -Version 0.1.0-preview4 [-Platform windows-x64|linux-x64] [-Exe PROGRAM]
# -Exe: a program with symbols already here (a build of your own: the same build the report came from, or the names lie).
# Needs nm (MinGW's or binutils, on the PATH). macOS reports: atos with the release's dSYM instead.
param([Parameter(Mandatory)][string]$File, [string]$Version = '', [string]$Platform = 'windows-x64', [string]$Exe = '',
      [string]$Cache = "$env:LOCALAPPDATA\spikeout-symbols", [string]$Repo = 'tastyrepublic/spikeout-recomp')
$ErrorActionPreference = 'Stop'
if (-not $Exe) {
    if (-not $Version) { # (the report's own first line says it: "Spikeout 0.1.0-preview4 ...")
        $first = Get-Content $File | Where-Object { $_ -match 'Spikeout (\d+\.\d+\.\d+[^ ]*)' } | Select-Object -First 1
        if ($first -match 'Spikeout (\d+\.\d+\.\d+[^ ]*)') { $Version = $Matches[1] } else { 'no -Version, and the file does not say one'; exit 1 }
    }
    $dir = Join-Path $Cache "$Version-$Platform"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Exe = Get-ChildItem $dir -File | Where-Object { $_.Name -match '^spikeout(-jit)?(\.exe)?$' } | Select-Object -First 1 -ExpandProperty FullName
    if (-not $Exe) {
        "downloading the symbols of $Version ($Platform)..."
        gh release download "symbols-v$Version" --repo $Repo --pattern "*$Platform-debug-symbols.*" --dir $dir --clobber
        if ($LASTEXITCODE -ne 0) { "no symbols for $Version $Platform (symbols-v$Version)"; exit 1 }
        Get-ChildItem $dir -File | Where-Object { $_.Name -like '*.zip' } | ForEach-Object { Expand-Archive -Force $_.FullName $dir }
        Get-ChildItem $dir -File | Where-Object { $_.Name -like '*.tar.gz' } | ForEach-Object { tar -xzf $_.FullName -C $dir }
        $Exe = Get-ChildItem $dir -File | Where-Object { $_.Name -match '^spikeout(-jit)?(\.exe)?$' } | Select-Object -First 1 -ExpandProperty FullName
        if (-not $Exe) { "the symbols' archive held no program"; exit 1 }
    }
}
# The program's functions in address order (cached beside it).
$table = "$Exe.functions.txt"
if (-not (Test-Path $table) -or (Get-Item $table).LastWriteTime -lt (Get-Item $Exe).LastWriteTime) {
    & nm -C -n --defined-only $Exe | Where-Object { $_ -match '^[0-9a-f]{8,16} [tTwW] ' } | Set-Content -Encoding utf8 $table
}
$syms = Get-Content $table
$addr = [uint64[]]($syms | ForEach-Object { [Convert]::ToUInt64($_.Substring(0, $_.IndexOf(' ')), 16) })
$names = $syms | ForEach-Object { $_.Substring($_.IndexOf(' ') + 3) }
# Windows: the report's offsets are from the module's base, the symbols' addresses from its ImageBase.
$base = [uint64]0
if ($Exe -like '*.exe') {
    $line = (& objdump -p $Exe | Select-String 'ImageBase').Line
    $base = [Convert]::ToUInt64(($line -split '\s+')[-1], 16)
}
function Name-Of([uint64]$a) {
    $lo = 0; $hi = $addr.Length - 1
    if ($hi -lt 0 -or $a -lt $addr[0]) { return '?' }
    while ($lo -lt $hi) { $m = [int][math]::Floor(($lo + $hi + 1) / 2); if ($addr[$m] -le $a) { $lo = $m } else { $hi = $m - 1 } }
    '{0} +0x{1:x}' -f $names[$lo], ($a - $addr[$lo])
}
foreach ($l in Get-Content $File) {
    if ($l -match '^(\s*)spikeout(-jit)?\.exe\+0x([0-9a-f]+)') { "$($Matches[1])$(Name-Of ($base + [Convert]::ToUInt64($Matches[3], 16)))" }
    elseif ($l -match '^(\s*)\S*spikeout(-jit)?\(\+0x([0-9a-f]+)\)') { "$($Matches[1])$(Name-Of ([Convert]::ToUInt64($Matches[3], 16)))" }
    else { $l }
}
