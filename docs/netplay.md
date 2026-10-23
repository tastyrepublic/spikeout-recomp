# Online play design

## How Spikeout multiplayer works on the arcade hardware

Each Spikeout cabinet is **one player**: one board, one joystick and four
buttons. Two to four players means two to four complete Model 3 boards,
linked through their network boards (mapped at `0xC0000000`). The game code
on each board exchanges state with the others every frame over that link.

So "four players on one PC" is not a mode the game has: it means running
four boards and a virtual link between them. See
[hardware-notes.md](hardware-notes.md#network-board-0xc0000000).

## Mode A: one board per player, link carried over the network

This is what Supermodel's `SimNetBoard` does: every PC runs one board, and
network board traffic is forwarded to the next cabinet in a ring over TCP.

- **Pros:** cheap (one board per PC); uses the game's own netcode; matches a
  known-working reference, so it's the right **first** milestone.
- **Cons:** the link expects cabinets on a local cable. Each frame waits for
  the neighbor's data, so it is effectively lockstep: fine on LAN or very low
  ping, stutters on ordinary internet connections.

## Mode B: rollback, every PC simulates all boards

Every PC runs **all** the linked boards (2–4) plus the virtual link inside
one process. Only controller inputs go over the network. Missing remote
inputs are predicted; when the real ones arrive and differ, every board is
restored to the last confirmed frame and re-simulated (GGPO style).

- **Pros:** responsive over the internet; spectators are easy (they receive
  confirmed inputs only); the same machinery gives local multiplayer on one
  PC (all inputs local, one window or split screen per player).
- **Cons:** N boards of CPU work per frame, more during rollback. Native
  recompiled code makes this realistic, and only the local player's board
  needs its graphics actually rendered — the others only need the Real3D
  state the CPU can observe.

### What Mode B needs from the runtime

1. **Determinism.** Same starting state + same inputs = bit-identical
   results on Windows, macOS and Linux, x86-64 and ARM64:
   - PowerPC floating point emulated exactly (rounding modes, `fres`/
     `frsqrte` estimates, fused vs. unfused multiply-add). Build with
     `-ffp-contract=off`, never fast-math.
   - No host time, thread scheduling or uninitialized memory reaching game
     state; the virtual link delivers data at fixed points in the frame.
   - Sound may run separately only if it never feeds back into the main CPU.
2. **Fast save/restore for N boards.** Per board: CPU registers
   (`ppc::CpuState`), 8 MB work RAM, backup and security RAM, Real3D/Tilegen
   memories, device registers, network board state. With 4 boards that's
   well over 32 MB, so saves must copy only dirty pages.
3. **Render separation.** Re-simulated frames are never drawn or heard.
4. **Desync detection.** Peers exchange a state hash every N frames.

### Measured: four boards on one PC (2026-10-21)

Spikeout is one player per cabinet, linked lock-step every frame, so four
players are four boards, and Mode B runs all of them on every PC. On an
i7-8700K (6 cores), `gamerun`, 4 linked boards (board 0 master), attract
mode, emulation only:

| Run | Speed |
|---|---|
| 1 board, as now (2D drawn, sound boards run) | 1.95x real time |
| 1 board, `--no-2d --no-sound` | 4.34x |
| 4 boards taking turns (one thread at a time) | 0.35x |
| 4 boards, `--parallel` (at once, meeting every 8 lines) | 1.30x |
| 4 boards, `--parallel --no-2d --no-sound` | 2.53x |

- **Parallel boards** (`tools::Turns` parallel, `m3::LinkRing` deferred):
  every board runs a slice at once; what they send is held and handed on in
  board order when all have reached the slice's end, so a packet takes at
  least a slice (8 lines, about 0.33 ms) to arrive. The ring still comes up
  (status 1 `0x2081`-`0x2084`, about 1,375 packets each way in 17.8 s), and
  three runs gave identical reports (each board's CPU registers, cycles,
  EEPROM, network state). The slice length matters little (4 lines 1.21x,
  16-64 lines about 1.35x): the boards wait for the slowest at each meeting.
- **A remote cabinet needs no 2D or sound**: the tile generator's drawing and
  the sound and music boards only make pictures and sound, which the game
  never reads back. Without them the game's state is identical (the same
  report line for line) and a board runs more than twice as fast.

So on this PC a session (the local board drawn and heard, the others not)
runs at roughly 2x real time: enough for delay-based play (inputs sent a few
frames ahead, nothing re-run), and for rollback of a frame or so. Deeper
rollback, slower machines (a Steam Deck) and spectators re-running the game
need more: cheap save states, then profiling the game's own emulation. Local
play on one network (LAN, a hotspot; no internet needed) can instead use
Mode A, a board per device at single-board cost.

## Session model (Mode B)

- 2–4 players plus spectators, peer-to-peer UDP with relay fallback.
  Candidate libraries: GekkoNet or GGPO (MIT); Steam Networking Sockets as an
  optional transport.
- ROM set CRCs and build version are compared at connect time; any
  difference guarantees a desync.

## Plan

1. Single board, single player (milestones 1–4).
2. Run two boards in one process with a virtual link → local 2-player.
   This proves the link emulation without any networking. **Done in
   `gamerun --boards N`** (2–4, `m3::LinkRing`; one thread per board,
   taking turns every `--slice` lines, the same every run; `--parallel`
   runs them at once, still the same every run, measured above). Still to do:
   the window frontend (one window or split screen per player, inputs per
   board).
3. Mode A over LAN, checked against Supermodel's link behavior.
4. Save states for all boards, determinism tests.
5. Mode B rollback online.
