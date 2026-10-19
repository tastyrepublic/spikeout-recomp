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

Register state lives in `ppc::CpuState` (`s.gpr[n]`, `s.cr`, `s.xer`, ...);
flags are computed by the small inline helpers in `recomp/guest.hpp`.
Every instruction's disassembly is kept as a comment.

What isn't handled yet throws when reached instead of guessing: the
system call and trap exceptions (and `eciwx`/`ecowx`, which Model 3 games
don't use). Running into code that wasn't recompiled throws
`recomp::MissingCode`; a callee returning somewhere other than the
instruction after its call throws (`bad_return`), so a wrong assumption
stops the run where it happens.

Every instruction the game uses is lifted: integer, branch, load/store,
CR, SPR, time base and floating point. The time base advances by
`Context::tb_step` on each TBL read: the recompiled code doesn't count
cycles, and this keeps delay loops finite and repeatable.

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
  mode follows writes to FPSCR), compares set FPCC, `mcrfs` clears the
  exception bits it reads. Not modelled: exception bits and FPRF after
  arithmetic, double rounding in single-precision results (computed in
  double, then rounded), and `fres`/`frsqrte` return exact values rather
  than the 603e's estimates.

## Bus

`m3::Bus` (`src/runtime/bus.*`): 8 MB of work RAM, the fixed and banked
program ROMs (with the bank select at `0xF0100008`), and the MPC106 PCI
bridge's configuration ports. Everything else is open bus (reads all
ones, writes dropped) and counted per region, so a run reports which
devices the game used that aren't emulated yet.

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
  blocks it ran and the devices it used that aren't emulated. Not a pass/
  fail test: it shows what to work on next.

On `spikeofe`: the boot ROM is 29 functions and 396 instructions. It runs
about 2.9 million blocks (most of them clearing 8 MB of RAM and copying
1 MB with a checksum) in well under a second, configures the MPC106 from
its register tables, and touches no other device.

The game's RAM program lifts to 2,175 functions and about 124,000
instructions with nothing left unimplemented, and is compiled as the
`game_ram` library (split over 8 files: about 100 s on 12 threads, 166 s
as one file). `gamerun` runs it about 4.6 million blocks into its startup
(PCI setup, a backup RAM test, I/O, network board, system and tilegen
registers) at over 100 million blocks per second, until the first `sc`
(`li r3, 17; sc` at `0x11678`): exceptions are next (roadmap milestone 2).

The game's PCI configuration routines jump into stub tables
(`base + size * 12`) that the analysis can't follow; their addresses are
listed in `GameSpec::code_entries`.
