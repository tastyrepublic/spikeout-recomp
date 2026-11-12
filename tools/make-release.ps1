param(
    [Parameter(Mandatory)][string]$Version,          # e.g. 0.1.0-preview1
    [string]$WindowsExe = "build-jit\spikeout-jit.exe", # the JIT window (no game code inside)
    [string]$LinuxExe = "",                            # the Linux JIT window, copied out of WSL (optional)
    [string]$Deps = "build-jit\_deps",                 # FetchContent's sources (their licences)
    [string]$Out = "build-jit\release")
# The downloads: the game's window built with the JIT (the game is translated from the player's own ROM set at its
# first start; nothing made from the ROM is inside), stripped, with the licences and a short readme:
#   spikeout-<version>-windows-x64.zip, spikeout-<version>-linux-x64.tar.gz
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repo
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory $Out | Out-Null

# Each program built as this version (cmake -DSPIKEOUT_VERSION=$Version: the menu shows it, the update check compares
# it with GitHub's releases, the lobby says it), found in the file as src/net/update.cpp marks it.
function check_version([string]$exe) {
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $exe))
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    if (-not $text.Contains("SPIKEOUT_VERSION=$Version`0")) {
        $found = [regex]::Match($text, "SPIKEOUT_VERSION=([^\x00]{1,40})\x00").Groups[1].Value
        throw "$exe is version '$found', not ${Version}: build it with -DSPIKEOUT_VERSION=${Version}"
    }
}
check_version $WindowsExe
if ($LinuxExe) { check_version $LinuxExe }

# A dependency's source folder: the build's own, else the one it was pointed at (FETCHCONTENT_SOURCE_DIR_*)
function dep_dir([string]$name) {
    $own = Join-Path $Deps "$name-src"
    if (Test-Path $own) { return $own }
    $cache = Get-Content (Join-Path (Split-Path $Deps) "CMakeCache.txt")
    $line = $cache | Where-Object { $_ -match "^FETCHCONTENT_SOURCE_DIR_$($name.ToUpper()):PATH=(.+)$" } | Select-Object -First 1
    if ($line -and ($line -match "=(.+)$")) { return $Matches[1] }
    throw "no source for $name"
}
$mit = @"
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"@
$llvm_license = Join-Path $Out "llvm-LICENSE.TXT"
tar -xf "$env:LOCALAPPDATA\spikeout-llvm\llvm-project-18.1.8.src.tar.xz" -O "llvm-project-18.1.8.src/llvm/LICENSE.TXT" > $llvm_license
$parts = @(
    @("SDL 3 (zlib licence)", (Join-Path (dep_dir sdl3) "LICENSE.txt")),
    @("RmlUi (MIT)", (Join-Path (dep_dir rmlui) "LICENSE.txt")),
    @("RmlUi's SDL GPU renderer", "third_party\rmlui_sdl_gpu\LICENSE.txt"),
    @("FreeType (FreeType Licence). Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.", (Join-Path (dep_dir freetype) "docs\FTL.TXT")),
    @("LLVM 18.1.8 (Apache 2.0 with LLVM exceptions)", $llvm_license),
    @("curl", (Join-Path (dep_dir curl) "COPYING")),
    @("Zstandard (BSD)", (Join-Path (dep_dir zstd) "LICENSE")),
    @("minimp3 (CC0)", "third_party\minimp3\LICENSE"),
    @("QR Code generator, Project Nayuki (MIT)", "third_party\qrcodegen\LICENSE"),
    @("Monocypher (CC0 or BSD-2-Clause)", "third_party\monocypher\LICENCE.md"),
    @("Moira", "third_party\moira\LICENSE"),
    @("Barlow Condensed font (SIL Open Font License)", "assets\fonts\OFL.txt"),
    @("Country flags", "assets\ui\flags-LICENSE.txt"),
    @("SDL_GameControllerDB, controller layouts (zlib licence)", "assets\controllers\LICENSE.txt"))
$text = New-Object System.Text.StringBuilder
[void]$text.AppendLine("Spikeout: Final Edition recomp ${Version}: the libraries and files built into it, and their licences.")
[void]$text.AppendLine("The program itself is under the GNU GPL version 3 (LICENSE.txt).")
foreach ($p in $parts) {
    [void]$text.AppendLine(""); [void]$text.AppendLine(("=" * 78)); [void]$text.AppendLine($p[0]); [void]$text.AppendLine(("=" * 78))
    [void]$text.AppendLine((Get-Content $p[1] -Raw))
}
[void]$text.AppendLine(""); [void]$text.AppendLine(("=" * 78))
[void]$text.AppendLine("winpthreads (mingw-w64), Windows only (MIT)"); [void]$text.AppendLine(("=" * 78))
[void]$text.AppendLine("Copyright (c) 2011 mingw-w64 project"); [void]$text.AppendLine(""); [void]$text.AppendLine($mit)
[void]$text.AppendLine("The GNU C++ library (libstdc++) and libgcc, Windows only: GPL version 3 with the GCC Runtime Library Exception.")
Remove-Item $llvm_license

$readme = @"
Spikeout: Final Edition recomp $Version

The arcade game Spikeout: Final Edition (Sega, 1999), recompiled to run natively.
No game files are included: no ROMs and nothing made from them. You need your own
ROM set, spikeofe.zip, as MAME and Supermodel have it.

First start: choose spikeofe.zip in the window. The game is then translated from it
(about 15-25 seconds, once; later starts take a moment). Esc opens the menu.

Your settings, the rankings and the translated game: Windows
C:\Users\<you>\AppData\Roaming\spikeout-recomp\Spikeout (%APPDATA%\spikeout-recomp\Spikeout),
Linux ~/.local/share/spikeout-recomp/Spikeout.
To keep them next to the program instead, put an empty file called portable.txt
beside it. Save states (F9) go in its saves folder, screenshots (F12) in its
screenshots folder.

New versions: at start the game asks GitHub whether a newer one is out, and the menu
says so (Esc > Game > Check for updates turns that off). Players need the same
version to play together.

Linux: needs glibc 2.38 or later (Ubuntu 24.04, Fedora 39 and later, current SteamOS) and
OpenSSL 3. Make it runnable (chmod +x spikeout) if your file manager doesn't.

This is a preview build. Not affiliated with or endorsed by SEGA.
"@

function stage([string]$name, [string]$exe, [string]$exe_name) {
    $dir = Join-Path $Out $name
    New-Item -ItemType Directory $dir | Out-Null
    Copy-Item $exe (Join-Path $dir $exe_name)
    Copy-Item LICENSE (Join-Path $dir "LICENSE.txt")
    [IO.File]::WriteAllText((Join-Path $dir "THIRD-PARTY-NOTICES.txt"), $text.ToString().Replace("`r`n", "`n").Replace("`n", "`r`n"))
    [IO.File]::WriteAllText((Join-Path $dir "README.txt"), $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"))
    return $dir
}

$win = stage "spikeout-$Version-windows-x64" $WindowsExe "spikeout.exe"
strip (Join-Path $win "spikeout.exe")
Compress-Archive -Path "$win\*" -DestinationPath (Join-Path $Out "spikeout-$Version-windows-x64.zip")

if ($LinuxExe) {
    $lin = stage "spikeout-$Version-linux-x64" $LinuxExe "spikeout"
    $wsl_dir = (wsl -e wslpath -a ($lin -replace '\\', '/')).Trim()
    $wsl_out = (wsl -e wslpath -a ((Resolve-Path $Out).Path -replace '\\', '/')).Trim()
    wsl -e bash -c "strip '$wsl_dir/spikeout' && chmod +x '$wsl_dir/spikeout' && cd '$wsl_out' && tar -czf 'spikeout-$Version-linux-x64.tar.gz' --owner=0 --group=0 'spikeout-$Version-linux-x64'"
}
Get-ChildItem $Out -File | ForEach-Object { "{0,-45} {1,6:N1} MB" -f $_.Name, ($_.Length / 1MB) }
