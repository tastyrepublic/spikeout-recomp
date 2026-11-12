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
**GCC, version 2.x (1996-99), most likely a build supplied to SEGA by
Cygnus**, changed to follow SEGA's house calling rules. Confidence: GCC
family **fairly high** (about 85%); the exact version **low**; the settings
about **-O2 with a 603-class CPU** (medium).

What it means for a matching decompile:

- For the hand-written 95% there is no compiler to match. C that compiles to
  the same bytes isn't possible in practice (no C compiler writes code like
  this). The equivalent goal is a **re-assemblable disassembly**: assembly
  source, with names and comments, that an ordinary PowerPC assembler turns
  back into the same bytes. That needs no special tools: GNU `as` for
  PowerPC (free) does it, and the project's own analysis already knows
  every function, block and jump table.
- Only the 47-function C block could be decompiled to matching C, and only
  with a GCC 2.x rebuilt with SEGA's changes (below).

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
- The condition register is saved through **r2** (stock GCC uses r12), and
  r12, r13, r14 and r15 are never touched (the game code uses r13 as its
  small-data base at 0x108000 and r14/r15 as its own pointers). These were
  reserved from the compiler.

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
- **Compares in cr1.** GCC's order for condition fields is cr7, cr6, cr1,
  cr0, then the saved cr4, cr3, cr2 (same file). Using cr1 first and cr0
  only when cr1 is busy is that order with cr6 and cr7 taken away, which
  SEGA's rule (cr2-cr7 kept across calls) explains; cr2 used only when two
  results must live together, saved and restored (`mtcrf 0x20`) like GCC's
  saved fields.
- CTR used now and then as a spare register (`mtctr`/`mfctr`), which GCC
  2.x allowed.
- No `stmw`/`lmw`: GCC with `-mcpu=603` or `603e` doesn't use them.
- Instruction scheduling (compares moved ahead of stores) and pointer
  stepping in loops: GCC 2.x does these at `-O2`. No counted loops with
  `bdnz` even where they fit, which points to a GCC before the loop counter
  optimisation was usual (2.7.x or 2.8.x), or settings that left it off.
- No small-data accesses (no r13 use): `-msdata=none` or `-G 0`.

### 5. Each candidate, for and against

| candidate | for | against |
|---|---|---|
| **Hand-written assembly** (game code) | everything in 2 and 4: r2 as scratch, stack-passed `printf`, conditional calls, shared tails, mixed styles, a save routine no compiler uses | none found |
| **GCC 2.x** (C block) | GCC's compare sequences, register order r0/r9/r11, cr order, frame layout, no `stmw` with `-mcpu=603`; SEGA used Cygnus' GCC for the Saturn | two prologue changes (return address in own frame, CR through r2) that stock GCC doesn't make: a modified build, or a configuration not yet found |
| Metrowerks CodeWarrior | was used for embedded PowerPC in 1997-99 | saves the return address before or at frame+4, uses `stmw`, `cntlzw` for "== 0", r3-r12 scratch order, MSL texts; none seen |
| Green Hills MULTI | big in embedded PowerPC then | standard frame (return address at frame+4), its library texts and helpers; none seen |
| Diab Data | likewise | standard frame, its own helper names and sequences; none seen |
| Microtec, MetaWare High C, SDS | existed for PowerPC | nothing matches; no texts |

That SEGA used Cygnus' GCC for Model 3 isn't documented anywhere found:
SEGA's Saturn SDK shipped Cygnus' GCC for the SH chips, and Cygnus sold a
PowerPC embedded GCC (GNUPro, `powerpc-eabi-gcc`) in 1998, so it's likely but
unproven. The video board was made by Real3D (Lockheed Martin); its test code
may have come from Real3D, also likely built with GCC.

## How to get the compiler

GCC is free software, so no licence question arises. Old versions are on the
GNU FTP site and its mirrors (gcc-2.7.2.3, gcc-2.8.1, egcs-1.0.3, egcs-1.1.2,
gcc-2.95.3), with binutils of the same years for the assembler and linker.

- Build them **on Linux (WSL)**: modern compilers refuse some of their old
  C, so build inside an old Linux in a container (for example Debian 3 or
  Red Hat 6 images) with its own old GCC, or with a modern GCC and
  `-std=gnu89 -fcommon` plus small fixes.
- Configure as a cross compiler: `--target=powerpc-eabi` (or `powerpc-elf`),
  no libraries needed (`make all-gcc`; the test only compiles, never links).
- Then the two SEGA changes, in `config/rs6000/rs6000.c` (the prologue and
  epilogue writer) and `rs6000.h`: return address saved at offset 4 of the
  new frame; CR saved through r2 instead of r12; r12-r15 fixed (or
  `-ffixed-r12 -ffixed-r13 -ffixed-r14 -ffixed-r15`); cr5-cr7 fixed or
  call-saved. These are a few lines each.
- Options to start from: `-O2 -mcpu=603 -mno-multiple -msdata=none -G 0`
  (603e if that version knows it), big-endian (the default).

No commercial compiler is needed, unless the GCC tests fail everywhere; then
CodeWarrior for Embedded PowerPC 3.x/4.x (1998-99) would be the next to try,
obtained legitimately.

## The next step: a first test compile

1. Pick a small C function: the string compare at 0x7DF08 (17
   instructions, no frame), then small framed ones such as 0x79568 or
   0x7958C (9 instructions each).
2. Write the C by hand from its behaviour (kept private, never committed:
   it's made from the ROM).
3. Compile with each GCC version (`powerpc-eabi-gcc -S` and `-c`) and
   compare with the game's bytes, read locally from the ROM by a small
   script in a scratch folder. A leaf function like the string compare
   needs none of SEGA's changes, so it tests the version and options first;
   the framed ones then test the prologue changes.
4. The version whose output matches (or comes closest: register choice,
   instruction order, which compare sequence) is the one; then the options
   are tuned one at a time.

For the hand-written 95%, the next step is different and needs no compiler:
make `codemap`'s disassembly re-assemblable (labels instead of addresses,
data kept as data) and check that GNU `as` gives back the same 1 MB, byte
for byte. That too stays private.

## Sources

- GCC's PowerPC register allocation order (the comment and `REG_ALLOC_ORDER`
  in `gcc/config/rs6000/rs6000.h`; read in GCC 4.6.4's copy, an order kept
  since the 2.x days):
  https://git.midipix.org/cbb/cbb-gcc-4.6.4/plain/gcc/config/rs6000/rs6000.h
- Cygnus GNUPro for PowerPC (1998): https://ecos.sourceware.org/docs-1.1/ref/gnupro-ref/powerpc/index.html
- SEGA's Saturn SDK and Cygnus' GCC: https://retroreversing.com/sega-saturn-sdk
- Model 3's CPU (PowerPC 603e): https://www.segaretro.org/MODEL3
