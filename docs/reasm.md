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

## All six moves the same (2026-11-18)

The 80 cycles were not the emulator's bookkeeping: the copy's DMA carried 16
words more. Its length, 0xAAAC words, is built with `lis`/`addi` and equals a
function's start, so it had been written as that function's address. Every
difference after it was the same mistake, a number taken for an address,
in places the runs' evidence couldn't settle:

- **Flag masks** built with `lis`/`addi` and tested with `and.` or set with
  `or` (0x18000, 0x1C000, 0x21004, 0x22000, 0x23000, 0x28000, 0x40800).
- **Angles, ids and offsets passed to routines:** angles to the rotation
  at 0xBB70 (0xC000, 0xE800, 0xE900, 0xFF00), sound ids to 0xE49C (0x1B58
  to 0x238E, beside plain ids 8929 and 8930), offsets added to a RAM base
  (0x5CFB0 and 0x1D6D4 onto the heap base at 0x100284: the copy's heap
  started 16 bytes on).
- **Data for the 3D board:** green, 0x0000FF00, in the colour records at
  0xB0370 (sent by DMA); words stored byte-swapped into the 3D buffer at
  0x694000 (0x55554, 0x1555C).
- **Script flags** 0x10000, 0x20000 ... 0x70000 (OR-ed into flag words by
  script action 0x19): 312 data words.
- **A table of sound ids** (0x9A00, 0x9A04 beside 0x9A07, 0x9A12).

Rules added to `reasm.py` from these (each checked by reading what it
changed in the listing):

- A value built by `lis` + `addi` that lands inside code never run (not in
  the code map) on no function's start is a number: 16 words around it decode
  as instructions, no two words in a row look like text, at least 8 differ,
  none was read as data by a run, and the word before it is no `blr`, `b` or
  `rfi`. (Tried on data words too and dropped: float tables and strings pass
  the instruction test.) A `bl` before an address doesn't end a function.
- A data word that is a multiple of 0x10000 on a function's start whose word
  before ends no function is a number (the code map's entries there came from
  these very words), also in packed byte tables.
- A data word on a function's start with a neighbour (4 or 8 bytes away)
  holding a value on no word within 0x40 of it is a number (a table of ids).

The rest are lines in the private `--numbers` file (50 or so). A
`lis`-built value whose next use is arithmetic (`and`, `or`, compare, `add`)
is the way to find candidates; computed jumps (an address plus an index
times 12, then `mtlr`) are the exception that stays an address.

How each was found: the picture alone differing while the CPU trace is the
same means the data sent to the 3D board differs (log each DMA with a
checksum of its words, then compare the source range at that frame); a
float register hash in the block trace finds where a computed value first
parts; the sound board's MIDI bytes, with the block that sent each, find a
wrong sound command; a word-level RAM comparison that leaves out the
copy's moved pointers (the original + 16) finds the rest. The local test
aids used for this are not in the repository.

Result: the moves at 0x45A4, 0x20018, 0x50124, 0x7E000, 0x8C048 and
0xB0260 all give the same pictures, 2D layers and sound as the original
over the 9,000-frame power-on fight. That proves the parts the fight runs;
the rest of the game (later stages, the ending) still needs runs that reach
it.

## The rest of the game (2026-11-18, in progress)

The bot's four replays (stage 1, and three later segments to the ending)
each start from a save state the original program saved, and nothing
recorded leads from power-on to them. A moved copy can only run them from
that state with its code addresses moved too: a local aid (not in the
repository) moves the state's pointers by rules checked against a moved
copy's own state at a frame both runs reach (labels and return addresses in
the moved span, not round numbers, pairs of halves, frame numbers or known
number tables; return addresses only on the stack and in the task area). It
can't be exact, so each difference is traced to its word and put down to
the aid or to the reassembly.

Found in the reassembly so far, all numbers equal to labels: sound numbers
passed to the sound routines in code only later stages run, and the stage
scripts' slot masks (the argument of action 0x28 with its flag set, and of
condition 0x0B; action 0x28 without it and 0x29 take a script address): a
mask equal to the data area's start switched on an extra script slot and
crashed stage 1 in a copy. The copies that move only the upper part
(0x8C048, 0xB0260) gave the same pictures, 2D and sound as the original
over all four segments; the full set is being run again with these fixed.

Then, with seg3's sound a little late in the copy that moves the most
(0x45A4: the same bytes to the sound board, each ~1,000 cycles later), the
source read for what that copy alone moves: two values in runs of records
were labels in one record and plain numbers in the records beside it
(numbers), and the opposite, lists that end with -2 and then where they go
on from (an entry a few words back, or the next word): 41 of them had a
label there, 24 were left numbers, as nothing else pointed at their
target. reasm.py now takes the word after a -2 as an address when it holds
a data address within 256 bytes. The source still assembles to the same
bytes, and seg3 is the same in every copy, sound too.

The aid, not the reassembly, accounted for the rest found so far: the first
word of a list of 16-bit pairs, and lists of 16-bit indexes ending in 0xFFFF,
where two indexes side by side read as an address in the moved span (seg4's
sound, a few hundred cycles off in three copies). Copies 0x20018, 0x50124
(being rerun), 0x7E000, 0x8C048 and 0xB0260 now give the same pictures, 2D
and sound as the original over all four segments; 0x45A4 over three, and
seg4's sound; its seg4 3D parts from frame 191315 (being traced).

## What the runs don't reach (2026-11-19)

The replays prove only the words they read. Coverage (a local interpreter
aid marking every instruction run, over power-on and the four segments):
61% of the code runs. For the rest, four checks that need no run:

- A static check, block by block, of how the code uses each program word
  (constants built with lis, words loaded and then used as an address, table
  bases and their entries): 142 pointer words found written as numbers, in
  tables 0x7F548, 0x8C048, 0x8CB10, 0x90730, 0xA0894, 0xA171C, 0x8F5F4,
  0x8F610, 0x8F638 and 0x913B8, and three masks (0xF0F00) and a byte run
  written as labels. Power-on alone found three more pointers (0xB16C4,
  0xB19A4, 0xB19D4).
- The stage scripts decoded whole (the table at 0xA1D2C, the dispatcher at
  0x5AA38; a record is type, command, length, sub-command, then arguments):
  180 scripts, 10,064 records. Each command's code reads its arguments the
  same way in every script, so where a run proved an argument an address
  the same argument in scripts no run reached is one too. Read from the
  handlers: about 700 addresses written as numbers (52 of them in scripts
  that start at odd addresses), 503 flag masks written as labels, and the
  lists behind action 0x28.
- Action 0x28's lists are spawn lists (read at 0x5AD54): 20-byte records
  read as halves and bytes, each followed by the next, -1 (end) or -2 and an
  address (the list goes on there). 919 lists, 1,981 records: 280 numbers in
  them (0x10F00, 0xC00 ..., equal to low function addresses) were labels.
- Object set-up lists (read by 0x44D38): pairs of a header (a type byte the
  reader ignores: 0x80 word, 0x88 number, 0x90 text; the size; the object's
  offset) and a value, a size of 0 ending them. 1,158 lists, 5,676 pairs:
  0x90 values and 0x80 values into the data area are addresses (checked:
  they land on lists, text tables and records), 0x80 values into the code
  area only functions. 830 addresses were numbers.

All of these change only words no run had read: the 30 runs above still
hold for the rebuilt copies (checked word by word).

### The developer menu

No run ever used the set-up lists: they belong to SEGA's developer menu
(0x44B68-0x48A94), which the game opens only like this (read from its code;
we found nothing about it published):

1. DIP switches 1 and 2 on (the I/O board's register 0x18; MAME's "DSW",
   all off by default; Supermodel always reads 0x7F, switch 1 on, so its
   menu can't open there). `Bus::Inputs::dips`, gamerun `--dips HEX`.
2. In attract, with service held: charge, jump, beat, charge, beat, right,
   right, charge, jump, right, right, charge, charge (the table at 0xA0630,
   checked at 0x492B8). Debug mode: "debug_mode" in the corner and a box of
   timings.
3. A second control panel (the I/O board's 0x0C, which a Spikeout cabinet
   doesn't use): its bit 2 held with start 2 turns the windows on; its bit 3
   held with bit 1 opens the "Window MENU" (player and enemy parameters,
   change stage, camera position, stdout, stderr, board and scenario info,
   CPU power, a cell picture test, object, door, gate and link parameters).
   Bit 3 held is a window manager (the directions move a window, beat and
   charge pick one, jump closes it); released, the buttons go to the window
   (up and down move between entries, beat opens one).

The window does it for tests (`SPIKEOUT_DEV_MENU=1`, never in network
play): F3 plays steps 2 and 3, each once the game has taken the one before
(the code is taken only in attract), F4 held is the second panel's bit 3.

Run through the menu (each entry from power-on, and walks inside the pages),
the copies give the same pictures and 2D as the original. Two copies' sound
differs while every byte sent to the sound board is the same: the 16 bytes
they insert are in code that runs (an interrupt handler's entry at 0x45A4,
a function's start at 0x20018), so the bytes arrive a few cycles apart.
21 of the menu's 22 list readers ran; the last (0x46554) builds an item type
(0xF02) that no page in the program contains. Four pages differ on screen,
each where it shows the program itself: the door and gate pages print their
data's program address ("rom adr", 16 higher in a copy that moves it), and
board info and CPU power print timings (the copies' inserted bytes cost
cycles).

The menu's "change stage" page sets the stage number (0x10402C) at once.
Its 16 names are BLANK, SUNSET, BASKET, BASKET2, DEPART, OPERA, SHIPYARD
and TEST7-TEST15, but the names don't say what is used: the game itself
(save states over the bot's whole run and its other routes) uses 7 (stage
1), 3, 6, 4, 5 and, at the ending, 1. Never seen in play: 0, 2 and 8-15.
Loaded in play (where stage 1's gym puts the player): BLANK and TEST8, 9,
12, 13 and 15 show only sky and sea there, BASKET and TEST14 draw white,
and TEST10 and TEST11 show DEPART's and OPERA's scenery by day.

The menu's pages also name the game's data: each item is a format text and
either a variable's address or a structure's (through a pointer) with the
field's offset and type. That gives SEGA's names for about 100 RAM
variables and 400 fields: the character record (motion, position, 26 body
points for stage collision, the controls, energy at +0x3F02, the bot's
health), the camera, the stage scripts' state, doors and gates. Read with
them, the damage routine (0x22608) takes the energy down, knocks the
character out at 0 (a voice, the KO count, a camera shake) and counts
defeated enemies (DF_ALL, DF_STG, DF_ARE); a player's damage is scaled by
the difficulty setting and the defeats so far. DIP switch 8 in debug mode
makes the players take none (tested: outside debug mode the game keeps only
switches 1 and 2).

### The test modes (DBG_TEST)

With debug mode on, pressing test without holding service gives mode 18,
DBG_TEST, instead of the test menu (0x12064). It switches the control,
collision, computer, object, radar and script tasks off and shows one page
of 15 at a time (a set-up and an every-frame function each, in a table at
0xB0DF0): holding start 2, down goes to the next page, up to the one
before. Their titles, as the pages print them: (a crowd of every
character), MOTION TEST, POLYGON TEST, TEXTURE TRANS TEST, STAGE TEST, CHAR
TEST, VIEWPORT PARAMETER TEST, LAY VECTOR TEST, POINT LIGHT TEST, COLI BALL
TEST, JU & MR MODEWORD TEST, GAMMA TEST, LOAD TEST, ADV INST and SCENE PLAY.

- MOTION TEST plays any of the 3,400 motions on player 1's character (up
  and down choose, left and right by 100, beat plays, charge resets). The
  count is at ROM 0xFFB00004; the names, in the banked ROM (bank 12, from
  0xFF605F29), show that many moves came from Virtua Fighter 3: names with
  that game's characters (Jacky, Sarah, Pai, Lau, Wolf, Kage, Lion, Akira,
  Jeffry), some ending in _VF3, beside dummy (DMY) and test motions.
- POLYGON TEST draws one of the 8,789 models (numbers 0-6679 and 7000-9109,
  listed at ROM 0xFFA00000; jump held goes down, the second panel's bit 3
  up). Their names (ROM 0xFFF78104: a count, then a name per model) are
  o_<character>_<part>; a model is drawn through the culling node whose
  address the game keeps at RAM 0x7204B0 + 4 x its number (the function at
  0xE2D4). Characters are about 50 parts each, many with lower-detail
  copies (_300, _500); the test characters' models are 300man, 500man and
  700man: dummies for polygon counts.
- CHAR TEST spawns any of the 127 character types (the directions choose,
  jump spawns, the second panel's bit 2 the colour set). The game numbers
  them through the table at ROM 0xFFF2AEA8 (a data block each, its name at
  +20; a spawn list's record gives the type in its third byte). Not spawned
  by any script and not seen in the bot's runs (every fifth save state over
  its routes): the polygon-test mannequins TEST500, TEST700 and STAND, and
  BBM, BBF, UNB, BLB3, BLB4, WARU04 and the boss DCMB (it has its own
  motions). The kid, the dog and the puppy appear only in the ending.
- STAGE TEST loads a stage (and an area of 29). BASKET and TEST14 never
  finish loading (the screen stays white, the stage in use doesn't change);
  TEST15 (named TESTF) reports "PMG ERROR: TOO MANY SIBTBL_0"; TEST9 and
  TEST10 repeat real stages, TEST8 and 11-13 are empty.
- LOAD TEST makes four players and switches the game's tasks back on in
  the current stage: free play. SCENE PLAY sets up ready-made fights (a
  list at ROM 0xFFF89684). COLI BALL TEST is an editor for the collision
  balls (an EDIT MODE and a CAMERA MODE).

`romcheck --dump-banked FILE` writes the banked ROM (all its banks, each as
the CPU sees it at 0xFF000000), beside `--dump-crom`; like everything taken
from the ROM, such dumps stay outside the repository.

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
