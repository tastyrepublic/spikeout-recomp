# Notes for working on this repo

- Hardware reference priority: Supermodel first, MAME second. See
  docs/REFERENCES.md. When adapting Supermodel code, keep its copyright
  notice and cite the source file.
- Never commit ROM data, dumps, or anything extracted from them. That
  includes recompiled code: it is generated into build/generated/.
- Build and test: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
  CI builds with warnings as errors on Linux and macOS.

## Latest (2026-11-05)

Read this first; the older hand-off below is history.
- RELEASE (owner's decisions): downloads must not contain the recompiled
  game code (made from SEGA's ROM), so the game is translated from the
  player's own ROM at start by an LLVM JIT, and cached. Easiest for
  players; Android too; iOS allows no JIT at all (an interpreter or
  sideloading, later). Measured first: Clang 18's build of the generated
  C++ runs exactly as fast as GCC 13's and gives the same hashes.
- LLVM JIT PROTOTYPE, DONE (2026-11-05): src/jit/ppc_ir.cpp is the
  lifter's second back end (LLVM IR, case for case as src/recomp/
  lifter.cpp: keep the two in step); src/jit/jit.cpp translates the ROM
  (analysis as ppcrecomp, 48 modules on every core, ORC), caches the
  objects (SPIKEOUT_JIT_CACHE, default ./jit-cache/<key>: the ROM, the
  translator version kTranslatorVersion (bump it when the IR changes),
  LLVM, the CPU, the Context/Bus layout) and gives the generated:: tables.
  Context and Bus have a friend (recomp::JitAccess) for the fields the IR
  touches. Windows: SehMemoryManager registers each object's .pdata
  (RtlAddFunctionTable), without which C++ exceptions (rfi, MissingCode)
  can't unwind through JIT code. CMake: -DSPIKEOUT_LLVM_JIT=ON (off by
  default; the C++ build stays the reference and the default) builds
  gamerun-jit (tools/gamerun.cpp, SPIKEOUT_JIT). LLVM (owner's choice:
  built ourselves, the same everywhere): cmake/llvm.cmake downloads LLVM
  18.1.8 (SHA-256 checked) and builds it once with this compiler into
  %LOCALAPPDATA%/spikeout-llvm or ~/.cache/spikeout-llvm (Windows 38 min
  on 6 jobs, 308 MB; WSL 34 min, 250 MB; SPIKEOUT_LLVM_JOBS; GCC 15+
  needs -include cstdint for LLVM 18); SPIKEOUT_LLVM_SYSTEM=ON uses an
  installed one (LLVM_DIR). -O1 is the default (owner: same speed as
  -O2, made ~15% sooner). Env: SPIKEOUT_JIT_THREADS, _OPT, _NO_CACHE,
  _VERIFY, _DUMP (IR per part). Build dir here: build-jit (Release,
  gamerun + gamerun-jit).
  Proved the same, by hashes, JIT against C++ on Windows and Linux, and
  the Windows JIT against the Linux JIT: attract with sound (WAV equal),
  the two-board mash fight, the bot's seg2 replay (11,356 frames, its 189
  hashes), save states (identical files, loaded across);
  tools/onepc-tests.ps1 (18) and onepc-net-tests.ps1 (14) with
  -Exe build-jit/gamerun-jit.exe; network play C++ <-> JIT both ways (WSL).
  Numbers (i7-8700K): first start 13.6 s on 12 threads, 18.1 on 6, 72 on
  1 (analysis + IR under 2 s, the rest LLVM); cached start 0.2 s; runs as
  fast as C++ or 1-5% faster; +43 MB memory; the exe +39 MB stripped
  (Windows 55 vs 16 MB, zipped 21 vs 5). --gc-sections gains nothing
  (PassBuilder names every pass).
  THE WINDOW WITH THE JIT, DONE (2026-11-05): spikeout-jit (CMake, with
  -DSPIKEOUT_LLVM_JIT=ON and the frontend; tools/spikeout.cpp under
  SPIKEOUT_JIT). After the ROM set is read (the picker's zip or a folder),
  jit::prepare runs on a thread; the window shows "PREPARING THE GAME" with
  a bar (jit::Options::progress: the step, parts compiled) only after 0.3 s,
  so a cached start (0.18 s) shows nothing. Closing the window stops it
  (Options::cancel, jit::Cancelled; ~4 s, each worker finishes its part;
  nothing half-written kept). Kept in the data folder's jit-cache, or
  settings.ini jit_cache (Game > ROM set: "Translated game" its size,
  "Choose its folder", used from the next start), or SPIKEOUT_JIT_CACHE
  when the setting is empty; Options::prune deletes the folder's other
  translations (only 16-hex-digit folders holding nothing but part*.o and
  complete). Error: a message box. Measured: first start 16 s on 12
  threads (12.4 MB), the window's recording replayed by the C++ gamerun:
  30/30 hashes. Build dir in the lan-play worktree: build-jit (window +
  gamerun-jit, test folder build-jit\wtest with portable.txt).
  Also then (2026-11-05): the app's icon, the owner's "SO" picture
  (assets/icon, made by tools/make-icons.ps1 -Source PICTURE: icon.png
  1024, icon-256.png built into the window and set with SDL_SetWindowIcon,
  spikeout.ico in the Windows .exe by spikeout.rc, spikeout.icns and
  spikeout.desktop for macOS/Linux packages, not used yet; CMake
  spikeout_window_app). The window is a Windows GUI program now (no
  console window; main() attaches to the console it was started from,
  and a script's redirected output still arrives). Quads drawn whole are
  on by default (owner, 2026-11-05; settings.ini keeps a saved choice).
  CI's jit job written (.github/workflows/ci.yml: Linux and Windows,
  LLVM in actions/cache keyed by the runner image; macOS left for (3):
  its minutes cost 10x on a private repo); not run yet. Its Linux build
  tried in WSL as CI does it (warnings as errors, GCC 13): LLVM 18's
  unique_function gave a false "maybe uninitialized" at -O3, now off for
  spikeout_jit only; then gamerun-jit and spikeout-jit built. The Linux
  JIT window: first start 21 s (16.1 MB), cached 0.20 s; its recording
  replayed by the Windows C++ gamerun: 30/30 hashes. WSL copy for this:
  ~/spikeout-jitwin (build/, portable).
  FIRST DOWNLOADS (2026-11-05): a private DRAFT release on GitHub,
  v0.1.0-preview1 (not published: the tag is made on publishing), with
  spikeout-0.1.0-preview1-windows-x64.zip (26 MB) and -linux-x64.tar.gz
  (29 MB), made by tools/make-release.ps1 -Version V -LinuxExe FILE:
  the JIT window as spikeout(.exe), stripped, LICENSE.txt (GPL-3),
  THIRD-PARTY-NOTICES.txt (every library's licence), README.txt.
  Windows: GCC's runtime linked in (SPIKEOUT_STATIC_RUNTIME, on with
  MinGW; ran with only Windows' folders on the PATH). Linux: built on
  Ubuntu 24.04, needs glibc 2.38 and OpenSSL 3 (an older build system
  would widen it). Both tested unpacked into empty folders. The repo is
  still private, so the downloads reach collaborators only; going public
  is the owner's call (GPL-3: the source must be available with them).
  README rewritten (vf3-recomp's style, the owner's plan). SCREENSHOTS
  ARE ALLOWED IN THE REPO (the owner's decision, 2026-11-05, as other
  recomp projects do): docs/images/*.jpg, taken from the window (sized,
  title bar cropped, PrintWindow) from the owner's saved states, 1280
  wide, JPEG 84. Nothing else made from the ROMs is.
  JOINING (2026-11-05, docs/netplay.md "Joining over a slow line or a
  relay"): the seat list shows each player's link (LAN/DIRECT/RELAY); a
  join's progress on both sides (the joiner's loading screen with MB and
  steps, the host's RECEIVING/CATCHING UP %); the reliable channel paced
  and with selective acknowledgements; a joiner's early inputs kept (they
  were thrown away: stuck for good); no backups to a joiner; tested on a
  simulated slow line (SPIKEOUT_TEST_LINK=KB,LOSS). The relay can't be
  fully tested on one PC (one public address): a second network needed.
  MACOS NEXT (the owner builds it on their own Mac): docs/building-macos.md
  is the hand-off (tools, the JIT and reference builds, what may break:
  exceptions through JIT code first, then MAP_JIT/allow-jit for a signed
  app; the hash proofs; the app bundle and download).
  NEXT, in order: (2) CI: build LLVM once per runner and cache it (actions/cache on the
  install folder), build gamerun-jit/spikeout with the JIT, run the hash
  proofs headless where the ROM is available locally only (CI has no ROM:
  build checks only); (3) macOS: Homebrew-free, our own LLVM build
  (arm64 + x86_64), MAP_JIT and the allow-jit entitlement when signing;
  JITLink MachO and EH frames to test; (4) size: our own pass list
  instead of PassBuilder's default pipeline, LLVM built -Os or with LTO;
  (5) Steam Deck: the Linux build (SteamOS); (6) then the default flips
  to the JIT (downloads carry no game code) and the C++ build stays for
  proofs.
- GITHUB PAGE / FIRST RELEASE (owner's design, before the LLVM work): the
  repo (github.com/tastyrepublic/spikeout-recomp) stays PRIVATE until the
  first playable download. Downloads for Windows, Mac, Linux and Steam
  Deck on GitHub Releases, with no game files and nothing made from them.
  README in vf3-recomp's style: the owner's "SO" icon as a banner; the
  no-game-files notice; features with screenshots (resolutions, picture
  shapes up to 21:9, CRT colours, filters, the widescreen screens); the
  network play challenges overcome; supported devices (Windows, Mac,
  Linux, Steam Deck) and later ones (Android, iOS, Xbox, Switch 1-player);
  a roadmap (smooth motion, the direct cable mode, maybe direct IP, a
  Chinese/Japanese menu); install guides for Windows, Mac and Steam Deck;
  FAQ; legal (not affiliated with SEGA); Buy Me a Coffee:
  https://buymeacoffee.com/donfy (also on the GitHub page). Memory note
  release-distribution.
- Fixed (2026-11-05, 360dfb6): a network one-board host's use after free
  at exit (a gone player's backup runs outliving NetPlay; Linux aborted or
  hung, Windows didn't notice); found by the JIT's tests.
- Spectators (docs/netplay.md "Spectators" and "Spectators on one board";
  the owner's choices): up to 4 a game, ~2 s behind (net::kWatchLag), talk
  to the room's host only (Session::watch, kWatcher*); survive a host change
  (the new host adopts them); Watch in the game list / by code; Left/Right
  (LB/RB, 1-4) switch the watched seat on every board; an eye + count after
  the room line and a listed game's name; a 5 s banner. Weak devices: one
  board (Auto by power), shadows from the players' backups (sent only to
  spectators on one board, last, under an adaptive cap that halves when a
  player's round trip rises; three kept, Keeper::copy_for). Switching on
  one board too (step 2: every cabinet made whole from backups, then one
  board again around the new player; a dimmed cover if it takes over 0.5 s;
  SPIKEOUT_TEST_SWITCH_MS). Lobby deployed
  (full games listed, watch by code). Tests: tools/spectate-tests.ps1 (6),
  tools/spectate-window-test.ps1 [-Mode switch|oneboard]; gamerun
  --net-spectate [--net-spectate-seat N], --pace (arcade speed). Fixed on
  the way: --net-leave-at hung in every-board runs.
- Save states' safeguards (owner's choices, 2026-11-04): each file a header
  (tools::state_file_bytes: SPKSTATE, version 2, the state format
  kStateFormat (raise it when what board_state writes changes shape), the
  game code's fingerprint (code_id), frame, checksum). Same format and code:
  loads as is; another version's (or a headerless old file): offered with a
  warning and tried (saves kept across updates); damaged: refused. F10
  pressed twice, the file checked first (read, checksum, loaded into a spare
  board with its sound and music boards),
  the game kept in spikeout-undo.state, F9 within 5 s goes back; F10 not in
  network play; notices at the bottom (STATE SAVED, CAN'T LOAD: why);
  network F9 only with --net-save-key.
- Network settings all automatic (owner's choice, 2026-11-04; no menu item
  now): rollback (12 frames, fit to memory) for an internet game or a player
  over 10 ms away one way, none on a LAN; input delay from the furthest
  player (with rollback half its frames, 2 at most), re-checked each second
  in play, moved a frame once the same for 3 s; board mode Auto as before.
  --net-delay N / --net-rollback N force them (tests). Distance simulated:
  SPIKEOUT_TEST_LATENCY=MS[,MS@SECONDS] holds a machine's datagrams;
  tools/net-window-matrix.ps1 -Modes every|one|auto,... (2-4 windows, each
  its board mode, latency 40 then 110 ms): 2 (every+one, one+every), 3
  (every+one+one), 4 (auto x4: each went to one board under the load) all
  the same, rollback on, delay 1 then 2. Not tried over a real distance yet
  (the owner's internet test). Next: 21:9 (memory notes).
- Done (2026-11-04, docs/netplay.md "Side runs on other cores"): one
  board's side runs on worker threads (min(other cabinets, cores - 2, at
  least 1); SPIKEOUT_SIDE_WORKERS=N, SPIKEOUT_SIDE_THREAD=0 for none,
  SPIKEOUT_SIDE_CHECK=1 compares each job with a serial copy); a worker's
  writes marked by its bus (Bus::mark_pages) and put back from a clean
  copy (no journal, no OS record on its RAM; the same on every OS); the
  OS write record with several readers (watched::new_reader). Mash fight
  1,490 frames: 10.4 s -> 7.3 s. The network board's 68000 wait-loop skip
  (exact, ~0.03 ms; --no-net-idle-skip). All tests the same.
- Sound measured again (corrects the note below): effects DSP ~1.4 ms a
  frame, sound 68000 ~0.6, music ~0.25, voices ~0.1. Tried and dropped: a
  DSP with each step sorted into templates (exact, slower). The sound
  68000 can't skip its idle exactly (a polling loop repeating only every
  ~3,800 cycles, run 256 at a time). Dropped (owner, 2026-11-04): turning
  the other boards' programs (the DSP, the 68000s) into C++; only the
  main PowerPC is recompiled.
- The camera events are all sorted (docs/netplay.md, 2026-10-30), and one
  board's side runs run the other cabinet's own code; the hand-off's
  "camera events still to sort" below is done.
- gamerun's RTC comes from the host clock: fresh runs' states differ in
  "rtc".
- ROM picker (2026-11-04, owner's choices): the window reads spikeofe.zip
  itself (src/runtime/zip.cpp: our own reader and inflate, no new
  dependency; a zip's files CRC-checked) or a folder (command line only).
  No set chosen, or one that can't be used: a screen like the no-GPU one
  (CHOOSE THE ROM SET, the reason in red, the system's file dialog; zip
  only, no folder option), kept as settings.ini's rom_path; Game > ROM set
  in the menu changes it from the next start (not yet tried by hand). A
  set whose CRCs differ from the known set's is refused in the window
  (the code is compiled from that program ROM); gamerun and romcheck
  still load it. A command-line set that can't be used: an error, no
  picker (scripts). settings.ini read past a byte order mark.

## Earlier (2026-11-03, main 7d3b090)

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
  fine. (Was open: after a shadow the host logged "board 1 handed back to
  side runs" every 300 frames, the shadow never handing back, and "the
  boards saved different frames (6129, 6130)": both the freeze below, the
  shadow's count stopped so never reaching its stop. Since its fix the
  hand-back happens two frames after it is asked, in both windows.)
  FIXED (2026-11-03): the host's window froze after the joiner's coin
  (its game logic +0 and its link's packets stopped, frames still run).
  With rollback, a board on side runs waiting for a shadow (shadow_wait)
  took a rollback's frame as the last one run (f = rewound, then ++f): a
  frame ahead of the staying board from then on, so the shadow taken after
  it was loaded a frame early, its link two packets past the staying
  board's, both waiting for ever. Now f = rewound - 1. Delay-based (no
  rollback) never took that path; gamerun's tests don't use rollback. The
  window now logs the ports' counts and the staying board's frame at a
  shadow's load, and the state's save/compress/wait/unpack times.
  DONE (2026-11-03): the stutter at a coin in one board mode (was ~0.5 s
  frozen): with rollback a shadow's state no longer waited for (late
  loads: on side runs meanwhile, every board back to its frame once it
  comes, shadows of one frame together), and sent as the board's backup
  of that frame (~700 KB of changes, packed off the game thread, not 27 MB
  saved and packed on it). Window: ~51 fps for a second at the coin. Fixed
  with it (all root causes, docs/netplay.md "Shadows that come late"):
  states served only for frames not run again; the ring's recent packets
  after a rollback; backups and rollback reading the same write record
  (backups compare pages now); a backup copy kept as its note came;
  "come" counting a shadow being loaded. Tests: ONEBOARD_LATE_TEST=N,
  ONEBOARD_WAIT_SHADOWS=1 (the old wait), ONEBOARD_WHOLE_STATES=1 (gamerun:
  whole states); the scratchpad's late-load checks were obrb.ps1 (2
  seats) and obrbn.ps1 (3-4) from the previous session, against a
  reference. Then FIXED (2026-11-03): onepc-net-tests "leave" failing
  now and then in a full run. The parting's announcement (reliable, 35
  frames ahead: 0.1-0.3 s in gamerun) can wait behind a big transfer
  while the inputs go on; taken after its frame it was dropped and still
  marked the seat departed, so the path for a player gone without one
  (its board from its backup) was skipped: nobody had that board whole,
  the joiner waited for ever. Now a parting come too late is ignored and
  the board run on from its backup once the player has gone
  (Game::parting). Tests: NET_PARTINGS_LATE=1 (gamerun: each parting
  taken only once its frame has passed), NET_PARTING_AHEAD=N (the
  leaver's lead).
- Network status list redesigned (2026-11-03, the owner's choices): the
  seats moved down the right side a little above the middle (the FPS alone
  at the top); a line above them "HOST <name> 繚 ROOM <code>" ("繚 LAN" with
  no code; the current host, Session::host_seat); each row right-aligned
  (seat, platform icon tinted as the row, flag, name, then a fixed-width
  ms and bars; our own row flush right, no RB), a badge line under each:
  "1 BOARD 繚 RB 繚 DELAY 0" (that machine's status: Session::set_status /
  kStatus, sent as it changes and every 2 s) and, for the others with
  rollback, "RB n" (their wrong guesses here in 10 s, InputTable::
  wrongs_since; green 0-2, yellow 3-15, red more: bands to tune). Icons:
  tools/platform_icons.cpp -> assets/ui/platforms.png (Windows' squares;
  macOS and Android from Simple Icons, CC0; Linux a flat penguin, belly and
  beak cut out: the owner's pick). Kept as is for now: explaining the
  badges (menu description, or badges only with the menu open: offered).
- Proof tools: gamerun GAMERUN_2D_HASH=1 (2D layers each frame),
  GAMERUN_STATE_PARTS=1, GAMERUN_NETBOARD_DUMP=DIR (network board state
  files at whole-state frames), GAMERUN_FLUSH=1, ONEBOARD_CALM=N,
  ROLLBACK_FREE_MB=N; SPIKEOUT_TEST_HOLD_INPUTS=N (window), gamerun
  --net-hold-inputs N (both now by time); tools/onepc-tests.ps1 and
  tools/onepc-net-tests.ps1 (all pass; leave's host margin 10,000 frames).
- Build lesson: PowerShell Copy-Item keeps file times; copying a header
  back left objects built against an old struct (sound broke). Touch files
  after copying.
- Speed work, where each idea stands (all done ones in 7d3b090, proved):
  DONE: decrementer check fix; idle skip (all boards); find_region table;
  program ROM 32-bit reads straight; tile generator colour tables; side
  runs' set-up compared 64 bytes at a time and buffers reused; sound DSP
  decoded once per program (~6% of a sound run).
  TRIED, DROPPED (no gain): a 4 KB page journal for side runs (slower);
  not recording the recent-blocks list in tick().
  NOT STARTED, ideas only: FPSCR's FPRF worked out lazily (the game does
  read FPSCR: ~40 mcrfs, mffs; set_fprf is already cheap, small gain);
  an idle skip for the network board's 68000 (Moira; guessed < 0.5 ms a
  frame); the sound's voice rendering (Scsp slots, most of the sound's
  ~2 ms, on the window's audio thread: battery on tablets); skipping
  wholly transparent tiles in draw_line; side runs on a second core (the
  big one, owner wants it).
  Measured now (two boards, 1,500 frames of a fight, gamerun): game logic
  ~3.3 ms a frame a board, 2D ~1.3 ms (the shown board only in the
  window), sound ~2.3 ms (audio thread in the window); one board's side
  runs ~3-4 ms a frame per other cabinet; the profile has no single hot
  spot left (tick(), the 68000 network board, then the game's own code).
  The machine varies 6-8% run to run: compare A/B back to back.
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
  `--record FILE`, `--load-state FILE`, F9 save state, F10 twice loads the last (F9 then undoes it),
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
  m3::TileSides (26 columns each side since 2026-11-04 for 21:9, 12
  before; in save states, kStateFormat 2: format 1 states still load); the tile
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
