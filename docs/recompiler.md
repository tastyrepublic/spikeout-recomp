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

Recompiled code doesn't count cycles. Time is `Context::blocks`, the
number of basic blocks run, times `cycles_per_block` (8 by default) CPU
cycles at 166 MHz: every run with the same inputs is exactly the same,
which save states and rollback netplay need. The time base and the
decrementer run at 16.5 MHz (bus clock / 4) on that clock; DEC going from
0 to -1 raises the decrementer exception (`0x900`).

`Context::tick()` is one compare per block. It falls into the slow path
only when something is due: a `Scheduler` event, the decrementer, the
block limit, or an interrupt that may now be taken, because the IRQ line,
MSR (`mtmsr`, `rfi`) or the decrementer changed. External interrupts
(`0x500`) are taken at a block start when the IRQ controller's line is up
and MSR[EE] is set.

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
