# Roadmap

Each milestone ends with something you can run and check.

## 1. Foundation — *in progress*

- [x] CMake/C++20 project, CI on Windows, macOS and Linux
- [x] PowerPC 603e decoder and disassembler
- [x] CPU state struct
- [x] Model 3 Step 2.1 memory map
- [x] ROM set definition for `spikeofe` and a verifying loader
- [ ] Confirm program ROM byte order against a real dump (`romcheck`)
- [ ] Control-flow analysis: find functions and jump tables starting from the reset vector and exception vectors
- [ ] Lifter: emit C++ for each function (integer, branch, load/store first, then FPU)
- [ ] Runtime bus: dispatch loads/stores to RAM, ROM and device stubs
- [ ] **Goal:** the recompiled boot code runs through the ROM/RAM self-test

## 2. Getting to the attract mode

- [ ] Interrupt controller, decrementer and VBlank timing
- [ ] Banked CROM switching
- [ ] 315-5881 security board decryption (key `0x29236FC8`)
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

- [ ] Four players on one PC in single-cabinet mode

## 6. Save states

- [ ] Complete serializable machine state (CPU, RAM, video, sound, I/O)
- [ ] Save/load in under a millisecond; determinism tests (same inputs → same state hash)

## 7. Online play

- [ ] Rollback netcode for 2–4 players, with spectators — see [netplay.md](netplay.md)
- [ ] Lobby / matchmaking; Steam networking as an optional backend
- [ ] LAN mode that emulates the original cabinet link board

## 8. Release

- [ ] Packaged builds for Windows, macOS (universal), Linux and Steam Deck
- [ ] First-run ROM import wizard
