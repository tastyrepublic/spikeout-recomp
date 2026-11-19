# Hand-off: the matching decompile of the C block (2026-11-12)

For the next session that takes over this job. Read `docs/compiler-id.md`
first (what was found and why); this page is how to carry on. Everything
here is about the decompile and the branch `claude/compiler-id-0d37da`
(worktree `Z:\Emu\spikeout-recomp\.claude\worktrees\compiler-id-0d37da`);
nothing else in the project is touched by it.

## The job

Spikeout's main PowerPC program is ~95% hand-written assembly (no compiler to
match) plus one block of **57 functions written in C**, RAM 0x78488-0x7DF4C
(the video board's test screens, JTAG scan routines and board set-up). The
goal: C that, compiled with the original compiler and settings, gives every
one of those functions **byte for byte**.

## The owner's rules (they apply here too)

- A decompile is **private**: never commit, push or share the C, the bytes,
  disassembly listings or anything else made from the ROM. Only generic
  tools and descriptive docs (like this one) go in the repository.
- Commits only when the owner asks (this branch's commits so far were
  asked for). Never push or open a PR unless asked.
- Prove claims with evidence (here: `MATCH` from the compare script).
- Explain in plain English: the owner doesn't write code.
- Keep CPU use moderate (`nice`, 3 jobs) when building; other sessions may
  be running network tests.

## Where everything is (this PC)

| what | where |
|---|---|
| report and findings | `docs/compiler-id.md` (this branch) |
| compiler build scripts | `tools/old-gcc/build.sh`, `build-snapshot.sh`, `compare.sh`, `gcc-2.7.2.3-sega.patch` |
| **private work folder** (the C, the game's RAM image, scripts) | `Z:\Emu\spikeout-private\cblock` (WSL: `/mnt/z/Emu/spikeout-private/cblock`); its `README.txt` and `PRIVATE.txt` |
| built compilers, assembler, sources | WSL `~/oldgcc` (`build-snap970101-v/cc1` is the one to use; `tools/bin/powerpc-eabi-*`; `src/`, `dl/`) |
| the ROMs | `Z:\Emu\spikeout-recomp\build\roms\spikeofe` (main checkout; never through a junction you might delete) |

WSL commands are run from PowerShell as `wsl -e bash /mnt/z/.../script.sh`
(quoting `bash -c '...'` through PowerShell breaks easily: write a small
script file and run that).

### On a fresh machine

1. `bash tools/old-gcc/build.sh` (binutils 2.42, GCC 2.7.2.3 stock and
   patched; 32-bit host libraries unpacked without root).
2. `VENDOR=1 bash tools/old-gcc/build-snapshot.sh 1997-01-01` (GCC's development tree
   of 1 January 1997 from GNU's archive, with SEGA's frame changes) gives
   `~/oldgcc/build-snap970101-v/cc1` (without VENDOR=1: `-s`, the public tree).
3. The game's RAM image: `romcheck build\roms\spikeofe --dump-crom crom.bin`
   (main checkout's build), then the 1 MB at offset 0x600000 of that file is
   RAM 0-0xFFFFF: put it in the private folder as `ram.bin`.
4. The private scripts (`match.sh` etc.) are only in the private folder;
   copy that folder (it is private: not through the repository).

## The compiler (settled)

GCC's own development tree of winter 1996-97 (the trees of 1 Nov 1996 to
1 Mar 1997 all give the same results; 1 Apr 1997 is worse) with SEGA's
changes: return address saved at offset 4 of the function's own frame, the
condition register saved through r2, frames and the stack-argument area
rounded to 16. Options:

    -O2 -mcpu=603 -mno-traceback -fno-exceptions -ffixed-12 -ffixed-14 -ffixed-15
    -ffixed-cr6 -ffixed-cr7 -ffixed-fr0 ... -ffixed-fr13

(r12, r14, r15 reserved: proven by register choices and a CTR spill that only
appear with them reserved. cr6/cr7 and 14 FPRs added 2026-11-13: the C block
never uses cr5-cr7 or any FPR; reserving cr6/cr7 puts comparisons in cr0 as
the game does, and 14 fewer usable registers make the loop optimiser keep
exactly the game's constants in its loops; see `docs/compiler-id.md`).
`match.sh` uses exactly this by default (`CC1` and `OPTS` can override it).

Not exactly any public day of GCC: three small vendor changes are needed
too (`combine_givs` from 2.7.2.3, a loop counter replaceable by a derived
value with a constant offset, and 16-byte-aligned spill slots;
`docs/compiler-id.md`). Build that
compiler with `VENDOR=1 bash tools/old-gcc/build-snapshot.sh 1997-01-01`: it
gives `~/oldgcc/build-snap970101-v/cc1`, which `match.sh` uses by default
(checked: rebuilt from scratch, every function gives the same as the
compiler it was found with). Likely a Cygnus release cut from that line with
small changes of its own; a few functions still stay a line or two off on
instruction order (see "Open questions").

## The workflow (private folder)

| script | what it does |
|---|---|
| `show.sh ADDR` | the game's instructions of a function (from `cblock.raw`) |
| `match.sh ADDR` | compiles `fADDR.c`, links it at ADDR, compares every byte; prints MATCH or a side-by-side diff |
| `d.sh ADDR...` | only the differing lines; `dl.sh ADDR [N]` all differing lines with line numbers; `m2.sh ADDR a,b` a range of the full diff |
| `tryvar.sh ADDR` | tries every `var/*.c` as the function, keeps the first MATCH, else restores the original |
| `all.sh`, `count.sh` | every function's status; totals |
| `lay.sh ADDR` | where our compiler put locals on the stack (from `last.s`, our last assembly) |
| `asm.sh ADDR` | branch/label structure of our assembly |
| `vt.sh ADDR FILE...` | tries each FILE as the function, prints each result, puts the original back (variants live in `vADDR/`) |
| `m.sh ADDR FROM TO` | lines FROM-TO of the full side-by-side diff |
| `ld.sh ADDR`, `ld2.sh ADDR N` | the loop optimiser's decisions (what it moved out of each loop and why); `ld2.sh` shows loop N (in the order GCC processes them: last loop first) with each candidate's RTL |
| `rg.sh FILE [REG]` | register use counts and lengths before allocation, and where each pseudo went |
| `sweep.sh K...`, `cmpcc.sh CC1 [opts]` | every function under K reserved FPRs / another compiler or option, to test a compiler change against all 57 at once |

Everything else in the folder (`s*.sh`, `snap*.sh`, `cpu*.sh`, `fu*.sh`,
`g.sh`...) were one-off experiments while dating the compiler; `funcs.txt`
lists the 57 functions (start, length in instructions), `starts.txt` their
starts.

### Conventions in the C files

- One file per function: `fADDR.c` (lower-case hex, e.g. `f7a56c.c`).
- Names `f_XXXXX` (a function at 0xXXXXX) and `d_XXXXXX` (data at that
  address) get their addresses automatically at link time. `strcpy` and
  `strcat` are linked to the game's own (0x10274, 0x10290).
- Always give full prototypes: without them GCC adds a `crclr` before
  calls, which the game never has.
- String constants: put `/* RODATA=0x80c8c */` (the address of the first
  string the function uses) in the file; `match.sh` places `.rodata` there.
  The strings must then appear in the same order as in the game, including
  ones the game emitted but never uses: an `if (0) strcpy(t, "...");` emits
  such a string without code.
- `strcpy(buf, "constant")` is expanded inline by GCC (word copies): that is
  how the original copies titles; with a non-constant source it calls the
  game's `strcpy`.
- Common prototypes: see `f7b6f4.c` (JTAG scan `f_79a38` with 9 arguments,
  `f_79fec`, `f_7a474`, `f_1036c` menu, `struct line` for `f_ff7c`).

### What made functions match (the original's style)

- With the `-v` compiler, plain index loops (`for (i = 0; i <= 80; i++)
  a[i] = ...`) give the game's stepped byte offsets; several older files still
  use the byte-offset form that was needed before, which also matches. A
  second loop over the same array may use its own counter variable (0x79134).
- The menu screens are `for (;;) { switch (menu(...)) { ... } }`, not
  `goto` loops (the optimiser only moves values out of real loops: 0x7BC0C).
- Values computed into variables first (`s = k % 32; w = k / 32;`, 0x79FEC;
  `err = 0;` after the call, not at the declaration) and stores through a
  stepping pointer (`*p++ = ...`, 0x7DC90) show in the order and registers.
- A constant passed through a variable (`n = 3` declared, then
  `f(..., n)`) is moved out of a loop into a register; a literal is not
  (0x79698).
- A small buffer's size shows in the frame: 0x7A474's check buffer is 2 words.
- The JTAG bit routine 0x79C30 takes `unsigned char` arguments: callers
  declared it `unsigned int f_79c30(unsigned char, unsigned char)`, which is
  why 0x79DF4 computes the bit in r0 and copies it to r3.
- An `if (...) f(x, A); else f(x, B);` compiles to one shared call (GCC merges
  the identical tails) and is what 0x7DC90 had, not `f(x, c ? A : B)`.
- A repeated constant was sometimes a variable (`k = 0x800000;` before the
  loop in 0x7DC90); that changes which register it gets.
- Return types matter to the scheduler: it boosts an instruction that sets a
  register set only once, and a call whose value is used sets `r3` again.
  0x79880 and the function it calls both return nothing (`void`).
- A parameter copied into a local first (`unsigned int *p = p0;` in 0x79698)
  changes the order of the copies at the start.
- 0x7A8BC's second ID test is `((buf[6] >> 5) | (buf[7] << 27)) & 0x0fffffff`
  (the same style as its first), and its third set-up copy has no
  `d_7201f8 = 196`.
- To see why the scheduler ordered two instructions, `sd.sh ADDR` prints its
  dump (ready lists and priorities; `7f000001` is the boost).
- 2026-11-15: 0x7BD14's unused "Walk 1's"/"Walk 0's" assignments were each
  wrapped as `do { msg = "..."; } while (0);` (a macro, most likely): the
  extra loop notes stop CSE following the path out of the check loop, which
  is why the game recomputes one address there and its twin 0x7C1E8 does not.
- 0x7D740: all four passes test `bit < top2 >> 1`, with `top2 = top;` once
  after `top` is computed.
- 0x7DC90's last loop is `for (i = 0; i < 16; i++)` with `i * 2 + 3` written
  for the row in each call (with the vendor rule above, the row replaces `i`).
- Sizes from macros: `(end - start) / 4`.
- Arrays of 8 bytes or less are placed on the stack late (in order of first
  use), larger ones in declaration order: the frame layout tells you the
  array sizes.
- `a = b = c` stores `b` first: `d = v[0] = 140` matches where `v[0] = d = 140`
  doesn't.
- `switch` gives the compare-tree shape (`== 1`, `< 1`, `== 2`...), an
  `if` chain gives sequential compares.
- Repeated blocks were macros (e.g. the board set-up tables in 0x7A8BC).
- Bit fields for hardware status words (0x7AF98's `struct sys`): GCC then
  writes them with `rlwimi`.
- `if (quick) sel = 99;` compiles to the branch-free mask code seen in the
  test screens.
- The memory-test screens (7bd14, 7c1e8, 7c6d8, 7cce8), learnt 2026-11-13:
  - the unused strings ("Walk 1's" etc.) were **real assignments** to the
    message variable right after each check loop (`msg = "Walk 1's";`), not
    dead code: GCC deletes them only after its jump threading, and their
    presence is what stops the threading the game doesn't have (the break
    jumps to a second test after the loop instead of straight to the report);
  - each check loop's message is **its own variable** (`msg1`...`msgN`, or a
    `char *msg;` declared in each nested `if` block): one shared variable
    gets a register too early;
  - the break test is written `if ((result = (unsigned char)f_79fec(...)) == 1) break;`;
  - order inside a loop decides the order of values moved out of it (0x7CCE8:
    the message assigned after the first data word in two loops);
  - the report's text structure was `struct line s[2]` (16 bytes) in 7c6d8
    and 7cce8 (the frame is 16 bytes bigger than with one), one in 7bd14/7c1e8.

## Status (2026-11-15): all 57 match (5,809 of 5,809 instructions)

Every function of the C block compiles byte for byte to the game's code with
`~/oldgcc/build-snap970101-v/cc1` (built by `VENDOR=1
tools/old-gcc/build-snapshot.sh 1997-01-01`) and the options above; `all.sh`
says `matched 57 of 57`.

The last ones and what made them match (2026-11-13 to 15): 79438 by the
cr6/cr7 reservation; 7b534 by the FPR reservation; 78b24, 791cc and 79134 by
the loop-optimiser changes with index loops; 7b6f4 and 7a8bc by the
spill-slot change; 7dc90 by the vendor's counter-replacement rule (constant
offsets, not pointer ones); 7c1e8, 7c6d8, 7cce8, 7bc0c, 79fec, 7a474, 79c30,
79df4, 79698, 79880, 7bd14, 7d740, 7a098 and 79c74 by source forms (see the
style notes above). Two of the last were macros: 0x7A098's scan call and
0x7BD14's unused message assignments were each inside `do { ... } while (0)`.
0x79C74's mask was built in two steps (`m = mask[w] >> s;` then the data bit,
then `m = (m & 1) ? 0xffffffff : 0;`).

## Open questions (compiler)

None left for the C block. Not exactly any public day of GCC: the January
1997 tree (1 Dec 1996 and 1 Feb 1997 give the same) with SEGA's frame changes
and the vendor's loop and spill-slot changes, most likely a Cygnus release
cut from that line.

## Next steps

1. The C block is done. If the decompile goes on, the next part is the
   hand-written 95%: a re-assemblable disassembly (`docs/compiler-id.md`,
   "Next steps"), kept private like the C. It exists now: `docs/reasm.md`
   (the source rebuilds identically; six moved copies proven the same; its
   section "What the runs don't reach" has the script, spawn-list and set-up
   list formats and how the game's developer menu opens).
2. Commit only the docs and generic tools, on this branch, when the owner
   asks.
