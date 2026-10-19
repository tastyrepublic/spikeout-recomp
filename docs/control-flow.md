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
The analysis is repeated until no new entries turn up.

## Results on `spikeofe`

With `--scan-pointers`:

| | |
|---|---|
| Functions | about 2,190 (955 found only by the pointer scan) |
| Instructions | about 112,000 (437 KB of the RAM copy) |
| Code area found | 70–98% per 64 KB of `0x0`–`0x7FFFF` |
| Tables | 35 (15 jump, 20 call), all guessed length |

What's left, reported as issues:

- **42 unresolved calls**: `bctrl`/`blrl` through function pointers loaded
  from RAM, e.g. a table whose address is itself stored at `0x710008`.
- **7 unresolved jumps**: three copies of a PCI-config stub that jumps to
  `base + size * 12` (in the boot ROM and RAM), and tail calls through
  struct pointers.
- The rest of the code is reachable only through those. The runtime will
  need a fallback (an interpreter that logs what it runs, roadmap
  milestone 2) or `codemap --entry` hints.
