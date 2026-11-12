# compiler-id-stats.awk: count the instruction habits that tell hand-written
# PowerPC code from compiler output, in two address ranges of a disassembly.
#
#   codemap <rom_dir> --scan-pointers --disasm ALL.dis
#   awk -v lo=0x78488 -v hi=0x7df4c -f tools/compiler-id-stats.awk ALL.dis
#
# Instructions inside [lo, hi) are counted as group "C", the rest of RAM as
# "M". The disassembly is made from the ROMs: keep it out of the repository.
# docs/compiler-id.md explains what each count means.
BEGIN { lo = strtonum(lo); hi = strtonum(hi) }
/^  [0-9a-f]{8}:/ {
  a = strtonum("0x" substr($1, 1, 8)); if (a >= 0x100000) next
  g = (a >= lo && a < hi) ? "C" : "M"; n[g]++; op = $3; line = $0
  if (op ~ /^cmp/) { if (line ~ /cr[1-7],/) crN[g]++; else cr0[g]++ }
  if (line ~ /lis     r2,/) lisr2[g]++
  if (op == "mr") mr[g]++
  if (op == "addi" && line ~ /, 0$/ && line !~ /r1, r1/) addi0[g]++
  if (op == "rlwinm." && line ~ /, 0, 0$/) bittest[g]++
  if (op == "stwu" && line ~ /r1, -/) stwu[g]++
  if (line ~ /addi    r1, r1, -16$/) push16[g]++
  if (op == "stmw") stmw[g]++
  if (op ~ /^b(eq|ne|lt|gt|le|ge|so|ns)l$/) condcall[g]++
  if (op == "subfic" || op == "subfe") carry[g]++
  if (op ~ /^bd/) bdnz[g]++
  if (line ~ /\(r13\)/) r13[g]++
}
END {
  for (g in n)
    printf "%s: instructions=%d cmp_cr0=%d cmp_other_cr=%d lis_r2=%d mr=%d addi_move=%d " \
           "rotate_bit_tests=%d stwu_r1=%d push16=%d stmw=%d conditional_calls=%d " \
           "subfic_subfe=%d bdnz=%d r13_refs=%d\n",
           g, n[g], cr0[g], crN[g], lisr2[g], mr[g], addi0[g], bittest[g], stwu[g],
           push16[g], stmw[g], condcall[g], carry[g], bdnz[g], r13[g]
}
