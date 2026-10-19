# Roadmap

Each milestone ends with something you can run and check.

## 1. Foundation — *in progress*

- [x] CMake/C++20 project, CI on Windows, macOS and Linux
- [x] PowerPC 603e decoder and disassembler
- [x] CPU state struct
- [x] Model 3 Step 2.1 memory map
- [x] ROM set definition for `spikeofe` and a verifying loader
- [x] Confirm program ROM byte order against a real dump (`romcheck`): as-is
- [x] Control-flow analysis: find functions and jump tables starting from the reset vector and exception vectors (`codemap`, [notes](control-flow.md))
- [x] Lifter: emit C++ for each function: integer, branch, load/store, CR, SPRs ([notes](recompiler.md))
- [ ] Lifter: floating point (13% of the game's instructions)
- [x] Runtime bus: dispatch loads/stores to RAM, ROM and device stubs; MPC106 PCI bridge
- [x] The recompiled boot ROM runs from reset to the jump into RAM: bridge setup, RAM clear, 1 MB copy with checksum (`boottest`)
- [ ] **Goal:** the recompiled boot code runs through the ROM/RAM self-test

## 2. Getting to the attract mode

- [ ] Interrupt controller and decrementer
- [ ] Banked CROM switching
- [ ] 315-5881 security board decryption (key `0x29236FC8`)
- [ ] Open-bus reads return all ones (Spikeout's ROM check needs it)
- [ ] Frame timing: 57.524 Hz, 424 lines, IRQ 0x02/0x0D/0x40 as in [hardware-notes.md](hardware-notes.md)
- [ ] EEPROM, RTC and backup RAM
- [ ] Reference interpreter for code that the static pass misses, with logging so it can be added to the recompiled set
- [ ] **Goal:** reaches the attract loop, verified against Supermodel's behavior

## 3. Graphics

- [ ] SDL3 window and frontend
- [ ] Tilegen 2D layers (text, HUD, menus)
- [ ] Real3D Pro-1000: display list / culling node walker
- [ ] Polygon and texture decoding, native renderer (Vulkan/Metal via SDL GPU)
- [ ] Widescreen and high resolution
- [ ] **Goal:** attract mode and gameplay render correctly

## 4. Sound and input

- [ ] 68000 sound CPU + SCSP (emulated; it's small)
- [ ] DSB2 MPEG music board
- [ ] Keyboard, gamepad and arcade stick mapping, coin/start/service

## 5. Local multiplayer

Spikeout is one player per cabinet; multiplayer means linked boards.

- [ ] Network board emulation (Supermodel `Src/Network/` as reference)
- [ ] Run 2–4 boards in one process with a virtual link → local co-op on one PC

## 6. Save states

- [ ] Complete serializable machine state per board (CPU, RAM, video, sound, I/O, network board)
- [ ] Save/load in under a millisecond; determinism tests (same inputs → same state hash)

## 7. Online play

See [netplay.md](netplay.md).

- [ ] Mode A: one board per PC, cabinet link over the network (LAN first)
- [ ] Mode B: rollback, every PC simulates all linked boards; 2–4 players plus spectators
- [ ] Lobby / matchmaking; Steam networking as an optional backend

## 8. Release

- [ ] Packaged builds for Windows, macOS (universal), Linux and Steam Deck
- [ ] First-run ROM import wizard
