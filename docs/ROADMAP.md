# Roadmap

Each milestone ends with something you can run and check.

## 1. Foundation — *done*

- [x] CMake/C++20 project, CI on Windows, macOS and Linux
- [x] PowerPC 603e decoder and disassembler
- [x] CPU state struct
- [x] Model 3 Step 2.1 memory map
- [x] ROM set definition for `spikeofe` and a verifying loader
- [x] Confirm program ROM byte order against a real dump (`romcheck`): as-is
- [x] Control-flow analysis: find functions and jump tables starting from the reset vector and exception vectors (`codemap`, [notes](control-flow.md))
- [x] Lifter: emit C++ for each function: integer, branch, load/store, CR, SPRs ([notes](recompiler.md))
- [x] Lifter: floating point (13% of the game's instructions); the whole game compiles (`game_ram`) and runs into its startup (`gamerun`)
- [x] Runtime bus: dispatch loads/stores to RAM, ROM and device stubs; MPC106 PCI bridge
- [x] The recompiled boot ROM runs from reset to the jump into RAM: bridge setup, RAM clear, 1 MB copy with checksum (`boottest`)
- [x] **Goal:** the recompiled boot code runs through the ROM/RAM self-test (and on into the game's own startup and its backup RAM test)

## 2. Getting to the attract mode — *in progress*

- [x] Exceptions: `sc`, traps, and returning from a handler with `rfi` ([notes](recompiler.md#exceptions)); the game now initializes and waits for an interrupt
- [x] Interrupt controller and decrementer; the game runs frame by frame in its interrupt handlers
- [x] Banked CROM switching (bank select at `0xF0100008`, in `m3::Bus`)
- [x] 315-5881 security board decryption (key `0x29236FC8`; `m3::Crypt315_5881`, MAME's cipher, checked against it on random streams)
- [x] Open-bus reads return all ones (Spikeout's ROM check needs it)
- [x] Frame timing: 57.524 Hz, 424 lines, IRQ 0x02/0x0D as in [hardware-notes.md](hardware-notes.md) (`recomp::FrameTiming`; not yet: waiting for the IRQ 0x02 acknowledge)
- [x] Backup RAM (the game's RAM test passes)
- [x] RTC (Epson RTC-72421, `m3::Rtc72421`): on the emulated time, settable by the game
- [x] Network board: its own 68000 runs the firmware the game uploads (`m3::CommBoard`, Moira core; the simulation stays as `--netboard sim`); with the link set to master it brings the ring of one up (status 1 `0x2021`) and the game goes on to its ROM check
- [x] Security board check: passes (it showed ILLEGAL ROM before the decrypter)
- [x] JTAG scan chain of the Real3D chips; Real3D PCI ID, DMA device and ping-pong bit: the game runs its main loop and sends the Real3D real frames
- [x] Settings EEPROM (93C46 on the I/O board, `m3::Eeprom93C46`): the game writes its defaults and runs as a link master on its own; before it, it read garbage, set up as a slave and the firmware reported NO CARRIER, as a lone slave should
- [x] Task resume points: code that tasks yield to (stored `lis`/`addi`/`addic` constants) is found by the analysis; the game now runs minutes of game time without missing code
- [x] Sound board link: MIDI port and IRQ 0x40 in VBlank (the game queues its sound commands and waits on them); the bytes are kept for a sound board
- [x] Return sites: a resumed task can return into a function it called from before it yielded (`resume_at`, about 7,000 sites)
- [x] The attract mode runs: logo, title, INSERT COIN, BASIC OPERATION (16,633 frames with no missing code)
- [x] Gameplay: coin, start, fighter select and the first stage with the fighters in 3D (`gamerun --press`), 11,000 frames with no missing code. Fixed on the way: FPSCR[FPRF] after FP arithmetic (the fighters were NaN), returns past several calls (the scheduler nested until the stack overflowed)
- [ ] Reference interpreter for code that the static pass misses, with logging so it can be added to the recompiled set
- [ ] **Goal:** reaches the attract loop, verified against Supermodel's behavior

## 3. Graphics

- [x] SDL3 window and frontend (`spikeout`: the game on its own thread at 57.524 Hz, keyboard to the I/O board)
- [x] Tilegen 2D layers (`m3::TilegenRenderer`, drawn a line at a time): boot text, logos, attract screens
- [x] Real3D Pro-1000: memory, texture uploads, the viewport and culling-node walker, model decoding (`m3::Real3d`, `m3::Real3dRenderer`)
- [x] Software rasterizer (depth buffer, perspective-correct textures, sun and ambient light; bands of lines on several threads): the attract mode in 3D at about real time
- [x] Fog and the ping-pong update buffers
- [ ] Still missing from the 3D: spotlight, specular, LOD blending, frustum culling, translucency sorting
- [ ] Native renderer (Vulkan/Metal via SDL GPU)
- [ ] Widescreen and high resolution
- [ ] **Goal:** attract mode and gameplay render correctly

## 4. Sound and input

- [x] 68000 sound CPU + two SCSPs (`m3::SoundBoard`, `m3::Scsp`, adapted from Supermodel's SCSP with fixes from the chip's register layout and MAME's board map): the sound effects play in the window (SDL audio) and `gamerun --wav` records them
- [ ] MIDI link timed like the board's serial chip (31,250 baud) instead of a burst per frame
- [ ] DSB2 MPEG music board
- [ ] Keyboard (done in `spikeout`: coin, start, test, service, joystick, buttons), gamepad and arcade stick mapping

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
