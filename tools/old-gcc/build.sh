#!/bin/bash
# build.sh: build the compiler Spikeout's C code was made with: GCC 2.7.2.3 for
# PowerPC, stock and with SEGA's frame changes, plus a modern PowerPC assembler
# and linker, in WSL / Ubuntu 24.04, without root.
#
#   bash tools/old-gcc/build.sh [WORKDIR]      (default ~/oldgcc)
#
# Gives WORKDIR/build-2.7.2.3/cc1 (stock), WORKDIR/build-2.7.2.3s/cc1 (SEGA's
# changes: tools/old-gcc/gcc-2.7.2.3-sega.patch) and WORKDIR/tools/bin/powerpc-eabi-*.
# Old GCC builds only as a 32-bit program, so Ubuntu's own 32-bit C library
# packages are unpacked into WORKDIR/sys32 (apt-get download: no root needed)
# and used through the WORKDIR/gcc32 wrapper. See docs/compiler-id.md.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-$HOME/oldgcc}
JOBS=${JOBS:-3}
mkdir -p "$W"/dl "$W"/src "$W"/debs "$W"/sys32
cd "$W"/dl

fetch() { # url sha256
    local f; f=$(basename "$1")
    [ -s "$f" ] || curl -sSfL --max-time 600 -o "$f" "$1"
    echo "$2  $f" | sha256sum -c --quiet
}
fetch https://ftp.gnu.org/gnu/gcc/gcc-2.7.2.3.tar.gz 16166e0f0f2064bb3114716650569065ef3855ae2331d7a29b85d4b9aa73773c
fetch https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800

# 32-bit C library, unpacked (not installed)
R=$W/sys32
if [ ! -f "$R/usr/lib32/libc.so" ]; then
    (cd "$W"/debs && apt-get download libc6-dev-i386 libc6-i386 lib32gcc-13-dev lib32gcc-s1 >/dev/null)
    for d in "$W"/debs/*.deb; do dpkg-deb -x "$d" "$R"; done
    sed -i "s# /usr/lib32/# $R/usr/lib32/#g; s# /lib32/# $R/usr/lib32/#g" "$R/usr/lib32/libc.so"
fi
cat > "$W/gcc32" <<EOS
#!/bin/sh
exec gcc -m32 -B$R/usr/lib32 -B$R/usr/lib/gcc/x86_64-linux-gnu/13/32 -L$R/usr/lib32 -L$R/usr/lib/gcc/x86_64-linux-gnu/13/32 -isystem $R/usr/include -idirafter /usr/include/x86_64-linux-gnu -Wl,-rpath-link,$R/usr/lib32 -Wl,--dynamic-linker=/lib/ld-linux.so.2 "\$@"
EOS
chmod +x "$W/gcc32"

# binutils (assembler, linker, objdump)
if [ ! -x "$W/tools/bin/powerpc-eabi-ld" ]; then
    (cd "$W"/src && [ -d binutils-2.42 ] || tar xf ../dl/binutils-2.42.tar.xz)
    mkdir -p "$W"/build-binutils && cd "$W"/build-binutils
    ../src/binutils-2.42/configure --target=powerpc-eabi --prefix="$W/tools" --disable-nls \
        --disable-werror --disable-gdb --disable-sim --disable-gprofng > cfg.log 2>&1
    nice make -j"$JOBS" MAKEINFO=true all-gas all-binutils all-ld > make.log 2>&1
    make MAKEINFO=true install-gas install-binutils install-ld > inst.log 2>&1
fi

# GCC 2.7.2.3: stock, and with SEGA's changes
cd "$W"/src
[ -d gcc-2.7.2.3 ] || tar xzf ../dl/gcc-2.7.2.3.tar.gz
if [ ! -d gcc-2.7.2.3-sega ]; then
    cp -a gcc-2.7.2.3 gcc-2.7.2.3-sega
    chmod -R u+w gcc-2.7.2.3-sega
    patch -d gcc-2.7.2.3-sega -p1 < "$HERE/gcc-2.7.2.3-sega.patch"
fi
HOSTCC="$W/gcc32 -std=gnu89 -fcommon -w -fno-strict-aliasing -O1 -g0"
for v in 2.7.2.3:gcc-2.7.2.3 2.7.2.3s:gcc-2.7.2.3-sega; do
    name=${v%%:*}; src=${v##*:}; B=$W/build-$name
    [ -x "$B/cc1" ] && continue
    rm -rf "$B"; mkdir -p "$B"; cd "$B"
    CC="$HOSTCC" ../src/$src/configure --host=i686-pc-linux-gnu --build=i686-pc-linux-gnu \
        --target=powerpc-eabi --prefix="$W/gcc-$name" > cfg.log 2>&1
    nice make -j"$JOBS" CC="$HOSTCC" HOST_CC="$HOSTCC" cc1 > cc1.log 2>&1
done
ls -l "$W"/build-2.7.2.3/cc1 "$W"/build-2.7.2.3s/cc1
