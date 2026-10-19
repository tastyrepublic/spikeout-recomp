# Hardware references

When sources disagree about how the Model 3 behaves, use them in this order:

1. **[Supermodel](https://github.com/trzy/Supermodel)** — the primary
   reference. It is dedicated to Model 3 and is the most accurate public
   implementation, especially for the Real3D Pro-1000, Tilegen, timing,
   the network board (`Src/Network/`) and the security board.
   Key files: `Src/Model3/Model3.cpp` (bus, system registers, IRQs),
   `Src/Model3/Real3D.cpp` and `Src/Graphics/New3D/` (graphics),
   `Src/Model3/TileGen.cpp`, `Src/Model3/53C810.cpp`, `Config/Games.xml`
   (ROM sets).
2. **Real hardware behavior** observed in recordings or reported by owners,
   when it contradicts an emulator.
3. **[MAME](https://github.com/mamedev/mame)** `src/mame/sega/model3.cpp` —
   secondary; useful for cross-checking and for ROM set naming.
4. CPU manuals: *PowerPC 603e User's Manual*, *Programming Environments
   Manual for 32-bit Implementations*.

## Licensing

Supermodel is GPL-3.0-or-later, so its code may be adapted here (this
project is GPL-3.0). Keep the original copyright notice in any file that
contains adapted code and name the source file in a comment.

MAME's `model3.cpp` is BSD-3-Clause. Only facts (addresses, ROM names,
sizes, checksums) are taken from it.
