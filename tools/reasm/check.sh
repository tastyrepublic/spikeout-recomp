#!/bin/bash
# check.sh: make a re-assemblable disassembly of IMAGE and prove it assembles
# back into the same bytes, keeping as raw words any instruction that doesn't.
#
#   bash tools/reasm/check.sh IMAGE CODEMAP.txt OUT.s [BASE]
#
# TOOLS (default ~/oldgcc/tools/bin) holds powerpc-eabi-as/ld/objcopy/objdump
# (tools/old-gcc/build.sh builds them). Repeats until the assembled bytes equal
# IMAGE; OUT.s.raw lists the words kept raw. The output is made from IMAGE:
# keep it private if IMAGE is.
set -euo pipefail
IMAGE=$1; CODE=$2; OUT=$3; BASE=${4:-0}
T=${TOOLS:-$HOME/oldgcc/tools/bin}/powerpc-eabi
HERE=$(cd "$(dirname "$0")" && pwd)
RAW=$OUT.raw
: > "$RAW"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
for pass in 1 2 3 4 5 6; do
    python3 "$HERE/reasm.py" "$IMAGE" --code "$CODE" --out "$OUT" --base "$BASE" --raw "$RAW" --objdump "$T-objdump" ${REASM_OPTS:-}
    "$T-as" -mppc -many -mbig -mregnames "$OUT" -o "$W/a.o" 2> "$W/as.err" || { head -20 "$W/as.err"; exit 1; }
    "$T-ld" -Ttext="$BASE" -e "$BASE" "$W/a.o" -o "$W/a.elf"
    "$T-objcopy" -O binary -j .text "$W/a.elf" "$W/a.bin"
    if cmp -s "$W/a.bin" "$IMAGE"; then
        echo "pass $pass: IDENTICAL ($(stat -c%s "$IMAGE") bytes, $(wc -l < "$RAW") words kept raw)"
        exit 0
    fi
    python3 - "$W/a.bin" "$IMAGE" "$BASE" "$RAW" <<'PY'
import sys
a = open(sys.argv[1], "rb").read(); b = open(sys.argv[2], "rb").read(); base = int(sys.argv[3], 0)
bad = [base + o for o in range(0, max(len(a), len(b)), 4) if a[o:o + 4] != b[o:o + 4]]
print("  %d words differ (sizes %d / %d), first %s" % (len(bad), len(a), len(b), " ".join("%x" % x for x in bad[:6])))
if len(a) != len(b): bad = bad[:1]          # a size change shifts everything: fix the first only
with open(sys.argv[4], "a") as f:
    for x in bad: f.write("%x\n" % x)
PY
done
echo "still different after 6 passes"; exit 1
