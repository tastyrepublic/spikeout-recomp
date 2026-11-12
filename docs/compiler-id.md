# Which compiler made Spikeout? (step 1 of a possible matching decompile)

Written 2026-11-12. Research only: no game code was changed. This page names
addresses and describes patterns; it holds no ROM bytes and no listings.

## The answer in short

**Almost none of the game was made by a compiler.** About 95% of the main
PowerPC program (some 2,850 of the 2,901 functions the analysis finds,
114,000 of 119,500 instructions) is **hand-written assembly language**: SEGA
AM2's programmers wrote the PowerPC instructions themselves, with their own
shorthand ("macros") and their own rules for how functions call each other.
Confidence: **high**.

The other ~5% is one block of **47 functions written in C** (RAM
0x78488-0x7DF48, about 22 KB, at the very end of the code, just before the
data). It looks like test-menu code for the video board (it sends bit-by-bit
read and write commands and prints numbers on the test screen; the nearby
texts are the video board's RAM and JTAG tests). That block was compiled by
**GCC 2.8.1 (1998), with `-O2 -mno-eabi`, changed in two small places to
follow SEGA's house calling rules.** This is proved: the rebuilt compiler
gives four of the game's functions **byte for byte** (see "The test
compiles" below). Most likely the build came to SEGA from Cygnus, as its
Saturn GCC did; that part is unproven.

Updated the same day after the test compiles. The first version of this page
guessed "GCC 2.x, version unknown" and said the cr1 compares were one of
SEGA's changes; the tests showed they are stock GCC 2.7/2.8 behaviour.

What it means for a matching decompile:

- For the hand-written 95% there is no compiler to match. C that compiles to
  the same bytes isn't possible in practice (no C compiler writes code like
  this). The equivalent goal is a **re-assemblable disassembly**: assembly
  source, with names and comments, that an ordinary PowerPC assembler turns
  back into the same bytes. That needs no special tools: GNU `as` for
  PowerPC (free) does it, and the project's own analysis already knows
  every function, block and jump table.
- Only the 47-function C block can be decompiled to matching C, with GCC
  2.8.1 rebuilt with SEGA's changes: `tools/old-gcc/build.sh` builds it in
  WSL and `tools/old-gcc/compare.sh` checks a function against its bytes.

## How it was found

The program as the game runs it (the 1 MB the boot code copies from ROM
0xFFE00000 to RAM 0) was disassembled by the project's own `codemap
--scan-pointers --disasm` into a scratch folder outside the repository.
`tools/compiler-id-stats.awk` counts the habits listed below in two ranges.

### 1. Text in the program: no compiler's name

Searched the whole 8 MB fixed program ROM and the RAM copy for Metrowerks,
CodeWarrior, MSL, Green Hills, GHS, Diab, Wind River, GNU, GCC, libgcc,
Cygnus, newlib, version and copyright lines, source file names and assert
texts. **Nothing.** The only copyright lines are SEGA's own (the Model 3
system program's, from 1995, and the game's, 1998-99). The library-like
texts are SEGA's own too: its own memory allocator's message, its own
`printf`-style screen printer, error screens. A compiler's library (MSL,
newlib, Green Hills' or Diab's) almost always leaves texts such as "(null)",
"NaN", "Inf" or assert formats; none are here.

### 2. Function starts and ends

**The hand-written part** (call it "game code"):

- A function makes room on the stack 16 bytes at a time: it moves the stack
  pointer down by 16, saves the return address at the *bottom* of that space
  (offset 0), then saves registers r16, r17, r18... upward, four to a 16-byte
  group, moving the stack pointer down again for each next group. 1,700 such
  16-byte steps; the ending undoes them group by group.
- It never writes the "back chain" every PowerPC C standard requires (zero
  `stwu r1` in the game code); the standard (System V / EABI) puts the
  return address at offset 4 of the caller's space, not 0 of its own.
- Larger functions call a shared save routine (0x37258) and a restore
  routine (0x372D4), 248 calls each, with the return address handed over in
  r2. They save r16-r31, **f12-f19 as single-precision floats**, CTR and the
  condition register. No compiler's helpers save that set: GCC's and
  CodeWarrior's (`_savegpr_N`, `_savefpr_N`) save r14-r31 and f14-f31 as
  doubles, and never CTR.
- Arguments arrive in r3, r4, r5... (like C), and are copied into r16, r17,
  r18... at the start.
- The screen printer (0x6D84) takes its format text in **r2** and its values
  **on the stack**, and keeps r3-r6 intact. No C convention passes a
  `printf`'s values on the stack while registers are free.

**The C block** starts and ends the way GCC 2.x does on PowerPC: `stwu r1`
for the whole frame at once; the saved registers stored one by one from the
lowest up at the top of the frame (no `stmw`); the condition register saved
just below them; on the way out the return address first, then the
registers, then the stack pointer moved back. All 19 frame sizes are
multiples of 16 bytes (the System V rule). The 9th argument is read from 8
bytes into the caller's frame, as System V says. Two things differ from a
stock GCC, and both are what SEGA's rules need:

- The return address is saved at offset 4 of the function's **own** frame,
  not the caller's. A stock GCC function called from the game code would
  overwrite the game code's saved r16 there. So the compiler must have been
  changed to fit.
- The condition register is saved through **r2** (stock GCC uses r12).
  Also r12, r13, r14 and r15 are never touched (the game code uses r13 as
  its small-data base at 0x108000 and r14/r15 as its own pointers): maybe
  reserved from the compiler, maybe just never needed in this small block.

The test compiles confirmed the first change and that a 16-byte frame comes
from the option `-mno-eabi`; the second change is in the patch but no tested
function exercises it yet.

### 3. Runtime helpers: none from a compiler

No 64-bit divide, modulo or shift routines (`__divdi3`, `__ashldi3`,
`__div2i`...), no float-to-integer helpers, no `setjmp`. The game code does
its divides inline (183 `divw`), turns integers into floats inline with the
usual 2^52 trick, and has its own small hand-written copy, fill and string
routines (fill at 0x4EA0, string copy at 0x10274, string append at 0x10290:
pre-decremented pointers with update forms, a style typical of hand-written
code). The C block uses no floating point, divides or 64-bit values at all,
so it pulls in no helper. Its one string compare (0x7DF08) is ordinary
compiled code.

### 4. Habits

| habit | game code | C block |
|---|---|---|
| instructions | 113,971 | 5,511 |
| `lis r2` (r2 as a scratch register) | 2,470 | 0 |
| moves written as `addi rX,rY,0` / as `mr` | 2,551 / 1,169 | 8 / 334 |
| single bits tested by rotating into bit 0 | 2,226 | 0 |
| conditional calls (`beql`, `bnel`...) | 90 | 0 |
| endless "stop here" loops | 40 | 0 |
| compares in cr0 / in another field | 3,547 / 1,547 | 12 / 197 |
| `stwu r1` frames | 0 | 41 |
| GCC's branch-free compare sequences | rare | 12 |

What the game code's habits say:

- **r2 as the assembler's scratch register.** Loading a value from an
  address is written as `lis r2,high` then a load through r2, the same
  `lis r2` repeated three times in a row for three neighbouring constants.
  That is an assembler expanding a one-line shorthand (MIPS assemblers do
  the same with their "at" register). A compiler would load the high part
  once; and every PowerPC C standard reserves r2.
- **Mixed ways of writing the same thing** (`addi x,y,0` and `mr` for a
  move), bit tests by rotation, conditional calls, functions with several
  entry points and shared endings, and values kept in registers across calls
  that any C standard says a call may destroy (the writer knew the called
  routine didn't touch them). Compilers are consistent and never do the
  last three.
- r13 is the small-data base (0x108000, 4,170 uses); CR fields 2-7 are kept
  across calls by the save routine (it restores with `mtcrf 0x3f`), cr0 and
  cr1 are scratch.

What the C block's habits say (GCC fingerprints):

- **GCC's own branch-free sequences for comparisons** from its PowerPC
  machine description: "x == 0" as `subfic` then `adde`; "unsigned a < b"
  as `subfc`, `subfe`, `neg`; a compare result taken from the condition
  register with `mfcr` then `rlwinm`. CodeWarrior and Diab use `cntlzw` or
  branches for these.
- **Scratch registers r0, r9 and r11 first**, then r10, r8...: exactly GCC's
  PowerPC allocation order (`REG_ALLOC_ORDER` in `config/rs6000/rs6000.h`:
  r0, r9, r11, r10, r8, r7...).
- **Compares in cr1**, cr0 only when cr1 is busy, cr2 (saved and restored
  with `mtcrf 0x20`, like GCC's saved fields) when more must live together.
  The test compiles showed this is simply what GCC 2.7.2.3 and 2.8.1 do;
  GCC 2.95.3 uses cr0 instead (and later GCCs cr7 first).
- CTR used now and then as a spare register (`mtctr`/`mfctr`), which GCC
  2.x allowed.
- No `stmw`/`lmw`: GCC 2.8.1's PowerPC target saves registers one by one.
- Instruction scheduling (compares moved ahead of stores) and pointer
  stepping in loops: GCC 2.x does these at `-O2` (the tests confirm `-O1`
  gives different code). No counted loops with `bdnz` even where they fit:
  GCC 2.8.1 doesn't make them.
- No small-data accesses (no r13 use): `-msdata=none` or `-G 0`.

### 5. Each candidate, for and against

| candidate | for | against |
|---|---|---|
| **Hand-written assembly** (game code) | everything in 2 and 4: r2 as scratch, stack-passed `printf`, conditional calls, shared tails, mixed styles, a save routine no compiler uses | none found |
| **GCC 2.8.1** (C block) | proved: four functions byte for byte with two small changes; GCC's compare sequences, register order, frame layout; SEGA used Cygnus' GCC for the Saturn | needs two prologue changes no option gives (a modified build) |
| GCC 2.7.2.3 | same code inside the tested functions | no 16-byte frame option (`-mno-eabi` doesn't exist yet): would need a third change |
| GCC 2.95.3 | GCC | different registers, cr0 instead of cr1, different code: ruled out |
| egcs 1.0.x (late 1997) | close relative of 2.8.1 | not tested (its download failed); could match as well |
| Metrowerks CodeWarrior | was used for embedded PowerPC in 1997-99 | saves the return address before or at frame+4, uses `stmw`, `cntlzw` for "== 0", r3-r12 scratch order, MSL texts; none seen |
| Green Hills MULTI | big in embedded PowerPC then | standard frame (return address at frame+4), its library texts and helpers; none seen |
| Diab Data | likewise | standard frame, its own helper names and sequences; none seen |
| Microtec, MetaWare High C, SDS | existed for PowerPC | nothing matches; no texts |

That SEGA used Cygnus' GCC for Model 3 isn't documented anywhere found:
SEGA's Saturn SDK shipped Cygnus' GCC for the SH chips, and Cygnus sold a
PowerPC embedded GCC (GNUPro, `powerpc-eabi-gcc`) in 1998, so it's likely but
unproven. The video board was made by Real3D (Lockheed Martin); its test code
may have come from Real3D, also likely built with GCC.

## The test compiles (2026-11-12)

**Getting the compiler.** GCC is free software: no licence question.
`tools/old-gcc/build.sh` builds everything in WSL (Ubuntu 24.04) without
root, in a few minutes on 3 cores:

- GCC 2.8.1 from the GNU site (checked by SHA-256), only its compiler proper
  (`cc1`), as a cross compiler for `powerpc-eabi`, twice: stock, and with
  `tools/old-gcc/gcc-2.8.1-sega.patch` (SEGA's changes, 6 lines in
  `config/rs6000/rs6000.c`: the return address stored at offset 4 of the new
  frame and read back from there; the condition register saved through r2
  instead of r12).
- Old GCC only builds as a 32-bit program, so Ubuntu's own 32-bit C library
  packages are unpacked into a folder (not installed) and used through a
  small wrapper; compiler flags `-std=gnu89 -fcommon` let a modern GCC build
  the 1998 source unchanged.
- binutils 2.42 (current) for the assembler, linker and `objdump`.

GCC 2.7.2.3 and 2.95.3 were built the same way for comparison (by hand; the
script builds 2.8.1 only).

**Comparing.** `tools/old-gcc/compare.sh` compiles a C file, links it at the
function's real address (with the addresses of what it calls and uses) and
compares every byte with the function's bytes, cut from the RAM image into a
scratch folder (`romcheck --dump-crom`, then the 1 MB at offset 0x600000).
The C files are written by hand from what each function does; they are made
from the ROM, so they stay private like the bytes.

**Results** (options `-O2 -mcpu=603 -mno-eabi`; `-mcpu` makes no difference
to these):

| function | what it does | GCC 2.8.1 + SEGA patch |
|---|---|---|
| 0x7DF08, 17 instructions | compares two strings | **byte for byte** |
| 0x79240, 8 instructions | swaps a word's byte order | **byte for byte** |
| 0x79568, 9 instructions | calls another function with a fixed second value | **byte for byte** |
| 0x79260, 24 instructions | reads two swapped words into two globals | **byte for byte** (even saving two registers it never uses, as the game does) |
| 0x792C0, 33 instructions | a loop copying swapped words | same prologue and ending; the loop differs: the right C not found yet |

And what rules the others out:

- GCC 2.95.3: different registers and cr0 for compares in every test.
- GCC 2.7.2.3: the same instructions inside the leaf functions, but it has
  no option for 16-byte frames, so it would need one more change.
- Stock GCC 2.8.1: the leaf functions match; framed ones store the return
  address at frame+4 of the caller (no option changes that), and without
  `-mno-eabi` frames are 8-byte aligned.
- `-O1`: different code; `-O3` gave the same as `-O2` here.

So: **GCC 2.8.1 (or egcs 1.0, its close relative, not tested), `-O2
-mno-eabi`, with the return address kept in the function's own frame.**
Confidence: high for the version family and options, given four exact
matches of different kinds; the CR-through-r2 change is still untested.

## Next steps

- For the C block (47 functions): write each function's C until
  `compare.sh` says MATCH, starting with the small ones, then the loops
  (0x792C0 shows the loop's exact form matters) and a function that saves
  the condition register (0x7B6F4 or 0x7BD14) to test the r2 change. If
  egcs 1.0.3 can be found, test it too.
- For the hand-written 95%: no compiler is involved. Make `codemap`'s
  disassembly re-assemblable (labels instead of addresses, data kept as
  data) and check that GNU `as` gives back the same 1 MB, byte for byte.
  That too stays private.

## Sources

- GCC's PowerPC register allocation order (the comment and `REG_ALLOC_ORDER`
  in `gcc/config/rs6000/rs6000.h`; read in GCC 4.6.4's copy; GCC 2.7/2.8 use
  cr1 first, as the tests showed):
  https://git.midipix.org/cbb/cbb-gcc-4.6.4/plain/gcc/config/rs6000/rs6000.h
- Cygnus GNUPro for PowerPC (1998): https://ecos.sourceware.org/docs-1.1/ref/gnupro-ref/powerpc/index.html
- SEGA's Saturn SDK and Cygnus' GCC: https://retroreversing.com/sega-saturn-sdk
- Model 3's CPU (PowerPC 603e): https://www.segaretro.org/MODEL3
- GCC and binutils sources: https://ftp.gnu.org/gnu/gcc/ and https://ftp.gnu.org/gnu/binutils/
