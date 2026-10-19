# spikeout-recomp

A static recompilation of **Spikeout: Final Edition** (Sega, 1999, Model 3
Step 2.1) to native code for Windows, macOS and Linux — with widescreen,
modern controls and **online co-op** as the goals.

Instead of emulating the arcade board's PowerPC CPU instruction by
instruction at runtime, the game's program ROM is translated ahead of time
into C++ that is compiled for your PC. The rest of the board (Real3D
graphics, Tilegen 2D layers, SCSP sound, I/O) is reimplemented natively.

> **Status: early foundation.** Nothing is playable yet. See the
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
| Memory map | `src/runtime/memory_map.*` | Step 2.1 address map ([docs](docs/memory-map.md)) |
| ROM loader | `src/runtime/rom.*`, `src/games/` | Verifies sizes/CRC32s, interleaves chips, normalizes byte order |
| Tools | `tools/` | `romcheck` (verify a set, show boot code) and `ppcdis` (disassemble) |

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

## Online play

Spikeout supports up to four players, originally over linked cabinets.
The plan is **rollback netcode**: every player runs the full game locally,
only controller inputs travel over the network, and late inputs are fixed by
quickly rewinding and replaying a few frames. Recompiled code is deterministic and
its state is small, which makes this practical. Details in
[docs/netplay.md](docs/netplay.md).

## Credits and references

- [MAME](https://github.com/mamedev/mame) (`src/mame/sega/model3.cpp`) — ROM set definition and address map.
- [Supermodel](https://github.com/trzy/Supermodel) — Model 3 hardware research and ROM interleave layout.
- Motorola/IBM *PowerPC 603e RISC Microprocessor User's Manual* and *Programming Environments Manual*.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
