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
| built compilers, assembler, sources | WSL `~/oldgcc` (`build-snap970101-s/cc1` is the one to use; `tools/bin/powerpc-eabi-*`; `src/`, `dl/`) |
| the ROMs | `Z:\Emu\spikeout-recomp\build\roms\spikeofe` (main checkout; never through a junction you might delete) |

WSL commands are run from PowerShell as `wsl -e bash /mnt/z/.../script.sh`
(quoting `bash -c '...'` through PowerShell breaks easily: write a small
script file and run that).

### On a fresh machine

1. `bash tools/old-gcc/build.sh` (binutils 2.42, GCC 2.7.2.3 stock and
   patched; 32-bit host libraries unpacked without root).
2. `bash tools/old-gcc/build-snapshot.sh 1997-01-01` (GCC's development tree
   of 1 January 1997 from GNU's archive, with SEGA's frame changes) gives
   `~/oldgcc/build-snap970101-s/cc1`.
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

Not exactly any public day of GCC: one loop (0x791CC) matches only the
2.7.2.3 release's loop optimiser, and a few functions stay a line or two off
with every compiler tried (see "Open questions"). Likely a Cygnus release
cut from that line with small changes of its own.

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

- Loops often step a **byte offset** (`off += 4`, compared against `n*4`)
  and use integer addresses: `*(unsigned int *)(i + base)`.
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

## Status (2026-11-13): 39 of 57 match (3,219 of 5,809 instructions)

Match byte for byte: 78488 785ac 786b8 788dc 78af8 78bf4 78fd8 79048 790c8
79240 79260 792c0 79344 7937c 79438 79568 7958c 795b0 7979c 797e8 79834
798bc 798f8 7991c 79944 79974 799b0 799ec 79a28 79a38 79f44 7a194 7a2fc
7a56c 7af98 7b534 7c1e8 7cce8 7df08. (New on 2026-11-13: 79438 by the cr6/cr7
reservation, 7b534 by the FPR reservation, 7c1e8 by the source forms above,
7cce8 written and matched.)

Written, not yet matching (differing lines in the side-by-side diff; what's
left):

| function | size | diff | what's left |
|---|---|---|---|
| 78b24 | 52 | 2 | one `li r0,0` placed before instead of after an address load (loop counter init order) |
| 791cc | 29 | 2 | same `li` placement; matches with the 2.7.2.3 release's loop optimiser (`build-2.7.2.3s6`) |
| 79880 | 15 | 2 | order of two argument moves; matches with `-fno-schedule-insns`, which breaks others |
| 79c30 | 17 | 2 | which register gets `ori 0x40` (operand order of an OR; `b \|= 0x40`, reordered ORs and brackets tried) |
| 7bd14 | 309 | 2 | one argument: the game recomputes the address `d_72003c` (`addi r5,r22,60`) for the call that starts the walking-0's part, ours reuses the copy in r27 (CSE follows the path out of the check loop); its twin 7c1e8 does reuse it in the game, so the source differs there somehow (tried: `continue` forms, `while`/`do` loops, `goto`, a block around the call, `result = f_79a38(...)`) |
| 7a098 | 63 | 12 | register choice (a/d swapped) and compare operand order |
| 7a474 | 62 | 14 | layout/register details |
| 79df4 | 84 | 19 | `bit` computed per branch then moved; `last` computed first (done) |
| 79698 | 65 | 27 | a hoisted constant 3 in a register; register numbering |
| 79fec | 43 | 27 | register allocation of the result/`err` and indices |
| 79134 | 38 | 30 | two loops: the value/offset forms (try byte-offset forms) |
| 7bc0c | 66 | 34 | which values GCC moves out of the menu loop |
| 79c74 | 96 | 53 | branch-free `?:` selection (`neg`/`srawi` mask) not produced |
| 7c6d8 | 388 | 103 | the depth-buffer test; structure, frame and registers right; only `d_720084`'s high half: ours moves it out of the whole function (its copy hoisted out of the nested row/column loop joins the other three loops' copies and their combined lifetime makes the move worthwhile), the game keeps it in each single loop |
| 7a8bc | 439 | 106 | first 147 instructions exact; the second copy of the set-up macro is scheduled differently in the game (so only its tail merges); ours merges fully |
| 7dc90 | 158 | 107 | layout now right; register numbering (which value gets r21/r22...) |
| 7b6f4 | 326 | 314 | prologue exact; spills (game spills 4 more values, frame 560 vs ours) |
| 7d740 | 340 | 357 | twin of 7b6f4 (walk 1's/0's address test), same spill difference |

All 57 are written now.

## Open questions (compiler)

Several near misses look like scheduling or allocation decisions rather
than C: 78b24/791cc (`li` placement), 79880 (argument move order), 7a8bc
(the two copies' order). Ideas not yet tried:

- the development tree on other days between 1 Nov 1996 and 1 Mar 1997
  (only the 1st of each month was tried; all equal so far);
- the 2.7.2.3 release's `loop.c` inside the January tree (one loop matches
  only 2.7.2.3);
- `-mcpu=603e`-era scheduling descriptions;
- Cygnus releases of early 1997, if their sources can be found (GPL: they
  were distributed with source).

When a function is stuck a line or two off whatever the C, suspect the
compiler first: a fix there helped many functions at once (r12/r14/r15,
the CR save, the frame rounding).

## Next steps, in order

1. The 2-line near misses (78b24, 791cc, 79880, 79c30, 7bd14): source forms
   first, then the compiler ideas above. Then 7c6d8 (103 lines, one hoisting
   decision; `ld2.sh` shows it).
2. Re-test the older near misses with the new reservations in mind: a
   function "a few constants hoisted differently" is now often a source form
   (one variable per loop, statement order inside the loop).
3. The register-allocation ones (7b6f4, 7d740, 7dc90, 7a8bc): try
   declaration orders, variable types and which values are named.
4. Keep `docs/compiler-id.md`'s "Next steps" progress line up to date; commit
   only that and generic tools, on this branch, when the owner asks.
