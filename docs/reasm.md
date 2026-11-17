# Re-assemblable disassembly (the hand-written 95%)

About 95% of Spikeout's main PowerPC program was written by hand in assembly
(`docs/compiler-id.md`), so there is no compiler to match. The equivalent of a
matching decompile for that part is a **re-assemblable disassembly**: assembly
source, with names, that an ordinary PowerPC assembler (GNU `as`) turns back
into the program, byte for byte.

`tools/reasm/` makes one from any PowerPC memory image and proves it. Like
every output made from the ROMs, the result is private: never commit or share
it (it stays in the private folder, outside the repository).

## The tools

| file | what it does |
|---|---|
| `tools/reasm/reasm.py` | writes the assembly source from the image and `codemap --disasm`'s map of the code |
| `tools/reasm/check.sh` | runs it, assembles the result with GNU `as`, links it at the image's address and compares every byte with the image; any word that doesn't come back the same is kept as a raw `.long` and the run repeated |

The assembler and linker are the ones `tools/old-gcc/build.sh` builds in WSL
(`~/oldgcc/tools/bin/powerpc-eabi-*`); Python 3 runs in WSL.

    codemap ROMDIR --scan-pointers --disasm codemap.txt      (Windows, the project's build)
    REASM_OPTS='--sda-base 0x108000' bash tools/reasm/check.sh ram.bin codemap.txt game.s 0

`ram.bin` is the 1 MB the boot code copies from ROM to RAM (see
`docs/decompile-handoff.md`, "On a fresh machine"). `--sda-base` is r13's
value, the program's small-data base, which the boot code sets.

## What the source contains

- **Code**, as instructions (GNU objdump's spelling), with every branch target
  a label: `fn_XXXXXXXX` for the functions `codemap` found, `L_XXXXXXXX` for
  other targets. Branches keep their static prediction bit as `+`/`-` on the
  mnemonic.
- **Addresses as names**: an address built in two halves (`lis` then `addi`,
  `ori` or a load/store) is written `NAME@ha` / `NAME@l` (`@h` with `ori`);
  loads and stores relative to r13 are written `NAME-SDA_BASE(r13)`. Names are
  labels where the address is a word of the image, `.set` definitions
  otherwise (the rest of the work RAM, hardware).
- **Data** as `.long` words; words that point at a function, a string or a
  named address are written as that name, and text is written as `.asciz`
  strings.

## Result (2026-11-15)

The whole 1 MB assembles back identical on the first pass, with no word left
raw: 119,482 code words as instructions, 15,500 labels, about 6,100 address
pairs and all 4,170 small-data accesses named, about 3,800 pointers in the
data named, 2,500 strings. What is still a plain number: constants that only
look like addresses (small values, loop limits), and the few address pairs
whose two halves are far apart.

Being identical at the same address proves the source is complete and
exact. *Movable* (code or data inserted, everything after it shifting) needs
every address in the program to be a name, and every number that only looks
like an address left a number.

## Movable: telling addresses from numbers by watching the game

Code is easy (an address is built by `lis` and a second instruction). Data
isn't: a word of 0x00070300 may point at the function there, or be four
bytes of a stage script. Guessing gets both wrong. So the game is watched:

- **The recorder** (`gamerun-interp`, `SPIKEOUT_INTERP_ACCESS=FILE`) writes a
  byte for every word of the program's MB: how the game read it while it
  still held the program's value (bytes, halfwords, words, floats), and
  whether a word it read was then **used as an address** (a load or store
  through it, up to 64 KB on, or moved to LR/CTR for a jump). A value is
  followed through registers, `mr`/`addi`, and through memory (stored whole
  with `stw`/`stmw`, loaded back later: pointers kept in structures and on
  the stack). The counting starts at the first block run in RAM (the boot
  code's copy doesn't count); `SPIKEOUT_INTERP_ACCESS_IMAGE=ram.bin` makes a
  run from a save state count words that no longer hold the program's value
  as already written. Test aid only: off, it costs one flag test a memory
  access.
- **`reasm.py --access FILE`** (any number, combined): a word seen used as an
  address is always named; a word seen read as bytes, halfwords or floats
  and never used as an address never is (some script data is also copied a
  word at a time, so a word read alone proves nothing). Words the game never
  read keep the old rule (a function's start, a string, a labelled word).
- **The test**: `reasm.py --insert ADDR:N --drop ADDR:N` builds a copy with N
  words inserted early and N zero words dropped later (same size; the boot
  code's checksum of the copy is compared with nothing).
  `SPIKEOUT_TEST_PROGRAM=FILE@0xFFE00000` (any build) puts it into the
  program ROM in place of the original; the picture hashes
  (`--gpu-hash-frames`, `GAMERUN_2D_HASH=1`) and `--wav` are compared with
  the original's.

Runs recorded (2026-11-16): attract (6,000 frames), a coin-up and fight from
power-on (9,000), and the bot's run to the end replayed from its states
(`--load-state` + `--inputs`, 4 segments to frame 198,905, every recorded
hash matching). 1,505 words were seen used as addresses.

Result: 16 bytes inserted at 0x6E034 (everything from there to 0x80FF0
moved, 20,452 words changed), attract 3,000 frames and the power-on fight
9,000 frames: every 3D picture, every 2D layer and the sound identical to
the original. On the way: words in stage scripts taken for function
pointers (the script bytes 00 07 03 00) and `lis r0` + `addic` pairs
(r0 means r0 there, not zero) were fixed.

What a run can't prove: data the game never read in any recorded run (about
7,700 words that look like addresses: the test menu, scripts and stages not
reached, unused tables) still rests on the old rule.

## Moving other parts (2026-11-16)

Six more copies, each with 16 bytes inserted at one point (0x4000, 0x20000,
0x50000, 0x7E000, 0x8C048, 0xB0260) and everything after it moved, compared
over the power-on fight. Each failure was traced to the first block where the
two runs part (below) and its cause fixed. What was found, in order:

- **The emulator's own addresses.** `GameSpec`'s code entries and hooks (and
  the credits test's return address) are the original program's; with
  `SPIKEOUT_TEST_PROGRAM_MOVE=FROM:BYTES:TO` they follow the move
  (`src/runtime/test_move.hpp`), as names would in a build from source.
- **A table's address added to an index** (`addis rX,rIndex,hi` then `addi`
  or a load): the mode dispatcher's function table. Named when it lands in
  data inside the image.
- **The test itself.** The game's start-up check walks the program to its end
  address (a word in the program), so a copy one way longer runs that loop
  longer, the time base differs and the game (which uses the time) drifts.
  The 16 bytes are now given back inside the program, just before its end,
  at a never-read run of zeros that ends where the next labelled item
  starts (0xD6074): the program's size and the check's time stay the same.
- **Numbers built like addresses.** Delays passed to the wait routine
  (0xD4347 and 0x82350 ticks) are built with `lis` (+ `ori` or `addi`) and
  lie inside the image's range. `lis` + `ori` is how numbers are built (45
  of them, all masks and delays): never named. For `lis` + `addi`, the
  recorder marks each `lis` it runs (128), and whether the value it built
  was used as an address (32, as for data) or as a number (8: added with
  carry, as to the time base, multiplied, divided, shifted). A pair with
  number use, no address use and not landing on code stays a number.
- **Addresses the recorder lost.** Fixed: pointers given to a device (the
  DMA queue's source, stored outside the 8 MB of RAM), `add`/`subf` of a
  pointer and an index (the operand whose mark is still its value's),
  registers saved on the stack and restored (the mark kept within 64 KB:
  a `lis` mark holds only the high half), comparisons and subtractions of
  two nearby values (a pointer against a table's end), and a fallback by
  exact value (an interrupt handler borrowing a register).
- **Pointers not on a word.** Packed byte tables hold pointers one byte past
  a word (0xA1581: read with `lwzx` at odd addresses). The recorder lists
  such reads used as addresses (`FILE.odd`); the disassembler writes them as
  `.long NAME` at their odd place with `.byte` around them, and the rest of
  the table from its neighbours.
- **Tables partly used.** A word next to a known pointer, pointing at data
  within 64 KB of where its neighbour points, is a pointer too (a list of
  records, the game walking only its first entries in the runs).

Test aids for this, all off unless set (one flag test where they hook in):
`SPIKEOUT_INTERP_TRACE=FILE:COUNT` (+ `_SKIP=N`): each block run, as the
original program's address, with the time base, LR and r3, to compare two
runs block by block; `SPIKEOUT_INTERP_WATCH=ADDR`: each write to a word,
with the block that made it; `SPIKEOUT_INTERP_MODES=FILE@ADDR`: for each
block, the values a byte (the game's mode) had while it ran.

- **The test's own nops run.** The inserted words are nops placed before the
  label at the insertion point, so they run only if code falls into that
  point. 0x20000 and 0x50000 were in the middle of running code: the copy
  ran 4 more instructions each time, and the time drifted. The points are
  now chosen where nothing falls in (after `blr`, `b`, `bctr` or `rfi`, or
  in data): 0x20018 and 0x50124.
- **The emulator's blocks.** The interpreter (as the recompiler) counts a
  block's cycles at its start, so the time a path takes depends on where
  blocks begin. The analysis starts functions at words in data that look
  like pointers; one of them (0x00050900, an argument in a script) isn't,
  but still splits the code there in the original. In a moved copy that
  word stays (a number), the code moves, the split goes and the time
  differs. `SPIKEOUT_TEST_PROGRAM_POINTERS=FILE`: a run of the original
  writes the functions its pointer scan found, a moved copy's run takes
  them (moved) in place of its own scan, so both have the same blocks.

- **A table's address with a low half of 0.** `addis rX,rIndex,9` then
  `addi rX,rX,0`: the table at 0x90000 (12-byte entries, picked by a byte of
  a character; an enemy's movement read the wrong entry at frame 962).
  Nobody adds 0 but an assembler writing a label's `@l`, so that form is
  named too (two places; every other `addis` on an index is angle
  arithmetic, 0x10000 or 0x8000).
- **Masks built like addresses.** `lis` + `addi` making 0x25000, then
  `and.` with a character's flags: the recorder now counts `and`, `andc`,
  `or` and `xor` as number use (8) too. It was also marked as a jump: the
  register had kept the `lis`'s mark through `mflr r2` (a return address
  within 64 KB of 0x25000), saved and restored by save_lr/restore_lr, then
  `mtlr`. Moves from special registers (`mflr`, `mfcr`, the time base)
  now clear the register's mark; so do `li` (an `addi` from zero copied
  r0's mark), shifts, rotates (unless the value stays within 64 KB: a
  pointer aligned), `neg`, `cntlzw`, sign extensions and `andi.`/`xori`:
  a mark left from an old value within 64 KB of the new one had made a
  link table's index (0x6DC4C) mark the mask.
- **Loop counts built like addresses.** `lis r4,1` + `addi r4,r4,-0x8000`
  (0x8000 pairs of words cleared at start-up, counted down with
  `addic. r4,r4,-1`): 0x8000 lands on code, so it was named, and a copy
  moved before it cleared 16 pairs more. `addic. rX,rX,-1` now counts as
  number use, and a pair with number use and no address use stays a number
  even where it lands on code. The same count is also put in CTR
  (`mtctr`, then `bdnz`: the backup RAM's test at 0xF42C): a move to CTR
  is no longer a jump target by itself; what was moved there counts as a
  jump target (64) when `bctr`/`bctrl` jumps through it, as a number (8)
  when `bdnz` counts it down.
- **Rules made narrower.** Only a store to the Real3D's DMA registers
  (0xC2000000-0xC20000FF) counts as giving an address to a device (uploads
  to the 3D board's memories copy data words, numbers among them); and a
  comparison counts as a pointer against an end only when both values are
  0x10000 or more (lengths are compared too).
  `SPIKEOUT_INTERP_ACCESS_WHY=ADDR` prints each new mark on a word with the
  block that made it. `SPIKEOUT_INTERP_CALLS=FILE` writes every call or jump
  through a register (`bctrl`, `blrl`, `bctr`: function pointers, the tasks)
  as site, target and count: the calls a listing can't follow, for the map.
- **A section's start.** The table at 0x7E044 is the program's memory map
  (16 bytes an entry: start, end, again the start, flags 0x210/0x213): code
  0x4000-0x7DF4C, data 0x7E000-0xD7314, then the work RAM; at start-up
  0x7937C sends each section by DMA (so its words are addresses). Inserting at
  0x4000 moves the code section's start off its boundary, which no build
  from source would do; the test inserts at 0x45A4 instead (after an `rfi`;
  0x4100 would be inside the PCI stubs, reached by a computed jump).
- **Sizes equal to a function's start.** The task table's work sizes
  (0x6000, 0x4000) equal code addresses; inserting below 0x6000 moved the
  size, so every later task's work area sat 16 bytes on. Two causes: an
  indexed access (`lfsx f1,r5,r4`, `stwbrx r3,r18,r2`) counted both
  registers as addresses, and its offset register (a sine table's angle
  step, a copy's running offset) held 0x6000 or 0x4000, traced by value to
  the size word. Now, with one register under 0x10000 and the other not,
  only the other counts. (A rule keeping every data word with number use as
  a number was tried and dropped: the task table's own pointers are copied
  and compared too, and they became numbers.)
- **Fixes by hand.** `--numbers FILE` lists words and `lis` that built a
  number whatever the run's evidence says: a move test's finding is fixed
  in minutes, without recording the runs again (about 75 minutes, the
  longest run's length, with the runs side by side). The recorder's rule
  behind it is fixed later, and the line can then go. One such rule: an
  `addis` onto a register (a table's high half added to an index, as
  `addis r2,r2,0x14` for the link buffer at 0x6DC54) now starts its own
  mark as a `lis` does; it kept the register's old one, here the mask
  0x25000's, which then counted as an address.
- **Round numbers.** 0x8000 is built with `lis`/`addi` in over 20 places
  (half a turn in the game's angles, the backup RAM's clear and test
  counts, sizes), and 0x4000, 0x6000 the same: each lands on a function's
  start. A value under 0x10000 that is a multiple of 0x1000 and lands on
  code stays a number (built or a data word) unless a run saw it called or
  jumped to. Found by the move at 0x45A4: the backup RAM clear at 0xF3F0
  cleared 4 words more, its test then counted once more.

Result now (2026-11-17, the six recordings again with these rules): moves at
0x50124, 0x7E000, 0x8C048 and 0xB0260 give the same pictures, 2D and sound
over the 9,000-frame fight; at 0x20018 the same pictures, the sound not; at
0x45A4 the pictures part at frame 914. For 0x45A4, with the cycle trace
(`SPIKEOUT_INTERP_TRACE_CYCLES`): the CPU, MSR and cycle count are the same
up to frame 90, every interrupt request comes in the same order and every
DMA is the same, but there the DMA interrupt (bit 0x100) comes 80 cycles
later in the copy: open, next to look at (when the DMA's end is noticed).

## Names

`reasm.py --names MAP` (any number) renames the generated labels and writes
each name's note above it (names change no bytes: the check still applies).
The maps are private like the source. The program keeps SEGA's own debug
name lists (pointer tables to short names): the 74 game modes in the
dispatcher's order (WARNING, ADV for attract, SEL, GAME, ENDING, NAME,
RANKING, OVER, then the test and unused modes; they match what a run shows,
the mode byte each frame beside screenshots; an older list of 128 stored
beside the table doesn't), the 20 per-frame tasks (fa_player, fa_enemy,
fa_object, fa_coli, fa_cam, fa_scenario ...: a table of work size, set-up
function, RAM slot and name; the task runner times each one), the bits of
the characters', objects' and doors' flag words, the reactions, the stages,
and the link's sync-check errors. The per-frame function of each task comes
from `SPIKEOUT_INTERP_CALLS` (the set-up puts it in the task's record).
Then: the services
found while tracing (the wait, DMA, copy and fill routines, the vblank
wait), what docs/netplay.md knows (the camera, the stage scripts and their
tables, the link) and the hooks in `src/games/spikeofe.cpp`. Then where each
function runs: `SPIKEOUT_INTERP_MODES` over the same runs as the recorder
(attract, the fight, the bot to the ending) gives, for each of the ~2,900
functions, the modes it ran in: about 1,950 seen running, 660 only in the
game, 260 only at start-up, 75 only in attract.
