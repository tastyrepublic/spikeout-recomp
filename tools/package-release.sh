#!/usr/bin/env bash
# A release download, as GitHub's release workflow (.github/workflows/release.yml) makes it on each system: the window
# built with the JIT (no game code inside: the game is translated from the player's own ROM set at its first start),
# stripped, with the licences and a short readme; and beside it the same program with its debug symbols (for reading
# crash reports: tools/player_report.hpp).
#
#   tools/package-release.sh VERSION PLATFORM EXE BUILD_DIR OUT
#     PLATFORM: windows-x64, linux-x64 or macos-arm64; EXE: the built spikeout-jit; BUILD_DIR: its build folder (the
#     libraries' sources are in BUILD_DIR/_deps); OUT: where the files go.
#   OPENSSL_LICENSE=FILE: OpenSSL's licence, when OpenSSL is built in (Linux, macOS).
#
# (tools/make-release.ps1 is the same for a build on this PC; this one is what the downloads are made with.)
set -euo pipefail
version=$1 platform=$2 exe=$3 build=$4 out=$5
repo=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
exe=$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe") # (whole paths: the steps below change folder)
build=$(cd "$build" && pwd)
name="spikeout-$version-$platform"
stage="$out/$name"
rm -rf "$stage" && mkdir -p "$stage"

# The version built in (src/net/update.cpp marks it in the file)
if ! grep -aq "SPIKEOUT_VERSION=$version" "$exe"; then
    echo "$exe is not version $version (build it with -DSPIKEOUT_VERSION=$version)" >&2
    exit 1
fi

dep() { # a library's source folder: the build's own, else the one it was pointed at
    local d="$build/_deps/$1-src"
    [ -d "$d" ] && { echo "$d"; return; }
    sed -n "s/^FETCHCONTENT_SOURCE_DIR_$(echo "$1" | tr a-z A-Z):PATH=//p" "$build/CMakeCache.txt"
}
llvm_license=$(mktemp)
curl -fsSL https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.8/llvm/LICENSE.TXT -o "$llvm_license"

notices="$stage/THIRD-PARTY-NOTICES.txt"
{
    echo "Spikeout: Final Edition recomp $version: the libraries and files built into it, and their licences."
    echo "The program itself is under the GNU GPL version 3 (LICENSE.txt)."
    part() { # (a licence file missing: the build took that library from the system, not built in: said, and stopped)
        [ -f "$2" ] || { echo "package-release.sh: no licence file for: $1 ($2): is it built in?" >&2; exit 1; }
        echo; printf '=%.0s' {1..78}; echo; echo "$1"; printf '=%.0s' {1..78}; echo; cat "$2"; }
    part "SDL 3 (zlib licence)" "$(dep sdl3)/LICENSE.txt"
    part "RmlUi (MIT)" "$(dep rmlui)/LICENSE.txt"
    part "RmlUi's SDL GPU renderer" "$repo/third_party/rmlui_sdl_gpu/LICENSE.txt"
    part "FreeType (FreeType Licence). Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved." "$(dep freetype)/docs/FTL.TXT"
    part "LLVM 18.1.8 (Apache 2.0 with LLVM exceptions)" "$llvm_license"
    part "curl" "$(dep curl)/COPYING"
    [ -n "${OPENSSL_LICENSE:-}" ] && part "OpenSSL (Apache 2.0)" "$OPENSSL_LICENSE"
    part "Zstandard (BSD)" "$(dep zstd)/LICENSE"
    part "minimp3 (CC0)" "$repo/third_party/minimp3/LICENSE"
    part "QR Code generator, Project Nayuki (MIT)" "$repo/third_party/qrcodegen/LICENSE"
    part "Moira" "$repo/third_party/moira/LICENSE"
    part "Barlow Condensed font (SIL Open Font License)" "$repo/assets/fonts/OFL.txt"
    part "Country flags" "$repo/assets/ui/flags-LICENSE.txt"
    case $platform in
    windows-*)
        echo; printf '=%.0s' {1..78}; echo; echo "winpthreads (mingw-w64), Windows only (MIT)"; printf '=%.0s' {1..78}; echo
        echo "Copyright (c) 2011 mingw-w64 project"
        echo "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated"
        echo "documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the"
        echo "rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit"
        echo "persons to whom the Software is furnished to do so, subject to the following conditions:"
        echo "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the"
        echo "Software."
        echo "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED."
        echo
        echo "The GNU C++ library (libstdc++) and libgcc: GPL version 3 with the GCC Runtime Library Exception." ;;
    linux-*)
        echo
        echo "The GNU C++ library (libstdc++) and libgcc, built in: GPL version 3 with the GCC Runtime Library Exception." ;;
    esac
} > "$notices"
rm -f "$llvm_license"
cp "$repo/LICENSE" "$stage/LICENSE.txt"

case $platform in
windows-*) data='%APPDATA%\spikeout-recomp\Spikeout' ;;
linux-*) data='~/.local/share/spikeout-recomp/Spikeout' ;;
macos-*) data='~/Library/Application Support/spikeout-recomp/Spikeout' ;;
esac
{
    cat <<EOF
Spikeout: Final Edition recomp $version

The arcade game Spikeout: Final Edition (Sega, 1999), recompiled to run natively.
No game files are included: no ROMs and nothing made from them. You need your own
ROM set, spikeofe.zip, as MAME and Supermodel have it.

First start: choose spikeofe.zip in the window. The game is then translated from it
(about 15-25 seconds, once; later starts take a moment). Esc opens the menu.

Your settings, the rankings, the translated game and the game's log (spikeout.log):
$data
To keep them next to the program instead, put an empty file called portable.txt
beside it. Save states (F9) and screenshots (F12) go in the folder the game was
started from.

Something wrong? Esc > Game > Report a problem makes one zip of the game's log, any
crash reports, the settings and your computer's details (no game files; internet
addresses and your user name taken out) and opens GitHub's bug report page: attach
the zip there. For network or speed problems, turn on Esc > Game > Detailed logging
first, start the game again and make the problem happen.

New versions: at start the game asks GitHub whether a newer one is out, and the menu
says so (Esc > Game > Check for updates turns that off). Players need the same
version to play together.
EOF
    case $platform in
    linux-*)
        echo
        echo "Linux: built in Valve's Steam Runtime (sniper) SDK: runs on Linux from about 2020 on"
        echo "(glibc 2.31 or later), the Steam Deck included. Make it runnable (chmod +x spikeout) if"
        echo "your file manager doesn't." ;;
    macos-*)
        echo
        [ "$platform" = macos-arm64 ] && kind="Apple silicon: M1 and later" || kind="Intel processors"
        echo "macOS 13.3 or later, for Macs with $kind. Not signed: the first time, right-click Spikeout and choose Open,"
        echo "or open it, then System Settings > Privacy & Security > Open Anyway. This is the first Mac"
        echo "build: please report how it goes." ;;
    esac
    echo
    echo "This is a preview build. Not affiliated with or endorsed by SEGA."
} > "$stage/README.txt"

case $platform in
windows-*)
    cp "$exe" "$stage/spikeout.exe"
    (cd "$out" && 7z a -tzip -bso0 "$name-debug-symbols.zip" "$(cygpath -w "$exe")" >/dev/null)
    strip "$stage/spikeout.exe"
    for f in LICENSE.txt THIRD-PARTY-NOTICES.txt README.txt; do sed -i 's/$/\r/' "$stage/$f"; done
    (cd "$stage" && 7z a -tzip -bso0 "$out/$name.zip" ./* >/dev/null) ;;
linux-*)
    cp "$exe" "$stage/spikeout"
    (cd "$(dirname "$exe")" && tar -czf "$out/$name-debug-symbols.tar.gz" "$(basename "$exe")")
    strip "$stage/spikeout"
    chmod +x "$stage/spikeout"
    (cd "$out" && tar -czf "$name.tar.gz" --owner=0 --group=0 "$name") ;;
macos-*)
    app="$stage/Spikeout.app/Contents"
    mkdir -p "$app/MacOS" "$app/Resources"
    cp "$exe" "$app/MacOS/spikeout"
    cp "$repo/assets/icon/spikeout.icns" "$app/Resources/spikeout.icns"
    cat > "$app/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Spikeout</string>
    <key>CFBundleDisplayName</key><string>Spikeout</string>
    <key>CFBundleIdentifier</key><string>io.github.tastyrepublic.spikeout-recomp</string>
    <key>CFBundleExecutable</key><string>spikeout</string>
    <key>CFBundleIconFile</key><string>spikeout</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>13.3</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
    dsymutil "$exe" -o "$out/spikeout.dSYM"
    (cd "$out" && ditto -c -k --keepParent spikeout.dSYM "$name-debug-symbols.zip" && rm -rf spikeout.dSYM)
    strip -x "$app/MacOS/spikeout"
    codesign --force --sign - "$stage/Spikeout.app" # (Apple silicon runs only signed code: signed for this machine alone)
    (cd "$out" && ditto -c -k --keepParent "$name" "$name.zip") ;;
esac
rm -rf "$stage"
ls -l "$out"
