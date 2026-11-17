#!/usr/bin/env python3
"""reasm.py: turn a PowerPC memory image into assembly source that GNU `as`
assembles back into the same bytes (a re-assemblable disassembly).

    python3 reasm.py IMAGE --code CODEMAP.txt --out OUT.s
                     [--base ADDR] [--raw RAWLIST] [--objdump PATH]

IMAGE      the bytes, as loaded at --base (default 0)
--code     `codemap --disasm` output: which words are code, and the names of
           functions and blocks
--raw      words to keep as `.long` (one hex address per line), written by
           check.sh for words that did not come back the same
--objdump  powerpc-eabi-objdump (default: on PATH)

Code is written as instructions, as GNU objdump prints them, with every
branch target inside the image replaced by a label; everything else is
written as `.long` words. The output is made from the image: if the image is
someone else's (a game's ROM), keep the output private.
"""
import argparse
import re
import subprocess
import sys


def parse_codemap(path):
    """Return (code word addresses, {address: name}) from codemap --disasm."""
    code = set()
    names = {}
    pending = None
    line_re = re.compile(r"^  ([0-9a-f]{8}):  ([0-9a-f]{8})  ")
    label_re = re.compile(r"^([A-Za-z_.$][\w.$]*):")
    with open(path) as f:
        for line in f:
            m = line_re.match(line)
            if m:
                addr = int(m.group(1), 16)
                code.add(addr)
                if pending is not None:
                    names.setdefault(addr, pending)
                    pending = None
                continue
            m = label_re.match(line)
            if m:
                name = m.group(1)
                if name.startswith(".L"):
                    name = "L_" + name[2:]
                pending = name
    return code, names


def objdump_text(image, base, objdump):
    """Return {address: instruction text} for every word of the image."""
    out = subprocess.run([objdump, "-b", "binary", "-m", "powerpc:common", "-EB", "-D",
                          "--adjust-vma=0x%x" % base, image],
                         check=True, capture_output=True, text=True).stdout
    text = {}
    line_re = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(.*)$")
    for line in out.splitlines():
        m = line_re.match(line)
        if m:
            text[int(m.group(1), 16)] = m.group(2).strip()
    return text


def apply_names(path, maps):
    """Rename the generated labels (fn_/L_/D_ + address) the maps name, and write each name's note above it."""
    rename, notes = {}, {}
    for m in maps:
        last = None
        for line in open(m):
            body, _, note = line.partition("#")
            parts = body.split()
            if len(parts) >= 2:
                addr = int(parts[0], 16)
                last = parts[1]
                for kind in ("fn", "L", "D"):
                    rename["%s_%08x" % (kind, addr)] = last
                notes[last] = [note.strip()] if note.strip() else []
            elif not parts and note.strip() and last and line.startswith(" "):
                notes[last].append(note.strip())    # a note's next lines (indented, only a comment)
            else:
                last = None
    taken = set()
    for n in set(rename.values()):
        if n in taken:
            sys.exit("reasm: --names: %s given twice" % n)
        taken.add(n)
    word = re.compile(r"\b(?:fn|L|D)_[0-9a-f]{8}\b")
    out = []
    for line in open(path):
        line = word.sub(lambda m: rename.get(m.group(0), m.group(0)), line)
        if line.endswith(":\n") and line[:-2] in notes and not line.startswith("\t"):
            out.extend("# %s\n" % n for n in notes[line[:-2]])
        out.append(line)
    open(path, "w").writelines(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--code", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--base", default="0")
    ap.add_argument("--raw")
    ap.add_argument("--objdump", default="powerpc-eabi-objdump")
    ap.add_argument("--max-addr", default="0x01000000",
                    help="name built addresses below this (default: 16 MB, RAM)")
    ap.add_argument("--sda-base",
                    help="r13's value (the small-data base): name r13-relative loads and stores")
    ap.add_argument("--no-strings", action="store_true", help="keep text in the data as numbers")
    ap.add_argument("--insert", action="append", default=[],
                    help="ADDR:N: put N nops before ADDR (to test that the source is movable)")
    ap.add_argument("--drop", action="append", default=[],
                    help="ADDR:N: leave out the N zero words from ADDR (to keep the size)")
    ap.add_argument("--access", action="append", default=[],
                    help="gamerun-interp's SPIKEOUT_INTERP_ACCESS map (a byte a word; several are combined): words seen "
                         "used as addresses are always named, words seen read only as bytes, halfwords or floats never")
    ap.add_argument("--names", action="append", default=[],
                    help="a map of names (lines 'ADDRESS NAME  # note'): each address's label renamed everywhere, "
                         "its note written above it")
    ap.add_argument("--numbers", action="append", default=[],
                    help="a list of addresses (lines 'ADDRESS  # why'): each word or lis there built a number, whatever "
                         "--access says (the cases a run's evidence got wrong, fixed by hand)")
    ap.add_argument("--no-pointers", action="store_true",
                    help="keep data words that hold a function address as numbers")
    args = ap.parse_args()

    base = int(args.base, 0)
    data = open(args.image, "rb").read()
    end = base + len(data) - len(data) % 4
    code, names = parse_codemap(args.code)
    entries = set(names)                               # the code map's function starts and branch labels
    raw = set()
    if args.raw:
        try:
            with open(args.raw) as f:
                raw = {int(x, 16) for x in f.read().split()}
        except FileNotFoundError:
            pass
    text = objdump_text(args.image, base, args.objdump)

    def word(a):
        o = a - base
        return int.from_bytes(data[o:o + 4], "big")

    # Branches: decide the instruction form and the target of each code word
    # that stays an instruction, and give every target inside the image a
    # label.
    labels = dict(names)
    branch = {}
    hint = {}
    for a in sorted(code):
        if a in raw or not (base <= a < end):
            continue
        w = word(a)
        op = w >> 26
        if op == 18:                       # b, bl
            if w & 2:                      # absolute form: leave as written
                continue
            disp = w & 0x03FFFFFC
            if disp & 0x02000000:
                disp -= 0x04000000
        elif op == 16:                     # bc family
            if w & 2:
                continue
            disp = w & 0xFFFC
            if disp & 0x8000:
                disp -= 0x10000
        else:
            continue
        target = (a + disp) & 0xFFFFFFFF
        if not (base <= target < end):
            raw.add(a)                     # target outside the image: keep the word
            continue
        branch[a] = target
        labels.setdefault(target, "L_%08x" % target)
        # The static prediction bit (y, the BO field's low bit when the branch
        # tests the condition or CTR): objdump drops it, the assembler takes
        # it as a '+' (forward branch) or '-' (backward branch) on the mnemonic.
        if op == 16:
            bo = (w >> 21) & 31
            if (bo & 0x14) != 0x14 and bo & 1:
                hint[a] = "+" if disp >= 0 else "-"

    # Addresses built in two halves: `lis rX,HI` followed (in the same
    # straight run of code) by `addi rY,rX,LO`, a load or store `LO(rX)`, or
    # `ori rY,rX,LO`. Both halves get the address's name (`NAME@ha` / `@l`,
    # or `@h` / `@l` for ori), and the name is defined once: a label if the
    # address is a word inside the image, else a `.set`.
    D_FORMS = {14, 12, 13} | set(range(32, 56))        # addi, addic(.), loads/stores
    STORES = {36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55}
    starts = set(labels)
    track = {}                                         # reg -> (lis address, high value)
    hi_use = {}                                        # lis address -> (symbol, kind)
    lo_use = {}                                        # use address -> symbol
    addr_names = {}                                    # value -> symbol
    max_ram = int(args.max_addr, 0)
    sda = int(args.sda_base, 0) if args.sda_base else None
    sda_use = {}                                       # r13-relative access -> symbol

    def name_for(v):
        if v in addr_names:
            return addr_names[v]
        if base <= v < end and v % 4 == 0:
            n = labels.setdefault(v, "D_%08x" % v)
        else:
            n = "D_%08x" % v
            if base <= v < end:                        # inside, not on a word: name its word too
                labels.setdefault(v & ~3, "D_%08x" % (v & ~3))
        addr_names[v] = n
        return n

    seen = bytearray()                                 # --access: how each word was read (1 b, 2 h, 4 w, 8 f, 32/64 used;
                                                       # code: 128 a lis run, 32 a value it built used as an address, 8 as a number)
    for path in args.access:
        m = open(path, "rb").read()
        if len(seen) < len(m):
            seen.extend(bytes(len(m) - len(seen)))
        for i, x in enumerate(m):
            seen[i] |= x

    numbers = set()
    for path in args.numbers:                          # --numbers: a number, never an address
        for l in open(path):
            l = l.split("#")[0].strip()
            if l:
                numbers.add(int(l, 16) & ~3)
                i = (int(l, 16) - base) // 4
                if i >= len(seen):
                    seen.extend(bytes(i + 1 - len(seen)))
                seen[i] = (seen[i] & ~96) | 8

    def round_number(v):                               # a round value under 64 KB that lands on code: almost always a
        return v < 0x10000 and v % 0x1000 == 0 and v in code   # size, a count or an angle (0x4000, 0x6000, 0x8000)

    def how(a):
        i = (a - base) // 4
        return seen[i] if 0 <= i < len(seen) else 0

    prev = None
    for a in sorted(code):
        if a in raw or not (base <= a < end):
            continue
        if prev is None or a != prev + 4:              # (a label reached by falling through keeps them)
            track = {}
        prev = a
        w = word(a)
        op = w >> 26
        rt = (w >> 21) & 31
        ra = (w >> 16) & 31
        imm = w & 0xFFFF
        if op == 15 and ra == 0:                       # lis
            track[rt] = (a, imm << 16, False)
            continue
        if op == 15:                                   # addis to a register (an index): a table's high half,
            track[rt] = (a, imm << 16, True)           # named only where the low half lands inside the image
            continue
        src = ra if op != 24 else ra                   # ori: rA = rS | imm, source is rS (bits 6-10)
        if op == 24:
            src = rt
        if sda is not None and op in D_FORMS and ra == 13:
            v = (sda + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            if not (base <= v < end):
                sda_use[a] = name_for(v)
        # (rA = r0 means zero, not r0, except for addic(.) and ori)
        if src in track and (src != 0 or op in (12, 13, 24)) and (op in D_FORMS or op == 24):
            lis_a, hi, indexed = track[src]
            if op == 24:
                v, kind = hi | imm, "h"
            else:
                v, kind = (hi + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF, "ha"
            # (a low half of 0 only as an explicit `addi rX,rY,0`: nobody adds 0 but an assembler writing a
            # label's @l, as the table at 0x90000 indexed by a character's byte times 12)
            if indexed and not (op != 24 and hi and (v & 0xFFFF or (op == 14 and imm == 0))
                                and base + 0x10000 <= v < end and v not in code):
                pass
            elif op == 24:                             # lis + ori: how a number is built (masks, delays), not an address
                pass
            elif v & ~3 in code and v not in entries and not how(lis_a) & 64:
                pass                                   # inside code but on no function or label (0x9EF6, an offset
                                                       # 0x1CFA0 added to a RAM base): a pointer to code is to an entry
            elif round_number(v) and not how(lis_a) & 64:
                pass                                   # 0x8000 (half a turn, a count) on a function's start: a number
            elif how(lis_a) & 8 and not how(lis_a) & 96:
                pass                                   # what it built used as a number (a delay added to the time base, a mask,
                                                       # a loop count: 0x8000 lands on code), never as an address
            elif v < max_ram:
                sym = name_for(v)
                if lis_a not in hi_use:
                    hi_use[lis_a] = (sym, kind)
                if hi_use[lis_a][1] == kind:
                    lo_use[a] = sym
        # Forget registers this instruction may write.
        if op in (16, 18, 19):
            track = {}
        elif op == 31:
            track.pop(rt, None)
            track.pop(ra, None)
        elif op in STORES:
            if op in (37, 39, 45, 53, 55):             # update forms write rA
                track.pop(ra, None)
        elif 20 <= op <= 30 or op == 24:
            track.pop(ra, None)
        else:
            track.pop(rt, None)
            if op in (33, 35, 41, 43, 49, 51):         # load with update writes rA too
                track.pop(ra, None)

    # Data words that hold a function's address become `.long name`.
    fn_names = {v: n for v, n in names.items()}

    last_re = re.compile(r",(-?\d+|0x[0-9a-f]+)$")
    disp_re = re.compile(r",(-?\d+)\((r\d+)\)$")

    def symbolize(a, t):
        if a in sda_use:
            t2, n = re.subn(r",(-?\d+)\(r13\)$", ",%s-SDA_BASE(r13)" % sda_use[a], t)
            return t2 if n == 1 else t
        if a in hi_use:
            sym, kind = hi_use[a]
            t2, n = last_re.subn(",%s@%s" % (sym, kind), t)
            return t2 if n == 1 else t
        if a in lo_use:
            sym = lo_use[a]
            t2, n = disp_re.subn(r",%s@l(\2)" % sym, t)
            if n == 1:
                return t2
            t2, n = last_re.subn(",%s@l" % sym, t)
            return t2 if n == 1 else t
        return t

    tgt_re = re.compile(r"0x[0-9a-f]+$")
    out = open(args.out, "w")
    out.write("# Re-assemblable disassembly, made by reasm.py from %s.\n" % args.image)
    out.write("# Assemble with: as -mppc -many -mbig -mregnames\n\n")
    if sda is not None:
        out.write("\t.set SDA_BASE, 0x%08x\t# r13\n" % sda)
    out.write("# Addresses outside the image\n")
    for v, n in sorted(addr_names.items()):
        if not (base <= v < end):
            out.write("\t.set %s, 0x%08x\n" % (n, v))
    out.write("\n\t.text\n\n")
    def is_text(b):
        return 0x20 <= b < 0x7F or b in (0x09, 0x0A, 0x0D)

    def quote(bs):
        out_s = []
        for b in bs:
            c = chr(b)
            if c in '"\\':
                out_s.append("\\" + c)
            elif b == 0x09:
                out_s.append("\\t")
            elif b == 0x0A:
                out_s.append("\\n")
            elif b == 0x0D:
                out_s.append("\\r")
            else:
                out_s.append(c)
        return '"' + "".join(out_s) + '"'

    def text_run(a):
        """If text starts at word a, the bytes from a to the word boundary after
        its last string, as (end address, lines); else None. Strings must be
        4 or more characters ending in a zero, and stay in data words with no
        label after the first."""
        def free(x):
            return x < end and not (x in code and x not in raw) and (x == a or x not in labels)
        o = a - base
        n = 0
        while o + n < len(data) and is_text(data[o + n]):
            n += 1
        if n < 4 or o + n >= len(data) or data[o + n] != 0:
            return None
        lines = []
        starts_ = []
        p = a
        while True:
            o = p - base
            n = 0
            while o + n < len(data) and is_text(data[o + n]):
                n += 1
            if n < (4 if p == a else 2) or o + n >= len(data) or data[o + n] != 0:
                break
            s_ = data[o:o + n]
            plain = sum(1 for b in s_ if chr(b).isalnum() or b == 0x20)
            if plain * 10 < n * 7 or any(b in (0x09, 0x0D) for b in s_):
                break                                  # looks like numbers, not text
            stop = p + n + 1
            if any(not free(x) for x in range(p & ~3, (stop + 3) & ~3, 4)):
                break
            lines.append("\t.asciz %s" % quote(data[o:o + n]))
            starts_.append(p)
            p = stop
            pad = 0
            while (p + pad) % 4 and data[p + pad - base] == 0:   # zeros up to the next word
                pad += 1
            if (p + pad) % 4 == 0:
                if pad:
                    lines.append("\t.byte %s" % ", ".join(["0"] * pad))
                p += pad
                if p in labels or not free(p):
                    break
        if not lines:
            return None
        if p % 4:                                     # finish the word with plain bytes
            q = (p + 3) & ~3
            lines.append("\t.byte %s" % ", ".join("0x%02x" % b for b in data[p - base:q - base]))
            p = q
        return p, lines, starts_

    # Data words that point at a string, or at an address with a label, become
    # `.long name` (found before writing, since naming adds labels).
    str_at = set()
    if not args.no_strings:
        a = base
        while a < end:
            if not (a in code and a not in raw):
                run = text_run(a)
                if run:
                    str_at.update(run[2])
                    a = run[0]
                    continue
            a += 4
    ptr = {}
    while not args.no_pointers:                        # naming adds labels: repeat until none new
        before, before_ptr = len(labels), len(ptr)
        for a in range(base, end, 4):
            if a in ptr or (a in code and a not in raw) or a in numbers:
                continue
            w = word(a)
            h = how(a)
            if round_number(w) and not h & 64:         # (the scripts' flag masks 0x8000 ... read near addresses: the
                continue                               # address marks on them came from a value lookup)
            if h & 96 and base <= w < end:             # seen used as an address
                ptr[a] = fn_names.get(w) or name_for(w)
                continue
            if h & 11 and not h & 4 or h & 3:          # read as bytes, halfwords or floats (a word read too can be
                continue                               # a copy): never used as an address, so not one
            if w in fn_names:
                ptr[a] = fn_names[w]
            elif 0x100 <= w < end and (w in str_at or (w % 4 == 0 and w in labels)):
                ptr[a] = name_for(w)
            elif w % 4 == 0 and base + 0x1000 <= w < end and w not in code and any(
                    n in ptr and word(n) not in code and abs(word(n) - w) < 0x10000 for n in (a - 4, a + 4)):
                ptr[a] = name_for(w)                   # a table's next entry: data near the data its neighbour points at
        if len(labels) == before and len(ptr) == before_ptr:
            break

    # Pointers not on a word (packed byte tables): --access FILE's FILE.odd lists those seen used as addresses; a
    # table's next entries (4 bytes on, the same distance from a word, near the data their neighbour points at) too.
    odd_seen = set()
    for path in args.access:
        try:
            odd_seen.update(int(l, 16) for l in open(path + ".odd") if l.strip())
        except FileNotFoundError:
            pass

    def u32(a):
        return int.from_bytes(data[a - base:a - base + 4], "big") if base <= a and a + 4 <= end else None

    def is_data(a):
        return all(not (x in code and x not in raw) for x in (a & ~3, (a + 3) & ~3))

    optr = {}
    todo = [a for a in sorted(odd_seen) if a & 3 and is_data(a) and u32(a) is not None and base + 0x100 <= u32(a) < end]
    for a in todo:
        optr[a] = None
    while todo:
        a = todo.pop()
        for n in (a - 4, a + 4):
            v = u32(n)
            if n in optr or v is None or not is_data(n) or not (base + 0x1000 <= v < end) or abs(v - u32(a)) >= 0x10000:
                continue
            optr[n] = None
            todo.append(n)
    for a in optr:
        optr[a] = fn_names.get(u32(a)) or name_for(u32(a))
        for w0 in (a & ~3, (a + 3) & ~3):
            ptr.pop(w0, None)
    odd_words = {}                                     # word -> the odd pointers that cover part of it
    for a in optr:
        for w0 in (a & ~3, (a + 3) & ~3):
            odd_words.setdefault(w0, []).append(a)

    inserts = {int(x.split(":")[0], 0): int(x.split(":")[1], 0) for x in args.insert}
    drops = set()
    for x in args.drop:
        d0, n = int(x.split(":")[0], 0), int(x.split(":")[1], 0)
        for d in range(d0, d0 + 4 * n, 4):
            if word(d) not in (0, 0xFFFFFFFF) or d in labels or (d in code and d not in raw):
                sys.exit("reasm: --drop %x: word %x is not an unlabelled zero (or erased, 0xFFFFFFFF) data word" % (d0, d))
            drops.add(d)

    in_code = None
    skip_to = base
    done_to = base                                     # bytes written so far (an odd pointer runs past a word)
    for a in range(base, end, 4):
        if a < skip_to:
            continue
        if a < done_to:                                # this word's start already written, in an odd pointer
            if a in labels:
                out.write("\t.set %s, . - %d\n" % (labels[a], done_to - a))
            if a in drops or a in inserts:
                sys.exit("reasm: --insert/--drop at %x, inside a pointer not on a word" % a)
        if a in odd_words or a < done_to:
            if a >= done_to and a in labels:
                out.write("%s:\n" % labels[a])
            b = max(a, done_to)
            while b < a + 4:
                if b in optr:
                    out.write("\t.long %s\n" % optr[b])
                    b += 4
                else:
                    out.write("\t.byte 0x%02x\n" % data[b - base])
                    b += 1
            done_to = b
            in_code = False
            continue
        if a in inserts:
            out.write("\t# %d words inserted (movability test)\n" % inserts[a])
            for _ in range(inserts[a]):
                out.write("\tnop\n")
        if a in drops:
            continue
        if a in labels:
            out.write("%s:\n" % labels[a])
        w = word(a)
        is_code = a in code and a not in raw
        if is_code != in_code and a not in labels:
            out.write("\n")
        in_code = is_code
        if is_code:
            t = text.get(a, "")
            if not t or t.startswith(".long") or "(bad)" in t:
                out.write("\t.long 0x%08x\n" % w)
                continue
            if a in hint:
                mn, _, rest = t.partition(" ")
                t = mn + hint[a] + (" " + rest.strip() if rest else "")
            if a in branch:
                t, n = tgt_re.subn(labels[branch[a]], t)
                if n != 1:
                    out.write("\t.long 0x%08x\t# %s\n" % (w, t))
                    continue
            t = symbolize(a, t)
            out.write("\t%s\n" % t)
        elif not args.no_strings and (run := text_run(a)) is not None and not any(
                x in odd_words for x in range(a, run[0], 4)):
            skip_to, lines, _ = run
            for l in lines:
                out.write(l + "\n")
        elif a in ptr:
            out.write("\t.long %s\n" % ptr[a])
        else:
            out.write("\t.long 0x%08x\n" % w)
    tail = data[end - base:]
    if tail:
        out.write("\t.byte %s\n" % ", ".join("0x%02x" % b for b in tail))
    # Names of addresses that are not a word of the image.
    out.write("\n# Addresses inside the image but not on a word\n")
    for v, n in sorted(addr_names.items()):
        if labels.get(v) != n and base <= v < end:
            w0 = v & ~3
            out.write("\t.set %s, %s + %d\n" % (n, labels[w0], v - w0))
    out.close()
    if args.names:
        apply_names(args.out, args.names)
    print("wrote %s: %d code words, %d labels, %d kept raw" %
          (args.out, sum(1 for a in code if base <= a < end and a not in raw), len(labels), len(raw)),
          file=sys.stderr)


if __name__ == "__main__":
    main()
