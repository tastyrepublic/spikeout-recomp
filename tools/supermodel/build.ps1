# Builds a patched Supermodel for comparing our 3D with theirs (docs/frame-dump.md): its source at -Commit (77d28ee:
# the owner's 0.3a-20260528 build; "master" for their newest), frame-dump.patch applied (a 3D frame dump written
# beside each Alt+S screenshot with SUPERMODEL_DUMP_3D=1; SUPERMODEL_DRAW_3D=DUMP draws one and quits;
# SUPERMODEL_MODELS_OUT=FILE lists the models drawn), built with this PC's WinLibs GCC. SDL2, SDL2_net (their MinGW
# packages) and zlib (built here) are downloaded once into -Work\deps. The program and a copy of the owner's
# Supermodel settings (-Settings: its Config and Assets) go to -Work\run. Nothing of it goes into this repository.
#
#   tools\supermodel\build.ps1 [-Commit master] [-Work Z:\Emu\supermodel-build]
#
# Supermodel is GPL-3 (github.com/trzy/Supermodel); the patch is ours, under the same licence.
param([string]$Commit = '77d28ee', [string]$Work = 'Z:\Emu\supermodel-build', [string]$Settings = 'Z:\Emu\Sega Supermodel 3')
$ErrorActionPreference = 'Stop'
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
$patch = Join-Path $PSScriptRoot 'frame-dump.patch'
New-Item -ItemType Directory -Force $Work, "$Work\deps", "$Work\prefix\include", "$Work\prefix\lib" | Out-Null

# The source, at the commit asked for, with the patch on it (put back first: a clean tree each time)
if (-not (Test-Path "$Work\src\.git")) { git clone -q https://github.com/trzy/Supermodel.git "$Work\src" }
Push-Location "$Work\src"
git fetch -q origin
git checkout -q -- .
git checkout -q $(if ($Commit -eq 'master') { 'origin/master' } else { $Commit })
git apply --whitespace=nowarn $patch
if ($LASTEXITCODE) { Pop-Location; throw "frame-dump.patch doesn't apply to Supermodel ${Commit}: update the patch (their code moved)" }
"Supermodel $(git log --oneline -1), patched"
Pop-Location

# The libraries, once: SDL2 and SDL2_net's MinGW packages, zlib built with this compiler
$get = @(
    @('SDL2-2.30.12', 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-devel-2.30.12-mingw.tar.gz'),
    @('SDL2_net-2.4.0', 'https://github.com/libsdl-org/SDL_net/releases/download/release-2.4.0/SDL2_net-devel-2.4.0-mingw.tar.gz'),
    @('zlib-1.4', 'https://github.com/madler/zlib/releases/download/v1.4/zlib-1.4.tar.gz'))
foreach ($g in $get) {
    if (Test-Path "$Work\deps\$($g[0])") { continue }
    "downloading $($g[1])"
    Invoke-WebRequest $g[1] -OutFile "$Work\deps\$($g[0]).tar.gz"
    tar -xzf "$Work\deps\$($g[0]).tar.gz" -C "$Work\deps"
}
if (-not (Test-Path "$Work\prefix\lib\libz.a")) {
    Push-Location "$Work\deps\zlib-1.4"; mingw32-make -f win32/Makefile.gcc libz.a | Out-Null; Pop-Location
    Copy-Item "$Work\deps\zlib-1.4\libz.a" "$Work\prefix\lib\"; Copy-Item "$Work\deps\zlib-1.4\zlib.h", "$Work\deps\zlib-1.4\zconf.h" "$Work\prefix\include\"
}
foreach ($pkg in 'SDL2-2.30.12', 'SDL2_net-2.4.0') {
    Copy-Item "$Work\deps\$pkg\x86_64-w64-mingw32\include\*" "$Work\prefix\include\" -Recurse -Force
    Copy-Item "$Work\deps\$pkg\x86_64-w64-mingw32\lib\*" "$Work\prefix\lib\" -Recurse -Force
}

# The build (without Supermodel's resource file: its manifest and the compiler's default one can't both be linked)
$env:CPATH = "$Work\prefix\include"
Push-Location "$Work\src"
cmd /c "mingw32-make -f Makefiles/Makefile.Win32 -j8 SDL2_INCLUDE_DIR=$Work\prefix\include\SDL2 SDL2_LIB_DIR=$Work\prefix\lib `"PLATFORM_SRC_FILES=Src/OSD/Windows/DirectInputSystem.cpp Src/OSD/Windows/FileSystemPath.cpp Src/OSD/Windows/WinOutputs.cpp`" > `"$Work\build.log`" 2>&1"
$failed = $LASTEXITCODE
Pop-Location
if ($failed -or -not (Test-Path "$Work\src\bin64\supermodel.exe")) { throw "the build failed: ${Work}\build.log" }

# The run folder: the program and a copy of the owner's settings (theirs left as they are)
New-Item -ItemType Directory -Force "$Work\run\NVRAM", "$Work\run\Saves", "$Work\run\Screenshots" | Out-Null
Copy-Item "$Work\src\bin64\supermodel.exe" "$Work\run\" -Force
foreach ($d in 'Config', 'Assets') { if (Test-Path "$Settings\$d") { Copy-Item "$Settings\$d" "$Work\run\" -Recurse -Force } }
"built: $Work\run\supermodel.exe"
