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

The host leaving (2026-10-27, the owner's choices):
- Before the game starts, the room goes with the host: its goodbye (or
  silence for kDropAfter) closes it for the others (Session::room_closed),
  who leave it and see "THE HOST CLOSED THE ROOM" where the room's line
  was (gamerun: "the host closed the room", exit). Seen with two windows.
- In a game in progress, the room goes on (host migration): every machine
  takes the lowest seat still connected as the room's host (host_seat, the
  same rule as the authority). It welcomes players, sends the roster and
  the start, and the boards' state to a player catching up (any machine
  can), announces the room on its networks, and (the frontend, seeing
  Session::hosting) lists it on the lobby again. The others take room news
  from host_seat, not seat 1; a player joining learns it from the welcome;
  free seats, the old host's included, can be taken. The authority is now
  the room's host while it's connected (else the lowest connected seat,
  leaving out a player joined in progress who hasn't its cabinet yet: else
  the new player in seat 1 would have been in charge before catching up).
  Proved: unit tests (closing before the start; the host leaving, a player
  joining through the new host, its seat taken, inputs right), and gamerun
  on this network: the host left at frame 900, A hosted the room, B
  (started later) found it by name at A, took seat 1 from frame 2995, both
  to frame 4000, no hash check differing. And in two windows: the host
  closed, the joiner hosting the room.
- The lobby remembers a room in progress whose host went (30 minutes:
  ROOM_KEPT, Durable Object storage); a player of it, hosting it now, lists
  it again under the same code, name, seats and listing (`host` with
  `takeover: code`, matched by the room's session, which only its players
  know); else a new code. lobby/test: the takeover, and a stranger refused.
  Deployed by the owner (2026-10-27). Through it: H hosted privately
  (KKFXVU), A joined by the code, H left at frame 900, A hosted and listed
  it again ("on the lobby as KKFXVU (taken over)"), B joined by that same
  code, took seat 1 from frame 2991; no hash check differing anywhere.
  (Before the deploy it came back under a new code, and the old one found
  nothing: the fallback.)
- Menus: entering a room puts its page straight over the main menu, made
  again (so "Your room" shows); leaving or closing makes it again too.
  Network play's Host and Join aren't there while in a room.

Being in a room, shown (2026-10-27, the owner's request): Esc closes the
menu without leaving the room (to change settings), so:
- the seat list shows from when a room is joined or hosted, not only in a
  game, with a line above it until the game starts: "ROOM (HOSTING)",
  "ROOM ABC123 (HOSTING)" on the internet, "ROOM (JOINED)" (that line
  stays with Network status off: it's the notice, the seats the status);
- the main menu's first item, while in a room, is "Your room" ("Hosting,
  1/2", "Joined, 2/2", "Playing as 2P"), selected when the menu opens;
  Enter goes to the room;
- the room page: "Start game" only for the host and only before the
  start; the leave item says what it does: "Close room" (the host before
  the start), "Leave room" (a player before it), "Leave game" (once
  playing: the others go on, this player's character idle).
Seen with two windows: the host alone, then joined (2P JOINED), the
joiner's ROOM (JOINED) with the host's ping, the menus before and after
the start.

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
The host leaving (2026-10-27, the owner saw it Windows host, Linux joiner):
the joiner dropped the host's seat, then froze. The host's goodbyes come
a few times; one coming after the drop met the code for "the host before
its welcome", which marked it present and connected again before seeing
it dropped. Taken as there, the gone host was the authority again (the
lowest seat connected), so the joiner stopped making the empty cabinet's
inputs and nobody made them: its board waited for them (found by a long
wait reported: the dropped seat's inputs stopped two frames after the
drop). Now a dropped player's datagrams are ignored first. Two windows,
the host closed in a game: the joiner on at 57.5 frames a second.
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

## Rollback: can it work here? (measured 2026-10-26)

Rollback runs ahead on guessed inputs and, when another player's real
input differs, goes back to a snapshot and runs the frames since again,
all before the next picture. With every PC running every cabinet, that
needs a snapshot of every board every frame and re-runs fast enough.
Measured with `gamerun --rollback-probe FRAME:N[:speed]`, frames 1800-2400
of the linked attract (the ring model), on the owner's i7-8700K (6 cores):

| boards | full save a board | plain copy a board | changed a frame a board (avg / worst) | re-run, nothing drawn, no sound |
|---|---|---|---|---|
| 1 | 30 ms | 5.5 ms | 412 KB / 1.5 MB | 340 a second (5.9x real time, 2.9 ms a frame) |
| 2 | 35 ms | 7 ms | 420 KB / 1.5 MB | 236 (4.1x, 4.2 ms) |
| 3 | 44 ms | 10 ms | 420 KB / 1.5 MB | 224 (3.9x, 4.5 ms) |
| 4 | 53 ms | 13 ms | 420 KB / 1.5 MB | 202 (3.5x, 4.9 ms) |

A board's state is 27 MB (main RAM, texture, culling and polygon RAM, the
tile generator's, the network board's...; 28 MB with sound). Saved whole,
as save states are (m3::State field by field), it takes longer than a
frame (17.4 ms): impossible every frame. But only about 1.5% of it changes
a frame (420 KB, 1.5 MB at worst), so a snapshot of the changed 4 KB pages
(and the small rest: CPU and devices) would be well under a millisecond a
board.

The budget a frame: 17.4 ms, less the frame itself (the window measured
about 7 ms for 3 cabinets, the shown one drawn) and the snapshots: about
10 ms left for re-runs, so about 2 frames with 4 cabinets, 2-3 with 2 (more
cores would not help much: the boards already run in parallel, the
re-run is one board's frame time, each board a thread). So:
- rollback of 1-2 frames fits in the frame, unseen;
- longer ones (a far player, a burst of wrong guesses) take 2-3 frames to
  catch up: a short hitch. GGPO-style rollback adds a small input delay
  (1-3 frames) for exactly this: at 100 ms round trip (3 frames one way),
  a delay of 2 leaves rollbacks of about 1 frame; at 200 ms (6 frames),
  about 4: hitches now and then with 4 cabinets;
- slower PCs have less room.

Verdict: feasible, with this work first (each measurable):
1. Snapshots of the changed pages: knowing which pages a frame wrote
   (a dirty bit a 4 KB page set by the bus's writes and DMA, or the OS's
   write watch, Windows' GetWriteWatch / Linux's soft-dirty bits) and the
   small state apart from the big memories; a ring of a few frames' pages
   to go back to. Proof: rolled back and run again, the same hashes.
2. Re-runs that draw and play nothing (the 2D, 3D and sound of the shown
   board only for the frame shown: gamerun --no-2d already does the 2D).
3. Sound: the game's sound commands from frames still to be confirmed held
   back (a few frames' delay on sound) or played and accepted as a glitch
   when wrong: to decide with the owner.
4. Rollback in the session: predicted inputs (each player's last), the
   frame to go back to when a real one differs, a small input delay with
   it (Auto from the round trip).
5. The game's own heavy moments: these numbers are the attract; a fight
   with many enemies may run slower. To measure in gameplay (the bot,
   save states) before tuning the delay.

Step 1 done (2026-10-26): `m3::Snapshots` (src/runtime/snapshots.*). A
board's big buffers (the 14 that save states walk with State::fixed: main,
security, tile and backup RAM; the Real3D's culling, polygon and texture
RAM; the network and sound boards' RAM) are now `m3::WatchedVector`s: on
Windows, memory allocated with MEM_WRITE_WATCH, so GetWriteWatch lists
the pages written since the last look, whoever wrote them (recompiled
code, DMA, the devices, the 68000s), at no cost to the writes (measured:
2M writes a frame as fast as plain memory; the list 0.08 ms for 27 MB;
comparing pages instead 3.8 ms). Linux and macOS have their own ways
(src/runtime/watched_memory.cpp, below). Each frame: the small
state (CPU, devices' registers: 64-87 KB) saved whole; for each page
written, its contents as at the last snapshot kept for that frame, then
the copy updated. Going back undoes the pages written since, then each
later frame's pages, newest first, and loads that frame's small state.

Proof (`gamerun --rollback-test FRAME:N:DEPTH:EVERY[:compare]`: a
snapshot every frame; every EVERY frames, back DEPTH frames and on again;
each frame's whole save-state hash kept the first time and compared each
time it's run again; after each going back, the big buffers' hash against
that frame's), every check equal:

| run | re-run frames checked | went back | snapshot a frame (avg / worst) | going back (avg / worst) |
|---|---|---|---|---|
| attract with sound, back 8 every 3 (overlapping) | 1584 | 198 x 8 frames | 0.55 / 3.2 ms (186 pages) | 1.7 / 4.1 ms |
| gameplay (the owner's state at frame 10044, a fight, punches pressed), back 6 every 4 | 894 | 149 x 6 | 0.54 / 2.5 ms (157 pages) | 1.1 / 2.8 ms |
| attract, comparing pages, back 4 every 5 | 240 | 60 x 4 | 7.3 / 19 ms | 15 / 38 ms |

So about 0.5 ms a board a frame against 30 ms for a save state. The
unit tests (tests/test_snapshots.cpp) check every way the system has, and
comparing, on a made-up board, back to every kept frame. The change of allocator changes
nothing in the game: 2400 frames' GPU pictures and the sound the same,
hash for hash. Re-run inputs come from the input table, which now keeps
the frames that may run again (during the test). Linked boards going back
together and re-runs that draw nothing: below (2026-10-28). Not yet: the
window.

Each system's own way (2026-10-27, the owner's choice: the fastest each
has, behind one interface, m3::watched):
- Windows: write watch (above).
- Linux 6.7 and later: userfaultfd's asynchronous write protection, the
  pages written listed (and protected again) by PAGEMAP_SCAN: the kernel
  notes them, no signals (the same Linux added for Wine's write watch).
  Checked once at startup (a page written, found, nothing else); if it
  isn't there or fails, write protection.
- Write protection (older Linux, macOS; Flycast's way for its GGPO
  rollback, core/hw/mem/mem_watch.h): the memory made read-only when a
  snapshot starts, the first write to each page after a snapshot caught
  (SIGSEGV / SIGBUS), noted, made writable; the next snapshot protects
  those pages again. Faults on Windows cost 3-8 us each (measured: 1-2.2 ms
  a frame for 160-400 pages, write watch 0.3-0.9 ms), so Windows keeps
  write watch. Nothing writes these buffers through a system call (a file
  read into one would fail): they're filled from the programs' own buffers.
  A debugger stops at each fault unless told (gdb: `handle SIGSEGV nostop
  noprint`). Memory isn't protected until a snapshot starts: runs without
  rollback pay nothing.

Measured on Linux (WSL 2, Ubuntu 24.04, kernel 6.6.87: no PAGEMAP_SCAN,
so write protection; built as CI does, warnings as errors), the same
rollback tests, every check equal:

| run | re-run frames checked | went back | snapshot a frame (avg / worst) | going back (avg / worst) |
|---|---|---|---|---|
| attract with sound, back 8 every 3 | 1584 | 198 x 8 frames | 0.70 / 2.5 ms | 4.4 / 12.5 ms |
| gameplay (state 10044), back 6 every 4 | 894 | 149 x 6 | 0.70 / 1.8 ms | 2.6 / 3.7 ms |

Going back costs more than on Windows (the pages put back fault again
before the memory is protected once more); it happens only when a guess
was wrong.

PAGEMAP_SCAN, after `wsl --update` (WSL 3.0.3, kernel 6.18.54; Microsoft's
WSL kernel has userfaultfd and PTE_MARKER_UFFD_WP on; unprivileged
userfaultfd is off there, UFFD_USER_MODE_ONLY is allowed): the startup
check passed, the unit tests ran it and write protection, and the same
rollback tests, every check equal, the fastest of the three:

| run | re-run frames checked | went back | snapshot a frame (avg / worst) | going back (avg / worst) |
|---|---|---|---|---|
| attract with sound, back 8 every 3 | 1584 | 198 x 8 frames | 0.35 / 1.3 ms | 1.7 / 4.1 ms |
| gameplay (state 10044), back 6 every 4 | 894 | 149 x 6 | 0.34 / 1.3 ms | 1.1 / 2.0 ms |
| attract, no sound, back 4 every 10 | 80 | 20 x 4 | 0.26 / 0.7 ms | 0.9 / 1.9 ms |

Cross-OS (the same build on Windows and Linux): whole save states, sound
included, the same byte for byte at frame 2400 of attract (--clock fixed)
and at frame 10650 of gameplay from the owner's state with punches
pressed. So Windows and Linux players on x86 compute the same game.
Not yet checked: macOS, and ARM CPUs (Apple Silicon).

Linked boards going back together, and re-runs that draw and play nothing
(2026-10-28; gamerun --rollback-test on `--boards N --parallel`, and its
`:quiet` and `:timed`). Each board keeps its own snapshots, at its own
point after each frame (as before). The link ring's part of each frame
(`m3::LinkRing::cut_state`: the packets on their way between the boards'
points, as a linked save state keeps them) is cut at the slices' meeting
once every board has saved that frame, all of them stopped. Going back is
done there too: every board's snapshot and the ring's cut of the same
frame put back, as a linked save state loads; each board then leaves the
meeting by going on from its own point. `:quiet`: the frames run again
draw nothing (2D, 3D, the widescreen sides, screenshots) and play nothing,
and the sound board is left out of the snapshots: it hears each frame
once, the first time (what a wrong guess does to the sound is step 3,
the owner's choice). `:timed`: nothing hashed, rollback's own costs alone.
Two boards linked through a player joining in progress (the linking
screen's repro: board 1 coins in at frame 2600, 15848 packets over the
link), back 7 frames every 10 from frame 3000 to 4200; and attract on one
board, back 7 every 10 from frame 2000 to 2600:

| run | checked | went back | the result |
|---|---|---|---|
| 2 boards | 840 frames a board, and the ring's part of 840 frames | 120 x 7, all the boards together | every check equal |
| 2 boards, `:quiet`, GPU pictures and sound | the same | the same | every check equal; each board's GPU hash and WAV the same as the run without the test |
| 1 board, `:quiet`, GPU pictures and sound | 420 | 60 x 7 | every check equal; the GPU hash and WAV the same as without |
| 1 board, `:quiet:timed` | - | 60 x 7 | re-runs at 278 frames a second (4.6x real time); going back 1.2 ms |
| 2 boards, `:quiet:timed` | - | 120 x 7 | re-runs at 221 frames a second each (3.7x); going back 1.4 ms for both and the ring |

So a re-run frame costs about 3.6 ms on one board and 4.5 ms with two
running at once (i7-8700K), against the 10 ms the budget above leaves: 2
frames of rollback fit in a frame for 2 cabinets, as estimated. The
4-cabinet number and gameplay's heavy moments are still to measure (step 5).

Network play between them (2026-10-27): the owner's Linux window (WSL,
WSLg) joined his Windows window's game from the nearby list (WSL's NAT
networking didn't stop it on one PC) and played about 8000 frames, no
hash check differing. The internet lobby didn't work on Linux ("the
lobby: Unsupported protocol"): Ubuntu 24.04's libcurl 8.5 has no
WebSockets, yet declares curl_ws_send, so CMake's check let it through
(CI's Linux builds too). Now CMake runs a tiny program asking the system's
curl for wss (cmake/curl_probe, cmake/curl_ws_check.cpp); without it, curl
8.15 is built with the project on OpenSSL (libssl-dev, now in CI's list;
on macOS Homebrew's openssl@3). Then: Linux hosting a private game through
the lobby, Windows joining by its code, 1500 frames, no hash check
differing on either side. The sound under WSLg stutters when the window is
resized: WSLg's audio goes to Windows over its remote-desktop channel;
not seen on Windows.

## Repeated host changes: found and fixed (2026-10-27)

The deep test (build\speedcheck\scripts\deep-test.ps1, gamerun, LAN): a
room of 4; H hosts and leaves at 900, A and B join at the start, C joins
after H left, A (hosting then) leaves at 1800, H joins again, B leaves at
2700 and joins again at once, the rest go on to 5000. It passed before and
after the fixes (every joiner seated, no hash check differing, every
process the same at the end), so on its own it doesn't show the report:
gamerun joins its seat at once and has no catch-up. Two faults found in
the code, each in the report's symptoms:

- Dead buttons: a late joiner asked for its cabinet (`kTake`) once, of
  whoever it took for the authority then. Reliable delivery gets it there,
  but a machine that isn't the authority (any more) drops it, and a host
  whose machine goes without a goodbye never answers: the joiner never had
  its seat, its inputs never went out. Proved by a unit test that failed
  so (tests/test_net.cpp, "a late joiner asking for its cabinet as the host
  leaves"; `Session::vanish` stops a session without a goodbye). Now asked
  again every 2 s and at once when the authority changes, until handed
  over; an authority asked again for a cabinet it has handed over answers
  again with the same frame.
- Frames a second differing, a joiner never catching up: the window ran
  fast from the host's state until its frame was within 4 of one other
  seat's inputs (seat 1, or 2 when it shows seat 1). After host changes
  that seat may be a cabinet whose player left, its inputs stopped where
  they did: the joiner stopped catching up far behind, took its seat (the
  others then waiting for its inputs) and ran on at the arcade's speed,
  never catching up. Now it catches up to the furthest of the other seats'
  inputs.

Also: a session's player id is the session's own now (it was the
process's: in a test with several players in one process, a second
player's join was taken for the first's asked again). The joining state is
plain zstd of every board: about 4 MB a few hundred frames in, 18-20 MB
by frame 2000 (the game's RAM and textures filled), whoever hosts; over
the internet that's several seconds of a join.

Not yet: the window test with two or three windows across host changes
(below, step 5), and the internet one.

## Open: repeated host changes (reported 2026-10-27)

The owner's report, two or more windows on the internet: after a few host
changes in a game in progress, a player who joined can't catch up, the
cabinets' frames a second differ, and sometimes the joiner's game buttons
do nothing while the menu keys still work. One host change was proven
(gamerun on the LAN and through the lobby, two windows); several in a row,
with joins and rejoins between them, never were.

Suspects, none checked in code yet:

- `Session::take_seat` sends `kTake` once, to `authority()` at that
  moment, with no retry. The receiver answers only if it thinks it is the
  authority and the cabinet is still open with no `until`. If the authority
  changes before `kTaken` arrives, or the machines disagree on it, the
  joiner's `mine_from` is never set and its inputs are dropped: buttons
  dead, menu keys fine (they never go through the network). A fix: resend
  until `kTaken`, to the current authority.
- The window's catch-up test (tools/spikeout.cpp, `catching_up`) compares
  our frame with `inputs_known` of seat 0, or 1 when we show seat 0. After
  host changes that seat may be the left host's idle cabinet (its stream
  made by the authority, maybe stalled or far ahead), so the joiner takes
  its seat too early or never and stays unpaced (`catching_up` runs fast):
  that would explain frames a second differing. A fix: the highest stream
  among the seats still played, or the frame of the state plus the inputs
  fed.
- `host_seat` and `authority()` can differ between machines for a moment
  (who's connected, `drops[].suspected`, a late joiner not yet seated);
  two machines both acting as host, or none, after several changes.
- `start_info` on a machine that itself joined in progress may be stale
  when it becomes the host and sends the start and state to the next joiner.
- A player rejoining a seat it left: `dropped`, `said_bye`, `gone_name`,
  `cabinets[s]` and the roster may keep the old player's values.

Deep test to write (gamerun first: the same every run, hashes to prove it;
the window after):

1. Four gamerun processes, private LAN room of 4 cabinets. H hosts, A and
   B join at the start. H leaves at frame 900; C joins; A (the new host)
   leaves at 1800; H joins again (a rejoin); B leaves at 2700 and joins
   again at once; go on to 5000. Then the same through the lobby (`-Lobby`,
   by the room code).
2. After every join, a key test: `--press` on the joiner (e.g. start and
   attack a few frames after its seat is taken) and check that the other
   machines' input table has its bits at that frame (log or
   `--net-trace`), and that the game reacts (a player on the map, the
   board's logic).
3. Proofs each run: no hash check differing (every 120 frames), every
   joiner logs its seat taken (`kTaken`), each process's pictures a second
   equal (gamerun `--renderer gpu --gpu-scale 1 --gpu-hash-frames`, count
   new pictures; never 0x1000A8 alone), and the same final hash on every
   process for the same frame.
4. Unit tests in tests/test_net.cpp for each case found (authority change
   while a `kTake` is in flight, rejoining the same seat, two changes in
   a row).
5. Windows: the same order with two or three windows
   (`SPIKEOUT_MENU_KEYS`, `SPIKEOUT_NET_DEBUG=1`), screenshots and logs.

Scripts (git-ignored, build\speedcheck\scripts; outputs to its out\):
migrate-test.ps1 (gamerun: H hosts 2 cabinets and leaves at 900, A goes
on, B joins; `-Lobby URL -Tag x`), hostleave-test.ps1 (two windows, the
host closes after the start), closed-room.ps1 (the host closes before the
start), room-shots.ps1, shot.ps1 (a window's picture by PrintWindow),
newpics.ps1 (new pictures counted), exit-test.ps1. The window tests run a
second copy of the window from build\speedcheck\pA (its own settings,
player name SPIKE1 X); the scripts copy build\spikeout.exe there first.

## What the cabinets send each other (investigated 2026-10-28)

The owner asked whether a PC could run only its own board (all four cost
too much on tablets). That depends on what the link carries, so it was
recorded: `gamerun --link-dump FILE` writes, for each data frame a board
sends, the sender's own part (its first 0x900 bytes, comm RAM
0x100-0x9FF, what its main board wrote that frame). Two boards, the
linking repro, then both players in one fight (frames 4600-5800, presses
on each board at known frames). Read with a scratch tool (not in the repo).

- The layout (the same through attract, joining, select and play): a
  header 0x000-0x0FF (counters changing every frame: 0x01C the logic
  counter, 0x058 the frame counter, 0x000 another), then four records of
  0x200 bytes (0x100, 0x300, 0x500, 0x700), one written each frame in turn:
  the last four frames.
- **Most of a record is the same on every cabinet.** Taking both boards'
  blocks at the same logic counter, about 100 of the ~120 bytes a record
  that change each frame are identical: every cabinet computes them itself
  (the game is lock-step: each runs the whole world). Several look like
  check values (16-bit words that jump every frame).
- **About 20 bytes a record are each cabinet's own:** 0x05 its buttons
  (0x40 right, 0x80 left, 0x01 beat, a frame after the press); 0x08-0x0B
  two 16-bit values that start moving a few frames after its player turns
  (its camera's angle, by the look: movement is relative to each player's
  own camera, so the others need it); 0x0E-0x0F, 0x11-0x13, 0x34-0x37,
  0x40-0x44, 0x5C, 0x68, 0x74 (slowly changing values and flags). The
  header has a few own bytes too (0x000-0x001, 0x014-0x015, 0x020,
  0x025-0x026, 0x042-0x047, 0x04C-0x04D, 0x058-0x059).

So a PC could run its own board alone, the link answered there (the
firmware's round trips stay local, unlike Mode A above), with only each
cabinet's own bytes travelling: a remote cabinet's block made from this
board's shared bytes and the remote's own bytes as received. Under
rollback the remote's own bytes would be guessed until they arrive: the
buttons change seldom, but the camera-like values change whenever that
player moves, so guessing them (the last ones, or carried on) would be
wrong more often than Mode B's guessed buttons. Computing them here
instead would mean running the game's own camera code for each remote
player (a change to the game's code).

**Tested (2026-10-28): a cabinet can be stood in for by its network board
and the block its main board writes.** `gamerun --link-record B:FILE`
logs every call a board's main board makes to its network board (syncs,
reads with the value read, writes, frame ends, gamerun's slice meetings);
`--link-standin B:FILE` runs that board's network board alone, from the
same state, driven by the log, its main board not run; both print board
0's main RAM hash every frame and its whole-state hash every 100. From a
linked state at frame 4700 of the fight above (`--save-state`), 1100
frames with presses on both boards: board 0 the same every frame (1100
RAM hashes, 11 whole-state hashes), and the stood-in network board gave
every one of the 1,271,600 reads the value it gave linked. In play the
main board's only writes are its block (0x900 bytes, 576 32-bit writes,
about 0.68 of a frame in, in one burst of 2,000 cycles); the rest is
reads and the clock. When it's written doesn't matter: the block held and
written at 2.0M, 2.4M or 2.8M main cycles after the frame's end (0.69,
0.83, 0.97 of a frame; `--link-standin-at CYCLES`), board 0 the same every
frame (the network board sends it only early in the next frame).

So with one board a PC, each remote cabinet is its network board (a small
68000) plus its block each frame, which is this board's shared bytes and
the remote's own; it could even send its whole block XORed with the one
predicted here (no need to know what each byte means). Guessing a
remote's own bytes until they arrive (`gamerun`-free scratch analysis,
frames 4600-5790): the buttons as the frame before 99.3% of frames; all
of a record's own bytes as before only 3-12%, or as before or carried on
at the same rate 22-39%. Almost all the misses are one 16-bit value,
record offset 0x0E-0x0F (wrong in 685 and 894 of 1189 frames on the two
boards); the camera-like 0x08-0x0B in 30-109, the rest under 70, the
buttons 8. What 0x0E is (a value that wobbles each frame) is the next
question: if the game's own code can compute it here, guesses would miss
about as seldom as Mode B's.

**What the own bytes are (2026-10-28).** Found with `--watch-write` (now
also printing r14), `--peek [B:]ADDR:LENGTH@FRAME[:N]` and `--ram-dump
B:LO-HI@FRAME:N FILE` (board 0's RAM each frame, searched for the values
board 1 sends, at the same logic counter ±2: a value found at the same
frame is computed on every board; found only 2 frames later, at the
received copies, it is that cabinet's alone). The block is copied each
frame (0x4E88, a copy adding a checksum) from main RAM 0x141000; each
record from the game's own at 0x140200 (r13 = 0x108000; players' objects
at 0x1A0000 + 0x4000 n, enemies' at 0x1B0000 + 0x4000 n). Record fields
(block offsets; the block's bytes are each 32-bit word reversed):
- 0x04-0x07: the cabinet's inputs (0x100100, bits moved): its own.
- 0x08-0x0B: its player's facing (player +0x2A, and +0x74 or +0x76),
  written by 0x55048. Computed by every board (board 0's copy of player
  2 the same, 100% of frames, before the record reaches it).
- 0x0E-0x0F: **its camera's heading**, camera object (pointer at
  0x104150, 0x11D480) +0x2A: written by 0x2C4DC each frame from the
  camera's position (+0x10-0x18) to the point it looks at (+0x404-0x40C)
  through an atan2 (0xC120); +0x28 the pitch. Moving that value doesn't
  turn the picture (the renderer uses the two points); every board puts
  it in that player's object (+0x3334) with the facing, two frames on:
  the stick moves each player relative to their own camera. Only that
  cabinet has its camera: its own, changing almost every frame as the
  camera eases after its player.
- 0x10-0x13: its player's state flags (player +0x00): computed by every
  board.
- 0x14-0xD7: a sync check entry for each character, the 4 players then
  the 8 enemies (0x747DC): its flags word, four status bytes and the sum
  of +0x74, +0x76, +0x3C, +0x3E and the position floats +0x380-0x388.
  The same on every board, but for the players not playing (their
  objects keep each board's own leftovers; constant) and the enemies'
  flags' low byte (bits 0x08 and 0x10, differing now and then between
  boards, a few values in 1100 frames: per cabinet, perhaps whether it's
  in that cabinet's view).
- 0xD8-0xDF: the logic counter and frame bits (the same on every board).
- The header: two per-cabinet counters (0x000, 0x058, one up each frame)
  and constants.

So in play a cabinet's own data is its buttons (seldom changing), its
camera's heading (every frame), a few enemy flag bits (seldom) and two
counters (one up a frame). Through a long fight (boss, players beaten
down; 4705-9690) the same list. Outside a shared game (a player joining,
CONTINUE?, game over, attract: the cabinets each on their own screens)
nearly the whole block is the cabinet's own: those phases would send it
whole. Not understood: 9000-9900 (players being knocked down) the shared
check words differed between the boards in about a quarter of frames
(the pairing by logic counter slipping, or a real difference).

**Tested (2026-10-28): a remote player's camera can be run here, exactly.**
`gamerun --camera-record 1:FILE` records board 1's camera (0x420 bytes
from 0x11D480) as each of its updates starts, with the logic counter;
`--camera-test 0:FILE` hooks board 0's camera update (0x2C4DC, a hook
added to kHooks for it) and runs the same game code once more on the
side, with a second camera in the camera's place: started from board
1's recorded camera at the first update, then run on by itself. The side
run has no scheduler and no interrupts, and RAM, the CPU and the clock
are put back after (`recomp::Context::set_aside` / `put_back`, a debug
aid), so board 0 runs on as without it: its main RAM hash the same every
frame, and its whole state every 100 (1100 frames, checked against the
recording run). The second camera's heading against board 1's at its
next update:

| run (from the fight state at 4700) | updates compared | heading the same, run on by itself | from the recorded camera each time |
|---|---|---|---|
| both players moving and punching, to 5800 | 1098 | 100% | 100% |
| player 2 moving, the boss coming in, players knocked down, to 9700 | 4998 | 100% | 100% |

So the camera's heading follows from the shared world and the camera's
own past alone: a PC can run each remote player's camera itself (the
game's own code, a second camera object for each), and the remote's own
data left to guess is its buttons, a few enemy flag bits and the two
counters. The whole 0x420 bytes copied matched only now and then (0.1-61%):
some of that range changes outside the update (not all of it the camera,
or the renderer's), but the heading never drifted. The update also writes
outside the camera: each character's +0x34A0-0x34BF (players' and
enemies' objects; one of the sync list's status bytes is +0x34A0: values
for that camera, made by the side run too) and the stack (0x73EE40-
0x73EFA0).

**Every check (2026-10-28): board 1's link data rebuilt from board 0
alone, exactly.**
- The shared check words: with each record paired by its own logic
  counter (+0xDC; the earlier pairing took only the newest slot and
  slipped), every character's checksum the same on both boards in 4959
  of 4960 game frames. The one left is player 2 dropping out (below).
- The enemy flags' low byte: bits 0x08 (on that cabinet's screen) and
  0x10 (just outside it), set by the view test 0x33A6C in each
  character's update (players from 0x51824, enemies from 0x52230): the
  character's position through the view matrix (0xBCEC, f20-f31) against
  the screen's edges (f12-f15, widened by f19). Board 0 uses board 1's:
  the stand-in's blocks with them all 0x03 (`--link-standin-patch
  enemy-flags`) changed board 0's RAM beyond the received copies
  (0x100033-0x10004F, 0x10009B, 0x103073, objects' flags). The view matrix
  is camera +0x640 (0x11DAC0), made after the update by 0x32578 (the
  camera's zone 0x4FDD4 at +0xAFC and +0x170, ..., the matrix): so the
  "camera" is 0xC00 bytes, and the side run is the update then 0x32578.
  Each view test run on the side with the second camera's matrix in
  f20-f31: the flags low byte as board 1's in 100.00% of 35,000
  character-frames (board 1's record of logic L carries the tests of
  L-2).
- The rebuild (scratch tool, not in the repo): each record of board 1 =
  board 0's of the same logic counter, with board 1's buttons (sent), the
  facing and flags from board 0's copy of player 2 (0x1A4000, `--ram-dump`),
  the second camera's heading and every character's view bits from the
  side runs (`--camera-test` writes them to FILE.view), the players not
  playing as board 1 had them when the test began; each header = board
  0's, board 1's two counters one up from its last and its constants.
  Byte for byte against board 1's real ones:

  | run | records exact | headers exact |
  |---|---|---|
  | from the fight state, player 2 moving, the boss, players knocked down (to 9700) | 4995 of 4995 | 4996 of 4996 |
  | from power-on, the second camera started just after player 2 joins (frame 4460) | 1527 of 1527 | 1528 of 1528 |

  So a joiner's camera need only be sent once, in the join state.
- The cost: the side runs' game code 0.35-0.81 ms a frame in the tests
  (two camera runs, the test's free one and its one-step check, and 7
  view tests), so about 0.4 ms a frame a remote player: against about 6
  ms for running its whole board.
- Out of the shared game: with nobody pressing anything, player 2 knocked
  out (frame ~9622, time running out) writes record +0x100 alone (0x08
  from 0x4C21C, then 0x04 and 0x14 from the link code 0x6EBCC / 0x6EDD4)
  and leaves the shared game for CONTINUE?: that is the one frame whose
  checksums differed (logic 8479), the second camera stops matching from
  there (logic 8482 on: board 1's own screen now), and header +0x04 is
  that cabinet's own in those phases. As with joining, a cabinet out of
  the shared game (knocked out, continue, game over, attract) would send
  its block whole until it is back in.
- Not tested: a stage's change (no two-player state reaches one: mashing
  punches on both boards cleared enemies but not the room before time
  ran out; the bot plays one board); a third and fourth player.

**Across a gate (2026-10-28): the camera also takes the game's events.**
The owner played a network game hosted alone (the window's F9 in network
play now saves every cabinet: spikeout-net-<frame>.state, which gamerun
--boards N --parallel --load-state runs on from) to "BREAK THE GATE!!",
stage 1; player 2 joined from it in gamerun (coin, start, the game
select, "HERE COMES A NEW PLAYER."), both broke the gate and went on
through two areas (frames 6250-8400). The second camera, run one step
from board 1's each time: 100%. Run by itself it went wrong at the gate:
code other than the camera's writes straight into "the camera" (board
0's), as the gate's script does (0x5D21C sets its mode, +0x48, to 8,
with 0x345EC and 0x372D4 setting its parameters, at the same game frame
on both boards). `--camera-test-mirror` passes such writes on to the
second camera (a write watch on the camera; the camera's own being the
update, from its hook until 0xC670 is called from 0x32514 just after it
returns, and the view routine, 0x32578, until the first view test: two
more observer hooks): the gate then right, 83.8% of frames by itself
(from 72.6%). But not every such event is shared: at logic 5485 0x5E550
writes camera +0xB80-0xB87 on board 0 with values board 1's camera
didn't get (a per-cabinet effect, perhaps a shake for its own player),
and from there and the next area (0x30154 sets mode 0x15 at 5614) the
second camera drifts. So running a remote player's camera means knowing,
for each kind of camera event, whether it is shared (copied) or that
player's own (to be made for the remote player): the events seen so far
0x5D21C, 0x345EC, 0x372D4 (the gate), 0x5E550, 0x30154; later stages
may have more.

How many there can be (2026-10-28). A scan of the recompiled code (every
function loading the camera pointer, `lwz rX, -16048(r13)`, and what it
stores through it; a scratch tool): 112 functions load it, 59 store into
the camera: about 10 the camera's own (setting it up, the update, the
view), about 10 only its flags word (+0x00), about 24 setting its mode
(+0x48, mostly a few lines each, as 0x5D21C: stage script commands), and
about 15 other fields (placing it and its target, and an area at
+0xB00-0xB9F, effects). Plus helpers given the pointer (0x345EC,
0x372D4). `gamerun --camera-events` counts, on every board, the writes
into the camera by code other than its own (the update's and the view
routine's times from their hooks, as for --camera-test-mirror), and with
two boards compares each writer's writes by game frame, offset and value:
- attract mode, 30,000 frames (the game's own demo play): 22 writing
  blocks, of them start-up (memory cleared, the camera set up at frame
  213) and 0xBE14 (0x32470's positions from player 1), so about 12 events:
  mode and placement (0x372D4, 0x345EC, 0x69E30, 0x6AD9C, 0x2C480) and the
  +0xB00 area (0x34348, 0x3442C, 0x4A1B0, 0x6B18C, 0x6B5DC/0x6B618 written
  every frame for long stretches, 0x6B6C8/0x6B6D8, 0x6C01C);
- the gate run (two players): 0x372D4, 0x5D21C and 0x5E550 the same on
  both boards (shared), 0x345EC not (18 writes on board 0, 3 on board 1:
  each cabinet's player's, it seems), 0xBE14 each board's own;
- the long fight: 0x32A80 nearly the same (112 against 114), 0xBE14.
So 0x5E550 (above) is shared: the drift after logic 5485 is something
else (0x345EC, or when the copied write lands). The +0xB00 area's
writers, seen in attract, are still to be classified in a two-player run. The empty enemy slots (enemies beaten before player 2
joined) keep different leftovers on each cabinet, as the empty player
slots do, until reused.

Debug aids added for these (gamerun): `--camera-trace N` (board 0's camera
updates and view tests: return address, r12, r14, the screen edges, the
view matrix and where it is in RAM), `--camera-test-from FRAME`,
`--link-standin-patch enemy-flags`; the hook list has 0x2C4DC and 0x33A6C
(observers: single-player's sound and save state the same, byte for
byte, with them).

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
