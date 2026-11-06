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
  joiner's cabinet comes with the host's own sound board (since
  2026-10-30, below: "Decided and done"), its music the host's track; it
  had come with one that never ran, silent until a reload.
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

Flags, the game's name, the delay, a speed in the lobby's list (2026-11-01,
the owner's design from mock-ups; uncommitted when written):
- Each seat's flag between the seat and the name ("2P [flag] ALEX"), as
  tall as the text's capitals; a seat without one keeps its room, so the
  names line up. The flags are vf3-recomp's (`assets/ui/flags.png` and
  `flags.rcss`, 48x36 sprites of flag-icons by Panayiotis Lipiridis, MIT:
  `assets/ui/flags-LICENSE.txt`; made by vf3-recomp's
  `tools/flags/make_flags.py`), embedded, and served to RmlUi by a file
  interface (`ui/menu.cpp`); the renderer reads PNG as well as BMP (SDL
  3.4's `SDL_LoadPNG_IO`).
- A player's country is only ever the lobby's (Cloudflare's, as VF3 does:
  never what a game says of itself). The lobby's welcome gives each player
  its own, `joiner` gives the host each joiner's (`Session::expect` with
  the country, matched by the addresses the lobby gave), and the host's
  roster passes every seat's on (a joiner keeps its own from its welcome).
  A game joined on a LAN has no lobby: its joiners have no flag.
- Above the seats in a game: its name and room ("ALEX · ROOM J7YKXP"; the
  name alone on a LAN) and a "DELAY nF" pill. A "ROLLBACK n" pill is drawn
  by `ui::NetPills` but not shown: network play has no rollback yet.
- The join list: each internet game's host flag, then on the right the
  same signal bars and "~N ms", an estimate the lobby makes from how far
  apart Cloudflare puts the two players (light in fibre over a route half
  as long again, plus 5 ms: Bangkok-Tokyo ~74, London-New York ~89;
  `estimate` in `lobby/src/index.js`), with no traffic to the host; the
  help line says it's an estimate and the real one shows in the game. The
  list is nearest first among waiting games, then among those in progress.
  Measuring it for real before joining was turned down: it would give
  every listed host's address to anyone browsing.
- Seen in the window against a local lobby (`wrangler dev`, which says
  every player is in GB at one place): the join list's flag, bars and
  "~5 ms"; in the game both seats' flags (the host's through the roster),
  the name and room line and the delay pill; on a LAN the host's flag and
  none for the joiner. The lobby with these changes isn't deployed.

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

**Decided and done (2026-10-30): (4) the host's own sound board, carried.**
With one board a PC in view, (2) can't work (the host won't run the
joiner's cabinet), so a fourth option was put and taken: when the window
captures every cabinet's state for another machine (a join, or a machine
put back in step), each cabinet whose sound it doesn't run carries its
own sound board (the window's capture, tools/spikeout.cpp). Why it's
right:
- In the game, every cabinet's music board plays the same track: `gamerun
  --music-hash N` (each board's music board, every N frames: a hash of its
  state, and what it plays: the stream's start and end, how far, the
  volume; `m3::Dsb2::now_playing`). Two cabinets both in the game
  (two-start.state with the both-players recording, frames 5100-7800): the
  same track and volume, the place the same or one MPEG frame apart
  (0x240 bytes, ~36 ms). The whole-state hashes differ (counters of each
  board's own history), so what plays is what's compared.
- A cabinet still waiting plays its own: from power-on (the linking
  repro), board 1 in attract and the select played its own tunes (stopped,
  the attract's, the select's a8135e) while board 0 played the stage's;
  as its player came in (~4400, 0x110000 set) its music board took the
  stage's track, at once and in step. So the game itself starts the shared
  track on a cabinet entering the game.
- Why a joiner had none: its cabinet in the host's state had no sound part,
  so it ran a sound board that had never run, and that board answered
  nothing (no select tune, no stage tune on entering, as two windows
  showed: stopped from the join at 4500 to 7800), the start-up exchange
  the main board makes only at power-on missing. Only a reload from the
  host gave it music.
- With the host's sound board carried (two windows on this PC,
  SPIKEOUT_NET_DEBUG=1, which now prints the shown cabinet's music every
  300 frames; J joins at 4500, coin and start, then put out of step at
  7080 by SPIKEOUT_TEST_POKE): J plays H's track from the join on, and on
  through its reload (7082) to the end. In the window each machine's music
  runs on its own audio device's clock, and the joiner's board ran
  unpaced while catching up, so J's place trailed H's by about a third of
  a second: the same track, about the same place.
- A cabinet still waiting when taken plays the host's track until its own
  game sends its tune (coin, the select, entering): it answers them now.
  A moment of the host's own sound effects may come along. The states sent
  grow by a sound board a cabinet (about 1.1 MB before packing; 11,990 KB
  packed for two cabinets against 11,943 before).

**Smaller states (2026-10-30).** Every cabinet's 8 MB of main RAM is mostly
the shared world, the same as the others', but 8 MB and more apart in the
state, beyond zstd's plain window (2 MB at level 3). `net::compress` now
packs with long-distance matching over a 2^27 window (128 MB, what a
reader takes by default: `ZSTD_decompress` unpacks it unchanged).
Measured on linked save states (a scratch tool): two cabinets 10,686 KB
packed plain, 5,803 KB long, packed as fast (151 ms, 152), unpacked
faster (31 ms, 19); four cabinets (`build/linkstudy/four2/four-8000.state`,
113,756 KB raw) 22,463 KB plain, 6,671 KB long (357 ms, 223 to pack).
Level 6 would save another 5% for half again the time; left at 3. In the
two-window test the state a joiner gets went from 11,990 KB to 5,981 KB,
the join, the music and a resync as before.

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

## Rollback in play (2026-11-02, uncommitted when written)

Step 4, every board on every PC (Mode B; one board a PC comes next). The
owner's choices: the sound plays at once (a frame run again plays nothing:
a rare wrong or doubled sound effect when a guess was wrong, no delay on
the sound); Mode B first.

- `InputTable::guess`: a board reads its inputs for a frame; not in yet
  and fewer than N frames past the last in (`--net-rollback N`), a guess:
  what that player held last (a dropped seat: none, as held). Each guess
  is kept; when that frame comes in different, the table notes the first
  frame guessed wrong. Further ahead than N: waited for, as before.
- `tools::rollback::Linked` (tools/rollback.hpp): each board's state kept
  after every frame by its written pages (`m3::Snapshots`, as the rollback
  test), the last snapshot asked at that frame's end (another state taken
  there would leave no cut); the ring's part cut where the boards meet,
  once all have saved the frame. A guess found wrong: there, every board
  and the ring put back to the frame before it (and each board's state
  where it started is kept too, for a first guess), each board leaving the
  meeting goes on from its own snapshot (`Rewound`), and the frames since
  run again with the inputs now in: drawing nothing (the window's 2D and
  GPU frame, the frame count it shows), playing nothing (the sound
  commands dropped), not paced; the texture RAM then taken afresh by the
  GPU.
- Only frames that won't run again leave the machine: the RAM hashes the
  machines compare (kept until every board's inputs up to that frame are
  in, none guessed wrong, and the board has run it again if it went back),
  the backups, every board's state for a player joining (its frame waited
  for, then saved if none was guessed wrong; else saved when it's run
  again). A linked save state of that frame takes the ring's part as the
  rollback cut it.
- gamerun `--net-rollback N`, `--net-hold-inputs N` (tests: this
  machine's inputs sent N frames late, `Session::hold_inputs`); the window
  `--net-rollback N` (one board off for now). Neither is on by default
  yet.

Proved (gamerun, two processes on localhost, from mash-5990.state, both
players mashing, input delay 0; each process's last run of every frame
compared with a run of both boards in one process):

| held back | rollback | went back (each process) | frames run again | result |
|---|---|---|---|---|
| 3 frames | 6 | 36, 35 | 190, 203 | 2,149 of 2,149 frames on each board, both processes; no hash check differing |
| 6 | 8 | 32, 31 | 265, 263 | the same |
| 4 | 8 | 36, 37 | 230, 246 | the same |

A snapshot 0.45 ms a board, going back 2.3-3.0 ms (both boards and the
ring). A byte poked on one machine is caught by the hash checks (1 on each
side), so they still run with rollback. A join in progress (player 2
leaving at 5,000, a new player joining, both holding inputs back): the
host's state of frame 5,425, the joiner and the host the same every frame
after (5,150 of 5,150). Two windows (`tools/onepc-window-test.ps1
-OneBoard none -Extra "--net-rollback 6"`): the host went back 8 times (24
frames), no hash check differing to frame 7,000.

A silent player and the drop's freeze. Guessed only a few frames ahead, a
silent player still stopped the game for the 3 seconds before the drop.
The owner's choice (2026-11-02): a window longer than that,
`tools::rollback::kWindow` = 210 frames (3.65 s), about 0.4-0.7 MB of
pages a board a frame (80-140 MB a board; memory only, a snapshot costs
the same 0.45 ms a frame however many are kept). A player not heard from
is guessed to hold what they held for 8 frames (`InputTable::kGuessHeld`),
then nothing pressed, which is what the drop makes them: the drop's idle
frames mostly match the guesses, and only the first few frames are run
again. A player who comes back within the window: the frames since run
again once.

Proved: gamerun with `--net-rollback 210`, player 2 gone at 5,000 without
a word (`--net-vanish-at`): the host went back once (7 frames) when the
drop came (idle from 4,998), and a new player joining after agreed with
the host every frame (4,438 of 4,438), the crashed player's own frames
too (10,000); no hash check differing. gamerun runs as fast as it can, so
there it waited 1.3 s at most (210 frames are 1.3 s at its speed). The
window at the game's own speed (`tools/onepc-window-test.ps1 -OneBoard
crash -Extra "--net-rollback 210"`, the joining window killed): the host
ran on at 17.35 ms a frame through the drop, nothing waited for; a gamerun
joining then got every board (frame 5,182) and agreed, no hash check
differing. On the way: the window's state for a joiner waited for every
seat's inputs of its frame, this machine's own too, which it sets after
(`InputTable::wait_all`'s `but`).

Time sync (2026-11-02, uncommitted when written). Without it a machine
with rollback ran on ahead of a slower one (or one stalled) as far as its
window, and each of the other's inputs then took it back that far (202
frames at once in the mixed test below). Now each machine reckons how far
ahead of each other machine it is from the inputs that have come
(`NetPlay::lead`: its own next frame less the other's, half the round trip
allowed; the same input delay assumed), leaving out any not heard from for
300 ms (gone silent: a crash; the game runs on with guesses for them). More
than 4 frames ahead: the window holds that frame a little later (a
twentieth of a frame for each frame ahead beyond, up to half a frame);
more than 8 behind the furthest on: that frame not paced (catching up).
gamerun, which runs as fast as it can, waits while more than 4 ahead.

Measured (two windows on one PC, rollback 210, 7,000 frames): the lead
between them stayed within -7 and +6 frames (without the sync's slowing
growing with the gap: up to 21); the one ahead slowed on 469 frames, none
ran unpaced; 8 rollbacks, 24 frames. gamerun: the mixed test went back once,
1 frame (time sync holding back 590 frames); held inputs (3 frames late,
window 210): 64 times, 233 frames, every frame the same as the reference;
the crash test: 5 times, 13 frames, the silent machine left out. Three
players from power-on (the third joining at 4,600, one machine 3 frames
late): 22,222 of 22,222 board-frames on each, 7 rollbacks each; four
players from four-8000.state (two mashing, two machines 3 frames late):
6,870 of 6,870 on each, 61-143 rollbacks of 3-4 frames, a snapshot 0.88 ms
for the four boards, going back 5-7 ms; no hash check differing anywhere.

Kept sparsely further back (2026-11-02, uncommitted when written): going
far back is rare (a drop, a long stall), so `m3::Snapshots::set_sparse`
keeps every frame for the newest 16 and beyond that only every 8th
(`rollback::kDense`, `kEvery`); a frame no longer kept has its pages merged
into the next kept one's (where both have a page, the older contents), so
going back past it gives the same. Going back then restores the nearest
kept frame at or before the one wanted and runs the few more frames again.
The oldest kept stays as it is (the run's start) until it is out of the
window. A unit test checks every kept frame against a copy, going back
anywhere in the window. gamerun, inputs held 40 frames late, time sync off
(`ROLLBACK_NO_SYNC=1`, rollbacks of up to 40 frames): the same frames as
keeping every one (`ROLLBACK_EVERY_FRAME=1`), every frame the reference's
but the last 40 (the run ending on guesses for them), and the memory 68-93
MB at most over the 210-frame window, where every frame kept was 340 MB
(39-41 frames kept, not 212). A joiner catching up takes no snapshots and
guesses nothing until caught up (every input of those frames is in).

Machines with rollback and machines without play together: each computes
the same game from the same inputs, however it gets there. Proved (gamerun,
power-on, both players joining, input delay 2): a host with rollback
(every board, window 210) and a joiner on one board, delay-based (held
whole until calm, then side runs): the host's boards and the joiner's own
the same as a plain run of both every frame (14,000 and 7,000), no hash
check differing. (The host went back once, 202 frames: gamerun runs as fast
as it can and the host ran far ahead of the slower one-board machine; at
the game's speed that can't build up.) So a slower PC or a tablet on one
board, waiting for inputs as before, can join players with rollback; only
it stutters when inputs are late, and it alone still waits out a silent
player's 3 seconds.

One board a PC with rollback (2026-11-02, uncommitted when written; gamerun
`--net-rollback N` with `--net-one-board`, `--one-board-held` and shadows
from the real boards). The cabinet module's own state (`Cabinets::Kept`:
its remotes, the players' fields by count, the ahead record, the view
tests' registers, the captured state; about 1.8 MB a remote cabinet) is
kept with the staying board's every kept frame and put back with it
(`Linked::attach_extra`); a board on side runs isn't a part of its own
(`Linked::set_active`): its network board is in the staying board's walk,
before the board's own state (`board_state` ends with its check that
nothing follows: after it, every going back during side runs stopped that
board, "save state: longer than this build's"). A switch between side
runs and whole happens only on a frame that won't be run again
(`Settings::settled`: every input in, none guessed wrong): around one
(a shadow asked, a hand-back) nothing is guessed (`Game::may_guess`, the
module's buttons too), a hand-back is asked only where every frame up to
it is in, and a shadow's frame is waited for (`Settings::wait_settled`:
the other machine's inputs for it come with time) and, guessed wrong,
asked again at once for a frame 30 on (the rules wouldn't ask again: what
wanted it is gone by). A board's state sent to another machine
(`NetPlay::take_board_requests`) is of a frame whose inputs are all in; one guessed
wrong is asked again and sent when the frame is run again. A board on
side runs going back re-syncs its turns (`Linked::rewound_to`); the
module isn't torn down while rolling (its kept frames hold its state).

Found on the way:
- The tests' late inputs (`--net-hold-inputs N`, `Session::hold_inputs`)
  held our frames back by count: sent once this machine reached N frames
  on. Around a switch every machine waits for the others' real inputs, so
  each waited for the other's next frame: no one went on. Now by time (N
  sixtieths of a second after each frame is set), as a network's latency.
- Four players from power-on (every player joining in turn, the fourth at
  6,400): the fourth cabinet's machine went wrong 67 frames after its
  hand-back (frame 8,490; with rollback, 3-9 frames after it, then right
  again), with or without rollback. The module keeps each player's fields
  by count (`players_at_`, and the view tests' registers `view_chars_`),
  dropping the oldest only; that cabinet ran alone in attract with its own
  count, higher, then joined the link and took its count: the entries above
  the new count stayed, and when the count got there the record for
  cabinet 2 was made from attract's player (first word 0x40100408, not
  0x0100800D). Now every entry after the count written is dropped. From a
  state at 8,300 (the module new there) it never showed.
- The rollback report's memory figure added every board's kept frames from
  each board's thread (another's changing, or replaced by `set_active`):
  nonsense now and then (23,096,868 MB). Each board's own peak now, added.

Proved (gamerun, power-on, both players joining, every machine
`--one-board-auto --one-board-held --net-one-board 100 --net-rollback 210`,
one machine's inputs 3 frames late): two players, the slave's coin at
6,100 during side runs: 7,209 of 7,209 frames on each, its shadow from the
other machine's state at 6,129 and handed back at 6,428 as without
rollback, 6-8 rollbacks each, 175 MB at most (three runs, and one as if
600 MB were free: 154 frames, 142 MB); three players (the third joining at
4,600): 7,195 of 7,195 on each, 7-9 rollbacks, 270 MB; four (the fourth
joining at 6,400, players 2 and 4 late): 9,526 of 9,526 on each, 10-13
rollbacks, 370 MB, and the same without rollback. `tools/onepc-net-tests.ps1`
(every run) the same as before.

The memory a machine can give it (`rollback::fit_window`): at the start of
a game, the window shortened where the kept frames of every board (about
0.44 MB a board a frame, measured, and a copy of its big buffers, about 32
MB) wouldn't fit in a third of the memory free then
(`m3::memory_free_bytes`: Windows' available physical memory, Linux's and
Android's MemAvailable, macOS's and iOS's free and inactive pages), 30
frames at the least, said in a line. Each machine its own: the window is
only how far its guesses run ahead. Tests: `ROLLBACK_FREE_MB=N`.

Not yet: one board a PC with rollback in the window (it still turns one
board off with rollback); rollback by default and the menu; Auto input
delay with it.

## Joining in progress: the wait (2026-11-02, uncommitted when written)

The owner's report (a Linux window, WSL, joining a Windows window's game):
the player list said JOINING for about two minutes, the window showing an
old picture and taking no keys. A machine joining in progress runs every
board on from the host's state, unpaced, until it reaches the others
(`catching_up`), and only then asks for its cabinet. Measured (two windows,
the joiner coming in at ~5,370, a busy scene): it started 46 frames behind
and never caught up in 1,100 frames: drawing every frame on the way, it ran
no faster than the game. How long it takes doesn't depend on how long the
game has run (the state is ~9 MB either way), only on how far behind it
starts (packing, sending, loading the state) and how much faster than the
game it can run.

- Catching up draws nothing (the 2D, the GPU frame; the textures taken
  afresh after), the sound still played (the music goes on). The same join:
  caught up in 1.0 s at 1.5x the game's speed. The Linux window (WSL): 0.9 s
  at 1.9x, where drawing it ran at about 0.64x.
- A machine still joining sends no backups (`Session::seated`): its first
  backup is the whole board (5-9 MB) on the reliable channel, and its
  cabinet's request waited behind it (taken 196 frames after catching up;
  now 12).
- A loading screen meanwhile (`ui::Menu::set_loading`): JOINING THE GAME,
  how far behind it is; the game's picture hidden until it's real.
- A PC that can't run every board faster than the game even so (not
  gaining 30 frames in 5 seconds) takes its cabinet anyway: the others then
  wait at the cabinet's first frame for it, as for any slow player, once;
  then Auto may take it to one board. (Not tested yet: no machine here is
  that slow without drawing.)

The Linux window joining the Windows window's game now: the state at frame
4,764, caught up 1.0 s later (2.36x), its cabinet at 4,915.

The owner's other report, the joined player's character in the game but
not answering and its timer stopped, isn't network play: two linked boards
in one process with the windows' settings EEPROM and the same buttons give
the same pictures. Player 2 started just as player 1's time ran out (TIME
OVER); its cabinet went into the stage and stayed there, its timer at 90,
while player 1's went to its continue, game over and attract. With the
game's default settings player 1's time hadn't run out by then and player
2 played. Whether the arcade does this or the link's emulation does is
open (docs/fidelity.md).

## Joining over a slow line or a relay, its progress, each link's kind (2026-11-05)

The owner's report: two windows, both through the relay (`SPIKEOUT_TEST_RELAY_ONLY`), the host's list going
JOINING, WAITING, JOINING and the joiner staying in the lobby with nothing shown. The host sent the 1-5 MB state at
once (256 pieces a burst, no pace), heard nothing for 3 s, dropped the joiner, which joined again, the state larger
each time. Tried on one PC with gamerun (`relay-join.ps1` in build\speedcheck\scripts: a host started alone on the
lobby, a joiner by its code, `-Relay both|host|joiner|none`, `-Extra` options) and a new switch,
`SPIKEOUT_TEST_LINK=KB[,LOSS]` (this machine's uplink as a slow line: KB kilobytes a second, a second's queue at most,
LOSS percent lost; with `SPIKEOUT_TEST_LATENCY`). Found and fixed, in order:

- **No pace.** Each player's reliable channel now has one (`Impl::pace`, a `Cap` as the spectators'): from 1 MB/s (8
  MB/s to an address on the same network), up a quarter each busy second, halved when more than 5% of the pieces
  had to be sent again, or when a player's round trip rises 40 ms above its least (the uplink filling delays the
  inputs, which come first; the joiner itself left out while it takes in the state). Pieces are sent again after
  twice the round trip and 0.1 s (not 150 ms: a far one's, or one queued on a full line, isn't lost yet). Inputs
  aren't on the reliable channel and aren't paced.
- **Cumulative acknowledgements only**: one lost piece made its whole window come again, which the pace took for
  heavy loss (down to its 32 KB/s floor on a 300 KB/s line). Each acknowledgement now carries which of the 64 pieces
  after the gap have come (`Reliable::sack_bits`, `acked(ack, sack)`), and only the missing are sent again.
- **A joiner's inputs thrown away** until it knew its cabinet's first frame (`Session::local_input`): with an
  input delay and a far host, the first frames that turned out ours were made before kTaken came and dropped, so
  every machine waited for them for good (both stuck at the cabinet's first frame). Kept now, the ones before it
  dropped when it comes (`Impl::taken`). Possibly behind the earlier "buttons dead after joining".
- **Backups to a joiner**: every player sends its board's backups to the others (one board a PC), a newcomer the
  whole (1.5-3 MB); the joiner's kTaken waited behind them in its reliable channel (the host waited 38 s at the
  cabinet's first frame). None to a player still joining now (`Session::joining`; its base forgotten, so the first
  after is whole).
- A joiner still taking in the state is dropped after 15 s unheard, not 3.

Measured (two gamerun, 300 KB/s each way, 3% lost, 60 ms each way, `--input-delay 8`): before, never got in (or got
in and stalled for good); now joined, the cabinet taken, no hash check differing, the longest wait 11 s (gamerun
takes its cabinet before catching up, as its comment says; the window catches up first). Windows on that line: the
4.1 MB state at ~95 KB/s; on this PC's network, the 4.2 MB state, caught up and the cabinet taken within seconds.

**The progress shown** (the owner's design from mockups): the joiner's loading screen covers the whole join now,
not only the catching up: JOINING ROOM CODE (or THE GAME on a LAN), the host, the players and the link (ON THIS
NETWORK, DIRECT, THROUGH THE RELAY), the bar with "Receiving the game: 1.8 of 4.3 MB" (`Session::join_progress`:
the host sends the state's size first, kStateSize), then catching up, then taking the cabinet, the steps along the
bottom (JOINED > RECEIVING THE GAME > CATCHING UP > YOUR CABINET); the room page closes once for it (Esc opens the
menu over it). The host's list: RECEIVING 42% (the state's pieces it has acknowledged) then CATCHING UP 80% (the
joiner's status, `Status::catch_up`, by fives), with a small bar (`NetRow::progress`).

**Each player's link** (the owner's ask): the seat list's tag starts with LAN, DIRECT or RELAY
(`SeatView::link`: through our relay or theirs; else an address on a network of its own, RFC 1918, link-local or
this PC; else straight over the internet). Seen: LAN (windows on this PC). DIRECT and RELAY not yet on screen.

**What one PC can't test**: a machine straight to another's relay. Both "machines" have this PC's one internet
address, and data sent to a relay from it never came through (the permission for that address granted; no Data
indication ever arrived): very likely the relay refusing a peer at its client's own address. Relay to relay goes
through (the windows, both relay-only, got the state moving), though headless runs of it were unreliable. A second
network is needed for those (a VPN would give one, on a second device).

Debugging (`SPIKEOUT_NET_DEBUG=1`, stderr): each 5 s the bytes sent by datagram type, every seat's inputs known,
each peer's queue, pace, round trip and first piece; big reliable messages queued (type, size); the TURN client's
answers and permissions; the first 40 STUN/TURN datagrams in.

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

### The camera events over the whole game, one player (2026-10-29)

The bot's run to the end (tools/spikebot.cpp, `--record-inputs`; docs/
bot.md) replayed by gamerun (`--load-state S --inputs FILE`: the bot's
buttons and RAM writes each frame, its RAM hash every 60 frames checked;
4 segments, every hash matched) with `--camera-events`. Segments (states
and inputs git-ignored, in build/bot and build/linkstudy): stage 1
(build/bot/bot-3500.state, build/linkstudy/stage1/inputs.bin, to frame
50904), seg2 (build/bot/fullrec2/owner-50707.state, seg2/inputs.bin, to
54639), seg3 (fullrec3/owner-62721.state, seg3/inputs.bin, to 72089), seg4
(fullrec4/owner-72989.state, seg4/inputs.bin, to 198905: the last stages,
the final boss, the ending). Between them the owner played (no inputs).

Three kinds of camera event, for one board per PC:
- **A. Stage script commands** (shared: the script runs the same on every
  cabinet; copied to the remote camera). The interpreter (0x5A980, a task:
  the task's +16 the current area's script, in the program image in RAM,
  walked from its start each frame) reads records: byte 0 the type (0 the
  end, 1 a condition, 2 an action), byte 1 the command, byte 2 the
  record's length. Conditions' handlers are at 0x8BA60 (22), actions' at
  0x8BAB8 (86). 11 actions write into the camera. Seen (the writing block,
  the action): 0x05 (0x5C73C; block 0x5C888, +0xB88/+0xB8A), 0x0A
  (0x5D21C, the gate's mode: +0x48, +0xC8, +0x514), 0x24 (0x5DB54,
  +0x400), 0x2B (0x5DE28, +0x154), 0x2D (0x5E004; 0x5E048,
  +0xAD4-0xAF4), 0x39 (0x5E230; 0x5E28C and 0x5E2B8,
  +0xB00/+0xB02/+0xB74), 0x40 (0x5E520, +0xB70/+0xB72), 0x4A (0x5E538;
  0x5E550, +0xB80-0xB86). **Never seen yet:** 0x09 (0x5D20C: +0x840),
  0x18 (0x5D81C: +0x400, the position +0x10/+0x14/+0x18, +0x28/+0x2A) and
  0x3F (0x5E48C: +0x404-0x40C and +0x10-0x18, floats, +0x28/+0x2A).
  Where the scripts use them (a scan of the program image for action
  records, each starting a chain of records that ends): 0x09 nowhere (no
  script uses it); 0x18 (4 bytes, no argument) at 0xAEA84, 0xAEE54,
  0xBFC70, 0xC00FC, 0xCC08C, 0xD31C4, 0xD3660; 0x3F (8 bytes: a pointer)
  at 0xB2A64 (0xB342C) and 0xB4EA0 (0xB5934). Which areas' scripts ran
  when (the task's +16, `--peek 11BA8C:8@FRAME:N` over the bot's run;
  0xAF288 between areas): 0xD44EC (stage 1's start, frame 3501), 0xD538C,
  0xD5C44, 0xD6476, 0xAEE28 (19609), 0xAE6C8, 0xA83DC, 0xA9238 (50708),
  0xAADD4, 0xAF85C, 0xD31A4 (66225), 0xCCDCC, 0xCEB8C, 0xCF724, 0xD0714,
  0xD1DBC, 0xD3DF4, 0xBFC50 (102655), 0xB2C08, 0xB7670, 0xB82DC, 0xB4800
  (123472), 0xBBAC8, 0xB3E84, 0xBC1F0, 0xBCE20, 0xC0C29, 0xCC06C
  (151913), 0xC367C, 0xC4888, 0xC7340, 0xC7688, 0xC8F60, 0xC9D84 (the
  final boss, 191314), 0xCC650, 0xCA878 (the ending, 195746). So 0x18 was
  seen after all: its handler only calls 0x37258 then 0x372D4 (the
  report names the writing block, not its caller), which wrote the camera
  the frame before the scripts holding 0x18 started (0xAEE28: 19608,
  0xD31A4: 66224, 0xBFC50: 102654, 0xCC06C: 151912). 0x37258 and 0x372D4
  are called from other code too: to sort them, the caller (LR) of each
  write is needed, a script's meaning shared. Each record's own script
  (its chain of records walked from a start in the table below): 0x3F's
  0xB2A64 in 0xB2A3C's, 0xB4EA0 in 0xB4E78's; 0x18's 0xAEA84 in 0xAEA58's,
  0xC00FC in 0xC00DC's, 0xD3660 in 0xD3640's: none of them a script the
  interpreter task above ran. Every script's start is in one table,
  0xA1D2C-0xA2028 (in twos and more: the area scripts above with the
  script before each, perhaps its set-up, which the task's +16 never
  showed; perhaps run by another task, or within a frame). Not yet known:
  which of the table's scripts are areas the bot never entered (the hidden
  route) and which are run some other way. Ways on: an observer hook at
  the action dispatch (0x5AB30: r31 the record, byte 1 the action) logs
  every action run, the definitive "seen"; the conditions (type 1 records)
  before an action say what makes it run; what writes the task's +16 from
  the table says which gate leads to which area.
  **Done (2026-10-30): `gamerun --script-actions`** (an observer hook at
  the dispatch's block, 0x5AB18; every record run, by its address, with its
  action, runs and first frame). Over the bot's recorded run (the four
  segments' `--inputs` replays, every RAM hash matched with the hook in;
  stage 1's inputs end at frame 23006, not 50904; seg2 logged none) the
  camera actions run: 0x05, 0x0A, 0x2D in every stage; 0x18 at 4 of its 7
  records (0xAEE54, 0xD31C4, 0xBFC70, 0xCC08C); 0x24, 0x2B, 0x39, 0x4A;
  0x40 only at the final boss (0xC92A0, 0xC94B8, 0xC9F7C, 0xCA204,
  0xCAB5C). Never: 0x09 (in no script) and 0x3F (both records in scripts
  this run's areas never were). So 0x18's other three and 0x3F are on the
  route not taken (the gates' other branches): runs through those would
  see them.
  **The sewer route (2026-10-30):** the bot broke the unmarked gate after
  Victor (spikebot `--unmarked-gate`, docs/bot.md "Gates, and the unmarked
  ones"; from `build/bot/fullrec/bot-31704.state`, recorded in
  `build/bot/sewer2`): Clint's concrete channels, three bosses at once
  (48504; Huey, Dewey and Louie by the guides), the two-arrow area
  (0xAB820 ran), back on the arrow route's walkways and the area at 120 45,
  then the story scene to a new stage at frame 59006 (the guides: stage 4,
  stage 3 skipped). Replayed with `--script-actions` (490 of 490 hashes):
  **0x18 at 0xD3660** ran (frame 59006, the stage change's script 0xD3640;
  the arrow route's change to stage 3 runs 0xD31C4).
  **The two-arrow area's other gate** (0xAB844's 0x07/0x08, where the
  sewer route broke 0x2B; `build/bot/branch-ab` from sewer2's 50916): two
  stage 2 areas the arrow route never has (0xAC45C, 0xACE9C), the
  basketball court (stage 2's last boss there; 0xAD030 ran: 0xAD054's
  area), the bonus round, then the mall stage (the guides' Astro Mall;
  Rikiya). Replayed to frame 87222 with `--script-actions` (605 of 605
  hashes): **0x3F at 0xB2A64** ran (frame 66459, the change to the mall:
  the set-up script 0xB2A3C), 0x18 at 0xBFC70 (seen before).
  **Route bit 0x0400** (found by reading the scripts, docs/bot.md "The
  route, read from the scripts": area 15's unmarked gate 0x0B/0x0C, broken
  from branch-ab's 61116; `build/bot/route57`, then `route57b` from 63520):
  stage change 57 then "Stage 3D, Astro Mall" (the branch's was 3C).
  Replayed (110 of 110 hashes): **0x18 at 0xC00FC** (frame 66011, the
  change's script 0xC00DC) and **0x3F at 0xB4EA0** (66462, area 24's set-up
  0xB4E78). Both 0x3F records now seen, and three of 0x18's four unseen;
  left: 0x18 at 0xAEA84 (area 22's script, which no go-to-area record
  names) and 0x09 (in no script).
  **Neither can run, by the code (2026-10-30).** An area starts from the
  byte -16365(r13); every store to it, and to the bytes feeding it
  (-7558, -16265), in the recompiled code: actions 0x00, 0x35/0x36 and
  0x01 (constants in the scripts: none is 22), the game flow 0x4BB74
  (starts what action 0x01 set), the area task's save and restore
  (0x72860 / 0x5A388: the area it was in, for a cabinet joining), a
  joining cabinet's copy from another (0x7327C, 0x60B04) and the stage
  ends' table (0x60A40: areas 83, 84, 85, 88 to their next stages). The
  area table lookup (0x5A94C) has those two callers only, and nothing
  points into area 22's scripts but the table itself. So area 22
  (0xAE9EC/0xAEA58) is never started: left-over content. Action 0x09
  (0x5D20C: the camera's +0x840 = 0) is called by nothing but the action
  table, and no record in the program image uses it (the one 0x0209
  pattern in all of RAM, at 0x4E6814, is in 3D data, not a script). Every
  camera action that can run has been seen.
- **B. The camera's own code** (run for the remote player: proven exact,
  `--camera-test`). 0xBE14 (+0x4A8-0x4BF, every frame) is each board's own.
- **C. Other code**, to sort as shared or that player's own: 0x32A80
  (+0xAC6), 0x33490 (+0xAC8-0xAE8), 0x345EC (+0xD2-0xD4: per player, from
  the gate run), 0x37258 (the position, +0x400), 0x372D4 (+0x48, +0x4A,
  +0x400, +0xAC6), 0x7682C (+0xB10-0xB1A); only at the final boss 0x33078
  (+0xB72, frames 191313-193046); only at the ending 0x2C480 (frame
  198414: +0x4B, +0xD1-0xD4, +0x113-0x128, +0x400), 0x69E30 (+0x48,
  +0xC8) and 0x6C01C (+0xB80). The static scan's other writers (about 40
  functions) weren't seen in one player's run. Sorting them needs
  two-player runs over the whole game (`--camera-events` with two boards
  compares each writer's writes by frame, offset and value): the bot
  driving two boards.

**Kind C sorted by reading the code (2026-10-30).** `--camera-events` now
gives each writing block's caller (LR: `00033490 (from 00033228)`), and the
writing block is often not the writer: the report names the last block run,
so a store made just after a call returns is put on the callee. 0x37258 and
0x372D4 are the game's register save and restore helpers (they write only
the stack); the LR names the code that really wrote. Every writer seen over
the bot's game, with its caller, read in the recompiled code:
- **The camera's own code** (r12 the camera; called from the function at
  0x32990 after the part that `--camera-test` times as the update, so
  outside its window and counted here; to be run for the remote player
  with the rest, not copied; not yet checked that `--camera-test` does):
  0xBE14 and 0x4584 from 0x32514/0x32540 (0x324C4: the camera's points
  relative to 0x1A0000, turned by its heading, +0x4A8-0x4BC); 0x32A80
  (0x32A44, called from the update at 0x32A1C: counts +0xAC6 down while
  the flags word's 0x02000000 is set); 0x33490 from 0x33228 (0x33164,
  called at 0x329F8: +0xAC8-0xAD0 eased toward the local player,
  -16348(r13)); 0x33078 (0x32E78, called at 0x32A10: a counter at +0xB72, the final boss);
  0x2C480 from 0x324DC (one of the camera's mode handlers, through the
  table at 0x91B04, chosen at 0x324C4; the ending's mode: +0x4B,
  +0xD1-0xD4, +0x113-0x128, +0x400).
- **Shared** (the same on every cabinet; to copy): every script action,
  through either interpreter (0x5AB18's dispatch, or 0x5A824's at 0x5A874,
  which runs a script through within a frame: the "set-up" scripts the
  area task never showed may be these; `--script-actions` now hooks both);
  0x345EC called from 0x5CA84 (inside action 0x05's handler, 0x5C73C, every
  time it runs); "0x372D4" from 0x1D8CC and 0x1DC04, really 0x1D894 (in
  0x1D734: a sequence run off the area object's counter, 0x10C13C +674,
  with sounds and a 2D message; at count 13 it sets the flags word's
  0x02000000 and +0xAC6 = 240) and 0x1DB94 (its end: both cleared); the gate
  run found these the same on both boards. "0x37258" from 0x5D824 (action
  0x18).
- **The player's own** (to make for the remote player): 0x345EC called from
  0x260CC (a character's routine, only when the character is the local
  player, -16348(r13), and its +0x1E68 byte is set: a shake, +0xD2-0xD4,
  from the table at 0x90114); the gate run had it 18 times on board 0 and 3
  on board 1. Also 0x345EC from 0x21CD4 (when the character, or the one at
  its +272, is the local player) and from 0x22770 (shake 7, when the
  character's +114 is the player the camera follows, the camera's +0x4A).
  And the cabinet's own game flow (the state machine at 0x49384, its mode
  byte -16374(r13)): the ending, 0x4BE54 (the camera's mode +0x48 = 10,
  +0x4A = 0, +0x400 = 2: "0x372D4" from 0x4BEB0) then 0x4BEE4 each frame
  running a cut scene's timeline, 0x69D84 (0x69E30 from 0x4BF88: a frame
  counter against a table setting +0x48 and +0xC8), skipped by the
  cabinet's own button held 40 frames (-32512(r13)), so each cabinet's
  though the cabinets reach it together; and the game over: the continue
  screen and then the ranking's name entry keep their working values in
  the camera's +0xB04-0xB1E (0x7682C from 0x4B68C, where the continue
  countdown, -7523(r13), reaches 639, and from 0x4BDD4 after the ending:
  the local player's score, +16136, and character; 0x76F78, 0x76868 and
  0x76944 from 0x4BFCC/0x4BFD0; it reads the cabinet's own buttons,
  -32508(r13), and the backup RAM's ranking), then the cabinet's reset
  (0x4942C: +0x840 = 0, called at 0x494CC after 0x51FAC; 0x6C01C from
  0x6BB28, 0x6BAA0 setting the effects up again: +0xB80 and flags). These
  last are the remote cabinet's flow, not its player's view in play: for
  one board a PC, the remote camera is needed while its player plays, and
  what the remote cabinet shows after its game over is its own business.

The local player's pointer, -16348(r13) (0x104024), is 0x1A0000 on board 0
and 0x1A4000 on board 1 (checked in a two-player state): code that tests
it, or reads the local player through it, is each cabinet's own.

Checked on two boards (2026-10-30): the bot's two-player run
(build/bot/two6, docs/bot.md) to stage 2's fence gate, then from its state
at frame 27865 nobody pressing (gamerun --boards 2 --parallel
--camera-events --script-actions, 13,450 frames): player 2's game ended
first (board 1 back in attract mode by 30000), then player 1's (the
continue countdown at 30000). 0x7682C wrote on board 0 only (frame 29961,
its continue screen), 0x33490 too (board 0's camera easing toward its
player, until 29761), "0x372D4" from 0x1DC04 too (29770); board 1 alone set
up its attract mode (0x4A1B0 from 0x496A0, +0xB00). The rest, from ~31700,
is both cabinets in attract mode running the same demo (0x69E30, 0x6AD9C,
0x2C480, the +0xB20-0xB30 writers, even 0x345EC from 0x260D0, the demo's
player being the same on both): the same on both boards. No script action
ran in that time.

Both players fighting (2026-10-30): spikebot --boards 2 from
build/bot/two-start.state without --idle-others (the bot playing both, in
turn), 3,000 frames of stage 1, recorded (--record-inputs: linked boards'
now, docs/bot.md) and replayed with gamerun --boards 2 --parallel
--load-state build/bot/two-start.state --inputs build/bot/bothfight/
inputs.bin --camera-events (every hash on both boards matched, 50 and 50).
0x345EC from 0x260D0 shook board 1 alone at frame 6657 (player 2's charge
attack) and board 0 alone at 7414 (player 1's sweep): "each board's own
(none the same)". A player's shake runs on that player's cabinet only, as
the code says (the local player test), so for one board a PC the remote
player's camera needs its own. The rest was as before: the script's
actions (0x5D21C, 0x5E048, 0x5E550, 0x345EC from 0x5CA8C) and the
area-start sequence shared, 0x33490 each camera's own easing. 0x21CD4 and
0x22770 didn't come up in these 3,000 frames.

With both dispatches hooked, stage 1's replay (every hash matched, so the
new hook changes nothing) runs 51 action kinds, 394 records (from 49, 290):
the second interpreter runs records at 0xA82DC-0xA88F4 and others among
the area scripts' (0xAE5F8, 0xD47C4, ...); two kinds only there, 0x29 and
0x31; of the camera actions 0x05 (0xA84A8) and 0x0A (0xA84E0) more often.
Still no 0x3F, and 0x18 no more than before (lists in
build/linkstudy/actions: stage1.txt before, stage1-both.txt after).

Four boards (2026-10-30): gamerun --boards 4 --parallel from power-on (the
link setting 1 on board 0, 2 on the other three: 3 and 4 are not slaves,
those boards stop at "TWO PROCESSORS DETECTED"), the players joining one
by one (presses as the two-board command's, boards 2 and 3 at 4200-5250
and 5800-6850), all four in one game by frame ~8000 (THE NEW TEAM, then
Spike, White, Linda, Tenshin together), then nobody pressing until every
game ended and the cabinets were back in attract mode (state:
build/linkstudy/four2/four-8000.state). With --camera-events:
- the area-start sequence (0x1D894, from 0x1D8CC) runs on every cabinet in
  the game each time a player comes in (board 0 at 2948, 4573, 6182, 7792;
  board 3 only at 7792, its entry): shared, as sorted above;
- its end, 0x1DB94 (from 0x1DC04), also comes at each cabinet's own game
  over (11910, 12717, 13443, 14163): one of its two callers is the
  cabinet's own flow;
- the continue screen (0x7682C) at each cabinet's own player's end
  (12101-14355); the rest new here (0x4B428 from 0x4B410, 0x5197C from
  0x4A30C, 0x51FE8 from 0x4B378, "0x372D4" from 0x4A4E0) is in the game
  flow's code (0x49384-0x4C020), each cabinet's continue and game over;
- after ~15000, attract mode on all four (the shakes from 0x21CDC and
  0x260D0, 0x69E30, the +0xB20 writers). These read "none the same" though
  they come at the same moments (the boards' frames one apart):
  --camera-events matches writes by the game's logic counter (0x1000A8),
  and with every game over each cabinet's counter goes its own way (not
  checked further); in the two-board check, the same demo writes matched.
Nothing camera-related that depends on there being more than two players
turned up; the players' own shakes while several play are still to see
(the bot's two-player run).

## One board a PC: the stand-in, step by step (2026-10-30)

**A remote cabinet is its network board and one block a frame.** From a
linked state at frame 4700 (both players in, `build/onepc/fight-4700.state`)
to 5800, both pressing, board 1 recorded (`--link-record`) and then stood in
for (`--link-standin`); board 0's main RAM every frame and whole state every
100 against the recording run, 3790 lines:
- the stand-in as before (every call replayed): all the same;
- `--link-standin-no-reads` (the main board's reads left out: they change
  nothing on the network board, `CommBoard::io_read` const): all the same;
- and `--link-standin-coarse` (its own syncs left out too: the network
  board brought to the time only at the slices' meetings and frames' ends):
  all the same;
- and `--link-standin-at 2400000` (the block written in one burst 2.4M
  cycles after each frame's end): all the same.

From power-on (the linking repro, board 1's calls recorded to frame 5690,
a scratch tool over them): after start-up (the memory tests and the
firmware loaded, frames 17-700) the main board writes nothing to its
network board but the block (576 32-bit writes a frame); a few stretches
have none (loading: 2300-2399, ~4200).

**Where the block comes from (main RAM; the network board's view of each
32-bit word byte-swapped against the main board's).** The cabinet's live
block: a header at 0x140100 (0x100 bytes) and its record at 0x140200
(0x200). The link code (0x6EE68 / 0x6EEAC, memcpy 0x4E88) copies them into
its outgoing block at 0x141000 (the header, then four records 0x141100,
0x141300, 0x141500, 0x141700, one a frame in turn), which goes to the
network board; what came in is copied from its shared RAM (0xC0000D00 on)
to 0x142000-0x142FFF (0x6EFA0) and on (0x6F040, 0x6F11C). The bytes that
differ between two cabinets in the same game (both blocks dumped with
`--link-dump` over the fight, records paired by their logic counter at
+0xDC, 3671 pairs), in the sent order, and in RAM:

| sent | the record in RAM (0x140200 +) | written by |
|---|---|---|
| +0x05 | the word at +0x04 (the buttons) | 0x6DA90 (from 0x6D928) |
| +0x08-0x0B | the halfwords at +0x08, +0x0A (the camera's heading) | 0x550CC (the player's update, 0x51824) |
| +0x0E-0x0F | the halfword at +0x0C | 0x550CC |
| +0x11-0x13 | the word at +0x10 (flags) | 0x6DA90 |
| +0x34-0x37, +0x40-0x43 | the float of the 12-byte character entries at +0x2C and +0x38 | 0x74808 / 0x74884 (from 0x742CC) |
| +0x44, +0x5C, +0x68, +0x74 | the flags word's low byte of the entries at +0x44, +0x5C, +0x68, +0x74 (the on-screen bits, 0x08 / 0x10) | 0x74884 |

The character list: 12-byte entries from +0x14 (a flags word, four bytes,
a float). The header's own bytes (sent +0x000-0x001, +0x014-0x015,
+0x01C-0x01D, +0x020, +0x025-0x026, +0x042-0x047, +0x04C-0x04D,
+0x058-0x059): counters at its +0x00, +0x1C (the logic counter), +0x58
(written by 0x6D9C4), the rest each cabinet's constants. Next: each
field's source on the other board (the side camera for the heading and
the on-screen bits, board 0's copy of the remote player for the rest),
then gamerun making the remote's block itself, checked against these.

**Better: the remote PC sends its block (2026-10-30).** With one board a
PC, the remote cabinet's board runs on the remote PC, which has its block
exactly: no need to make it here. Its per-cabinet bytes are its own
buttons word (0x100100, 0x6DA90 writes it into +0x04), its player's
object's first word (0x1A0000 + 0x4000 a player, +0x10), its camera and
view (the player's update, the character list) and its counters; all of
it is in the block its main board writes. Each frame's 0x900 bytes XORed
with the frame before change by 26 bytes on average (both boards over the
fight; packed alone by zstd, 82 bytes a frame, at most 108: 4.6 KB/s; a
plain byte-diff would be less). With an input delay (as Mode B has) it
arrives in time; making it here (the side camera, board 0's copy of the
remote player) is only for guessing it under rollback, later.

`--link-standin-blocks` proves it: the stand-in takes from the recorded
calls only each frame's writes (as the remote PC would send them; the
frames counted by their ends) and keeps its own time: the network board
brought to each slice's meeting at its nominal cycle (display lines 7,
15, ... 383: frame start + 40 lines of VBlank + the line's) and to each
frame's end, the block written 2.4M cycles after the end of the frame
before it (`--link-standin-at`), frames without one (loading) skipped. All
board 0's lines the same as the recording run's:
- the fight (from fight-4700.state, 1100 frames): 3790 of 3790 (once the
  stand-in also ran the meetings of the frame the log stops in: without,
  only the last whole-state line differed, packets left on their way);
- the bot's two-player run (`build/bot/tworec`, its linked inputs, both
  boards at once): stage 1's last area, its boss, the story scene and the
  change into stage 2 (from bot-19730.state, 2997 frames, 2868 blocks: 129
  frames loading with none): 3030 of 3030; a boss, the gate called (29260)
  and broken (29757) and the next area (from bot-28449.state, 2689
  frames): 2760 of 2760. (From bot-28449 both runs part from the bot's
  recording alike at 31020: a replay matter, not the stand-in's.)
- standing in for the master (board 0; board 1 checked, the fight):
  3705 of 3705: either cabinet can be the one stood in for;
- from power-on (the linking repro, `--clock 1800000000`, to frame 4926):
  5014 of 5014, through the start-up's writes (the memory tests, the
  network board's firmware, its start: up to ~2,500 writes a frame, all
  written in the frame's one burst), attract, player 1's start, and player
  2 joining on the stood-in cabinet (its coin, start and select carried by
  its writes alone). Without `--clock` the whole-state lines differed
  every 100 frames while RAM, the CPU, the frame timing and the network
  board (`GAMERUN_STATE_PARTS=1`) were the same: the real-time clock, set
  from the PC's time at each run's start.

So a PC can run its own board and stand in for every other with what that
board's PC sends: each frame, its main board's writes to its network
board (its block in play; more at start-up).

**A dropped cabinet, its blocks made here (2026-10-30).** The owner wants
every feature with one board a PC: a player who drops stands idle. With
no PC running that cabinet, every other PC makes its block from what they
all have alike. `gamerun --drop B@FRAME`: board B's main board stops at
the end of FRAME (its PC gone); from there its network board is fed made
blocks (`made_blocks`), at the stand-in's times:
- the header: board 0's, with B's own bytes as its last block had them,
  and its three counters (+0x00, +0x1C the logic count, +0x58) one up a
  frame;
- the next record, in its slots' turn: **board 0's record for the logic
  count B is at.** A slave runs a frame behind the master (board 1's +0x1C
  one less than board 0's: 0x0DBD against 0x0DBE at the fight's start), so
  that is board 0's record from the frame before, still in its slots.
  Taking board 0's newest instead gave "Communication error!! Please call
  service staff (Sync_sys_broken)" within a frame;
- in it, B's own fields: no buttons (+0x04); from board 0's copy of B's
  player a frame before (GAMERUN_ONEPC_CHECK measured it: all 3500 frames
  of the fight): its facing (+0x08, the player's +0x2A), +0x0A (its +0x76
  when its +0x44's top bit is set, else +0x74; 0x550CC) and its first word
  (+0x10; 0x6DA90); held as B's last record had them: the camera's heading
  (+0x0C), the empty slots' leftovers (+0x34-0x37, +0x40-0x43, constant)
  and the on-screen bits (+0x47, +0x5F, +0x6B, +0x77).

From the fight state, board 1 dropped at 4800, player 2 idle from then:
the game goes on (board 0 to frame 8452 as without the drop, no link
error), player 2 standing and being hit ("HELP!!"). Against the run where
board 1 ran on idle (both dumped, `--link-dump`, by data frame): the first
three made blocks differ from the real ones only in the camera's heading
(record +0x0E); over 400 frames the heading (+0x0E/0x0F), one character's
on-screen bits (+0x068, 20 frames) and a few flag bits (+0x11-0x13, the
game parting from the other run once the views differ). Every PC makes
the same blocks (only the shared game goes into them), so the PCs stay in
step; to be exactly what an idle cabinet sends, the dropped player's
camera would run on the side (`--camera-test`'s), from its camera (0xC00
bytes) as its PC last sent it.

**An empty seat (2026-10-30).** A cabinet nobody has taken waits in
attract mode. From power-on (`--clock`, the link pokes, board 0's player
starting at 1200, board 1 left alone; both dumped): over frames 1800-2350
board 1's newest record never changed; its header changed in its three
counters (one up a frame) and, a few times, at +0x10 (2), +0x14 (4),
+0x15 (1) and +0x25 (1). So its blocks can be made (held, the counters
run on). But the waiting cabinet's board follows the others' game: its
RAM against a run where nobody plays parts at frame 1202, just after board
0's coin. So a board for a player taking that seat can't be made exactly
from nothing; to try: a waiting board from the start (after its start-up)
brought to the present (its counters and time moved on to what its made
blocks said), then let run, joining as a cabinet does (coin, start).

**Taking an empty seat works (2026-10-30).** `gamerun --wait-seat
B@FROM:UNTIL`: board B stopped at its first block after FROM's end (a
clean point); its network board fed its last block held, the header's
three counters one up a frame, until UNTIL starts; then the board brought
to the present (`FrameTiming::move_on`, the CPU's cycles moved on with the
time base held, the RAM its counters come from, 0x1000A4, 0x1000A8 and
0x140048, set to go on from the held blocks, each as far ahead of them as
it had been) and run on. From power-on (`--clock`, the link pokes, board
0's player in at 1200), board 1 stopped at 1100 and back at 3000 (1900
frames held), its player's coin at 3200, start 3300, select 3900/4250:
board 0 shows "HERE COMES A NEW PLAYER." (4400) and both players fight
the boss by 5200, no link error. So an empty seat needs nobody running
it: its blocks made, and for whoever takes it a waiting board from the
start (every cabinet boots together; kept by the host) brought to the
present.

**Taking a seat in the game (a dropped player's), from another board's
state (2026-10-30).** The two boards' RAM in the same fight differs in
~575,000 bytes, mostly what each draws (the 3D display lists at
0x54A000-0x5DB000, the 2D layers, render buffers), remade every frame.
`gamerun --clone-seat B@FRAME`: board 0's state at its clean point after
FRAME-1 (a slave's world is the master's a frame back: taking FRAME's gave
"Communication error!! ... (code: 0x... 1)"), loaded into board B at its
clean point after FRAME, B keeping its own network board, EEPROM, link
buffers (0x140000-0x143000) and counters, its time moved on the frame, and
told which cabinet it is: the local player's object (0x104024), its
number (0x10065B), the link role (0x10019E = 2, a slave), the player its
camera follows (camera +0x4A). On a live board 1 mid-fight (cloned at
4950): both boards on at full speed to 6890, no link error; board 1's
screen shows White as its own player, Spike as the other, its camera on
White, the picture at the end almost the same as without the clone (board
0's lines part from frame 4953: the clone isn't board 1 to the byte).
The cabinet's own TIME showed board 0's player's at first (64 where board
1 shows 77): the players' times are a shared table, 1/64ths, a halfword a
player at -7504(r13) (0x1062B0), and each cabinet shows its own through a
pointer to its entry, -8184(r13) (0x106008 = 0x1062AE + 2 x its player's
number), set with the player's object pointer at 0x11C3C. Set too, board
1 shows 77, its picture at 6000 as without the clone.

Not the same board to the byte (board 1's lines part a frame after the
clone, at 4951): the board a rejoining player takes has no "real" one to
match. What must hold is that it agrees with the others on the shared
game, and it does: its sent records paired with board 0's by logic count
(2191 pairs, `--link-dump` and the pairing above) differ only at the bytes
two real cabinets differ at (the buttons, the camera's, the on-screen
bits, the counters and constants), none other.

**So, with one board a PC, every seat can be had:** a dropped player's
cabinet made by every PC alike (its character standing idle); an empty
seat made, and taken with a waiting board from the start brought to the
present; a seat in the game taken with a playing board's state as that
cabinet (its own network board, EEPROM, link buffers and counters, and
which cabinet it is: 0x104024, 0x10065B, 0x106008, 0x10019E, camera
+0x4A), a frame behind the master's. All in gamerun (`--drop`,
`--wait-seat`, `--clone-seat`).

**But a sent block comes too late over the internet (2026-10-30).** The
link wants each cabinet's data from exactly one frame before ("Sync_sys_
broken" otherwise): a cabinet's block, written about 0.68 of a frame in,
goes out early in the next frame (it may be written as late as 0.97 of
the frame, measured 2026-10-28), so a PC needs every other cabinet's block
within about a frame (17.4 ms) of its PC making it. The stand-in proofs
ran every board in one process, where it always was. Over the internet
(round trips of 30-100 ms) it can't be, and an input delay doesn't help:
it delays buttons, not the link, which is the emulated hardware's own
timing. So with one board a PC each PC **makes** every other cabinet's
block, as it makes a dropped player's, and only the buttons go between
them (as Mode B now, the same input delay). In play everything in it is
known here: the buttons (sent ahead, the input delay), the player's
fields (board 0's copy of that player a frame back: all 3500 frames), the
counters, the leftovers, and the camera's heading and on-screen bits from
that player's camera run on the side (`--camera-test`'s; 2026-10-28 made a
remote's block that way byte for byte over 5000 frames). The blocks the
PCs send then only check the made ones (and put them right when they
weren't). Open: a cabinet outside the shared game (its attract, its
select, its continue) runs its own code, not made from the shared game;
whether a block sent late will do there (the others may need only its
counters) is to try.

**Made blocks exact in play (2026-10-30, gamerun `--drop B@F
--camera-test 0:CAM --camera-test-free`).** Board 0 hash for hash the
same as the two-board run, with board 1's PC gone and its blocks made from
its buttons alone: an idle player (1,272 frames), a moving one (1,272),
and both players mashing every button in a fight for 5,327 frames (364
presses), every made block byte for byte the real one's. What it took:
- Its buttons word (+0x04) is what 0x5094 makes of the I/O board each
  frame (held: `~(port 0x04 select 0 | 0x08 << 8 | 0x0C << 16 | 0x04
  select 1 << 24)`, the EEPROM's line idle; 0x100104 pressed, 0x100108
  released): from its raw inputs, which the PCs send ahead (the input
  delay), so every PC makes the same word for the same logic count.
- Its player's fields as board 0 has them when board 0 writes its own
  record of that logic count, at the same point of the code (a hook at
  0x6DA90, where a record is written); copied at a time instead, a flag
  set and cleared within a frame (0x81100000 at 0x20AC8) came out wrong.
- Its player's view bits (+0x10's low byte) from its side camera's test
  of the logic count before; the list's from two before, except a
  character gone since (board 0's entry all zero: 0 there, as the real).
- Its side camera run with its buttons (the camera reads them: shift), the
  slave's frame for a logic count being the master's + 1; and with
  device writes held during a side run (`Bus::hold_device_writes`): the
  camera's update can send a sound command, and the MIDI byte and its
  interrupt on board 0 parted the boards by a few cycles.
- The camera shakes decided by a cabinet's own player (0x345E4, from
  0x21C88: a character's animation command when the character is the
  local player or holds it, or in mode 3 for all; from 0x22750: kind 7
  for the player the camera follows) decided again for it (hooks at both)
  and board 0's own (written at 0x345EC) not copied into its camera.
Where it stops: board 0's TIME ran out at 10,124 (each cabinet has its
own), and the header's +0x06 (each cabinet's status: bit 2 its time over,
0x4B588, then its continue countdown -7523(r13), -8188(r13)'s flags) is
board 0's, copied. A cabinet's own flow (time over, continue, game over,
joining) is the next part: its own variables, run on the side as its
camera is, or that board run without pictures or sound while it's outside
the shared game.

**A cabinet's own flow run on the side (2026-10-31; the owner chose this
over running that board).** Its flow is the mode dispatcher 0x492AC,
called once a frame from the main loop (0x11A80; the in-game mode's
routine 0x4B460: time over, the continue countdown -7523(r13), the flags
-8188(r13), its header status 0x140106). Run on the side at board 0's call
(a hook), with that cabinet's own variables swapped in and kept after
(`kCabinetOwn`: the bytes of r13's areas 0x104000- and 0x106000- that
differ between two linked boards through a time over, a continue and a
game over, less the main loop's timings; its buttons' words 0x100100-
0x10011F, its toggles flipped once a frame; its header status
0x140104-0x140107, as its flow ORs bits into +0x06; and each character's
view of it, +0x34A0-0x34BF and the first word's low byte, which its view
tests write and its camera's update reads: found by bisecting the real
board's RAM into a side run). Its camera's update and view tests run with
the same own variables; board 0's own continue and name entry (0x7682C,
0x76F78, 0x76868, 0x76944, into its camera's +0xB04-) aren't copied into
it. Seeded from that board's own while it still runs (its flows since run
forward). Result (the mashing run from 10,100, board 1's PC gone at
10,110): board 0 the same as the two-board run, frame for frame, through
board 0's time over, its continue and its game over, 833 frames, every
made block exact, the side flow's own bytes the real ones'.

Where it stops, and why it can't be run on the side: **a cabinet misses
frames by its own drawing load.** In the shared game neither board missed
a frame (4,700-10,900); on a continue screen and after a game over they
do, by their own pictures: board 1 at 10,943, in its continue countdown
while board 0 still played (held at 99 seconds), its drawing phase
0x11DF4 (the game times it, -16244(r13)) taking 3.4 times its usual as
its picture changed to the continue view; the master then waits a frame
(board 0's logic count held at 10,944), so the shared game sees it. The
side flow itself costs 100-300 cycles; what overruns is that cabinet's own
drawing, most of a board's frame, known only by running it. After a game
over it doesn't matter (that board no longer takes part; every PC makes
the same held block for it, and it comes back by a join, `--clone-seat`);
during a continue countdown (about 11 seconds) it does.

**Which frames a cabinet misses, the rule (2026-10-31).** Not its picture
switching, as first thought: its banners. A banner (TIME OVER, the
continue's, ...) unpacks its tile graphics straight into tile RAM in one
go (0x8E2C, called from 0x8AE0: chunks of up to 128 tiles, 64 bytes each,
about 400,000 cycles a chunk; TIME OVER is 8 chunks, 3.03 million
cycles, more than a frame of 2,885,744). The game's frame starts when its
main loop wakes (about cycle 1.95 million of the emulated frame, when the
link's data comes: loop top 0x118AC, the link 0x6D758 writing its record
and copying its block to the network board, 0x6EE78, then 0x11DF4, 0xCB84,
0x36E94, then the wait 0x36F44); a normal frame's work ends about cycle
250,000 of the next (about 1.2 million cycles). A frame is lost when that
work runs past the next wake-up: TIME OVER's frame woke at 10,941
(1.96M), reached its wait at 10,943 (79k); 10,942 had no frame, no block.
The same in the emulated cycles on every device (a slower one takes
longer, never differently). A banner's unpacking is fixed by its data, but
the rest of 0x11DF4 is the cabinet's own drawing: board 0's and board 1's
differ 2-5% a frame (the game's own timer of it, -16244(r13)), and the
second banner's frame ended about 2% of a frame before its wake-up, so
it can't be worked out exactly here without that board's drawing code.
Not a fault of ours: our clock leaves out cache misses and the bus, so a
real board is slower and loses these frames at least as surely.

**How close it comes (measured 2026-10-31, board 1 over 1,860 frames:
the fight, its time over, continue, game over, ranking and attract, 14
frames with banners).** The drawing phase waits for the vblank (f_36E04:
the vblank counter -32712(r13), then the tile generator's buffers flipped,
0xF118000C), so a frame whose work before the vblank fits always ends
about 0.3 million cycles after it (58% of a frame before the next
wake-up); only a frame whose work runs past the vblank (a banner's
unpacking: up to 24 unpacks, 7.7 million cycles) ends anywhere. Every
frame's end was at least 28% of a frame (800,000 cycles) from a wake-up,
before or after: the 14 banner frames 28-34%, everything else 41% or
more. The earlier "2%" was a misreading (the unpacking's end, not the
frame's). Board 0's drawing as a stand-in for the remote's is 2-5% of
that phase off (25,000-55,000 cycles), so with the banners' unpacking
costs (fixed by their data) every lost frame here is predicted right with
room to spare; only a frame ending within a few percent of a wake-up
would need the remote's drawing timed exactly (a side run of 0x11DF4
needs the vblank wait inside it handled: not working yet) or a rollback.

**A slave makes the master's blocks too (2026-10-31).** The worry: the
master runs a frame ahead, so its newest record (logic L+1, the slave at L)
holds what a slave hasn't computed. Changing one field at a time in the
master's real blocks (with gamerun switches since removed, the link
buffers 0x140000-0x14FFFF left out of the hash): the
slave's game reads the master's record from two counts back (L-1), which
it has made itself by then; the newer ones only once, briefly (player 1's
+0x3330 for a frame). So a slave makes the master's block by rebuilding
its four records (slot L & 3, as 0x6EE78 puts them) from its own records
of the same counts, the newest a placeholder from its own newest: board
1's game identical to the two-board run with the master's PC gone, idle
(1,277 frames) and in the mashing fight (4,904), its link buffers only
differing. The buttons for the side runs are the remote's frame for the
same count: the local frame + 1 for a slave, - 1 for the master.

**The module (2026-10-31): `tools/remote_cabinet.hpp`,
`tools::remote::Cabinets`.** The experiments' pieces in one place, for
gamerun and the window: per other cabinet its side camera (update, view
tests, shakes decided for it, the game's other camera writes mirrored),
its side flow (the dispatcher with its own variables), and its blocks
(every record by logic count from this board's own, its own fields
added; the counters from this board's logic count and the cabinet's
offset), written into its network board at this board's meetings. A
side run now undoes RAM by the bus's journal (`Bus::journal`,
`undo_journal`: every work RAM write the slow way, the bytes before kept)
instead of copying 8 MB twice, and has its own cycle budget
(`Context::limit_aside`). Each character's first-word low byte is only
partly a cabinet's own: the view bits 0x08 and 0x10 (0x40 and the low
three bits are the shared game's). gamerun `--one-board B@FRAME
--one-board-camera FILE` (board B's main board gone from FRAME; its side
runs started ten frames before, its camera from a `--camera-record` of
it, its own variables from its own as its flows started): every test
exact, board for board against the two-board run:

| run | board leaving | frames | result |
|---|---|---|---|
| idle | 1 (slave) | 1,277 | board 0 identical, blocks identical |
| moving player | 1 | 1,273 | the same |
| mashing fight, a time over, continue, game over | 1 | 6,146 | the same |
| idle | 0 (master) | 1,377 | board 1's game identical |
| mashing fight | 0 | 6,130 | the same |

The mashing runs took 101-118 s for about 6,100 frames, one board and
the side runs (gamerun, software 2D): about real time (90 s once the
timing hooks used to measure the banners were taken out again; the same
6,146 hashes). The measuring tools of the sections above (the main loop's
stage times, the unpack costs, RAM dumps by frame) were scratch switches
and are not in gamerun.

**Past the first missed frames (2026-10-31, uncommitted when written).**
The same mashing run carried on past frame 10,846 (where the runs above
stopped) into the banners, both players' continues and board 0's game
over. What the real blocks showed:

- A board that misses a frame writes no block; its network board sends
  the other bank again, the block from two frames before (both banks a
  double buffer). Given which frames the remote cabinet misses
  (`--one-board-miss FRAME`, for now taken from the real run), skipping its
  block there is exact.
- The link (0x6D758) is a protocol of its own, each cabinet deciding from
  what it received: whether its count goes on (0x6DBE0: the others' counts
  too old: the master needs the slave's from the frame before no more than
  two behind its next, a slave the master's from the same frame no more than
  one; held back, the frame counter and the header's +0x48 go on, the count
  and the records stay), who is in the game (0x6DCB0 and 0x6EB30: the
  header's node mask +0x28; board 0 leaving the game at 11,163 set its own
  to 0, board 1's to 0x40000000), the record's stamps (0x6DA48: +0x04 the
  buttons, +0x10 the player's first word, +0xD8 the node, +0xD9 the count &
  3, +0xDA the nodes - 1, +0xDC the count), its frame counter (0x1000A4,
  bumped at the main loop's top). Rather than copying each rule, the
  module now runs the remote's link itself ("link run"): its link memory
  (0x140000-0x14FFFF: control, header, working record, the others'
  records, its block, what it received) and variables (0x1000A4-0x1000AB,
  -7544..-7531(r13)) in, its network board lent to the bus for the run
  (`Bus::borrow_netboard`: the link reads what that board received and
  writes its block there itself), its next record made from this board's
  into its working record, its own variables as its last link left them
  with its flows' changes since applied (a flow runs at this board's time,
  which for a slave is before its link of that count). A held-back frame
  ends at the hook on 0x36E94. The record word +0x100 is the cabinet's
  own flow's (0x4C21C) and link's.
- The link run writes the block late in the frame (2.8M cycles in), after
  this board's view tests of the count, which a cabinet a count ahead
  carries in its newest record; the newest record of a count this board
  hasn't reached is its working record then. The other cabinet reads a
  record two counts on, and every block carries the last four again, so
  each link run first brings its older records' camera fields up to date.
  A cabinet's view of its own player is run right after its camera's
  update (this board's own view tests run as late as 100,000 cycles into
  the next frame in heavy frames, after the block has gone).
- A third camera shake: a hit (0x26070, before 0x260CC; the kind the
  hitter's +7784, when the local player hits or is hit, or in mode 3 with
  a flag unless the one hit is down). Missing it put a new enemy off the
  master's screen in its record 200 frames later.
- `--one-board-camera`'s recording is now also a check: the first
  difference of the side camera from it, as each update starts.

Results, the side link in (board for board against the two-board run):
board 1 leaving: exact through 11,164 (6,364 frames after the hand-over),
through both of its given missed frames, board 0's time over, continue
and leaving the game. Board 0 (the master) leaving: exact through 10,943
(6,143 frames), every hold-back the same. The runs take 180 s (90 s
before the link runs: to be made cheaper).

Where it stops, and why:

1. **The cabinets apart.** From 11,163 board 0's player is out (game
   over) and board 1 plays on alone. Their worlds are no longer the same
   game, so board 1's records can't come from board 0's world, and board 0
   does read them: from 34 frames on, its copy of player 2 (0x1A6xxx)
   follows them, and it spreads into its globals. Board 1's world then has
   to come from board 1's own game running.
2. **This board missing a frame while the master is a count ahead.** At
   10,942 board 1 (here) is still unpacking its TIME OVER while the master
   links two counts ahead; its newest record needs the master's world step
   of a count board 1 hasn't run. Running the master's flow and its tasks
   (0x11DF4 up to its vblank wait, hooked at 0x36E04) on board 1's world
   is the way, but the tasks there run out of their cycle budget in the
   banner's unpacking (0x8F98).
3. Which frames a remote cabinet misses, still given, not predicted.

**A shadow board for those stretches: tried, not exact (2026-10-31).**
The idea: when side runs can't follow, run a whole board as the other
cabinet ("shadow"), copied from this board's state where both have
finished the same count (0x36E94: board 0 finishes count C about a frame
before board 1 does, both linking at about 1.95M cycles in the same
frame, board 1 a count behind), with the module's own variables, camera,
link memory and variables put in (gamerun `--one-board-shadow FRAME`:
the leaving board waits, taking its turns, then runs as the shadow).
The two boards' RAM at the same count differs in about 565 KB; most is
what each draws, but not all. With every byte that differed at the
hand-over taken from the leaving board (its own, as it was then), 29 KB
still differ from the real board 1 right after the copy: fields inside
the characters' and enemies' objects (0x1A0000-0x1C3FFF; player 1's
+0x2249 is 05 on board 1 now, 00 at the hand-over), 0x1F0000-, 0x200000-,
each cabinet's and changing in play. Side runs never needed them (the
blocks stayed exact without them), but a whole board's game reads them:
the shadow's records part from the real board's within three counts
(its enemies' entries). A board copied in mid-game can't be that
cabinet unless all of its own state has been kept, which means having
run its whole game all along.

**Which of those bytes matter: bisected (2026-10-31).** The real board 1's
RAM saved where it finished count 4818 (its 0x36E94), and the shadow
started as above with the real bytes put in over chosen ranges, then run
450 frames on, its blocks and board 0 compared with the two-board run.
All the real bytes but the live stack (0x73EFA0-0x73FFFF: the copied CPU's
return addresses): the shadow's blocks and board 0 exact through 6,554,
the end (the shadow's inputs for the copy's own frame set from its press
list too: its frame's input read comes before the copy point). None: off
at 6,003. Halving, a range left out or kept at a time: not needed are the
link memory, the r13 variables and camera, the 2D layers 0x180000-, the
objects' other bytes, 0x1D0000-0x20FFFF, 0x230000-0x73EFA0. Needed, and
enough together:

- two enemy view-area bytes (0x1B74A2-3, 0x1BF4A4: the characters'
  +0x34A0 area, which the module keeps; its copy there one step out of
  time at the copy point);
- about 350 bytes of a table at 0x217E00-0x22A800, entries 0x100 apart,
  written about 450 times a frame on board 1 by objects' own routines
  (0x62BA0, 0x63584, 0x639B8, 0x4F180, 0x4F42C, called through pointers).
  Side runs never run them, so the module doesn't keep it.

So a shadow started mid-game can be exact if the module keeps that table
for the other cabinet too, which means running those routines as it
(part of its frame's tasks), at a cost still to be measured.

**Tasks mode, and a shadow that is exact (2026-10-31).** The table is the
characters' pose buffers: 0x62AF4 steps a character's animation (from the
ROM's tables at 0xFFB00008) and 0x62BA0 works out its pose, from the
characters' updates (0x1E3D8, through 0x42240, 0x5154C, 0x51D18; 0x4F180
from 0x51820 and 0x5222C too); a character a cabinet's camera doesn't see
isn't posed the same, and enemies' view areas are set when they appear,
by code side runs never ran. The frame's order (board 0, measured): the
link about 1.95M cycles in, the dispatcher, the characters' updates and
animation (2.0-2.2M), the camera's update (2.3M), the drawing's vblank
wait (2.45M), the view tests in the next frame (about 80,000 cycles in),
0x36E94 (260,000). The part before the drawing is about 43% of a frame's
work. Tasks mode (`Cabinets::use_tasks`, gamerun `--one-board-tasks`):
each other cabinet's flow run with its frame's tasks (0x11DF4) up to the
drawing's vblank wait (hooked at 0x36E04), as it, its own variables,
camera and pose buffers (0x210000-0x22FFFF) kept from that; then what
makes its camera's view (0x32578, the drawing's). That replaces the camera,
shake, mirror and animation side runs (its own code does all of them); view
tests and link runs as before. Its blocks exact as before (board 0
identical over 3,390 frames); its side runs 2.8 ms a frame against 1.3 ms
(gamerun, this PC). Found on the way: inside a side run gamerun's hook
skipped the module, so a link run that held its frame back, and a tasks
run reaching the vblank wait, ran on to their cycle budget unseen (their
results were right, the time wasted); fixed.

From the module in tasks mode, with nothing of the real board's: a shadow
started at frame 6,000 gave board 1's blocks exact and board 0 identical
over the 550 frames tested (four pose bytes off at the copy, none that
mattered). One more of each cabinet's own: the main loop's count
(-32764(r13), 0x100004, one up every time round), which a link resync puts
in the header's +0x20 (after the game over, 12,848); now a link variable,
one up in each link run. Started at 10,900 (a calm point before the time
overs) and run on: board 0 identical to the two-board run for 10,664
frames, 4,700 to 15,364 (the reference's own end, where its other board
stopped at its block limit): both time overs (the shadow missed its frames
by itself), both continues, board 0 leaving the game, board 1 on alone,
both game overs, ranking, attract, the link's resync. A shadow can't
easily be started in the middle of the trouble: around 11,150 board 0
misses frames to its own banners, its work for a count ending after its
next wake-up, and the module's state for the count isn't there at the copy
point (the shadow waits and asks again each frame, never finding one).
Started before, it carries through.

Still to do: when to start a shadow and stop it again (the games together
in plain play: handed back to side runs, as at the first hand-over), the
master's shadow (a count ahead of this board), tasks mode in banner frames
(its side runs there ran to their cycle budget in the unpacking, 0x8EE0),
and what a shadow costs on a tablet.

**Handing back from a shadow: the slave's timing (2026-10-31).** Side runs
start from the other cabinet's own state after the count before the one
this board is about to run (its own variables, camera, poses; in tasks
mode the world they run on is this board's before its tasks of the
count). The tests above took the camera from a recording of an earlier
run. Taken instead from the leaving board itself where it finished a count
(its 0x36E94; gamerun now does that without `--one-board-camera`): a
master's is there in time (it finishes a count a frame before this board
starts the next), a slave's never is: when this board's dispatcher of
count L comes (1.98M cycles into frame F), the slave has finished only
L-2 (its 0x36E94 for L-1 is 0.25M into frame F+1). So side runs of a
slave can't be started from its live board at the right count, and a
shadow of a slave can't be handed back that way. Ways round: this board's
world kept from the frame before (page-tracked snapshots, as rollback
keeps), the slave's catch-up count run on that; or a shadow, once started,
left running. Also seen: tasks mode with the master leaving failed in
plain play (4,846) on its cycle budget in the tile unpacker (0x8EE8).

**Tasks mode with the master; handing back; the master's shadow (2026-10-31).**
The master's tasks unpacked from nonsense (0x7FFF tiles from 0x5655EC2C):
its map load (0x92EC, map 0xBB) reads banked CROM, and the bank switch
(the system board's CROM bank register) was a held device write, so the
side run read the wrong bank. The bus now lets the CROM bank through while
holding device writes and puts it back after (`Bus::hold_device_writes`).
With that, tasks mode with the master leaving is exact through 10,943
(where this board misses a frame with the master ahead), as the plain mode.
The record's player word takes only the view bits (0x08, 0x10) from the side
view test; its 0x40 is the shared game's (the side test runs before the
tasks that set it). Kept for each cabinet in tasks mode: its 2D area
(0x180000-0x19FFFF) besides its poses.

Handing back (module: `ask_hand_back`, `hand_back`; gamerun
`--one-board-hand-back FRAME`, and `--one-board-auto`'s rules): at this
board's next dispatcher (count L) the count the shadow stops after is set
(L-1 for a slave, L+1 for the master) and for a slave this board's world
kept (8 MB, once); the shadow stops at its 0x36E94 for that count; the
module takes its own variables, camera, own areas, link memory and
variables there; its first link run takes its working record as left; a
slave's next count is run on the kept world, with view tests of the
characters this board's took. Tried: a shadow of the master from 6,000,
handed back at 6,303: the hand-back went, but the shadow had parted at
6,002. A master's shadow can't be made from this board: the master runs a
count ahead (when this board finishes count C, the master has finished
C+1), so this board's world is always a count behind what it needs. A
slave's shadow works (above); the slave's first side runs, without a
camera recording, need the same catch-up as a hand-back (its state after a
count comes a frame after this board needs it).

**Every hand-over exact, both ways; the rules (2026-10-31, uncommitted when
written).** The master's shadow is made from a side run after all: this
board, at its dispatcher of count L, runs the master's count L as it (flow
and tasks, its link memory and variables swapped in) and on through its
drawing to the main loop's 0x36F24, the vblank wait's loop (0x36E08) and
the OS's transfer wait (0x4DD8) passed at once; that RAM and CPU (its MSR
as a board's is there) go to the leaving board, which goes on at the idle
wait 0x36F44 with the flag read as 0 and a decrementer interrupt pending
(its wake-up had come at its link time). Its players 3/4 objects
(0x1A8000-0x1AFFFF, which its link's entries read) are its own too. The
slave's hand-back glitch (four frames, a player's view bits) was the
catch-up's view tests called from the dispatcher with its registers: the
view test reads more than the view matrix from its caller's (the draw
loop's) registers. Each view test's registers are now kept as this board
calls it, and the catch-up's run with them. And the first link run after
a hand-back kept the shadow's working record but also skipped putting the
player's first word (as this board had it at that count) into the
player's object, so the link stamped this board's current one: now put in
as in every other link run.

With those, a cabinet's main board itself is handed over the way a shadow
is (`Cabinets::running`, then `ask_hand_back`): gamerun `--one-board B@F`
without `--one-board-camera` does that, so no camera recording is needed
any more, for a slave either (its next count caught up on this board's
world kept, as after a shadow). That is how network play will start side
runs: from the leaving PC's own state.

`--one-board-auto`'s rules, fixed: a shadow's state wasn't asked again
while one was waiting for the leaving board (each frame's ask had replaced
the one about to be taken, so the shadow started only when the trouble
ended), and the first link mask seen isn't a change.

The fast test (a script over two saved two-board states of the mashing
run, `build/onepc/mash-5990.state` and `mash-10890.state`, twelve runs in
parallel with `--no-2d --no-sound`, about a minute), each compared frame by
frame with the two-board run's RAM hashes for as long as both boards ran:

| run | from | board that stays | frames, all identical |
|---|---|---|---|
| side runs (camera recording) | 6,000 | master | 538 |
| side runs | 6,000 | slave | 539 |
| side runs, no recording | 6,000 | master | 538 |
| shadow from 6,100 | 6,000 | master / slave | 534 / 483 |
| shadow 6,100, handed back 6,250 | 6,000 | master / slave | 538 / 539 |
| shadow from 10,910 (time overs) | 10,900 | master | 2,100 |
| the rules | 6,000 | master / slave | 2,151 / 2,148 |
| the rules | 10,900 | master / slave | 2,102 / 2,054 |

From 6,000 the rules never wanted a shadow (plain play); from 10,900 one
starts at once (5 s or less left) and carries through both time overs,
the continues, game over, the ranking and attract. Not yet tried: the
rules handing a shadow back in the middle of a game after trouble (the
hand-back itself is exact, above); banner frames in tasks mode; the
players 3/4 objects as a cabinet's own only while those seats are empty.

**The test in the repo; the rules mid-game; banners; four cabinets
(2026-11-01, uncommitted when written).** `tools/onepc-tests.ps1` runs the
table's runs and more (fourteen, in parallel, about 80 s; exit code 1 if
any differs); `-Prepare` makes the reference and the two states from
`build/onepc/fight-4700.state` with the presses in
`tools/onepc-mash-presses.txt` (made again, they came out the same: both
states byte for byte, all 21,361 hashes). New in it: the leaving cabinet's
coin at 6,100 in the middle of the fight, both ways: a shadow from 6,041
(the 60 frames before a press), handed back by the rules at 6,405, every
frame the same as its own two-board run (1,075 / 1,074).

Banners: side runs can't follow them. A side run of the other cabinet's
tasks now includes its 2D task (its own 2D area kept; every run above the
same with it), and that shows them: its work, usually about 0.3M cycles,
was 3.5M, 3.8M and 5.6M in the TIME OVER and continue frames. Whether such
a frame is lost depends on the cabinet's drawing (not run on the side) and
on its own player's banner (each cabinet shows its own), so only a shadow
started before covers it: the rules (5 s or less). A side run whose tasks
take over 1M cycles is counted, and gamerun reports it ("heavy frames on
side runs"): a frame the other may have lost with no shadow running. Also
fixed there: the OS's wait for a transfer (0x4DD8) in a side run's tasks
spun until its budget ran out (device writes held, the transfer never
ends); passed at once now, as in the master's capture.

Players 3 and 4's objects (0x1A8000-0x1AFFFF) are a cabinet's own only
while their seat is empty in the link's players (`region_own`). gamerun's
`--one-board` takes more than two boards (the master stays unless it is
the one that goes). Tried on four players in one game
(`build/linkstudy/four2/four-8000.state`, any of seats 2-4 leaving at
8,100): the made blocks fail the link's own check within four counts. The
side link run sets error bit 1 (0x106288, -7544(r13)) and ends in the game's
"link broken ... call staff" halt (0x74350). The check (0x74364) compares,
for each of the 12 characters, the entry's +8 in this cabinet's record with
every other cabinet's, wherever either is marked. Not from the players 3/4
change (the same with those objects always own).

Found and fixed (2026-11-01), two faults, both invisible with two
cabinets:
- The made record kept its +0x34 and +0x40 from the cabinet's previous
  block, as "the empty slots' leftovers". Those are players 3 and 4's
  entries' +8 (a coordinate). Each cabinet's own while the seat is empty
  (0x74808 writes them from its unused players' objects), the shared
  game's once a player is in it. Now kept only while the seat is empty.
  (Found by evaluating the check again at the end of each side link run:
  characters 2 and 3 disagreed, one old value in every count.)
- A main board handed over in the same frame as its own link (its 0x36E94
  for the count before that frame's end) was linked again by the module in
  that frame: the hand-over passed `frames() - 1` as its last frame. The
  extra link is an extra write to 0xE0, so the network board's bank swap
  and the ring's forwarding moved one exchange on, and the next cabinet
  round the ring received the master's block one exchange early (seen as
  its header's counters, +0x03 and +0x5B, one ahead). Seat 4, handed over
  in the next frame, didn't show it. gamerun now passes the frame of the
  board's own last link (its record write, 0x6DA90).
Ruled out on the way: the time the made block is written (1.5M to 2.8M
cycles into the frame, the same every time; the real slaves link at
1.96-1.98M); the working record's address (0x140200 is right: 0x6DA74
sets r31 = 0x140100 and 0x6EE80 copies 0x140200 into the block's slot,
whole).

Four players in one game, seat 2, 3 or 4 made by the master from 8,100,
in tasks mode, and seat 4 by the rules: board 0 the same as the
four-board run for all 1,899 frames compared (to the reference's own end,
where its first board stopped at its block limit). In the test script.
(Run again 2026-11-01: 1,849 frames, 8,001-9,849, where all four of the
reference's boards stop.)

## One board a PC over the network (2026-11-01, uncommitted when written)

gamerun `--net-one-board FRAME` (with `--net-host` / `--net-join`, on every
machine): the session runs as Mode B up to the end of FRAME, every board
on every machine; then each machine hands every other board over to its
own board's `tools::remote::Cabinets` (tasks mode) and runs only its own.
The remote cabinets' network boards stay on the ring here, fed by the
module; their buttons come from the input table, which the session fills
with the other players' inputs as Mode B does (the input delay brings them
before this board's frame needs them; the module waits if one isn't in).
Mode B stays the default: without the switch nothing changes.

**Why nothing is sent at the hand-over.** The brief was to send what the
module takes from a leaving board (own variables, camera, own areas, link
memory and variables, working record) over the network. But the module
also needs that board's network board in step with this machine's, which
only running its main board up to the hand-over gives (or its writes to
the network board, which the link needs within a frame: the reason the
blocks are made here at all, "But a sent block comes too late"). With the
Mode B frames before the hand-over each machine has every leaving board
itself, exactly, and takes all of it there, as `--one-board` does in one
process: so no transfer, and no stall. Sending it is still needed for a
seat taken later (a player joining a one-board game in progress); not
done yet.

What changed for it:
- `--load-state FILE` in network play: every machine starts from its own
  copy of the same linked state (its hash mixed into the build id, so
  machines with different states don't join each other).
- The module's buttons for a remote seat: from the input table (network
  play), else from `--press` as before. The table keeps 64 frames back.
- `--one-board` repeated hands several boards over at once (network play:
  every seat but this machine's), the one that stays making them all; each
  one's offset from its role (the master a count ahead of the slaves: +1,
  -1 or 0, slave from slave). Camera recordings, given missed frames,
  shadows and the rules stay one board only.
- A handed-over board whose shadow can never be wanted (no rules, no
  `--one-board-shadow`) leaves the meetings instead of passing turns
  (`Turns::finish`): the board that stays meets no one.
- The camera write watch only outside tasks mode (its callback returned at
  once in tasks mode, and the watch sends every work RAM write the slow
  way).
- A state request (a player joining in progress) after the hand-over is
  refused with a message: the other boards no longer run here.
- Reports: each board's thread CPU time and the process's; the module's
  time with its set-ups and its network boards'; `--no-frame-hashes` for
  timing runs without the comparisons' 8 MB hash a frame.

**Proved** (`tools/onepc-net-tests.ps1`): processes on this PC over
localhost, input delay 2, each process's own board's RAM hash every frame
against `gamerun --boards N --parallel --input-delay 2` with the same
presses (`--one-board 1@99999999`: hooks only, the hashes printed), for as
long as all of the reference's boards ran:

| run | from | hand-over | process (board) | frames, all identical |
|---|---|---|---|---|
| two seats, both mashing | mash-5990.state | 6,000 | host (0) / joiner (1) | 2,149 / 2,148 |
| four seats, no presses | four-8000.state | 8,100 | each of the four | 1,093-1,094 |

Hashes at the last compared frames, the same in the processes and the
reference: two seats, board 0 at 8,139 `1311e7868460da7b`, board 1 at
8,138 `f3e3b11d7cf5e099`; four seats at 9,093, boards 0-3 `0408e7edd12704d4`,
`0aa89b99e78f98d3`, `c67f84d9bc3c57ff`, `bdae448a28d0f641`. No network
hash check differed. In one process first, `--parallel` (network play's)
like the turns of `tools/onepc-tests.ps1`: master or slave staying, 1,075 /
1,074 frames; four boards with three going, the master staying (1,098) and
a slave staying, making the master and the other slaves (1,099 and 1,101):
the first runs of a slave making another slave (offset 0), all exact.

**What it costs (measured 2026-11-01, i7-8700K, quiet, no 2D or sound on
any board).** Thread CPU times from `GetThreadTimes` were wrong here:
Windows charges whole clock ticks (15.6 ms) to whichever thread runs at
each, so linked boards that wait every few hundred microseconds came out
far too low (under 1 ms a frame a board). gamerun now counts cycles
(`QueryThreadCycleTime`, its rate measured against the performance counter;
`m3::thread_cpu_seconds`, within about 10%). Those show that linked boards
run `--parallel` keep their threads busy while they wait at each meeting
(the waits spin), so a parallel run's CPU time isn't its work. Each board
alone on one thread is: Mode B in turns mode (the boards one at a time on
one core) against one board making the others (alone, no meetings),
one process, wall time:

| seats | frames | Mode B, every board | one board, the others made | a remote cabinet |
|---|---|---|---|---|
| 2 | 2,154 | 46.5 s (21.6 ms a frame) | 27.9 s (13.0 ms) | run 9.5 ms, made 3.7 ms |
| 4 | 1,100 | 48.0 s (43.7 ms a frame) | 25.2 s (22.9 ms) | run 10.9 ms, made 4.3 ms |

(A board's own frame here, the idle loop included, is about 9.5 ms; real
time is 17.4 ms.) The module's own timers for the two seats: 5.7 s in its
side runs, 8.0 s with their set-ups (the cabinet's camera, own variables,
288 KB of own areas and 64 KB of link memory copied in and out, RAM put
back from the byte journal), 0.65 s its network board. So making a cabinet
costs about 40% of running it; most of the rest is the board's own. The
two processes over localhost, as the table's two seats: Mode B 45.2-45.6 s
CPU a process (two threads, busy waiting included), 25.5 s wall; one board
31.7-32.4 s CPU on one thread, 29.7 s wall (both faster than the 37.5 s of
real time).

Where the module's time goes (two seats, 527 frames from 6,000): 95% of
its side runs' time is the tasks run (the other cabinet's flow and its
frame's tasks up to its drawing, about 2.6 ms a frame); its view tests and
link runs are small (0.08 s together). They write about 270 KB of RAM a
frame, put back after (17% of it the cabinet's own areas swapped in, the
rest the game code's own writes), each byte the slow way into the bus's
journal. Tried: a journal of whole pages instead (each page kept once, at
its first write): 4 KB pages copied 645 MB against 141 MB of bytes and were
slower, 256-byte pieces the same as the bytes. So the journal isn't the
cost; the cabinet's own game work is, about what its own board's frame
does before its drawing. The bigger part of a frame here is the board's own
(about 9.5 ms, mostly its idle loop spinning until the next frame), which an
idle-wait skip would cut for Mode B and one board alike.

**Without shadows it doesn't last a game.** From mash-10890.state, board 1
handed over at 10,900, side runs only: board 0 the same for 53 frames, then
apart at 10,944, the TIME OVER (the side run out of budget in the banner's
unpacking, 0x8704). So network play needs the shadows and the rules too.

**Shadows and the rules over the network (2026-11-01, uncommitted when
written).** A shadow needs nothing from the network: each machine runs the
other cabinet's board again itself, from its own board's state and its
module's own data, as in one process; its inputs come from the input table
(the first frame's too). Only the rules' look ahead changed: they started a
shadow when the other cabinet's coin or start came within 60 frames (from
the `--press` list), and over the network only the input delay's frames are
known. `--one-board-lookahead N` limits it in one process: with 2, the
mid-fight coins (`slave-midgame`, `master-midgame` above) still exact, 1,075
and 1,074 frames, the shadows started at 6,099-6,100 for the coin at 6,100
and handed back at 6,406. So the 60 frames were never needed. Over the
network the rules read the other seat's inputs from the table, as far as
they are in (`--one-board-auto` with `--net-one-board`, two seats). A shadow
no longer reports its hash to the session (it is exact in what it sends,
not in all of its RAM: those checks differed every time).

Proved, two processes over localhost, input delay 2, each machine's own
board against `--boards 2 --parallel --input-delay 2` (`tools/onepc-net-tests.ps1`
runs them all):

| run | from | what happens | board 0 / board 1, frames identical |
|---|---|---|---|
| apart | mash-10890, handed over 10,900 | shadows at once (5 s or less left), both time overs, continues, board 0's game over, the ranking | 2,067 / 2,057 |
| coin1 | mash-5990, 6,000 | the slave's coin at 6,100: a shadow from 6,098, back at 6,407 | 1,075 / 1,074 |
| coin0 | mash-5990, 6,000 | the master's coin at 6,100: its shadow from 6,101, back at 6,404 | 1,075 / 1,074 |

(In apart the joiner's shadow of the master stopped at its own block limit at
12,946, its board on alone from there: compared to 12,944, as
`tools/onepc-tests.ps1` does.) No network hash check differed. The script's
run (two, four, apart, coin1, coin0) gave the same, apart 2,083 / 2,054 frames
and the coins 1,074 / 1,074, after one fix: a machine's own board stopping
now stops its shadows too (it closes its input table). The host's own board
had stopped at its block limit while its shadow of the joiner's board waited
for inputs the joiner, gone already, would never send: the process hung.

**One board a PC in the window (2026-11-01, uncommitted when written).**
The hand-over, the shadows and the rules moved out of gamerun into
`tools/one_board.hpp` (`tools::one_board::Game`, a `Board` for each board:
its context, bus and frame timing, which the program keeps running), used by
gamerun and the window alike. gamerun gives the same as before: the 18
`tools/onepc-tests.ps1` runs and the network runs, frame for frame.

The window can't start from a state in the middle of a fight: a network game
starts at power-on. So the window hands the other cabinet over 10 frames in
but holds it whole (`hand_over(..., held)`, gamerun's `--one-board-held`):
a shadow from the start, until the rules find the game calm (both players in
it, more than 15 s each, 300 frames), and from then on side runs, a shadow
again where trouble comes. Proved first in gamerun over localhost
(`tools/onepc-net-tests.ps1`'s poweron: player 1 in a game alone at 1,300,
player 2 joining it at 2,700, handed back at ~4,620): 7,374 of 7,374 frames
the same on both machines. It found one fault: the module's "stop after" of
0 (none asked yet) matched the logic count, still 0 through the boot, and
handed the board back at once; 0 is now never a stop.

`spikeout --net-one-board` (two players; off by default, the command line
only while it's tested, as `--smooth`): each board registers its part as it
starts, the last sets them up; the other board's thread waits as a shadow
would. Nothing sent differs, so the others needn't do the same. From the
hand-over a state asked for (a player joining in progress, a machine out of
step) is refused, and F9 saves nothing: the other board doesn't run whole
here. The other cabinet's RAM hash isn't reported. Tests:
`SPIKEOUT_TEST_PRESS=NAME@FRAME[:N],...` holds this player's buttons as
gamerun's `--press`; `tools/onepc-window-test.ps1 [-OneBoard joiner|host]`
runs two windows on this machine, one hosting, one joining, through their
menus, one of them on one board, and the session's hash checks (every 120
frames) compare the one-board window's own board with the other window's
copy of it:

| one board | handed back to side runs | hash checks differing, to frame 7,000 |
|---|---|---|
| the joiner | 4,624 | none |
| the host | 4,621 | none |

(Both players in the game at frame 7,000, by the windows' screenshots.)

Its speed (SPIKEOUT_NET_DEBUG=1, the test's two windows on the i7-8700K):
two plain windows held 17.4 ms a frame to the end (17.4-18.2); with the
joiner on one board, the same to ~5,700, then 20.6-21.0 ms in both windows
(~48 fps) to the end. The board that stays runs its own frame and the other
cabinet's side runs one after the other on its thread (~18.5 ms there in
the fight's heavier scenes, against 17.4), where every board runs on a core
of its own; the other window waits for its inputs. So on a PC with cores to
spare one board is slower than every board, for all it costs less in all
(13.0 ms against 21.6 ms a frame, above): it is for a machine that can't run
every board at full speed. (Each player's TIME is their own: the two
windows show different ones.)

**Auto: one board only where every board can't keep up (2026-11-01,
uncommitted when written).** What holding costs a PC that never needs it
(gamerun, two boards from power-on, 4,000 frames): 19.0 s plain, 35.2 s
held (each board's 8 MB RAM kept at every logic count: 22.5 s without it).
So a held board keeps its RAM and its seeds (`Board::keeping`) only from 10
frames before the rules ask for it back: 22.4 s, the rest the module's own
bookkeeping (~0.85 ms a board a frame). Better: none of it until wanted.
The module is set up where the boards meet (`Turns`' `between`, every board
stopped there; gamerun `--one-board-late`: from frame 3,500 of the power-on
game, 7,376 of 7,376 frames the same while both ran), the other board held
whole until the game is calm.

The window's Auto (`--net-auto-board`): this player's board's frames timed
(its own work and its meetings, not the waits for inputs or the pacing), 300
at a time from frame 600; over 97% of a frame (16.86 ms), one board from the
next meeting. Then, once the other cabinet is on side runs, timed again: kept
only if 5% faster than every board was, else the other run whole again for
the rest of the game (`Game::keep_whole`: a shadow never handed back), and
once its shadow runs the module goes (`Game::tear_down`: the game's own
hooks back; its hash and state still kept to itself). Seen in
`tools/onepc-window-test.ps1 -OneBoard auto` (the joiner told it's slow,
`SPIKEOUT_TEST_AUTO_MS`): one board at 899, handed back at 4,624, then 13.1
ms against every board's 8.7: every board again from 4,924, the module gone,
18.3-18.7 ms in the fight's heavy scenes (17.4-18.2 plain; 19-19.6 with
the module kept); no hash check differed. Two games on one PC are slow
enough there that the host's own Auto switched too (17.2-17.9 ms).

**Not yet: a slave's shadow in the power-on game.** In the power-on game
(gamerun, both boards in one process, the master staying), a shadow of the
slave never started: "no shadow state for count N, again" every count (each
a state taken: 27 ms frames). The side runs stay exact (6,043 frames the
same), but where the master has finished count C (its 0x36E94, where the
state is taken), the module has the slave's link only of C-1 and its own
variables only after C-1 (`Cabinets::shadow_state` wants both after C; in
the fight tests, from the same point, it has them, and a count of own
variables more). A coin at 4,700 the same: not the heavy frame at 4,920
(first thought), nor the hold (an immediate hand-over at 4,620 the same),
nor the counts' relation at the frames' ends (the slave a count behind the
master in both games), nor the hand-back's catch-up (the same in both).

Where each board links and finishes a count (`ONEBOARD_TRACE=FROM:TO`, the
line in its frame): in the fight both finish a count at line ~40 (the
master's work spilling into the next frame) and link at ~290; in the
power-on game the master finishes at ~390, in the frame it linked, the
slave at ~40 two frames on. The shadow's rule (the master's state a frame
on is the slave after C) holds in both; but there the shadow starts at
line ~390, after the frame's meetings, and the check was at the frame's
top, before the slave's link (~287) and flow. Fixed:
- the leaving board takes the shadow's frame's meetings up to its start,
  and checks then (`shadow_wait`; in the fight the start comes before the
  first meeting: as before);
- the module makes the slave's link near its frame's end (write_at, line
  ~412): asked for one at the meeting before the shadow's start instead
  (`Cabinets::link_early`, a request kept to its own frame);
- 0x1040C4-0x1040CF are twelve countdowns the vblank's interrupt steps down
  (0x496C), which side runs never take: 0x1040C7, a cabinet's own, stayed at
  10 in the module's copy; now stepped each frame (`Cabinets::end_frame`).
With them a coin at 4,700 (a shadow 21 frames after the hand-back) is exact,
7,376 of 7,376 frames (the shadow, and its hand-back at 5,005), and the
test scripts the same as before (onepc-tests 18/18, the network runs 6/6).

Still not: a coin at 6,100 (its shadow 1,420 frames after the hand-back)
differs 4 frames after the shadow starts: the slave's record's camera
heading (+0x0D) first. (Where master and slave differ, 100 counts of that
fight paired by count: their variables, link memory, drawing, players 3
and 4, the camera and around it; the bytes the module keeps cover the
variables, the camera and the areas.) The master finishes count C at line
~391 of frame F, the slave at ~38 of F+2, 1.17 frames on (its own camera's
view drawn: its work longer, over the next vblank), so the shadow, the
master's state a frame on, starts ~70 lines before the real slave finishes.
Tested with the real slave's whole RAM where it finished C (dumped in the
whole-board game, given to the shadow as it starts): differs at 6,045; the
same placed where the slave finished (71 lines on): 6,044. So not its RAM,
nor only where it starts: its devices and CPU too (its Real3D and DMA
timings, its decrementer: the master's, in a state taken from the master).
A slave's shadow made from the master's state is exact only where the
slave's work fits the master's frame (the fight tests, the coin at 4,700),
not in general.

**Shadows from the other machine's real board (2026-11-01, uncommitted
when written).** In network play a shadow is no longer made from this
board's state: the rules, wanting one, ask the other cabinet's machine
(`net::Session::want_board`: a reliable message, as a joiner's state comes)
for its own board's state at the end of a frame 30 ahead
(`one_board::Game::kAskAhead`); that machine takes it there, as a save
state is taken (`tools::board_state`, no sound board), with the packets its
network board had taken in and sent (`LinkRing::taken`, `sent`), and sends
it compressed (`Session::send_board`; ~5.3 MB packed in the fight). Here
the leaving board's thread waits for it at the top of that frame (the
staying board at its first meeting meanwhile: 0.5-0.7 s on this machine,
a pause), loads it whole and runs on as the shadow. Exact by construction:
the module's guesses at the other cabinet go no further than the blocks it
makes. gamerun `--one-board-local-shadows` keeps the old way (and one
process has no other machine: unchanged there).

Getting it exact (the tools: `GAMERUN_PARTS_FRAMES=FROM:TO`, each part of a
board's state hashed every frame (CPU, frame timing, network board, bus,
RAM); `GAMERUN_MEET_FRAMES=B:FROM:TO`, its network board at each meeting;
`ONEBOARD_SENDS=B:FROM:TO`, its packets taken in and sent): the state is of
the end of frame Y, the frame count then Y, so it goes on from the top of
frame Y here; first it went on from Y+1, a frame behind the staying board
(its packets a frame ahead of it), and it differed within two frames. With
that right, both machines' counts of the board's packets are the same where
it is taken, and the shadow is the real board, every part, frame after
frame. (`LinkRing::rewind` and `unsend` put the ring right were they to
differ: not seen since.)

Proved, `tools/onepc-net-tests.ps1` (two processes, every frame of each
machine's own board against the whole-board run): the eight runs the same,
among them late1 and late0, the coin at 6,100 1,480 frames after the
hand-back that no shadow could follow before (a shadow of the slave, and
of the master), 7,374 of 7,374 frames on both machines; apart, coin1 and
coin0 with shadows from the real boards too. And two windows
(`tools/onepc-window-test.ps1 -OneBoard joiner`, the host's coin at
6,100): the joiner's shadow of the host's board from the host's window
(5,256 KB, 555 ms), handed back at 6,430; no hash check differed to frame
7,000.

A shadow wanted after the trouble has started is proved by these runs: the
coins at 6,100 are seen at 6,097-6,101 (the input delay's frames ahead)
and their shadows start at 6,127-6,131, the side runs following the
cabinets 27-31 frames into it; apart's shadow is wanted at 10,902 with the
time already at 0 and starts at 10,932. All exact.

**Three and four players (2026-11-01, uncommitted when written).** With
shadows from the real boards the rules follow each other cabinet
(`Game::rules_each`; the old rules, one other cabinet, kept for this
board's own shadows): a shadow of each asked of its machine, its own calm
frames and hand-back. Trouble and calm made for more than two: a cabinet
run whole here (held or shadowed) out of the game's mode, a player
joining, is trouble for all and keeps every one from calm; any cabinet's
coin or start too (a join changes every cabinet's game). A machine asked
by several at once sends them one state. And with these shadows no board
keeps its RAM at each count (`keep_ram`: the local shadows' only). Found
on the way:
- three players: the third cabinet's records differed as soon as it was
  on side runs (in one process too, not the network): player 4's slot,
  empty, carries each cabinet's own leftovers in its entry's second word as
  in its third (0x74808 writes it from the unused players' objects); the
  module now takes both (+0x30 and +0x34 for player 3, +0x3C and +0x40 for
  player 4) from the cabinet's own while the seat is empty. In one process,
  board 2 of three handed over alone: 7,444 of 7,444 frames; boards 1 and
  2: to 7,385, where the reference's others stopped.
- a leaving board's thread waits through the meetings as long as it isn't
  alone: with four, three of them went on meeting each other after the
  staying board had stopped (`Turns::finished`: they stop with it).

Proved (`tools/onepc-net-tests.ps1`, now ten runs): fourauto, four
processes from four-8000.state, each other cabinet held until calm, seat
3's coin at 8,600 (every other machine a shadow of it from its real board,
one state sent to three; every machine shadowing all the others then, any
coin being everyone's trouble): 1,718 of 1,718 frames on each of the
four; three, three processes from power-on, the third player joining at
4,600 (everyone held until all three are in and calm, ~6,620).

The window's default is Auto again (`--net-every-board` for every board,
`--net-one-board` from the start).

**gamerun and the window in one game (2026-11-01).** A gamerun host with a
window joining differed on the host's board by frame 120, every board run
in both. gamerun sends no settings EEPROM (unless `--eeprom`); for a seat
with none the window wrote zeros, gamerun kept the chip blank (all ones, as
a fresh board has it, and as the window's own first run has it). The window
now keeps it blank too (two windows always send theirs: unchanged). Then
the two agreed, but the cabinets didn't link: both network boards held in
reset, the game's network check cancelled on both machines alike. Not the
settings' values (the window's were the game's defaults: export, a coin a
credit, Normal): in gamerun alone, two linked boards from power-on, a blank
chip on the master with the slave's set up never linked; both blank, both
set up, or a blank slave did (7,356-7,370 packets each way by frame
~4,000). Why the slave's check gives up then isn't known (the blank master
writing its defaults first, at a guess). A window never sends a blank chip
(its game writes its defaults as it first starts), so only a gamerun host
meets it. `tools::seat_eeprom`: a host with none doesn't set the country,
and its own cabinet starts as the first other player's (every cabinet
blank, or every one set up: as before; each player's coins, free play and
difficulty still their own, the country the host's). Then a gamerun host
with a window joining linked (master and slave, 13,778 packets each way)
and no hash check differed to frame 7,000.

Still to do: shadows with more than two seats (the rules follow one other
cabinet); a seat taken in a one-board game (the state for it sent); the
menu setting and an automatic choice (one board when this PC can't keep
up); the module cheaper; rollback (step 2).

**Leaving, and joining in progress, with one board a PC (2026-11-02,
uncommitted when written).** A player joining a game in progress gets
every board's state from the host. A host running one board doesn't have
the others whole, and a board on side runs has nothing more than the
packets it sends. So, two pieces:
- Leaving hands over the board. A machine leaving tells every other of a
  frame half a second ahead (`Session::announce_parting`, kParting, the
  reliable channel). At that frame's end it sends its board's state to
  each (as a shadow's, `send_board`), waits until they all have it
  (`Session::sending`) and says goodbye. Each other machine loads it into
  that board at that frame, as any shadow from a real board, and runs it
  whole from then on (`Game::parting`: the seat departed, never handed
  back to side runs; its inputs none once the authority makes it idle, as
  any dropped seat's). A seat already whole there goes on as it is.
- A state asked for (a player joining). If every other board runs whole
  here, it's saved as usual. Otherwise each board on side runs is asked of
  its machine as for trouble (`Game::want_whole`: trouble for the rules
  until the state is saved, and 10 frames on); once every board runs whole,
  the state is saved and sent; then the rules hand them back as they
  become calm.

A player who goes without a parting (a crash, the network lost) takes
the only whole copy of a board on side runs with them. Hence backups,
below; without one (none had yet), their character stands idle as in
Mode B, but nobody can join in progress any more (`Game::seat_gone`; the
host says so and sends nothing). A board already whole somewhere (a
shadow at that moment) is kept on as it is.

**A player who drops, one board a PC (2026-11-02, uncommitted when
written).** `tools/board_backup.hpp`. Every 120 frames (about 2 s) each
machine takes its own board by pages (`m3::Snapshots`, the rollback
code's: well under a millisecond, where a whole save is 30 ms) and sends
every other machine the pages written since its last backup (`kBackup`,
the reliable channel), or the whole board to one that has none of it
(`kWantBackup` asks for it: a player joined, or changes from one it
lacks). Measured over the three-player game: the changes 96 KB a backup
at the median, 860 KB at the 90th percentile, 3.6 MB at the most (a scene
loading its textures); the whole board 5.3 MB packed. Each machine keeps,
for every other board (`backup::Keeper`, in `tools::NetPlay`): its last
backup, the packets sent to it since (`LinkRing::on_departure`, by their
number on its link: the next it takes is the one its backup's count
names) and its player's inputs since.

Its player gone (the session's idle), each machine runs that board on
from its backup (`backup::Replay`): a board of its own on its own thread
(the bus, the network board with a link that hands it the logged packets
at their times, the game's code and hooks), each frame once this
machine's game has run it (all its packets in the log by then; they come
a slice of lines ahead), its inputs as they were (the player's, then
none from the idle frame, as everyone's). It runs to a frame as far
ahead as it is behind (at least 120); there it goes in as a shadow from
a real board would (`load_remote_shadow`), whole from then on, never
handed back. The game waits there if it hasn't come (it hasn't yet: a
board alone runs several times faster than the game). Then a player can
join in progress as after a parting.

Found on the way: a context ran only the last of several snapshots asked
for in one frame (`Context::request_snapshot`); they now run each in turn
(the backups at every 120th frame met gamerun's whole-state hashes at
every 100th). And the session's callbacks run under its lock: a backup
that couldn't be applied asked for the whole from inside one, which hung
that machine (now asked from the game's thread). A board asked of a
machine that then drops is no longer waited for 30 seconds (the wait ends
with the drop).

gamerun `--net-vanish-at FRAME` goes at the end of FRAME without a word
(the session stopped, the process ended at once); `ONEBOARD_BACKUPS=1`
prints each backup's size. The window takes and sends backups and runs a
board on from its backup the same way.

The window's first test of it hung the host as soon as the other window
was dropped, before any of this: a cabinet nobody has gets its inputs of
none from the authority only as far as the authority's own, and one board
a PC reads the other cabinets' a frame or so ahead of its own; with no
input delay (the window's) it waited for frames made only once it went
on. A dropped seat's inputs are now none in the input table at once, from
its idle frame until a player takes it (`InputTable::hold_from`,
`hold_until`: the same values the session sends). Any drop in a one-board
window game would have hung it so.

The window (`tools/onepc-window-test.ps1 -OneBoard crash`): both windows
on one board, the joiner's ended mid-game as a crash would (its process
killed, ~4,560-4,640); the host drops it, runs its board on from its
backup at 4,560 to 4,751 (2.1 s); a gamerun asking to join meanwhile
(4,641) gets every board once that board is whole (4,754), takes the seat
at 4,793, no hash check differing to 6,700.

Proved (`tools/onepc-net-tests.ps1 -Only vanish,vanish3`): vanish, the
poweron game, player 2 gone at 5,000: the host runs its board on from its
backup at 4,920 to 5,123 (1.2 s), the RAM there the same as the board's
own every frame to 5,000 (80 of 80); a new player then joins (5,625) and
agrees with the host on both boards every frame (4,796 of 4,796 hashes).
vanish3, the three game, player 3 gone at 7,000: the host and player 2
each run its board on from 6,960 to 7,123 (2.8 s) and agree with each
other and with the board itself; the new player's join then asks player
2's board of its machine (side runs) as before, and every machine agrees
with every other (5,745 hashes with the host, 5,434 with player 2).

The game still stops for the 3 seconds before a silent player is dropped
(`kDropAfter`: every machine waits for that player's inputs, as in Mode B
before any of this; the backup's run itself costs nothing seen, the game
at 56-58 frames a second meanwhile, 41 ms at the switch). The owner's
decision (2026-11-02): no shorter drop time; rollback will take it away
(the game run on with that player's inputs guessed as none, which is what
the drop makes them; run again if they come back). Done for every board
on every PC: "Rollback in play", its long window; one board a PC with
rollback is still to do.

Then (2026-11-02, uncommitted when written):
- A board run whole here for a player gone goes back to side runs once a
  new player takes its seat (`Game::seat_taken`, from the session's
  `taken`): that player's machine has it now, asked of it as any other's,
  handed back when the game is calm (vanish: at 5,963, 4 seconds after the
  seat was taken; vanish3 at 7,651).
- Backups are packed and sent on a thread of their own (`NetPlay::
  send_backup`, one queue, so each machine gets them in order): the game's
  thread only takes the pages and copies them out (the whole board's
  copy, 27 MB, a few ms) instead of packing 5 MB (110 ms).
- Every machine in network play sends backups of its own board, one board
  or not: a machine running every board can be the one that drops. The
  window test showed it: the gamerun that joined (every board there) ended
  without a parting; the host ran its board on from its backup (6,600 to
  6,821, 2.4 s).

Proved again: the suite's 14 runs (the leave and vanish ones with the
hand-back after the join), ctest, and the window's leave and crash tests.

gamerun: `--net-leave-at FRAME` (leaves 30 frames after FRAME, as above),
`--frame-hashes` (the per-frame hashes in any run: a joiner's, compared).
The window: leaving (the menu, or closing it) hands its board over the
same way, a few seconds at most; F9 saves only when every board runs whole
here.

Proved (`tools/onepc-net-tests.ps1 -Only leave`): the poweron game, player
2 leaving after 5,030 (its state 5,360 KB packed, loaded by the host 590-750
ms later, the hash at 5,031 the leaver's own), a new player joining at
~5,400 (every board whole already; 5,774 KB packed) and taking the seat:
the joiner, every board run there, and the host agree on both boards every
frame from the join to 8,000 (2,630 of 2,630 frames each, 52 of 52 whole
states), no hash check differing. `-Only leave3`: the three game, player 3
leaving after 7,030; the new player's join (7,168) finds player 2's board
on side runs, asks its machine for it (loaded at 7,198), saves every board
at 7,201 (6,331 KB packed) and hands it back at 7,508 once calm. Every
machine's hashes agree with every other's where both have them (the joiner
with the host on all three boards, 5,963 of 5,963; with player 2, 5,652
of 5,652). The other ten runs of the suite unchanged.

The window (`tools/onepc-window-test.ps1 -OneBoard leave`): both windows
on one board, the joiner's quitting at 5,500 (`--frames` in such a game
starts its leaving 35 frames before the end): its state at 5,496 to the
host window, run on whole there; then a gamerun joining in progress got
every board at 5,700 and took the seat at 5,741, no hash check differing
to 6,700 (they compare the host's own board only: a board it doesn't run
on side runs isn't hashed).

### Shadows that come late (2026-11-03)

The owner's report: in the window (one board, rollback) the host froze
for good right after the joiner's coin, and the coin's shadow made every
game stutter. The freeze: a board on side runs waiting for its shadow
(Game::shadow_wait) took a rollback's frame as the last one run, a frame
ahead of the staying board from then on; the shadow taken after it was
loaded a frame early, its link two packets past the staying board's, and
both waited for ever (game logic +0, no packets). Fixed: f = rewound - 1.

The stutter was the shadow's state: saved (35 ms) and packed (110 ms,
27 MB to 5.3 MB) on the asked machine's game thread, waited for here
(~500 ms on one PC; seconds over the internet). Now, with rollback:
- Late loads. The frame asked for is pinned when asked (kept as it is).
  At it, a state not come yet isn't waited for: the game runs on, the
  cabinet on side runs (`not come yet`); once it comes, every board is
  sent back to that frame (InputTable::run_again_from, as a guess found
  wrong), the shadow loaded there as the boards meet again, the frames
  since run again quietly, the next frame's inputs waited for (nothing
  gone back to the load's frame again: the ring's part of it is from
  before the load). Shadows asked for one frame load there together or
  wait together (a frame with one loaded can't be gone back to for
  another); each decides one at a time, marked loading before it takes
  its state (those come are counted from what has come: one taken out
  for loading still counts). Come too late to go back (the window less
  20 frames), waited for there.
- A machine asked for its board's state takes it only for a frame it
  won't run again (its inputs in, no guess wrong) and while no shadow of
  its own waits late for an earlier frame (it goes back there: its
  first run of this frame had that cabinet on side runs).
- The state is the board's backup of that frame (tools::backup::Maker):
  the pages written since its last backup, sent to every machine by the
  backup thread (packed there), then "my backup" (pack_remote with no
  state) to those who asked; the receiver keeps the copy as that note
  comes (another backup may follow at once) and loads it as a backup
  replay does. ~700 KB unpacked at a coin. A player leaving still sends
  the whole state (its process ends after it).
- Backups with rollback: taken at the first settled frame once 120 have
  passed (only on multiples of 120, they almost stopped); and they
  compare pages with their copy: the memory's record of writes is read
  once and starts again, rollback's snapshots read it every frame, and
  each took the other's pages (a backup's changes missed most; the
  frame's snapshot at a backup missed its).
- The ring: after a rollback's cut is loaded, the packets a port took
  after that point dropped from its recent list (a later rewind gave them
  again: 64 stale packets, the loaded board wrong from its first frame).

Proved against a reference (linked boards in one process, every frame's
RAM hash): 2 seats (either coin, the state on time or held back 15 or 40
frames, ONEBOARD_LATE_TEST), 3 and 4 seats (a coin after the hand-backs:
6 and 12 shadows, all loaded); onepc-tests and onepc-net-tests the same.
The window: ~51 fps for a second at the coin (was ~0.5 s frozen). The
old ways for comparison: ONEBOARD_WAIT_SHADOWS=1, ONEBOARD_WHOLE_STATES=1
(gamerun).

### Side runs on other cores (2026-11-04)

A board on its own runs the other cabinets' tasks (side runs) on worker
threads: at the mode dispatch (0x492AC) each other cabinet's flow, tasks
and own-view test become a job, handed out in turn to the workers; the
main board goes on and waits for them (settle) only where it needs their
results: its own view test, a skipped link, the next dispatch, the link's
run in meet(), the end of the frame and every public call. Each worker
has its own m3::Bus (the same ROMs) and recomp::Context (Context::follow
copies the CPU, clock and return stack). Its RAM is brought up on its own
thread from a shared clean copy, kept from the pages the main board wrote
(watched::take_written with a reader of its own: the OS record now has
several readers). The worker's devices are copied (Bus::mirror_devices)
and its network board is a frozen copy of the comm RAM and I/O
(NetBoard::host_comm_view). A side run's writes are marked by the
worker's bus (Bus::mark_pages, a byte per 4 KB page in write8/16/32) and
put back from the clean copy: no journal and no OS record on a worker's
RAM (that record's page faults made it slow on Windows). The counts each
worker keeps are merged at settle in worker order, so reports match.

Workers: min(other cabinets, cores - 2, at least 1); none on one core.
SPIKEOUT_SIDE_WORKERS=N forces a count, SPIKEOUT_SIDE_THREAD=0 runs side
runs on the board's thread as before; SPIKEOUT_SIDE_CHECK=1 also runs
each job serially on a copy and compares, SPIKEOUT_SIDE_INLINE=1 runs the
workers' jobs on the main thread (debugging). gamerun reports the
workers' time, the board's wait and the hand-over.

Measured (mash fight, 1,490 frames, the board's thread): 9.7-10.4 s with
serial side runs, 7.3 s with a worker (wait 0.19 s, hand-over 0.28 s);
four players on one PC (four processes), each board's wait for side runs
5.5 s with one worker, 1.1 s with three. Proved: onepc-tests 18/18,
onepc-net-tests 14/14, the rollback test, every-board runs with 2 and 3
players: the same, every frame.

The network board's 68000 also skips its wait loops (CommBoard::Cpu::
skip_waits: `tst.b $a.w; beq.s` at 0xE8C, 0x1198, 0x11A0 and the counted
loop at 0xC52; one round run, then whole rounds up to the slice's stop
added to the clock and counters). Exact (two-board states and sound the
same), small (~0.03 ms a frame). Off: --no-net-idle-skip (or
--no-idle-skip, which turns off both).

## Spectators (2026-11-04)

The owner's choices: up to 4 a game (`net::kMaxWatchers`), about 2 s
behind the players, Watch in the game list and by code, an eye and the
spectators' count after the room's line (in the room and in the game) and
after a listed game's name; never slowing the players; every board or one board
chosen by the device's power (only every board so far: below).

A spectator talks to the room's host only (`Session::watch`): its join
says so, and the host gives it a number, not a seat (it sends as
`kWatcherBase` + its number). The host sends it every seat's inputs as far
as it has them (its own as they're set, the others' as they come: each
frame complete only once every player's is in), the start, and, a game in
progress, every board's state as a late joiner gets it; its inputs then
start after that frame. The players never wait for a spectator, never
count it (drops, the authority, hashes), and never hear from it. The
spectator gives each frame's inputs out only once every seat's are in
`net::kWatchLag` (120) frames beyond it: a late input never stalls its
picture, and it runs on inputs all in (no rollback, no one board here).
Every second the host sends its spectators every seat as it sees it
(`kWatchInfo`: names, countries, round trips, each machine's mode) for
their status list, and tells the players how many watch (`kWatchers`).

A host change: the host's `kWatchers` names each spectator and where it
can be reached, so every player has the list. The player that comes to
host the room (`set_idle`'s host migration) takes them over and greets
each (`kWatchWelcome` again); a spectator takes inputs from it from then
on, from where it is (nothing sent until it says how far it has them).
A spectator whose host has gone waits 10 s for a new one (the players drop
a silent host after 3), then its game ends ("THE GAME YOU WATCHED ENDED").
On the host's goodbye a spectator runs every frame complete so far.

The window: Network play > Join a game: a listed game (nearby or the
lobby's; full ones too) opens Join / Watch; Watch by code next to Join by
code. Watching, Left / Right (a pad's shoulders, 1-4 for a seat) change
whose screen is shown: each board takes it at its next frame's start (its
texture RAM taken afresh). The sound goes on from the board shown before
(its MIDI then the new board's): a moment of the other player's sound, the
music the same in a linked game. The status list: "WATCHING · HOST ...
· ROOM ...  (eye) N", the watched seat yellow; at the bottom for 5 s, once
the game is past its boot screens (frame 600) and after each change, a
banner: "WATCHING KENJI (2P)  LEFT RIGHT / LB RB  another player" (the
owner's pick of the mockups; `ui::Menu::set_watch_banner`). The eye is in
the platform icons' sheet (tools/platform_icons.cpp). The lobby lists full games last
(to watch), with their spectators' count, and lets a spectator join a
full game by code (`watch: true`).

What it costs the players: the host sends ~4 bytes a seat a frame to each
spectator, and saves every board's state once for one that comes into a
game in progress (as for a late joiner: in one board a PC, every board
made whole first, `Game::want_whole`).

Proved (`tools/spectate-tests.ps1`, gamerun `--net-join NAME
--net-spectate`; two players running every board, the spectator's boards
against the host's, every frame): in the room before the start, 7,600
frames the same; coming 15 s into the game, every frame from then the same;
the host gone at frame 2,500 without a word (`--net-vanish-at`), or saying
goodbye (`--net-leave-at`): 7,600 the same, through the other player as the
new host. (Found on the way: `--net-leave-at` hung in every-board runs, the
leaver's other boards waiting for its inputs: every board there now stops
at its parting frame, `g_parted_after`.) The window, two windows
(`tools/spectate-window-test.ps1`): watched from the room, seats switched.
### Spectators on one board (2026-11-04)

A device too slow for every board (Auto, as a player's: frames over 97% of
the budget; `--net-one-board` forces it) runs only the cabinet it watches
whole; the others are made from their players' buttons, and where that
can't follow (a coin, a time over) a cabinet's shadow is its player's
backup run on here to the frame (`backup::Replay`, as for a player gone),
not its machine's state asked for: the players never do anything for a
spectator but send what they already make. The owner's chosen option and
rules:
- Only a spectator on one board gets backups: it tells the host
  (`Session::set_watch_one_board`, kWatchMode), the host's kWatchers marks
  it, and each player sends its own board's backups to it too
  (`Session::backup_watchers`: kWatcherBase + its number as send_backup's
  seat; the host on its Watcher channel, the others on a channel of their
  own, `Impl::Bulk`, the spectator opening its router to each with
  datagrams to the addresses kWatchInfo gives). Back on every board: none.
- Spectator traffic goes last and within a share: one rate for all of a
  machine's channels to spectators (`Impl::Cap`, from 256 KB/s, 25% more
  each second up to 4 MB/s), halved and paused a second as soon as a
  player's round trip rises 40 ms above its least (`guard_spectator_cap`):
  the inputs never wait behind it.
- A spectator runs about 2 s behind the players, whose backups are taken
  live: it keeps each board's last three (`Keeper::set_keep`) and runs on
  the newest at or before the frame (`Keeper::copy_for`); none: said, the
  shadow not had (the module goes on as when a state doesn't come).

Switching players on one board (step 2, the owner's choice: built after
one board itself). Left / Right ask for it (`Shared::switch_to`; one at a
time); the banner says "SWITCHING TO KENJI (2P)...". The watched board's
thread then takes the module's "every board again" path
(`Game::keep_whole`, `one_board_back`): every other cabinet run whole, as a
shadow from its player's backup run on here; at the meeting where the
module is torn down, the new player's board, whole now, is shown, the
module's last set-up let go (`Game::again`: each board's seeds, flags and
the module's shadow state) and one board set up again around it
(`one_board_wanted`: the one watched before goes, held whole until calm,
then on side runs). Three windows (tools/spectate-window-test.ps1 -Mode
oneboard): Right while the other cabinet was on side runs (frame 5,880):
its backup of 5,880 run on to 5,893 in 0.1 s (131 ms waited), one board
around player 2 from 5,893; player 1's cabinet handed to side runs at
6,405, its backup run on at the host's coin (6,720 to 6,771), handed back
at 7,105. Right before the first hand-over (every cabinet still whole):
the same at once.

Proved (`tools/spectate-tests.ps1 -Only oneboard,oneboard2`; gamerun
`--pace`, new: the players at the arcade's speed, as windows, else they run
far ahead of the spectator and its backups): watching seat 1, then seat 2,
the other cabinet held whole until calm (as the window), handed to side
runs at 4,621, the other player's coin at 6,100: its shadow its player's
backup of frame 6,120 run on to 6,130 in 0.1 s, handed back; the watched
board the same as the players', every frame of 7,600. The window: a
spectator forced to one board (two windows), the host told; Right refused
with the note.

## The first tests over the internet (2026-11-05)

Every test before was on one PC or a LAN. Two GitHub Codespaces (Azure,
UK South and Central India; .devcontainer, tools/codespace.sh: gamerun
built there from the ROM set, headless) and this PC (a home connection in
the UK) made three networks: 2, 3 and 4 players (the 4th a second player on
the India machine), each machine every board, rollback 12, at the arcade's
pace, then a spectator, then the window joining by its code.

What held: every run the same on every machine (0 hash checks differing),
direct and through the relay. This PC reached Azure directly in both places
(UK 10 ms, India 144 ms round trips); everyone forced through Cloudflare's
relay (relay to relay) 14 and 162 ms. Two players on one machine's network
found each other as LAN (1 ms). In play, once everyone was in: 0-8 frames
in a run over 1.5 frames from the last, 0-23 rollbacks at 144 ms, never
waiting for inputs. A spectator in India: the same, no stutters. The
window chose rollback 12 and delay 2 by itself (the furthest 72 ms away).

Found and fixed:
- The relay after a run that ended without releasing its allocation: a
  cloud machine's router gives every run the same public address and port,
  so the next run's allocation was refused (437) for 10 minutes. The
  allocation is released on leaving (a refresh with lifetime 0), and the
  next of the relay's addresses (Cloudflare's 3478, then 53) tried on a
  refusal (TurnClient's servers).
- SPIKEOUT_TEST_RELAY_ONLY sent straight from the socket but took in only
  what came through its relay: a direct send to the other's relay had its
  answer, straight back from that relay, refused. Now it sends through its
  relay too (every exchange relay to relay, as behind the strictest routers).
- gamerun, the test tool: hosting through the lobby it stopped reading it
  once the game started (--net-start-alone), so nobody could join on the
  internet (on one PC a joiner's datagrams reached it anyway); now it reads
  it the whole run. --net-catch-up joins as the window does (unpaced until
  within 4 frames of the others, then the cabinet; tools/codespace.sh join
  uses it): taking the seat at once held the host 2-3 s. Its time sync
  when paced is the window's (a little slower or sooner each frame), not a
  wait until close, which made stutters; it doesn't sleep the last 2 ms on
  Windows (its sleeps end up to 15 ms late). The end says each player's
  link, the pace (stutters: over 1.5 frames from the last, and the longest
  gap), and how often a frame waited over a frame's time for inputs;
  SPIKEOUT_NET_DEBUG a line each 300 frames.

Then found (the packed state is about 3 MB a cabinet in play, 9.3 MB with
three: fixed in size, not growing with the game's length) and fixed:
1. The window joining in progress stood still 13-21 s after catching up
   (everyone held, its inputs stopped at the frame it took its cabinet
   from). A frame caught up is "unseen" and unseen frames skipped pace(),
   so the pacing's start never moved on: the first frame after waited out,
   at the game's speed, every frame caught up. Now pace() runs while
   catching up (it only moves its start then). On one PC the catch-up was
   short (70 frames, ~1 s): it passed for a join's hitch.
   What looked like the window going out of step was the tests' own:
   their --poke wrote seat numbers (1, 2, 3) into the link role (0x10019E:
   1 master, 2 slave), which the window writes itself over a cabinet's
   first 3,000 frames (kLinkSettingFrames): a window joining before then
   differed in that byte. gamerun now writes it as the window does in
   network play (no --poke needed).
2. The host's hold at each join or spectator (0.34-0.63 s on a Codespace
   host, more cabinets more): the state's packing, and putting the boards'
   states together, moved to NetPlay's backup thread (send_state_later,
   LinkedState::parts_sink); each board's save made with its room at once
   (kBoardStateBytes: grown, it was copied over and over: 40-60 ms, now
   18-20); and, with rollback, no wait for a far player's inputs at the
   frame taken (0.12-0.13 s): the state is armed, the game paced 0.3 of a
   frame slower each frame, and taken once every player's inputs are here
   past it (players_inputs_past; after 2 s as before). Measured on the UK
   Codespace, 3 cabinets, India 140 ms away: the longest gap at a join
   24 ms. On the way: a host with rollback and no input delay stopped for
   good at a join (the window as a one-board host, alone at first): it
   waited for every board's inputs of the frame before setting its own,
   and the empty cabinets' (the authority's fill) come with them. The
   capture is decided after our own inputs now.
3. A player gone without a goodbye held everyone 2.8 s (the drop's 3 s
   less rollback's 12 guessed frames). Dropping after 1 s was tried and
   undone: a machine only stalled (four games flat out on one PC, each
   running a board on from its backup at once) was dropped and went out of
   step (onepc-net-tests vanish3). Instead (the owner's ask, "no freeze"):
   the game plays on. The session tells, from its own thread, when a
   player's machine has been quiet 150 ms (kQuietAfter, Callbacks::quiet ->
   InputTable::set_quiet); a quiet player's inputs are guessed as far as
   rollback's long history (tools::rollback::kWindow, 210 frames, as memory
   allows: quiet_ahead), held 8 frames then none, as the drop at 3 s will
   have them (so a gone player's frames need nothing run again); a heard
   one's still only 12 ahead. Measured (two gamerun, 40 ms apart, rollback):
   the host's longest gap when the other vanished 2,794 ms -> 93 ms; a
   player silent 1.5 s while pressing buttons, then back: the host went
   back 13 times, 123 frames run again, its longest gap 76 ms, nothing out
   of step (the silent player's own game held 0.9 s). Test switch:
   SPIKEOUT_TEST_SILENT=SECONDS,MS (nothing sent for MS ms then).
   Cost of the long history (6,000 frames, two players, flat out): CPU
   84.7 -> 91.3 s (+8%), each process's peak memory 500 -> 608 MB, the
   kept frames 36 -> 141 MB (two boards; about 70 MB a board).
4. Rollback's snapshots lazy (Linked::wants): a frame kept only while a
   guess is outstanding or a wrong one is still to be gone back for, or
   when the next frame's inputs aren't all in (the boards this machine
   fills itself aside: Session::fills_inputs, Linked::set_local), pinned
   frames, and every 32nd (kAlways: a guess not foreseen goes back no
   further). Decided once a frame, the first board to ask for all
   (InputTable::settled_but). With an input delay of 1 or more, at the
   arcade's pace: a player's machine kept 419 of 2,999 frames (86% not
   needed); at no input delay (a LAN's) nothing can be skipped: the next
   frame's inputs are never in before it. ROLLBACK_EVERY_FRAME_SAVE=1:
   every frame, as before.
5. A snapshot's cost (ROLLBACK_SAVE_PROFILE=1: where its time goes): the
   small state 0.04 ms, the written pages asked of the system 0.185 ms,
   their copies 0.15 ms (~110 pages, twice each), the thinning 0.03 ms.
   The pages asked: each memory block its own lock now (every board saved
   at once queued on one), and with one reader the system's list as it is
   (watched::take_written): 0.185 -> 0.140 ms; a save ~10% cheaper.
6. leave3 failed now and then (also with the build before these): when
   the leaver had joined before player 2 (seat 2: -LeaverFirst does that),
   its board, run on from its backup on each other machine, was taken
   whole from the frame each noticed the player gone, a frame apart, and
   the boards' network boards (the 68000's registers and some of its RAM;
   GAMERUN_STATE_PARTS, GAMERUN_NETBOARD_DUMP) then differed, the main RAM
   the same. Now taken whole kGoneHandOver (600) frames after its inputs
   end (cb.idle's frame, agreed: NetPlay::take_gone gives it), on every
   machine (later only if this one noticed too late, said).
The internet tests as a script (2026-11-06): tools/internet-tests.sh drives
the two Codespaces and this PC (this checkout's changed sources synced and
built there first; one run at a time; every Codespace command time-limited;
the Codespaces stopped at the end): two, relay, three, four, vanish, stall,
spectator, spectators (four at once, a fifth turned away), oneboard. Every
machine "0 hash checks differed" and the joiners in, the worst gaps
reported (vanish and stall: none over 300-500 ms allowed). The first run
(before committing the work above): all passed; the longest gaps 37-96 ms;
four spectators each the same, the players' longest gap 36 ms as they came,
a fifth "the host refused: 4 are watching already". Found by it: a joiner
on one board from the start (gamerun --net-one-board 100, at frame 1309)
caught up at 28 frames a second (the others' boards made by side runs) and
took its cabinet "not gaining", the others held 4.5 s (gamerun's own:
fixed 2026-11-06, below).

One board: a one-board player joining, a one-board host with two joiners,
players leaving it and one vanishing: the same, nothing out of step, no
hold at the joins. Left: a joiner whose PC can't run every board well
over the game's speed (a Codespace: 1.6x for three) doesn't catch up a
long state's gap within 5 s, takes its cabinet anyway, and the others wait
for it to come up to that frame (4.5 s seen). The window's Auto may go to
one board then; it should rather catch up on one board, or wait longer.
Test switches: the window's SPIKEOUT_TEST_JOIN=CODE and SPIKEOUT_TEST_HOST
=SEATS (no menu keys); SPIKEOUT_RAM_DUMP=DIR@FROM-TO (both: each board's
RAM at the hashed frames); SPIKEOUT_NET_DEBUG adds long pacing waits, long
frame ends, a joiner's held inputs, and the host's capture timings.

### The one-board joiner's slowness: found and fixed (2026-11-06)

The "28 frames a second" joiner above was gamerun's own: it handed the
other cabinets over to side runs only at the end of exactly
--net-one-board's frame (100). A joiner starts from the host's state much
later (frame 337 locally, 1306 on the internet), never ended that frame,
and so ran every board whole for good, while the side-run module, finding
their seeds, also ran a side run of each one's tasks every frame beside
them. Those failed (each to its 30M-block budget at the vblank wait's loop
0x36E08, 25-46 ms each, every frame: 1,211 of 1,211 in a local two-player
run). The window was never affected (it hands over at its set-up). Now
gamerun hands over at the first frame's end from that frame on, once
(Machine::one_board_handed). Two players, one board each, on this PC:
joiner 15.6 -> 57.52 frames a second, host 19.4 -> 56.8, no failures;
onepc-tests 14/14 and onepc-net-tests all the same. Internet oneboard
(UK host, India, this PC, three players one board each): this PC's
longest gap 3204 -> 75 ms, every machine the same.

Seen then: when this PC quit at its last frame, the host and India never
heard its goodbye and held kDropInGame (3.1-3.2 s; input delay, no
rollback: they need its inputs). Its reliable goodbye (kBye, pumped three
times as it quits) never came, with only 2 pieces queued (so not the
channel's window). A goodbye is now also a datagram of its own (kGoodbye,
three times; a build without it ignores the type), handled as kBye
(Impl::on_bye, whichever comes first); leave() logs the pieces still
queued. Internet oneboard again: both heard "gamerun left", longest gaps
117 / 172 / 75 ms.

Still open: a player vanishing without a goodbye (a crash, a cut line) in
a game with input delay and no rollback still holds the others
kDropInGame (3 s): only rollback can guess its inputs meanwhile.
Worth doing later: one shared set-up for the window and gamerun (the
hand-over above was gamerun's own copy), so the tests run the window's
code.

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
