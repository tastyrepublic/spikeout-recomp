# Recompiler and runtime

How analyzed code becomes C++ and what it runs against. The control-flow
analysis that comes first is in [control-flow.md](control-flow.md).

## Pipeline

```
ROM set ──codemap / analysis──> functions, blocks, tables
        ──lifter (ppcrecomp)───> generated C++ (build/generated/, never committed)
        ──compiler─────────────> native code, linked with the runtime
```

`ppcrecomp --rom DIR --segment crom --name boot_rom -o boot_rom.cpp`
recompiles the boot ROM; `--segment ram --scan-pointers` the game's RAM
program. The generated file defines `generated::<name>_functions()`, a
table of every function it contains.

## Generated code

One C++ function per guest function, taking a `recomp::Context&`:

| Guest | C++ |
|---|---|
| basic block | a label, starting with `c.tick(pc)` (block budget, current PC) |
| branch inside the function | `goto` |
| `bl` | `s.lr = next; f_target(c);`, then a check that the callee's `blr` returned to `next` |
| `blr` | `c.returned_to = s.lr; return;` |
| jump to another function | `f_target(c); return;` (a tail call) |
| jump table | `switch` over the table's targets |
| `bctrl`, `blrl`, unresolved `bctr`/`blr`, `rfi` | `c.call()` / `c.jump()`: look the address up in the function table |
| the instruction after a call | a return site: a label the function can be entered at (below) |

**Return sites.** Spikeout's tasks run on stacks of their own and yield;
when one is resumed and returns, its `blr` goes to the instruction after a
call it made before it yielded, where no C++ call is waiting. So every
instruction after a call is in the function table too, as `{site, f,
site}`: `Context::call()` sets `c.resume_at`, and the function's first
lines `goto` that site's label. All state is in `c.cpu`, so starting a
function in the middle is just a jump (about 7,000 sites in the game).

**Returns past several calls.** A task yields by switching back to its
scheduler's stack and returning to the scheduler's call: past every
function it was called through. Each call pushes its return address on
`Context`'s return stack; when a callee returns somewhere else and an
outer call waits for that address, the frames in between return at once
(`follow_return` says so) instead of running it as new code, which nested
the scheduler one level deeper each time until the C++ stack ran out.

Register state lives in `ppc::CpuState` (`s.gpr[n]`, `s.cr`, `s.xer`, ...);
flags are computed by the small inline helpers in `recomp/guest.hpp`.
Every instruction's disassembly is kept as a comment.

What isn't handled yet throws when reached instead of guessing
(`eciwx`/`ecowx`, which Model 3 games don't use). Running into code that
wasn't recompiled throws
`recomp::MissingCode`; a callee returning somewhere other than the
instruction after its call throws (`bad_return`), so a wrong assumption
stops the run where it happens.

Every instruction the game uses is lifted: integer, branch, load/store,
CR, SPR, time base and floating point. The time base advances by
`Context::tb_step` on each TBL read: the recompiled code doesn't count
cycles, and this keeps delay loops finite and repeatable.

### Exceptions

`Context::exception(vector, srr0, cause, resume)` enters a handler as the
603e does: SRR0 and SRR1 (the old MSR's low half plus cause bits), a new
MSR that keeps only ME, IP and ILE, and the vector at `0` or `0xFFF00000`
by MSR[IP]. It runs the handler as a C++ call and returns when the
handler's `rfi` goes back to `resume`, so the recompiled code that raised
the exception simply carries on. That `rfi` unwinds with a C++ exception,
so it may sit any number of calls deep inside the handler; an `rfi` to any
other address is an ordinary jump (that is how the boot code enters the
game). A handler that returns without `rfi` is an error.

- `sc`: vector `0xC00`, resumes after the `sc`.
- Traps (`tw`/`twi`): vector `0x700` with SRR1's trap bit; SRR0 points at
  the trap and the code resumes after it (handlers step SRR0 past it).
- Interrupts: the same entry at the start of a block, resuming at that
  block (see below).

### Clock and interrupts

Time is counted in 603e cycles at 166 MHz. The lifter works out each
basic block's cost when it generates it (`ppc::block_cycles`: two
dispatches a cycle, in order, to the integer, floating-point, load/store,
system and branch units, each instruction waiting for its source registers
and its unit; latencies after the 603e user's manual; every access a cache
hit) and passes it to `c.tick(pc, cycles)`. Counted, not measured: every
run with the same inputs is exactly the same, which save states and
rollback netplay need. The game averages about 4.3 cycles a block. The
time base and the decrementer run at 16.5 MHz (bus clock / 4) on that
clock; DEC going from 0 to -1 raises the decrementer exception (`0x900`).

`Context::tick()` is one compare per block. It falls into the slow path
only when something is due: a `Scheduler` event, the decrementer, the
block or cycle limit, or an interrupt that may now be taken, because the IRQ line,
MSR (`mtmsr`, `rfi`) or the decrementer changed. External interrupts
(`0x500`) are taken at a block start when the IRQ controller's line is up
and MSR[EE] is set.

Found 2026-11-03 (uncommitted when written): `cycle_of_tb` compared the
decrementer's due time with the time base's offset unsigned. The game sets
the time base lower than the time already run, the offset went below zero
(wrapped), every due time looked past, and `tick()` took the slow path at
every block from then on: about a fifth of a board's time. Now taken as a
difference; and a TBL read (which moves the time base on by `tb_step`) or a
TBL/TBU write looks again at the next block (the decrementer then due at
another cycle). The same interrupts at the same blocks: every frame's RAM,
whole states every 100 frames, the GPU's pictures and the sound byte for
byte the same as before over 1,500 frames of two linked boards
(mash-5990.state), and every one-board test; a board about 40% faster with
sound and 2D (24.0 s to 14.2 s for those frames, with the idle skip).

The idle skip (`Context::skip_loop`, `m3::games::set_idle_skip`; gamerun
and spikeout `--no-idle-skip`, `SPIKEOUT_NO_IDLE_SKIP=1` turn it off): the
game's waits (the main loop's at 0x36EF8, 0x36F24 and 0x36F44, the
drawing's vblank wait at 0x36E08) are each one block that reads and
compares until an interrupt changes what it waits for; at its hook, while
it still waits, it is gone round at once up to where `tick()` next looks,
its cycles and blocks counted as if run. The same state (the same tests).
Found with it: with one board a PC (`tools/onepc-net-tests.ps1` vanish and
leave), the staying board's network board differed now and then (one run
in three) from the joiner's plain run of the same board, in one field only
(`CommBoard::send_done_`, about 2,000 of its cycles; never RAM, the CPU or
the frame timing). A shadow's own thread hands it back where its count ends,
between two meetings, while the staying board runs on beside it: when the
staying board came to the next meeting first, the module made that
cabinet's link from a meeting later. Spinning, the staying board was always
the slower; skipping the waits, now and then not. Now the staying board
waits at each meeting, while a hand-back is to come, until the shadow has
come to that meeting too or been handed back
(`one_board::Game::wait_hand_backs`, `Turns::waiting`): the hand-back always
first, as the boards' speeds have it or not (ten runs of ten the same, with
the skip on everywhere).

`recomp::FrameTiming` is the board's `Scheduler`: Supermodel's frame at
57.524 Hz, 424 lines, with 40 lines of VBlank ending in IRQ `0x0D` and
IRQ `0x02` at display line 383. Not modelled yet: running VBlank until
IRQ `0x02` is acknowledged, the MIDI IRQ (`0x40`), the Real3D ping-pong
bit.

Spikeout's game logic runs inside its interrupt handlers: `main` sets up
and idles in `b .`, and the external interrupt handler (`0x41DC`) saves
everything, calls a sub-handler for each pending IRQ (each acknowledges
through Tilegen register `0x10` and polls `0xFE100018` until the bit
clears) and returns with `rfi` to the idle loop.

Spikeout's `sc` handler (`0x2428`) is a small dispatcher, not a task
switcher: it calls a service from a table the game builds at
`*(0x710008)` (interrupt enable/disable, user mode, reading the
decrementer) and returns to the caller.

### Floating point

FPRs hold raw double bits; arithmetic runs on the host's doubles and
single-precision instructions round the result to single. Details that
matter:

- `lfs`/`stfs` are format conversions done bit by bit, as the hardware
  does: `stfs` truncates instead of rounding, and NaNs (signaling ones too)
  pass through unchanged, which a C++ cast would not do.
- The multiply-adds use `std::fma`. Generated code is built with
  `-ffp-contract=off` (the `spikeout_fp` CMake target), so the compiler
  never fuses a separate multiply and add: results are the same on every
  machine, which save states and rollback netplay depend on.
- FPSCR: the rounding mode is honored (`fctiw`, and the host's rounding
  mode follows writes to FPSCR), compares set FPCC, arithmetic sets FPRF
  (the result's class), `mcrfs` clears the exception bits it reads.
  Spikeout tests FPRF: it checks a vector's length was zero with `mcrfs`
  before normalizing it, and without FPRF its characters' matrices came
  out NaN (the fighters didn't show). Not modelled: exception bits,
  double rounding in single-precision results (computed in double, then
  rounded), and `fres`/`frsqrte` return exact values rather than the
  603e's estimates. Built with `-DSPIKEOUT_FP_TRAP=ON`, the game reports
  the first instruction whose result is a NaN (`gamerun` prints it).

## Bus

`m3::Bus` (`src/runtime/bus.*`): 8 MB of work RAM, the fixed and banked
program ROMs, the MPC106 PCI bridge's configuration ports (with the
Real3D's PCI ID at device 13), the system registers (CROM bank select,
IRQ enable/pending/acknowledge, LEDs, JTAG), the interrupt controller
(`m3::Irq`), the Tilegen registers and RAM (stored, not drawn yet), the
128 KB backup RAM, the network board (`m3::NetBoardDevice`), the JTAG
scan chain of the Real3D chips (`m3::Jtag`), and the Real3D's DMA device
and status registers (with the per-frame ping-pong bit). Everything else is
open bus (reads all ones, writes dropped) and counted per region, so a run
reports which devices the game used that aren't emulated yet.

Speed (2026-11-03, uncommitted when written; each proved the same output:
every frame's RAM and whole states, the sound, every frame's 2D layers
(gamerun `GAMERUN_2D_HASH=1`), the one-board tests): `find_region` looks a
64 KB block up in a table made once (the one region covering it whole, none,
or else searched as before: a unit test checks it against the search at
every 16th address), not searching the 27 regions for every access outside
RAM (the program ROMs last); a 32-bit program ROM read takes its four bytes
straight from the ROM; the tile generator's colours from three 32-entry
tables made once a layer pair (the offset added and clamped already), the
palette read straight. Two linked boards over 1,500 frames of a fight,
gamerun with sound and 2D: 24.0 s before the decrementer fix (above), 14.2
s after it with the idle skip, 10.4 s with these (the 2D 44% less, the game
logic 11%). Tried and left: a 4 KB page journal for side runs (slower than
the byte journal), the recent-blocks list's store in `tick()` (no gain).

The JTAG chain and Real3D DMA/status registers are adapted from Supermodel
(`JTAG.cpp`, `Real3D.cpp`, and the matching cases of `Model3.cpp`),
keeping its byte-order behavior exactly.

The network board comes in two forms behind `m3::NetBoardDevice`, both
sending frames round the ring over a `NetLink` (a `LoopbackLink` for a lone
cabinet: a ring of one):

- `m3::CommBoard` (what `gamerun` uses): the board's own 68000 (the Moira
  core, `third_party/moira`, MIT) runs the firmware the game uploads,
  against the board's memory, control and I/O registers and link chips as
  MAME and Supermodel describe them. It runs at its own clock and catches
  up to the main CPU before every access and at the end of each frame. The
  parts nobody knows for sure are options (`m3::CommBoardOptions`), see
  [fidelity.md](fidelity.md).
- `m3::SimNetBoard`: Supermodel's high-level simulation, a once-a-frame
  state machine that fakes the firmware's results (`--netboard sim`).

Tilegen registers are 32-bit only and byte-swapped on the bus (the tile
generator is little-endian), as in Supermodel; register `0x10`
acknowledges IRQs, and that is the one Spikeout uses.

The MPC106 (`m3::Mpc106`) follows Supermodel's `MPC10x.cpp`: registers
little-endian as on PCI, reset values from `CMPC10x::Reset()`, the bridge
itself at bus 0 / device 0 and nothing else on the bus yet.

## The interpreter (no JIT: iOS)

`gamerun-interp` (`src/interp`, CMake `SPIKEOUT_INTERP`, on by default)
runs the game from the player's ROM with no machine code made at all, for
systems that allow no JIT (iOS in the App Store) and downloads that hold
nothing made from the ROM. At start it runs the same analysis as
`ppcrecomp` and the JIT (about 1 s), then decodes each function once into
a list of 16-byte steps: a block's tick with its cycles
(`ppc::block_cycles`) and hook, each instruction with its registers,
constants, masks and SPR fields worked out (`li`/`lis` apart), and the
block's way out with its targets resolved (a block of the same function, a
function run in the same loop for a tail call, or `Context::jump`), jump
tables sorted for a binary search. It runs them with the lifter's semantics
case for case: the same calls (`push_return`, the callee run, `pop_return`,
`follow_return`), returns, tail calls, conditional calls, `bl $+4`, return
sites and block resumes, exceptions and `rfi`. Keep it in step with
`lifter.cpp` and `ppc_ir.cpp`.

Its tables have the same rows as the lifter's (functions, return sites,
block starts, each with the first function that has it), so
`tools::build_id` is the same; every row runs one entry point, which finds
its function by the row `Context::call` and `resume_block` note
(`Context::entered`). 2,900 functions, 22,369 blocks, 131,696
instructions: 159,402 steps, 4.3 MB.

Proved the same as the C++ build (2026-11-06, Windows): attract to frame
600 (the same 427,473,268 blocks, registers and interrupts); the bot's
seg2 replay (`--load-state build/bot/fullrec2/owner-50707.state --inputs
build/linkstudy/seg2/inputs.bin`, 3,932 frames of stage 2: its 65 RAM
hashes matched, the save states at 54600 identical); the seg3 replay with
sound (`fullrec3/owner-62721.state`, `seg3/inputs.bin`, 9,368 frames: its
156 hashes matched, the WAV and the save states at 72000 identical).

Speed, the same runs on the i7-8700K (the 2D drawn; a network test ran
beside them): seg2 without sound 6.8 ms a frame against the C++ build's
4.2 ms (26.8 s and 16.4 s); seg3 with sound 9.6 ms against 7.7 (90.4 s and
71.9 s): 2-2.6 ms a frame more, the game's own code about twice as long.
Most of a frame is the bus, the devices and the sound, the same code in
both. A frame lasts 17.4 ms.

Not yet: the window with it; network play against the C++ build; a phone
or a tablet (an iOS build); speed work (a computed-goto dispatch, steps
for the commonest pairs); the raw word in an unimplemented step's message.

## Tests

- `lifter_tests`: a test program written with a small assembler
  (`tests/lifter/program.cpp`) is lifted at build time and run: arithmetic
  with carries and overflow, rotates and shifts, every load/store form,
  `lmw`/`stmw`, `dcbz`, loops, calls, tail calls, conditional calls, jump
  tables, calls through pointers, CR logic, the time base,
  `lwarx`/`stwcx.`, and the ways a run stops. Needs no ROM.
- `boot_rom`: with `-DSPIKEOUT_ROM_DIR=path/to/spikeofe`, the build
  recompiles the boot ROM from your ROMs and `boottest` runs it from the
  reset vector until it jumps into RAM at `0x100`, then checks that RAM
  holds an exact copy of ROM `0xFFE00000`-`0xFFEFFFFF`, the PVR is at
  `0x80` and the rest of RAM was cleared (it starts filled with garbage).

- `gamerun` (also with `SPIKEOUT_ROM_DIR`): runs the recompiled boot ROM
  into the recompiled game and reports where and why it stopped, the last
  blocks it ran and the devices it used that aren't emulated. `--text`
  prints the text the game has put on the tile layers (which screen it is
  on). Not a pass/fail test: it shows what to work on next.

On `spikeofe`: the boot ROM is 29 functions and 396 instructions. It runs
about 2.9 million blocks (most of them clearing 8 MB of RAM and copying
1 MB with a checksum) in well under a second, configures the MPC106 from
its register tables, and touches no other device.

The game's RAM program lifts to 2,175 functions and about 124,000
instructions with nothing left unimplemented, and is compiled as the
`game_ram` library (split over 8 files: about 100 s on 12 threads, 166 s
as one file). With frame timing and interrupts, `gamerun` runs it frame
by frame at about 290 million blocks per second (some 25 times real time).
On the way it went through, in order, each fixed by emulating what the
game checks:

1. "ABORT: BACKUP RAM ERROR!!": backup RAM.
2. "COMMUNICATION BOARD : NO DEVICE": the network board's comm RAM test.
3. "INVALID CONFIG DETECTED": reading the Real3D chips' ID codes over JTAG.
4. Waiting for the Real3D DMA device to be idle.
5. A blr that returns into another function's epilogue (printf), which the
   runtime now follows.
6. An assertion in `dma_memcpy`: the DMA init (`0x4F34`) checks the Real3D's
   PCI ID (`0x178611DB`, device 13) before marking DMA ready.
7. Waiting for the Real3D's ping-pong bit to flip.

It now shows "TWO PROCESSORS DETECTED / COMMUNICATION BOARD : DOWNLOADED",
runs its main loop, and sends the Real3D real frames (display lists,
polygon RAM, textures), which nothing draws yet.

Then, in order:

8. NO CARRIER! CHECK NETWORK CABLE: without its settings EEPROM (a 93C46
   on the I/O board) the game set the cabinet up as a link slave, and a
   lone slave's firmware (running on the network board's own 68000) waits
   for a master that never comes, as a real one would. With the EEPROM
   emulated the game writes its defaults ("M3SEGA" signature) on the blank
   chip and runs as a master on its own.
9. ILLEGAL ROM: the security board's 315-5881 decrypter, now emulated
   (MAME's cipher, Spikeout's key `0x29236FC8`), so the check passes.
10. Code reached only through task resume points: tasks yield by storing
    the address to continue at, so the analysis now takes such constants
    as entries ([control-flow.md](control-flow.md#pointer-scan---scan-pointers)).

11. A red screen, the "TEAM SPIKEOUT PRESENTS" logo stuck halfway through
    its fade: the game queues sound commands and sends them in the MIDI
    interrupt (IRQ `0x40`), which nothing raised, so the queue filled up
    and the game waited. The frame timing now raises it in VBlank as
    Supermodel does.
12. Code the analysis missed: a state machine's dispatch explored first
    with a constant index, false entries explored before the real
    function, a table at an odd address, a callback held in a register,
    shared tails ([control-flow.md](control-flow.md)).
13. A task returning into a function it had called from before it
    yielded: return sites (above).

With the RTC emulated too, the game now runs its attract mode with
nothing missing: 6 billion blocks, 16,633 frames (almost 5 minutes of game
time). The 2D layers (`m3::TilegenRenderer`) show the boot text, the
logo, the title, "INSERT COIN(S)" and the "BASIC OPERATION" screens over
their backgrounds. Drawing costs:
`gamerun` runs about 35 million blocks per second with it, close to twice
real time.

The Real3D's memory is now kept (`m3::Real3d`) and its scene drawn in
software (`m3::Real3dRenderer`, see [hardware-notes.md](hardware-notes.md)
and [fidelity.md](fidelity.md) for what's left): the attract mode's
stages, title and demo screens show in 3D between the 2D layers. With
the 3D the window runs at about real time; `gamerun` draws it only when
it writes screenshots.

The game's PCI configuration routines jump into stub tables
(`base + size * 12`) that the analysis can't follow; their addresses are
listed in `GameSpec::code_entries`.
