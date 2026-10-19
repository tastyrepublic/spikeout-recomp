# spikeout-recomp

A static recompilation of **Spikeout: Final Edition** (Sega, 1999, Model 3
Step 2.1) to native code for Windows, macOS and Linux — with widescreen,
modern controls and **online co-op** as the goals.

Instead of emulating the arcade board's PowerPC CPU instruction by
instruction at runtime, the game's program ROM is translated ahead of time
into C++ that is compiled for your PC. The rest of the board (Real3D
graphics, Tilegen 2D layers, SCSP sound, I/O) is reimplemented natively.

> **Status: playable in a window.** Attract mode, coin, start and the
> first stage run, with the 3D drawn in software and the sound effects
> playing; no music yet, and the 3D is missing a few effects. See the
> [roadmap](docs/ROADMAP.md).

## No game data included

This repository contains **no Sega code or assets** and never will. You need
your own dump of the arcade ROMs (the MAME set `spikeofe`). The tools read
them from a folder on your machine; nothing from the ROMs is ever committed.
Please don't open issues or PRs containing ROM data or links to it.

## What's here so far

| Part | Path | What it does |
|---|---|---|
| PowerPC 603e decoder | `src/ppc/instruction.hpp`, `decoder.cpp` | Decodes every 603e instruction, including the 603-only `tlbld`/`tlbli` |
| Disassembler | `src/ppc/disasm.cpp` | Text output with simplified mnemonics (`li`, `mr`, `beq`, `blr`, ...) |
| CPU state | `src/ppc/cpu_state.hpp` | Registers as a plain struct, ready for fast save states |
| Memory map | `src/runtime/memory_map.*` | Step 2.1 address map ([docs](docs/memory-map.md), [hardware notes](docs/hardware-notes.md)) |
| ROM loader | `src/runtime/rom.*`, `src/games/` | Verifies sizes/CRC32s, interleaves chips, normalizes byte order |
| Control-flow analysis | `src/analysis/` | Functions, basic blocks and jump tables reachable from the vectors, through the boot ROM's copy into RAM ([notes](docs/control-flow.md)) |
| Lifter | `src/recomp/lifter.*` | PowerPC functions to C++: every instruction the game uses, floating point included ([notes](docs/recompiler.md)) |
| Runtime | `src/recomp/context.*`, `src/runtime/bus.*` | What recompiled code runs against: CPU state, function table, bus with RAM, ROM and the MPC106 PCI bridge |
| Sound board | `src/runtime/soundboard.*`, `scsp.*` | The 68000 sound CPU (Moira) and two SCSPs: slots, envelopes, LFOs, FM, the effect DSP ([hardware notes](docs/hardware-notes.md#sound-board)) |
| Tools | `tools/` | `romcheck` (verify a set, show boot code), `ppcdis` (disassemble), `codemap` (map the code), `ppcrecomp` (recompile), `boottest` (run the recompiled boot ROM), `gamerun` (run it into the recompiled game) and `spikeout` (the game in a window) |

## Building

Requires CMake 3.20+ and a C++20 compiler (GCC 13+, Clang 17+, Xcode 15+ or
Visual Studio 2022).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Checking your ROM set

Unzip your `spikeofe.zip` into a folder, then:

```sh
./build/romcheck path/to/spikeofe
```

This lists every chip (ok / missing / wrong size / bad CRC), reports the
detected program ROM byte order, and disassembles the code at the CPU's
reset vector (`0xFFF00100`). To dig further:

```sh
./build/romcheck path/to/spikeofe --dump-crom crom.bin
./build/ppcdis crom.bin --base 0xff800000 --offset 0x700100 --count 64
```

To map the game's code (functions, blocks, jump tables, and what couldn't
be followed):

```sh
./build/codemap path/to/spikeofe --scan-pointers --disasm code.txt
```

`--disasm` writes an annotated listing of everything found. Like the dumps
above it comes from your ROMs, so keep it out of the repository.

## Running recompiled code

Point CMake at your unzipped ROM set and the build recompiles the boot ROM
into `build/generated/` and adds a test that runs it:

```sh
cmake -S . -B build -DSPIKEOUT_ROM_DIR=path/to/spikeofe
cmake --build build
ctest --test-dir build --output-on-failure
```

The build also recompiles the whole game (`game_ram`, a few minutes the
first time). `boottest` runs the recompiled boot code from the reset vector to the
jump into RAM and checks what it did; `gamerun` keeps going into the game
and reports where it stops (`--screenshot`, `--shot-every N` save the
screen as PNG; `--wav FILE` records the sound board; `--press coin@1200 --press start@1300` presses inputs at
given frames, to play through menus without a window). Generated code is
derived from your ROMs: it stays in the build directory and is never
committed.

`spikeout` runs the game in a window (SDL3: CMake uses an installed one,
or downloads and builds SDL 3.4 with the project; `-DSPIKEOUT_FRONTEND=OFF`
leaves it out):

```sh
build/spikeout path/to/spikeofe
```

It shows the 2D layers and the 3D (drawn in software for now, without the spotlight
and a few other effects) and plays the sound effects (the music board isn't
emulated yet; `--mute` for silence). Keys: `5` coin, `1` start, `F2` test,
`F1` service, arrows, `Z` beat, `X` charge, `C` jump, `V` shift; hold
`Tab` to run as fast as possible, `F12` for a screenshot, `Esc` to quit.

## Online play

Each Spikeout cabinet is one player; co-op means up to four cabinets
linked through their network boards. The plan has two stages:

1. **Link mode:** each PC runs one board and the cabinet link goes over the
   network, the way Supermodel does it. Best on LAN.
2. **Rollback:** each PC runs all the linked boards, only controller inputs
   travel over the network, and late inputs are fixed by rewinding and
   replaying a few frames. The same code gives local co-op on one PC.

Details in [docs/netplay.md](docs/netplay.md).

## Credits and references

- [Supermodel](https://github.com/trzy/Supermodel) — the primary Model 3 hardware reference (ROM layout, address map, Real3D, network board).
- [MAME](https://github.com/mamedev/mame) (`src/mame/sega/model3.cpp`) — secondary reference for cross-checking and ROM set naming.
- Motorola/IBM *PowerPC 603e RISC Microprocessor User's Manual* and *Programming Environments Manual*.
- [Moira](https://github.com/dirkwhoffmann/Moira) by Dirk W. Hoffmann (MIT, vendored in `third_party/moira`) — the 68000 core that runs the network board's firmware and the sound board's program.
- [SDL 3](https://github.com/libsdl-org/SDL) (zlib license) for the window, downloaded at build time if not installed.
- MAME's 315-5881 decryption by Andreas Naive, Olivier Galibert and David Haywood (BSD-3-Clause; tables and algorithm in `src/runtime/crypt315_5881.cpp`, with their notice), as also used by Supermodel.

See [docs/REFERENCES.md](docs/REFERENCES.md) for how sources are used.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
