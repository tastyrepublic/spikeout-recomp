#!/bin/bash
# build-snapshot.sh: build GCC's development tree as it stood on a given day
# (from GNU's archive of the old FSF GCC repository, 1987-1999) for PowerPC,
# with SEGA's frame changes. The tree of 1 January 1997 is the closest found to
# the compiler that made Spikeout's C code (docs/compiler-id.md).
#
#   bash tools/old-gcc/build-snapshot.sh [DATE] [WORKDIR]   (1997-01-01, ~/oldgcc)
#
# Run tools/old-gcc/build.sh first (it makes WORKDIR/gcc32, the assembler and
# the GCC 2.7.2.3 sources this borrows its generated files from). Gives
# WORKDIR/build-snapDATE-s/cc1 (VENDOR=1: build-snapDATE-v, see below). Options: see docs/compiler-id.md
#
# The repository has no generated files (configure, the C parser), so they come
# from GCC 2.7.2.3; the trees of late 1996 and early 1997 still use its
# hand-written configure. `cvs` is unpacked from Ubuntu's package, not installed.
set -euo pipefail
D=${1:-1997-01-01}
W=${2:-$HOME/oldgcc}
JOBS=${JOBS:-3}
N=snap$(echo "$D" | tr -d - | cut -c3-)
G=$W/src/gcc-2.7.2.3
[ -d "$G" ] && [ -x "$W/gcc32" ] || { echo "run build.sh first"; exit 1; }

# the archive (checked by SHA-512) and the cvs tool
mkdir -p "$W/dl" "$W/cvsroot" "$W/cvstool"
cd "$W/dl"
A=https://gcc.gnu.org/pub/gcc/old-releases/old-cvs
[ -s gcc-cvsroot-1999-05-06.tar.bz2 ] || curl -sSfL --max-time 600 -o gcc-cvsroot-1999-05-06.tar.bz2 $A/gcc-cvsroot-1999-05-06.tar.bz2
curl -sSfL --max-time 60 -o old-cvs-sha512.sum $A/sha512.sum
grep " gcc-cvsroot-1999-05-06.tar.bz2$" old-cvs-sha512.sum | sha512sum -c --quiet
[ -d "$W/cvsroot/x" ] || { mkdir "$W/cvsroot/x"; tar xjf gcc-cvsroot-1999-05-06.tar.bz2 -C "$W/cvsroot/x"; }
if [ ! -x "$W/cvstool/usr/bin/cvs" ]; then
    (cd "$W/cvstool" && apt-get download cvs >/dev/null && dpkg-deb -x cvs_*.deb .)
fi
export PATH=$W/cvstool/usr/bin:$PATH
R=$W/cvsroot/x
[ -d "$R/CVSROOT" ] || cvs -d "$R" init

# check out the tree of that day
S=$W/src/$N-s/gcc
rm -rf "$W/src/$N-s"; mkdir -p "$W/src/$N-s"; cd "$W/src/$N-s"
cvs -Q -d "$R" co -D "$D" gcc
chmod -R u+w .

# generated and host files the repository doesn't keep
for f in configure config.sub config.guess version.c c-gperf.h c-parse.c c-parse.y c-parse.h \
         bi-parser.c bi-parser.h move-if-change install.sh obstack.c; do
    [ -f "$S/$f" ] || cp "$G/$f" "$S/"
done
for f in $(cd "$G" && ls *.h *.def); do [ -f "$S/$f" ] || cp "$G/$f" "$S/"; done
(cd "$G/config" && find . -type f ! -path "./rs6000/*" | while read -r f; do
    [ -e "$S/config/$f" ] || { mkdir -p "$S/config/$(dirname "$f")"; cp "$f" "$S/config/$f"; }; done)

# SEGA's frame changes (docs/compiler-id.md, items 1-3)
C=$S/config/rs6000
sed -i 's/info->lr_save_offset + sp_offset/info->lr_save_offset/g' "$C/rs6000.c"
sed -i '/mfcr %s/s/reg_names\[12\]/reg_names[2]/
        /reg_names\[12\], info->cr_save_offset/s/reg_names\[12\]/reg_names[2]/
        /0x8, reg_names\[12\]/s/reg_names\[12\]/reg_names[2]/' "$C/rs6000.c"
sed -i 's/ALIGN (current_function_outgoing_args_size, 8)/ALIGN (current_function_outgoing_args_size, 16)/' "$C/rs6000.c" "$C/rs6000.h"
sed -i 's/^#define ABI_STACK_BOUNDARY ((TARGET_EABI) ? 64 : 128)/#define ABI_STACK_BOUNDARY 128/' "$C/sysv4.h"

# With VENDOR=1, the loop optimiser's two differences found in Spikeout's code
# (docs/compiler-id.md): combine_givs as in GCC 2.7.2.3 (the newer tree re-prices
# a combined induction variable that is used once), and a loop counter may be
# replaced in the loop test by a derived value with a constant integer offset
# (the public trees accept only symbol or pointer offsets; this one takes
# constant offsets and not pointer ones); plus 16-byte spill slots, below. Output: build-DATE-v.
SUF=s
if [ "${VENDOR:-0}" = 1 ]; then
    SUF=v
    perl -e 'local $/; open F, $ARGV[0] or die; my $old = <F>; open G, $ARGV[1] or die; my $new = <G>;
        my $re = qr/\nstatic void\ncombine_givs \(bl\).*?\n}\n/s;
        my ($o) = $old =~ /($re)/; my ($n) = $new =~ /($re)/; die "combine_givs not found\n" unless $o && $n;
        $new =~ s/\Q$n\E/$o/;
        $new =~ s/(if \(CONSTANT_P \(v->mult_val\) && INTVAL \(v->mult_val\) > 0\n\t\t&& \()(GET_CODE \(v->add_val\) == SYMBOL_REF)/$1GET_CODE (v->add_val) == CONST_INT || $2/ or die "biv test not found\n";
        # (in that same test, a giv offset by a pointer register is not taken)
        $new =~ s/(GET_CODE \(v->add_val\) == CONST_INT \|\| GET_CODE \(v->add_val\) == SYMBOL_REF\n\t\t    \|\| GET_CODE \(v->add_val\) == LABEL_REF\n\t\t    \|\| GET_CODE \(v->add_val\) == CONST)\n\t\t    \|\| \(GET_CODE \(v->add_val\) == REG\n\t\t\t&& REGNO_POINTER_FLAG \(REGNO \(v->add_val\)\)\)\)/$1)/ or die "biv pointer test not found\n";
        open H, ">", $ARGV[1] or die; print H $new;' "$G/loop.c" "$S/loop.c"
    # and the stack slots reload gives to spilled values are 16-byte aligned (as
    # the frames are): the two assign_stack_local calls in reload1.c's alter_reg.
    perl -0pi -e 's/(x = assign_stack_local \(GET_MODE \(regno_reg_rtx\[i\]\), )total_size, -1\)/${1}((total_size + 15) & ~15), 128)/ or die "spill slot 1 not found\n";
        s/(x = assign_stack_local \(mode, )total_size, -1\)/${1}((total_size + 15) & ~15), 128)/ or die "spill slot 2 not found\n"' "$S/reload1.c"
fi

# build cc1 in the tree (these makefiles expect it)
touch "$S/c-parse.y"; sleep 1; touch "$S/c-parse.c" "$S/c-parse.h" "$S/bi-parser.c" "$S/bi-parser.h"
cd "$S"
HOSTCC="$W/gcc32 -std=gnu89 -fcommon -w -fno-strict-aliasing -O1 -g0"
./configure --host=i686-pc-linux-gnu --build=i686-pc-linux-gnu --target=powerpc-eabi > cfg.log 2>&1
for i in 1 2 3 4 5 6; do
    nice make -j"$JOBS" CC="$HOSTCC" HOST_CC="$HOSTCC" target_alias=powerpc-eabi cc1 > cc1.log 2>&1 && break
    m=$(grep -o "No rule to make target '[^']*'" cc1.log | head -1 | sed "s/.*'\(.*\)'/\1/") || true
    [ -n "$m" ] || { tail -20 cc1.log; exit 1; }
    f=$(find config -name "$(basename "$m")" | head -1)
    if [ -n "$f" ]; then ln -sf "$S/$f" "$m"; else touch "$m"; fi
done
mkdir -p "$W/build-$N-$SUF"; cp cc1 "$W/build-$N-$SUF/cc1"
ls -l "$W/build-$N-$SUF/cc1"
