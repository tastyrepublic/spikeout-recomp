# Control-flow analysis

`src/analysis/` finds the code the recompiler has to translate: functions,
basic blocks and jump tables, starting from the CPU's reset and exception
vectors. `codemap` runs it on a ROM set and reports the results.

## How Spikeout boots

The analysis follows the boot ROM by itself, but the copy into RAM is
described in the game spec (`GameSpec::boot_copies`):

1. Reset at `0xFFF00100` (high vectors). The boot code probes the MPC106
   PCI bridge at two addresses (`0xFEC00CF8` and `0xF0800CF8`) for vendor
   `0x1057`, sets the bridge up and sizes RAM.
2. `0xFFF03090` copies 1 MB from the fixed CROM at `0xFFE00000` to RAM
   address 0, with a running checksum (`0xFFF033FC`), then writes the PVR
   to RAM, and does `li r3, 0x100; mtlr r3; blr`.
3. RAM `0x100` sets r13 (small-data base `0x108000`), and `rfi`s to
   `0x1540` with MSR `0x3002` (FP on, low vectors).

The game runs entirely from that RAM copy: about 512 KB of code at
`0x0`–`0x7FFFF`, then data. The high exception vectors are all
`bl 0xFFF03340`, a shared handler that never returns.

## What the analysis does

Recursive descent from the entry points, with a small register tracker
along each straight-line path. The tracker knows constants
(`lis`/`addi`/`ori`), loaded values and scaled table indices, which is
enough for:

- **Constant `mtlr`/`mtctr`/`mtsrr0` + `blr`/`bctr`/`rfi`**: how the boot
  code enters RAM. The target becomes a new function.
- **Jump and call tables.** This compiler's pattern is: index scaled by
  `slwi`/`rlwinm`, table address added with `addis`+`addi` on the index
  (or `lis`+`addi` and `lwzx`), `lwz`, `mtctr`/`mtlr`, `bctr`/`bctrl`/`blrl`.
  It never guards them with `cmplwi`; the index is usually a byte
  (`lbz`, `srwi 24`). So the length is a guess: entries are read until one
  doesn't look like code, or another table starts (tables sit back to back).
- **Returns.** A `blr` is a return when LR holds what it had at function
  entry, or a value loaded from memory. Spikeout's epilogue often is
  `bl 0x372D4; mtlr r2; blr`: `0x37258`/`0x372D4` save and restore
  r16–r31, f12–f19, r2, CTR and CR, and r2 holds the saved LR. The code
  doesn't keep to the SVR4 ABI (r2 is a scratch register), so after any call
  the tracker forgets every register but r1.

### Pointer scan (`--scan-pointers`)

Most of the game is reached through callbacks stored in RAM at run time
(task structs, hooks in small data, pointer tables built at boot), which
the vectors alone don't reach: without the scan, the analysis finds about
a quarter of the code. The scan takes as a function entry any word in the
RAM copy, or constant built by the code, that points into the RAM copy at
plausible code right after a function boundary (`blr`, unconditional
branch, `rfi`, padding). Plausible means it decodes up to its first
unconditional branch, its branches stay in the image, and it doesn't start
with a conditional branch or a branch to itself and has no absolute
branches: floats like `0x42EB0000` (117.5) decode as `bc` to itself.

A pointer may also land inside a function, on an instruction the analysis
already reached, and that is taken as an entry too:

- Spikeout's tasks yield by storing the address to resume at (`lis` +
  `addi`, sometimes `addic`, then `stw` into the task at `12(r12)`); the
  task loop at `0x11E10` calls it on a later frame.
- Callbacks kept in a register across calls (`lis r31` / `addi r31` …
  `mtlr r31; blrl` at `0x6532C`, which the tracker loses at the calls).
- Tables pointing at shared tails: the record table at `0x93424` points at
  `0x4119C`, the last instructions of the function at `0x41188`.

Most such constants are false (plain numbers like `0x1EE00` land on code
too), which costs only an extra function, as long as a real function's
code is explored first: each instruction is explored once, with the
registers of the first path to reach it, and a guessed entry in the middle
of a function starts with nothing known. So the entries are explored in
order, each with everything it reaches before the next: the seeds and
their calls first, the scan's guesses last.

The analysis is repeated until no new entries turn up.

### Tables the register state misses

Exploring each instruction once also means a dispatch reached first on a
path where the index is a constant (a state machine after `li r3, 1`, at
`0x324C4`) gives a constant address and one target. So before an indirect
branch the analysis also reads the instructions themselves:
`rlwinm` of the index, then `addis`/`addi` of the base, `lwz rX, d(rB)`,
`mtlr`/`mtctr`, or the same with the base built separately by `lis`/`addi`
and `lwzx rX, rA, rI`. Either way it's a table of guessed length. Table
entries are data and may be unaligned: the one at `0xA1579` is.

## Results on `spikeofe`

With `--scan-pointers`:

| | |
|---|---|
| Functions | about 2,900 (1,445 found only by the pointer scan) |
| Instructions | about 119,900 (466 KB of the RAM copy) |
| Code area found | 70–98% per 64 KB of `0x0`–`0x7FFFF` |
| Tables | 42 (17 jump, 25 call), all guessed length |

What's left, reported as issues:

- **61 unresolved calls**: `bctrl`/`blrl` through function pointers loaded
  from RAM, e.g. a table whose address is itself stored at `0x710008`.
- **7 unresolved jumps**: three copies of a PCI-config stub that jumps to
  `base + size * 12` (in the boot ROM and RAM), and tail calls through
  struct pointers.
- The rest of the code is reachable only through those. The runtime will
  need a fallback (an interpreter that logs what it runs, roadmap
  milestone 2) or `codemap --entry` hints.
