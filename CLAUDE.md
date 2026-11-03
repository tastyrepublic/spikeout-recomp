# Notes for working on this repo

- Hardware reference priority: Supermodel first, MAME second. See
  docs/REFERENCES.md. When adapting Supermodel code, keep its copyright
  notice and cite the source file.
- Never commit ROM data, dumps, or anything extracted from them. That
  includes recompiled code: it is generated into build/generated/.
- Build and test: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
  CI builds with warnings as errors on Linux and macOS.

## Latest (2026-11-03, main 7d3b090, pushed)

Read this first; the older hand-off below is history.
- Done and pushed: rollback for one board a PC (340b46f, docs/netplay.md
  "Rollback in play" end: module state kept, switches pinned
  (Snapshots::pin), settled waits, 2-4 players exact); a board ~2.3x faster
  (7d3b090, docs/recompiler.md "Clock and interrupts" and "Bus": the
  decrementer check fix, the idle skip, region table, 2D colour tables,
  DSP decode once, side-run set-up chunked; all proved the same output;
  the hand-back race fixed: one_board::Game::wait_hand_backs).
- Window, two windows on one board (scratchpad-style test: host + joiner
  with SPIKEOUT_MENU_KEYS, --net-one-board --net-rollback 210,
  SPIKEOUT_NET_DEBUG=1 for per-300-frame timings): now full speed, music
  fine. OPEN, window only (gamerun's coin test is fine): after a shadow
  (a coin at 6100) the host logs "board 1 handed back to side runs" every
  300 frames but the shadow never hands back (stays whole: correct, just
  costlier); one "rollback: the boards saved different frames (board 0
  6129, board 1 6130)" at the shadow's load (benign, once).
  OWNER REPORTS (2026-11-03, the w2w run, build 7d3b090): after the coin,
  one of the two windows HANGS. Not yet explained: both logs
  (build/onepc/window/w2w-host.txt, w2w-join.txt; SPIKEOUT_NET_DEBUG's
  "frames run, shown a second" lines) show 57.5 run and shown after the
  coin but for ~1 s at 11-13 fps at the shadow's load (~6120), and both
  ran to 7500. So the hang may be the picture/presentation, the window's
  message loop, or the hand-back loop above, not the boards. FIRST TASK:
  reproduce with the owner watching, note which window, and look at the
  window's present/menu threads (gdb the frozen window: only safe once it
  is already stuck; pausing a live one makes the other drop it).
- Proof tools: gamerun GAMERUN_2D_HASH=1 (2D layers each frame),
  GAMERUN_STATE_PARTS=1, GAMERUN_NETBOARD_DUMP=DIR (network board state
  files at whole-state frames), GAMERUN_FLUSH=1, ONEBOARD_CALM=N,
  ROLLBACK_FREE_MB=N; SPIKEOUT_TEST_HOLD_INPUTS=N (window), gamerun
  --net-hold-inputs N (both now by time); tools/onepc-tests.ps1 and
  tools/onepc-net-tests.ps1 (all pass; leave's host margin 10,000 frames).
- Build lesson: PowerShell Copy-Item keeps file times; copying a header
  back left objects built against an old struct (sound broke). Touch files
  after copying.
- Next, owner's order: more single-board speed is mostly spread over the
  game code now (sound voices on the audio thread ~2 ms; 68000 network
  board idle skip maybe); then side runs on a second core (owner: one
  board must use 2+ cores; a worker needs its own Context and a Bus copy;
  Bus::held_reads counts the devices side runs read); then the window's
  hand-back issue above; the owner's internet test; stage/scene/ending
  tests with 2-4 players (owner asked).

## Hand-off (state as of 2026-10-24)

How the owner works: asks for each commit and push explicitly ("commit and
push, then X"); wants fidelity to real Model 3 hardware (Supermodel first,
then MAME, then datasheets/manuals; change behaviour only with evidence,
keep the old behaviour selectable, enhancements default off);
docs/fidelity.md is the backlog and records every such decision. Prove
"same output" claims with hashes (gamerun's GPU hash, replays, states).
Keep the game's draw order on the GPU (grouping by kind was declined; the
SPIKEOUT_BATCH_KINDS experiment in real3d_gpu.cpp is the fallback). A
decompilation may come later and must never be committed.

This machine (Windows):
- Toolchain: WinLibs GCC + CMake + Ninja (+ gdb) from winget. In PowerShell,
  refresh PATH first: `$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')`.
  Executables need that PATH (run them from PowerShell, not Git Bash). No Python.
- Linux: WSL 3.0.3, Ubuntu 24.04 (kernel 6.18: PAGEMAP_SCAN), CI's build packages installed
  (2026-10-27). A copy of the sources at ~/spikeout-recomp (copied with tar
  from the worktree, without build/ and .git; touch what changed after a
  build ran), built as CI does; ROMs read from /mnt/z/Emu/spikeout-recomp/
  build/roms/spikeofe. Don't build on both at once: 16 GB runs out.
- ROMs: `build/roms/spikeofe` (unpacked from `Z:\Emu\Sega Supermodel 3\ROMs\spikeofe.zip`;
  git-ignored). `SPIKEOUT_ROM_DIR` in build/CMakeCache.txt points there.
- References outside the repo: `Z:\Emu\refs` (Supermodel and MAME sources,
  the MPC106 and SCSP manuals, comparison scripts in `refs/tools`).
  vf3-recomp (a sibling project, SDL3 + RmlUi + SDL_GPU, JIT): github.com/mr-finster/vf3-recomp.
- A lifter change regenerates build/generated/ only once ppcrecomp is rebuilt
  (`cmake --build build` does it; a full rebuild takes ~2 min).
- Window: `build\spikeout.exe build\roms\spikeofe` (Esc menu; GPU 3D;
  `--record FILE`, `--load-state FILE`, F9 save state, F10 load the last,
  F12 screenshot, `--frames N` quits after N). Owner's PC: GTX 1080,
  2560x1440 at 99.9 Hz, render scale 4, picture shape = window's (16:9).
  Driving it from a script: keybd_event with KEYEVENTF_SCANCODE (flags 8/10)
  after SetForegroundWindow; presses are sometimes lost, so check the output.
  stdout only shows if the window exits by itself (--frames or WM_CLOSE).
- Headless: `build\gamerun.exe build\roms\spikeofe --blocks N` (see the
  file's header: `--press`, `--poke`, `--wav`, `--screenshot`/`--shot-every`,
  `--text`, `--replay FILE`, `--save-state FILE@FRAME`, `--load-state FILE`,
  `--renderer gpu --gpu-scale N` (prints a hash of every GPU picture),
  `--gpu-time`, `--wide 93`, `--interp`). Game: ~4.3 cycles a block,
  ~670k blocks a frame (38M/s for real time); attract runs ~90M blocks/s with sound, ~117M without.
- Shell edits with sed/perl mangle `\n` in C strings; use the Edit tool.

Where it stands: attract mode, gameplay, sound, music, linked boards (in
one process, turns or parallel), the GPU renderer (draw-order exact, ~2-3 ms
a frame at scale 4), the settings menu and widescreen all work.
- Save states (2026-10-24): m3::State walks each device (one function for
  save and load; struct padding refused at compile time, so equal states are
  equal bytes); resumed at any block (lifter kBlocks tables,
  Context::resume_block; the snapshot is taken at the first block outside an
  exception after a frame ends). Verified: a state loaded at frame 2000 gives
  the same state at 3000, byte for byte, same sound and pictures. minimp3 was
  patched to clear its scratch (its stack garbage made states differ).
  Linked boards too (2026-10-25, gamerun --boards N: one file, the ring's
  packets on their way cut between the boards' points). Not yet: the
  window's linked boards in a state; a recording from a state.
- Online/local play: Mode B (every PC runs every board, inputs only),
  delay-based, works (2026-10-25, uncommitted when written): src/net/ (UDP,
  LAN discovery, sessions, STUN/TURN, the lobby client over libcurl),
  tools/netplay.hpp, gamerun --net-host/--net-join (+ --lobby URL), the
  window's Network play menu; the lobby is a Cloudflare Worker in lobby/
  (`npm test` runs it here). Deployed 2026-10-26 (wrangler logged in on
  this PC; `npx wrangler deploy` in lobby/) at
  wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby, the build's
  SPIKEOUT_LOBBY_URL; its TURN key secrets set (the relay works, tested
  with SPIKEOUT_TEST_RELAY_ONLY=1 on a host). Not yet: a test between two
  real networks (the owner's second machine or a phone hotspot). Also
  working: a host starting alone, players joining a game in progress,
  machines out of step put back from the host's state. Mode A
  (a "Cable link" menu, Ethernet) comes after Mode B. docs/netplay.md has
  the design, proofs and what's left.
- LINKED SPEED (2026-10-26): linked cabinets ran in slow motion, in gamerun
  and the window, network play or not: 2/3 speed on 2 cabinets, 1/2 on 3,
  2/5 on 4 (a cabinet's game skips a frame when another's frame count
  arrives too old). The CommBoard defaults tried for it (3 ticks, 20 MB/s)
  were reverted: they only fooled the logic counter (blank screens). Never
  judge speed by 0x1000A8 alone; count new pictures (gamerun
  --renderer gpu --gpu-scale 1 --gpu-hash-frames --screenshot X.png).
  Fixed with the ring model, the default since 2026-10-26 (owner's
  decision; slaves forward the master's frame, both banks a double
  buffer, a slave's tick only from 0xE0): every board at full speed on
  2-4 cabinets, by pictures; `--net-old-link` (gamerun and the window)
  brings the old link back. The network board's options are in
  tools::build_id, so machines that differ can't join each other.
  Timeline tool: gamerun --net-trace N --net-host-trace FRAME:N;
  firmware: --net-disasm FILE.
  Built since (committed): the network status display, the new Join by
  code page, goodbyes on every normal exit (all in docs/netplay.md); then
  a player name setting, and gamerun's RAM starting at zero as the
  window's (it was 0xA5: same pictures and sound, but network play
  compares all of RAM). Then (all committed, 2026-10-27): the reorganized
  Network play menu; lobby list pages with the host's country; rollback
  step 1 (m3::Snapshots, each OS's own page tracking, proved by gamerun
  --rollback-test); the Linux lobby fix (curl built when the system's
  can't do wss); the room shown while in it (ROOM line, "Your room");
  the host closing a room before the start takes the others out; host
  migration in a game in progress (the lobby keeps the code: deployed).
  All in docs/netplay.md. The owner's report (2026-10-27: after a few
  host changes a joiner can't catch up, cabinets' fps differ, its buttons
  sometimes dead): two faults found and fixed (2026-10-28;
  docs/netplay.md, "Repeated host changes: found and fixed"): the cabinet
  request (kTake) now asked again until handed over (unit test proves it,
  Session::vanish), and the window catches up to the furthest of the
  other seats' inputs. The gamerun deep test (deep-test.ps1 in
  build\speedcheck\scripts; mini-migrate.ps1 one change then a join) and
  the three-window test (three-windows.ps1: two host changes, a rejoin,
  keys held) pass. Still to do: the owner's own test over the internet.
  Also fixed then (docs/fidelity.md): the 3D's last screen column (the
  viewport 495.75 wide, drawn at that scale over all 496 pixels: a line of
  the 2D at the right edge before); the picture shape changed with the
  game paused (the frame made again for it); the Speakers setting crashing
  the window (page helpers referenced after the page was made); the
  ranking after a game over (its fade continued, and the layer clear
  0x795c hooked: the title's columns had stayed beside every screen after
  it). The window logs the menu opening and closing and picture shape
  changes, with the frame (for reports). Then: rollback step 2 (re-runs that draw and
  play nothing); the two-network test; the late-join music (the owner
  decides); spectators, Auto delay during play, Mode A (the owner's
  "direct connect": a cable link, decided later).
  What's known and tried: docs/fidelity.md, "Linked games ran in slow motion". Window
  debug: SPIKEOUT_NET_DEBUG=1 prints each board's logic, inputs and
  network board every 300 frames; SPIKEOUT_MENU_KEYS="esc,down,enter,
  right,down,down,enter,enter" hosts a 3-cabinet game alone (F13 in the
  list waits a quarter second; the keys go on through a network game).
- Record/replay: spikeout --record, gamerun --replay (66/66 hashes).
- Smooth motion (frame interpolation, m3::interpolate, spikeout --smooth /
  menu setting, default OFF): ghosts remain (tower light, car wheels, gate,
  sea birds, sea level); to be revisited with replays and states.
- Widescreen, title doors (between character select and the map) and the
  title's strips after attract (uncommitted when written): their sides now
  come from the game's own tile map (map 0x53, the whole title; each door
  is a part of it drawn at the screen's edge, never clipped). Hooks in the
  recompiled code (m3::GameSpec::hooks, lifter emits c.hook(pc);
  games/spikeofe_hooks.cpp: draw 0x9628, fill 0x7e34, copy 0x8230) keep
  m3::TileSides (12 columns each side, in save states); the tile
  generator draws those rows into bottom_own/top_own, and WideLayer puts
  them over its usual sides. Only rows whose edge tile is the row's
  background (band, logo, black bar) carry their edge tile; the riveted
  wall (map 0x53's rows 0-10, 37-47, 62 different tiles a row) repeats the
  map's own columns every 31 (owner's choice from previews, uncommitted
  when written): it slides with the doors, the leaving bands stay
  widened, and the still title's wall sides changed (owner approved). The faces layer on the map screen pans (line
  scroll) and its sides wrap the name table: the left side shows the
  photo's dark edge (the "left side goes black"), the right a repeated
  face; not addressed. Repro state: build/spikeout-819.state (the owner's
  818 converted; states from before the side table don't load): `gamerun
  --load-state build\spikeout-819.state --blocks 400000000 --renderer gpu
  --wide 93 --screenshot X.png --shot-every 4` (doors close ~1061-1071,
  open ~1204-1214). The layer buffers: 0x188000 + n * 0x2000, 64 entries a
  row; descriptor +8 columns (64), +10 rows, +16 buffer, +20 name table.
- Widescreen, linking screen (a player joining mid-game, both cabinets'
  "THE NEW TEAM" screen; uncommitted when written): the marquee runs full
  width (TileSides' pass draws the other layers too, rows marked only where
  a tile exists), and the picture's left-edge spotlight fade is continued,
  easing into black (maps 0x39-0x3c, TileSides::kFade, WideLayer::fade;
  owner chose this over new art). Repro: `gamerun --boards 2 --parallel
  --poke 0:10019E=1@0:5000 --poke 1:10019E=2@0:5000 --press 0:coin@1200
  --press 0:start@1300 --press 0:beat@1700 --press 1:coin@2600 --press
  1:start@2700 --press 1:beat@3300 --press 1:beat@3650 --blocks 3100000000
  --renderer gpu --wide 93 --screenshot X.png --shot-every 50` (board 1
  ~4100-4450 on main's link model). gamerun --dump-layers also writes the
  tile generator's own sides (-sideabove/-sidebelow, -ownabove/-ownbelow,
  marked rows red).
- Smooth motion is no longer read from or saved to settings.ini (only
  `--smooth`), as it's still being tested.
- Main is the LAN branch's line since 2026-10-28 (claude/lan-play-f60b31
  fast-forwarded onto main after the host-change fixes; single-player
  checked the same: sound, tile RAM and backup RAM byte for byte over
  boot, select, map and play). Work goes on on main.
- The bot (2026-10-25, uncommitted when written; paused at the owner's
  request): tools/spikebot.cpp, docs/bot.md (how it plays, the RAM it
  reads, the coast road, the longplay's stages, the player's guide, what it
  costs). It clears stage 1 and reaches stage 2's coast road enemies; ~13
  frames a second while fighting (3x faster than at first; same play,
  frame for frame). Checkpoint states in build/bot/ (git-ignored); the
  owner's F9 states in the repo root are not to be committed. Next for it:
  a run to the end with no frame limit (8-12 hours estimated), fixing where
  it stops and going on from its last state; running and charge levels not
  used yet.
- Rollback steps 2-3 done (2026-10-28, docs/netplay.md): linked boards
  going back together (gamerun --rollback-test on --boards N --parallel),
  quiet re-runs (:quiet, :timed). Then the owner's goal changed to ONE
  BOARD PER PC (tablets): proved that a remote cabinet can be stood in for
  by its network board and the 0x900-byte block its main board writes each
  frame, and that block rebuilt byte for byte from this board alone plus
  the remote's buttons, by running the remote player's camera with the
  game's own code on the side (gamerun --camera-test; docs/netplay.md,
  "What the cabinets send each other"). Open: the game's camera events
  (script commands writing straight into the camera, e.g. 0x5D21C at a
  gate): shared ones are copied (--camera-test-mirror), per-player ones
  (0x345EC so far) to work out; --camera-events lists and sorts them
  (shared / each board's own) in any two-player run. Needed: two-player
  runs reaching every stage.
- The window's F9 in network play saves every cabinet
  (spikeout-net-<frame>.state; gamerun --boards N --parallel --load-state
  runs on from it, player 2 joined by --press). The owner's save at stage
  1's gate: spikeout-net-4387.state (repo root, never committed);
  build/linkstudy/joined-6250.state is both players in, at the gate.
- The bot's run to the end replayed (2026-10-29): spikebot
  --record-inputs FILE (tools/input_log.hpp: each frame's buttons and the
  bot's RAM writes, a RAM hash every 60 frames), gamerun --load-state S
  --inputs FILE (every hash matched over 4 segments: docs/netplay.md, "The
  camera events over the whole game"; the segments' states and inputs in
  build/bot and build/linkstudy, git-ignored). Used for: the camera events
  over one player's whole game (docs/netplay.md: the script's 11 camera
  actions, 0x09 used by no script, 0x3F never run, 0x18 seen through its
  helpers 0x37258/0x372D4; the Kind C list), the area scripts run (the
  interpreter task's +16), and widescreen's cut scenes in 16:9: the stage
  cards and Mikhail's eyes get sides of the picture enlarged and blurred
  (TileSides::blurred, WideLayer::blurred_sides; the faces 0x96 scroll and
  are left alone, the owner says that's right); the staff credits
  (TileSides::black, from their set-up's fill at 0xA164) mirror the sky
  on the left, the names' black on the right, the sunset's 3D opened out
  on the left only (a full-height viewport at one edge), and no wider than
  16:9 (21:9's dog popped in); the ranking after them lost the credits'
  scroll fog (docs/fidelity.md; --fog-over-nothing for Supermodel's).
  Checked: seg2/seg3 pictures in 4:3 and 16:9 the same as before but the
  card; stage 1 and seg4 not re-compared.
- Next (a separate session): the camera events still to sort. Kind A's
  0x3F (both its records, 0xB2A64 and 0xB4EA0, in scripts the area task
  never ran; every script listed in the table at 0xA1D2C-0xA2028) and the
  scripts never run (the hidden stages, or set-ups run some other way): an
  observer hook at the action dispatch 0x5AB30 would log every action run. Kind C as shared or per-player,
  with each write's caller (LR, which --camera-events doesn't report yet:
  0x37258/0x372D4 are the script's when called from 0x5D81C), needing
  two-player runs over the whole game (the bot driving two boards). Then
  the owner's internet test of network play; the smooth-motion ghosts.

Owner's decisions for online play: each player on their own cabinet
(board) on their own computer; rollback netcode (Mode B: every PC runs all
the session's boards); games found through LAN discovery or an internet
lobby only, never a direct-IP option; a disconnected player's character
stands idle; spectators wanted and must never slow the players; local play
with no internet too (handhelds over one device's hotspot: Mode A, a board
per device). 4 boards in parallel without the remote boards' 2D and sound
ran ~2.5x real time on the i7-8700K.

Owner's choices on options: operator (game-native) settings yes, PC
conveniences such as skipping the boot or forced free play no (free play is
the game's own COIN/CREDIT setting); "quads drawn whole" on. Game facts: no
lives setting; COMM ID is EEPROM word 15 high byte; region EEPROM word 12
high byte. Owner's PC: mailbox presenting (FIFO gave 33 Hz), the game thread
busy-waits between frames (sleeping let the CPU clock down), every audio
output reports 2 channels ("Two outputs" mode for rear speakers).
