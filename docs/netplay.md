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
- **Measured (2026-10-25):** the game tolerates about 4 ms of game time on
  the link one way (at 5 ms both cabinets stay on NETWORK CHECKING; see
  `fidelity.md`, network board), because the firmware makes several round
  trips each frame. Carried with the link's time kept exact (a packet
  arrives a fixed delay after it was sent, as `m3::LinkRing` does), the
  game would run the same whatever the network, but at most about
  (that delay) / (the network's delay) times real time: a wired LAN
  (0.2-0.5 ms) keeps up, a Wi-Fi hotspot (often 2-10 ms, with spikes)
  would not. So local play, hotspots included, goes through Mode B first,
  delay-based; Mode A waits (owner's decision).

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
   **Measured (2026-10-25)**, a state of one board is 29.1 MB: work RAM
   8.4 MB, the Real3D's memories 17.8 MB, tile RAM, backup and security
   RAM 1.3 MB, the sound board 1.2 MB (left out for a board run without
   sound), the network board 0.2 MB. Compressed (zstd, its default level,
   75-140 ms) it is 2.8 MB (the title) to 5.0 MB (attract mode, frame
   3000). From one frame to the next only 3-28 KB change (30-85 4 KB
   pages of work RAM, 7-18 of the Real3D's); as a delta against the frame
   before, compressed, that is 6.5 KB (title) to 26 KB (attract), and
   108 KB over a second (60 frames). So a resync over a LAN or hotspot
   (whole states, compressed: 3-5 MB a board) takes a second or so, and
   rollback can keep a frame of every board in well under a megabyte if
   the bus and the Real3D note the pages they write.
   Linked boards' states (the packets on their way) work: see
   `fidelity.md`, network board.
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
need more: cheap save states, then profiling the game's own emulation.
Mode A (a board per device, at single-board cost) would suit weak devices,
but only on a wired LAN (see Mode A above), so it waits.

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
3. Save states for all boards. **Done** (2026-10-25: `gamerun --boards N
   --save-state` / `--load-state`, one file for all the boards and the
   packets on their way; resumed, the same bytes as a run straight on).
4. Mode B, delay-based, on a local network (LAN or a device's hotspot, no
   internet): every PC runs every board, only inputs go over the network,
   each player's sent a couple of frames ahead (17.4 ms a frame), a frame
   held only when one is missing.
   - Each board takes its inputs from a table by frame. **Done in
     `gamerun`** (2026-10-25, `tools::InputTable`: each board's inputs set
     at the end of a frame for the next, as recordings keep them; a frame
     not yet set is waited for). `--input-delay N` sets them N frames on,
     as delay-based play will. With no delay, runs are the same as before
     it: one board and two linked boards with `--press`, the same reports
     and the same sound, byte for byte. The window takes it with network
     play.
   - Found by LAN discovery only (broadcast; no address typed anywhere);
     seats in joining order, seat 1 the host and the link's master.
   - Every PC starts from the same settings and clock (the host's), or
     from the host's state; state hashes compared every few seconds, the
     host's state sent to a PC that differs.
   - A player who drops: their board gets no inputs, their character
     stands idle.
   - Headless proof: `gamerun` processes over localhost give the same
     hashes as one `gamerun --boards N --parallel` with the same inputs.
   - The window: a Network play menu page and a lobby.
5. Mode B with rollback, online (an internet lobby), on top of 4: every
   board saved each frame (pages written only), re-run when a guess was
   wrong.
6. Mode A, if wanted for weak devices on a wired LAN.
