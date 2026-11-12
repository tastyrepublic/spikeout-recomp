#!/bin/bash
# compare.sh: compile one C file with an old GCC and compare it with a game
# function's bytes, byte for byte.
#
#   [TEXT=0xADDR DEFS="sym=0xADDR ..."] bash tools/old-gcc/compare.sh CC1 FILE.c GAME.bin [cc1 options]
#
# CC1      e.g. ~/oldgcc/build-2.8.1s/cc1 (from build.sh)
# GAME.bin the function's bytes, cut from the RAM image (see docs/compiler-id.md);
#          made from the ROM: keep it, and the C, out of the repository
# TEXT     the function's address: links the object there, with DEFS giving the
#          addresses of what it calls and uses, so every byte is final.
#          Without TEXT, calls and addresses are left unlinked.
# Options used for Spikeout: -O2 -mcpu=603 -mno-eabi
set -u
CC1=$1; C=$2; BIN=$3; shift 3
T=${TOOLS:-$HOME/oldgcc/tools/bin}/powerpc-eabi
O=$(mktemp -d)
trap 'rm -rf "$O"' EXIT
gcc -E -P -undef -nostdinc "$C" > "$O/a.i"
"$CC1" -quiet "$@" "$O/a.i" -o "$O/a.s" || exit 1
"$T-as" -mppc -many "$O/a.s" -o "$O/a.o" || exit 1
if [ -n "${TEXT:-}" ]; then
    D=(); for d in ${DEFS:-}; do D+=(--defsym "$d"); done
    "$T-ld" -Ttext="$TEXT" "${D[@]}" -e 0 "$O/a.o" -o "$O/a.elf" || exit 1
    "$T-objcopy" -O binary -j .text "$O/a.elf" "$O/a.bin"
else
    "$T-objcopy" -O binary -j .text "$O/a.o" "$O/a.bin"
fi
dis() { "$T-objdump" -b binary -m powerpc:common -EB -D "$1" | awk '/^ +[0-9a-f]+:/{ $1=""; print }'; }
if cmp -s "$O/a.bin" "$BIN"; then
    echo "MATCH ($(stat -c%s "$BIN") bytes)"
else
    echo "differ: ours $(stat -c%s "$O/a.bin") bytes, game $(stat -c%s "$BIN")"
    diff -y -W 110 <(dis "$O/a.bin") <(dis "$BIN") | head -80
    exit 1
fi
