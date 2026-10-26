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

## Session model (Mode B), as built (2026-10-25)

Delay-based, every machine running every board; our own code, no netcode
library (`src/net/`, `tools/netplay.hpp`).

- **Seats:** the host's cabinet is seat 1, the link's master (link setting
  1); players take the next free seat as they join (slaves, setting 2); the
  ring goes in seat order. 2–4 players.
- **The start:** the host's Start powers every machine's boards on at once,
  from the host's clock. Each board starts from its own player's cabinet:
  every player's machine sends the host its settings EEPROM and backup RAM
  (the window: its first cabinet's) as it joins, and the start carries all
  of them to everyone. The country is set to the host's on every board, as
  linked cabinets must share one (a new cabinet, with no settings yet,
  starts as the host's). After the game each machine keeps its own
  player's board as its cabinet (its country back as it was), as a cabinet
  keeps what it played. Only machines with the same build (the ROMs
  sampled, the recompiled game, the protocol: `tools::build_id`) can join.
- **Boards:** all of them run in each process at once, meeting every 8
  lines, the link's packets a slice late (`gamerun --parallel`); only the
  local player's board draws its 2D and 3D and runs its sound.
- **Inputs:** each player's inputs set at the end of a frame (for the
  next, `tools::InputTable`), `input delay` frames ahead, and sent to every
  other machine (UDP, every frame not yet acknowledged sent again until it
  is, the others' passed on too). A board waits at the end of a frame until
  every player's inputs for the next are in. Input delay 0 is as the
  arcade (a frame waits for the network once, about a millisecond on a
  LAN); a setting gives 1–4, or Auto (chosen at the start from the round
  trip: 0 within about 8 ms one way; not yet adjusted during play).
- **Checks and recovery:** every 120 frames each board's main RAM hash
  goes to the others. A machine whose boards differ from the host's asks
  for every board's state (a linked save state, zstd-compressed: 2-9 MB
  for two boards), runs on from it unpaced until it has caught up, and the
  game goes on; the others only wait for its inputs meanwhile.
- **Drops:** a player not heard from for 3 s (or who says goodbye): the
  lowest seat still there stops everyone taking that seat's inputs from it,
  asks how far each has them, gets them as far as anyone has, and announces
  the frame from which the seat has none. Its character stands idle and the
  game goes on, the same on every machine; the cabinet is then free.
- **Empty cabinets, joining in progress:** every cabinet of a game is
  powered on at the start, as in an arcade, whether a player has it or not:
  the host can start alone, the others standing in attract mode. A cabinet
  no player has gets inputs of none from the authority (the lowest seat
  still connected), sent as anyone's. A player can join the game in
  progress (listed "playing" nearby and in the lobby while a cabinet is
  free): the host sends the start and every board's state, the inputs
  since come as usual, the new player's boards run unpaced until they have
  caught up, then the authority hands the cabinet over from a frame a
  little ahead (8 frames). The cabinet is played as it was set up (not
  from the joiner's own, which isn't kept afterwards either). Sound: the
  joiner's own board starts with its sound board fresh (the host's state
  carries only the host's), so the music comes back with the next tune.
- **Finding games, on this network:** a host broadcasts on every network
  this machine is on (port 47326, once a second); "Join a game nearby"
  lists them. No internet needed (a LAN, one device's hotspot).
- **Finding games, on the internet:** the lobby, a Cloudflare Worker with
  one Durable Object (`lobby/src/index.js`, free plan; WebSocket hibernation
  so idle players cost nothing). Hosting gives a 6-letter code; a game is
  listed for anyone with the same build, or private (by its code only).
  Names only, no accounts. The lobby's address is fixed in the build
  (CMake's `SPIKEOUT_LOBBY_URL`), not the player's to change.
- **Connecting over the internet:** each machine learns its router's
  address from Cloudflare's STUN, and sends the lobby every address it may
  be reached at (its own, its router's); both sides send to all of the
  other's at once (the host as the lobby tells it of a joiner), which opens
  most home routers ("hole punching"), and keep to the first that answers.
  When routers won't allow that, Cloudflare's TURN relay: the lobby hands
  out short-lived credentials (its key kept in the Worker's secrets) and a
  machine takes a relay address the others can send to; a direct path is
  preferred as soon as one works. Mode B's traffic is a few KB a second a
  player, so a relay costs little.
- **The window:** Esc, Network play: players, input delay, host on this
  network or on the internet (listed or private), join a game nearby or on
  the internet (the lobby's list, or by code: a letter at a time, pads
  too). The lobby page shows the code, the seats and Start (the host) or
  Leave. In a network game the menu doesn't pause (the others play on;
  this player's controls are let go meanwhile), and save states, recording
  and the arcade settings are off.

How it was proven (2026-10-25, one PC):

- Two `gamerun` processes over this machine's network, found by LAN
  discovery, give the same reports for both boards and the same sound
  (both boards', byte for byte) as one `gamerun --boards 2 --parallel` with
  the same presses (board 1's reached the host only over the network); the
  same with `--input-delay 2`; and with a player who leaves at frame 1316,
  the host announces it idle from 1317 and plays on to the end.
- The same through the lobby run here (`wrangler dev`): a private game
  joined by its code, the same reports and sound again.
- Two windows: one hosts on this network, the other finds it nearby and
  joins; both play from power-on to frame 1500 with every hash check
  equal, each showing its own cabinet.
- Keeping each player's board after the game (two portable windows, each
  with its own cabinet files: the host export and coins, the joiner Japan
  and free play, with no saved data yet). The host leaves from the menu at
  about frame 970: it keeps seat 1's board as its cabinet; the joiner plays
  on with the host standing idle, then its window is closed: it keeps seat
  2's board, its settings back to Japan and free play (its board played as
  export, the host's country), and now has saved data of its own.

- Joining in progress (2026-10-26): the host starts alone (`gamerun
  --net-host ... --net-start-alone`), a second gamerun joins 8 s later,
  gets every board's state at frame 897 (4.6 MB packed) and takes cabinet
  2 from frame 957; its presses (coin and start at 2000-2060) reach the
  host only over the network. The host's run to frame 3000 is the same,
  reports and both boards' sound byte for byte, as one `gamerun --boards
  2 --parallel` with those presses (board 1 idle before them); the
  joiner's game state is the same too (its report differs only in
  gamerun's own counters, kept from where it began). Two windows: one
  starts alone, the other finds it "playing" nearby 15 s later, joins at
  frame 945 (5.3 MB), catches up in about a second and has cabinet 2 from
  frame 1008, every hash check equal.
- Recovery (2026-10-26): a 2-player gamerun game in which the joiner
  alone pokes a byte of board 0's RAM at frame 1500: both see the
  difference at frame 1560, the joiner gets every board's state at 1563
  and runs on; no check differs after, and the host's run is the same as
  the one-process reference, reports and sound byte for byte. In two
  windows (`SPIKEOUT_TEST_POKE=0:100800=5A@900` on the joiner's): the
  difference seen at frame 960 on both, the joiner put back in step at
  963 (5.8 MB), no check differing after.

- The lobby deployed (2026-10-26) to the owner's Cloudflare account,
  `wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby` (the build's
  `SPIKEOUT_LOBBY_URL`; its workers.dev subdomain registered by that first
  deploy). Two gamerun processes through it: a private game joined by its
  code, every hash check equal; two windows: one hosting a listed game on
  the internet, the other joining it from "Join on the internet".

- The relay (2026-10-26): the TURN key's two secrets set on the Worker
  (`npx wrangler secret put TURN_KEY_ID` and `TURN_KEY_API_TOKEN`, the
  values typed by the owner). The lobby now hands out credentials and
  each machine takes a relay address. With `SPIKEOUT_TEST_RELAY_ONLY=1` on
  the host (it offers only its relay address and takes only what comes
  through it), two gamerun processes play 600 frames all through
  Cloudflare's TURN, every hash check equal. Found doing so: the relay
  refuses a permission request naming a private address (a LAN one), so
  only public addresses are asked for, one a request.
  (`SPIKEOUT_NET_DEBUG=1` prints the relay's answers.)

Not done yet:
play between two different networks (hole punching and the relay are
tested only against Cloudflare's STUN from here); input delay adjusted
during play; spectators.

Built (the owner's design, 2026-10-26): a network status display in the
window, only in network games, with a "Network status" setting under
Show FPS (on by default; settings.ini `show_net`). It sits at the top
right under the FPS counter, in its style (yellow `#ffd34a`, black
outline). One row per seat: "1P YOU" (no ping), others with the round
trip in ms and, right of it, phone-style signal bars (4 under 40 ms
green, 3 to 80, 2 to 140 yellow, 1 over 140 red, none until measured;
tuned after the two-network test; narrow, the tallest as high as the
capitals, on the text's baseline: the owner's adjustments). No separate
messages: a row shows JOINING while a
player joining in progress catches up, JOINED (green) for 4 seconds then
its bars, LEFT for 4 seconds then OPEN (a goodbye), IDLE (grey) while a
player who went quiet is dropped, WAITING for a free seat while hosting
(OPEN to the others). From `net::Session::seats()`, 4 times a second
(`ui::Menu::set_net`). Seen working: WAITING, bars and ms, IDLE after a
joiner was stopped, a game joined by code. Not yet seen: JOINED and LEFT
on screen (they last 4 seconds).

Join by code (2026-10-26, the owner's choice): one box of the code's six
letters (`ui::CodeInput`), typed on the keyboard (Backspace deletes one,
Delete all, Ctrl+V pastes, Enter joins; letters a code never has, I, O,
0 and 1, are ignored), or picked from an on-screen keyboard of the 32
code letters with Delete and Join keys: the arrows, a pad (D-pad or
stick, A presses, Y deletes, Start joins, B back) or the mouse. Seen
working: typing, Backspace, moving and pressing keys on it, and joining
a private lobby game by its code; the owner tried both and they work.
Fixed with it: a host starting before every seat was taken told the lobby
all were ("taken" as the seat count), so joining it by code said "that
game is full"; now the seats with players (window and gamerun).

Leaving says goodbye (2026-10-26, checked each way): closing the window,
Alt+F4, the menu's Quit and Network play's Leave all send it (the window
calls Session::leave as its loop ends, or on Leave). Tested with a host
window and a joiner window, the joiner leaving each way: the host drops
the seat the same second, its game never stopping (57.5 frames a second,
60 the next while it caught up a few frames). Fixed doing so: with
Alt+F4 the host froze 3 s (dropped only when silence ran out) because
the leaving machine went on sending its inputs while it shut down and
the host took them as signs of life; now nothing heard after a goodbye
counts, and a machine that has said it sends nothing but the goodbye.
Only a crash or a lost connection still waits for kDropAfter (3 s);
shortening that, or showing who is being waited for, is left until
rollback (the owner's call).

gamerun and the window start alike (2026-10-26). gamerun used to fill the
main board's RAM with 0xA5 at power-on (the window: zero), so a game
between them was out of step at the first hash check (frame 120) and put
back from the host's state. The game's pictures and sound were the same
either way (proved: a lone cabinet's 2400 GPU picture hashes and its
sound, two linked boards' pictures, each the same as with zero RAM), so
gamerun now starts at zero too: a window hosting and gamerun joining from
power-on, 2260 frames, no hash check differing.

The Network play menu (2026-10-26, the owner's layout): Join a game, Host
a game, Player name, Input delay. Join a game is one list of every game:
this network's first ("nearby", found by its broadcasts from when Network
play opens), then the internet lobby's listed ones ("internet"), each with
its players and "playing" for one in progress, then Join by code and
Refresh. A game in both (an internet host's broadcasts reach this network
too) shows once, nearby, by its session: the lobby's list now carries
each game's session (deployed 2026-10-26, version 4b4f8409; an older
lobby's list has none, and the game then shows twice). Host a game:
Players, Where (This network / Internet (listed) / Internet (private)),
Start hosting. Seen: a host on the internet and on this network listed
once, nearby, in another window's Join a game.

The lobby's list with many games (2026-10-26, deployed, version
7a976fcd): games still waiting for players first, then those in progress
with an empty cabinet, the newest first in each (`created` when hosted);
50 a message (`list {from}` -> `games {games, from, total}`), the window
offering "More games (51-100 of 230)" and "Earlier games" rather than a
cap (the owner's concern: every game stays reachable); each game with
its host's country as Cloudflare sees it (`request.cf.country`, two
letters: "(1/2, internet, GB)"), a hint for a near one. Tested in
lobby/test (the order, two pages of 52 games, the total) and on the live
lobby (a game under a made-up build listed with its country, GB here).
One Durable Object holds every player; a lobby per region would be the
next step if that ever gets crowded.

Player names (2026-10-26, the owner's choice): "Player name" in the
Network play menu, the same page as Join by code: up to 10 of A-Z, 0-9,
space, - _ . (typed lower case comes out upper case; anything else is left
out; Ctrl+V, the on-screen keyboard, OK). Kept in settings.ini
(`player_name`); none set: this computer's user name, cleaned the same
way (`tools::clean_name`); the others' names shown cleaned too (the
menu's font has only Latin letters). The game's name in the nearby list
and the lobby is the host's. Seen: a name typed (SPIKE1 X), kept, a game
hosted and found by it; - and . typed, / left out.

Open, the owner to decide (2026-10-26): joining a game in progress starts
without background music until the next scene change. Each PC runs sound
only for its own cabinet, so the host's state for the joiner's cabinet
has a sound board that never ran. Options put to the owner: (1) replay the
cabinet's recent sound commands (cheap; the music restarts from its
beginning); (2) the host runs every cabinet's sound board without playing
it, so the state it sends is exact (only the host pays, roughly a quarter
of a cabinet's run time each; option 1 if the host has left); (3) every PC
runs every cabinet's sound (exact always; every player pays). Suggested:
2, measured on 4 cabinets first.

Seen in the same test (two windows on one PC, both with the ring model): joined
at frame 1991, seat taken at 2105, no hash check differed; 50-60 frames a
second with both windows running all three cabinets on one PC (about
15 ms of each 17.4 ms frame). Fixed then: leaving before catching up left
the next game unpaced (111 frames a second).

Testing it: `cd lobby && npm install && npm test` (the lobby, run here);
`gamerun ROMS --net-host NAME --seats 2 ...` and `gamerun ROMS --net-join
NAME ...` (two terminals), or with `--lobby ws://127.0.0.1:8790/v1/lobby`
and `wrangler dev --port 8790` for the internet path.

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
   - **Done** (2026-10-25), with the internet lobby too: see "Session
     model" above (seats, start, inputs, checks, drops, LAN discovery, the
     lobby, the window, and how it was proven). Still to do: deploying the
     lobby, play between two networks, input delay adjusted during play,
     resync, rejoining, spectators.
5. Mode B with rollback, online, on top of 4: every board saved each frame
   (pages written only), re-run when a guess was wrong.
6. Mode A, if wanted for weak devices on a wired LAN.
