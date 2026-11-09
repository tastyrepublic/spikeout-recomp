# Hand-off, 2026-11-08 (one board a PC, network play)

## The owner's rules: read before changing ANYTHING

These are the owner's standing rules for this project (from CLAUDE.md, the memory notes and the owner's own words).
Breaking one costs the owner's trust and time. When unsure, ASK the owner in one short question before acting.

1. **Commit and push only when the owner says so**, each time ("commit and push"). Never on your own. A push goes to
   the working branch and main is fast-forwarded to it (`git push origin HEAD:main`), as the owner has approved.
   Never force-push, rebase pushed commits, delete branches, tags or releases, or rewrite history.
2. **Never commit**: ROM data or anything made from it (dumps, extracted data, recompiled code in build/generated),
   build folders (`build*/`), `.claude/`, the owner's files in the repo root (`net-log*.txt`, `readme-preview.html`,
   `spikeout-*.png`, `*.state` save states), anything secret. Stage files by name, never `git add -A` or `.`.
3. **Nothing public or outside the repo without asking**: no GitHub releases, issues, comments or PRs; no lobby
   deploy (`npx wrangler deploy` in lobby/) and no Cloudflare changes; no new Codespaces (they cost the owner money;
   only two run at once; stop them after use: `gh codespace stop`).
4. **Change only what was asked** (scope discipline). No "while I'm here" refactors, renames, reformatting or
   cleanups. Unrelated problems you notice: write them down and tell the owner; don't fix them.
   **gamerun and the window must never differ in network play** (owner, 2026-11-09: many bugs blamed on the game were
   the two not matching). Every network step through shared code (tools/net_frame.hpp), called the same way; no
   gamerun-only network modes; test switches only off by default and never changing the play; speed reports (57.5 fps,
   stutters) one rule and one text for both. Any difference still left: name it to the owner.
5. **Fidelity to the real Model 3 hardware**: Supermodel first, then MAME, then datasheets (docs/REFERENCES.md).
   Change the game's behaviour only with evidence; keep the old behaviour selectable (an option or env switch);
   enhancements default OFF. docs/fidelity.md records every such decision.
6. **Prove "the same" with hashes**, never by eye: gamerun's RAM/frame/GPU hashes, replays, save states, the finder
   (tools/onepc-diff.ps1). Say exactly what was tested and what wasn't.
7. **Every network change passes the WHOLE test set before the owner is asked to commit** (memory network-test-suite):
   `tools/onepc-tests.ps1` (14/14; 4 skipped without cam3.rec), `tools/onepc-net-tests.ps1` (all the same) and
   `-Only leave3,vanish3 -LeaverFirst`, the internet cases (`tools/internet-tests.sh`, every case), and the window's
   own tests for window changes. Bots press all the time in every test (the owner's rule). Report each result.
8. **Fail fast**: every batch stops at its first failure; every run and every wait has a time limit; read a long
   run's first lines within a minute or two; check the build is new before trusting a result (the finder prints it).
9. **Don't rebuild `build\gamerun.exe` while a test uses it** (internet runs, suites): a half-written exe breaks it.
10. **Explain in plain, simple words** to the owner (English is not their first language): the result first, no
    invented jargon, short. Ask one question at a time when a decision is theirs.
11. Things the owner decided (don't change them): network settings are automatic (no menu items for rollback/delay);
    each player on their own board; one board a PC for weak devices; spectators must never slow the players; no
    direct-IP option; "quads drawn whole" on; free play is the game's own setting; smooth motion off by default.
12. Shell edits (sed/perl) mangle `\n` and backslashes in C strings and PowerShell paths: use the Edit tool.

## A starting prompt for a new session (paste this)

> You are continuing work on the Spikeout network play (one board a PC) in the worktree
> `Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31` (branch claude/window-jit). First read
> `docs/handoff.md` completely, especially "The owner's rules", and follow them exactly: never commit or push unless
> I say so, change only what I ask, prove results with hashes, run the whole test set for network changes, stop
> tests at the first failure, and explain to me in simple words. Then tell me in a few lines what you understood the
> current state and the next step to be, and wait for my go-ahead before changing any file.

Read this first, then CLAUDE.md's "Latest" and docs/netplay.md "Three cabinets" (the full history of this work).
Everything here is about **one board a PC** (a machine runs only its own board; the other cabinets are "made" on
it by side runs) in network play with rollback.

## In progress (2026-11-09): plan step 2, one shared network set-up — START HERE

The owner chose: step 2 first, then find a way to REPRODUCE the step 1 bug (not many blind test runs).

**Part A** (approved): move the window's network steps at a frame's end into `tools/net_frame.hpp`, used by both
tools/spikeout.cpp (run_game's on_frame_end, ~line 1070-1430) and tools/gamerun.cpp (set_up's on_frame_end, ~1668-2860),
so gamerun runs the window's code. Differences found between them:
1. Order at a frame's end. Window: pace (in the drawing part), then backups, parting, board requests, partings/gone/
   taken, capture of every board's state, catch-up/take_seat, its own inputs out, capture snapshot, hashes, inputs read,
   rollback snapshot. gamerun: own inputs out, hashes, inputs read (after a lead>4 sleep loop when not --pace), capture,
   partings, backups, board requests, catch-up, status/delay tuner, pace, capture snapshot, rollback snapshot.
2. One-board set-up: window at a meeting (set_up_one_board, spikeout.cpp ~1489: hold_guesses + pin, hand_over held,
   attach_extra); gamerun before the boards start (set_up_one_board ~944) and hands each board over at its own frame
   end (~1678); `--one-board-late` sets up at a meeting like the window.
3. gamerun erases pending hashes after a going back (main ~4512); the window doesn't (harmless: re-runs re-hash).
4. Settings: window input delay 0 on a LAN; gamerun tests use 2; time sync differs without --pace.

Pieces, one at a time, each followed by `tools/onepc-tests.ps1` (14/14) and `tools/onepc-net-tests.ps1` (all the
same) plus `-Only leave3,vanish3 -LeaverFirst`, stopping at the first failure:
- **Piece 1, DONE, UNCOMMITTED, tested (2026-11-09)**: `tools/net_frame.hpp` (`Hashes::frame_end`, `read_inputs` with
  `Read{guess, ahead, quiet_ahead, keep}`), used by both programs; gamerun gained `g_rollback_from` (set to frame + 2
  when a catch-up ends; guesses only from it; its frame's rollback snapshot always kept), as the window.
  onepc-tests 14/14 the same; onepc-net-tests all the same (two, four, apart, coin1, coin0, poweron, late1, late0,
  fourauto, three, leave, leave3, vanish, vanish3; 0 hash checks differed); leave3,vanish3 -LeaverFirst all the same.
- **Piece 2, DONE, UNCOMMITTED, tested (2026-11-09)**: onepc-tests 14/14, onepc-net-tests all the same,
  leave3,vanish3 -LeaverFirst all the same. Backups and board requests in net_frame.hpp (`backup_due`, `backup_now`
  (returns `BackupSent`), `board_requests`, `board_state_now`, `board_by_backup`, `board_whole`). Window: the same steps,
  only moved. gamerun now does them the window's way: the "settled?" check of a board request is made at the snapshot
  point (it was at the frame's end), `wait_all` skips its own seat, a leaver's whole state uses the window's code and
  message ("net: this board's state at frame F to seat S (N KB packed, M KB; saved in ... ms ...)"); its by-backup
  message is now "net: this board's state at frame F to N seat(s): its backup". gamerun keeps its own extras:
  ONEBOARD_BACKUPS, ONEBOARD_WHOLE_STATES, the "guessed wrong or to be run again" print (--one-board-local-shadows:
  removed in piece 6).
- **Piece 3, DONE, UNCOMMITTED, tested (2026-11-09)**: the three suites all the same (the leave3 run went through
  "every board whole here first" and the partings). Partings/gone/taken (`SeatNews`, with gamerun's NET_PARTINGS_LATE as
  its `late`) and every board's state asked for (`Capture`, over each program's own flags: the pacing reads `armed`).
  gamerun now does them in the window's order (seat news before the capture) and prints the window's "a board whose
  player went is had nowhere whole" text; its "every board whole here ...: their state saved" print stays (window: none).
  NOT moved: the window's F9 save and resync check (window only).
- **Piece 3b, DONE, UNCOMMITTED, tested (2026-11-09)**: `capture_now`, the check that a board saves its part of
  every board's state at the capture frame (the save itself stays each program's: sound handling, gamerun's
  --save-state). gamerun's `wait_all` now skips its own seat, as the window's. The three suites all the same.
- **Piece 4, DONE, UNCOMMITTED, tested (2026-11-09)**: the three suites all the same; rbbot -Seats 3 -OneBoard 1
  -Frames 9000: 0 differs on all three, 0 hash checks differed, both joiners "caught up" by the shared code. Catch-up (`CatchUp`); each program still takes the seat and sets
  rollback_from itself. gamerun (only with --net-catch-up; without it the seat is taken at once, as before) now
  prints the window's lines ("catching up from frame", "not gaining on the others", "caught up at frame F: N frames
  in S s (X.XXx the game's speed)": internet-tests.sh's grep still matches) and honours SPIKEOUT_TEST_CATCHUP_MS.
  The three suites don't use --net-catch-up: tools\debug\rbbot.ps1 -Seats 3 -OneBoard 1 does (run as a fourth check).
- **Piece 5, DONE, UNCOMMITTED, tested (2026-11-09)**: `rollback_frame_end` (tell the boards filled here, then the
  lazy snapshot; the network optional: gamerun's --rollback-sim/--rollback-force have none). The three suites all the
  same; rbbot as piece 4: 0 differs. Finder (three-6000.state, 3 boards, board 1 stays, --one-board-tasks
  --rollback-sim 6, -NoShadows, frames 6010-7500): SAME every frame.
- **Piece 6, DONE, UNCOMMITTED (2026-11-09; owner: "network only", and "make sure gamerun and the window won't
  differ from now on")**: the one-board set-up of network play is one function, `tools::net_frame::set_up_one_board`
  (the window's Settings: rules on, every other cabinet handed over held, shadows from the real boards or backups run
  on here, rollback's lambdas, attach_extra, hold_guesses + pin), and its timing `one_board_due` (at a meeting, wanted:
  no guesses; not while every board's state is saved; with rollback only on a settled frame, not where going back).
  The window's `set_up_one_board(Shared&)` only fills `OneBoardSetUp`. gamerun's network play now sets up at a meeting
  from `--net-one-board`'s frame on (`net_set_up_one_board`, after `Linked::between` as the window), never before the
  boards start, no hand-over at each board's frame end; set up anew after a resync (`g_one_board` rebuilt). Its old
  network-only ways are gone or refused: `--one-board-auto`/`--one-board-held` are implied (the scripts still pass
  them, harmless), `--one-board-local-shadows` removed, `-late`, `-hand-back`, `-whole`, `-lookahead`, `-miss`,
  `-stays` refused with `--net-one-board`; a spectator's set_watch_one_board only at the set-up (as the window);
  backups and by-backup board requests on `real_netboard` (as the window). So tests `two` and `four` (were: no rules)
  and `apart`, `coin1`, `coin0` (were: rules, not held) now run the window's way. Offline one board (`--one-board`) is
  unchanged (the window has none). gamerun keeps test-only switches, all off by default: NODE_CORRECT,
  ONEBOARD_WHOLE_STATES, NET_PARTINGS_LATE, ONEBOARD_BACKUPS, and the "replay board" RAM print the suite reads.
  Still the window's only, as decisions not steps: Auto board mode by measured speed (one board wanted later, or
  every board again via keep_whole + tear_down) and a spectator's switching (no gamerun options drive them).
  Test results: onepc-tests 14/14 the same; onepc-net-tests all the same (all 14 cases, 0 hash checks differed;
  two/four/apart/coin now the window's way); leave3,vanish3 -LeaverFirst all the same; rbbot -Seats 3 -OneBoard 1
  -Frames 9000: 0 differs on all three. BUT rbbot's speed (--pace, the "Pace:" line) was lower than piece 5's:
  50.9/51.1/51.6 frames a second against 53.8/54.0/54.8, stutters 256/184/172 against 169/140/114, rollback "back
  where it was again" 56/75/56 ms on average against 42/60/42. Both below the arcade's 57.52. Being re-run (p6b) to
  tell this PC's variation (6-8%) from a real slowdown. p6b: 49.2/49.3/49.7, so a real cost: A/B against the old
  gamerun (the player-reports worktree's build/gamerun.exe is main's code) is the next check.
- **Piece 7 (2026-11-09, UNCOMMITTED): the ORDER of the frame-end steps.** gamerun's set_up on_frame_end now runs the
  window's steps in the window's order (spikeout.cpp run_game): pacing, backups, a parting, board requests,
  partings/gone/taken, every board's state, the out-of-step and parted checks, catch-up, status, this player's
  inputs out, the capture, the hashes (and gamerun's test hashes), the inputs read, --frames at the very end. Blocks
  moved whole (every line kept, checked by sorting). Tests: see the next lines when run.
- **Speed checks (the owner's, 2026-11-09, UNCOMMITTED)**: tools/speed_report.hpp, one measure for both programs:
  at each paced frame's end (the window's pace(), gamerun --pace), a stutter is a frame over 1.5 frames after the
  last (a stall counted; Tab, catching up, the menu start it again); with SPIKEOUT_NET_DEBUG a line each second
  ("speed: frames F-G: X a second, N stutters, the longest gap M ms") and at the end "speed: in all ...".
  tools/speed-check.ps1 FILE...: SLOW when a second after the first 5 is under 56.5 or has a stutter. The four
  window tests run it on every window (SPIKEOUT_NET_DEBUG on in all four now): "the same, but SLOW", exit code 2
  (correct but slow; FAILED stays 1); rbbot prints it; internet-tests.sh says SLOW per machine (not a failure: a
  Codespace is small), its "longest gap" now read from the Pace: line only.
- **Piece 6's slowdown: none** (2026-11-09): rbbot -Seats 3 -OneBoard 1 -Frames 9000, old gamerun (main's code: the
  player-reports worktree's build) and new (pieces 1-7) back to back, twice each (-Exe, new in rbbot.ps1): 53.9 against
  54.2 frames a second on average, stutters 117-175 both, rollback back in 41-61 ms both, 0 differs in all 12. The
  49 was this PC's slow minutes. Piece 7's suites: onepc-tests 14/14, onepc-net-tests all the same, -LeaverFirst all
  the same, rbbot 0 differs.
- **THE ONE-BOARD BUG: FOUND AND FIXED (2026-11-09)**. Traced with the window's new
  per-frame dump (SPIKEOUT_RAM_DUMP_RANGE=B:LO-HI@FROM:N:FILE, gamerun --ram-dump's rows; window-autoplay-test.ps1
  -Dump, -BotDump SPEC1,SPEC2) and -HashFrom: first wrong at frame 4368 (5 after a hand-back at 4363), 4 bytes of
  each made cabinet's record: entry 5's +8 (0x140558, 0x140858; received at 0x142758, 0x143058). 0x747DC (called
  from 0x742CC) fills the record's character table at 0x140214, 12 bytes an object (players 0x1A0000+0x4000k,
  enemies 0x1B0000+0x4000k): +0 the first word, +4..+7 low bytes (kEntryFields, already kept per cabinet), +8 THE
  SUM of the object's +0x74, +0x76, +0x3C, +0x3E (u16) and +0x380, +0x384, +0x388 (words). Enemy 1's object was
  empty (+0 zero) and those fields its leftovers, different on each cabinet (master +0x74 63ac, slaves 3245, 3107):
  the made cabinets sent the staying board's sum. Fix: kSumFields and Remote::kept_sum (remote_cabinet.hpp), kept
  as kEntryFields are: at start_making each cabinet's own sum and the staying board's; make_record puts its own in
  while this board's record still has this board's sum of then; shadow_state and one_board.hpp's tracked list carry
  the seven fields for a shadow. window-autoplay-test seed 1 (x2), 2, 3: 0 differs (two hand-backs at 4355-4359,
  4317), where every run went out of step at 4440 before. The whole test set then passed: onepc-tests 14/14, onepc-net-tests, -LeaverFirst, rbbot (no differences), the 13 window tests (all the same; SLOW as before: several windows on one PC) and all 12 internet cases, mixed and mixed-host1 included (0 hash checks differed on every machine; slow seconds as before the fix, e.g. oneboard 122/106/88).
- (history) **THE ONE-BOARD BUG, NOW REPRODUCED EVERY TIME (2026-11-09)**: window-autoplay-test.ps1 (the window hosting on
  one board, two gamerun bots, seed 1, 6000 frames): once the window hands the other cabinets back to side runs
  (frame ~4331-4395), every process differs from frame **4440** on ("node check: seat 2 packet 4137...: the whole
  packet differs" from ~4137). 4 of 4 runs: new/new, new window + old bots, OLD window + new bots (so not pieces
  1-7: the old window goes out of step too). Two old/old runs never handed back (joins at other frames), 0 differs.
  Old/new mixing works (the same build id). The window is SLOW there (one board's side runs and two bots on one PC;
  the old window as slow). Scratchpad scripts autoplay-ab.ps1 (-Bin, -BotBin) did the A/B. This is the bug of the
  owner's plan step 1 (the internet case `mixed`): a fixed local repro.
- **Window tests, pieces 1-7 (2026-11-09)**: all 13 CORRECT ("the same"/"watched": onepc-window joiner, host,
  auto, leave, crash, none, plain; spectate-window switch, oneboard; net-window-matrix every+one, one+every,
  every+one+one, auto x4), all SLOW by the new check: two windows on every board 57.1-57.3 frames a second in all but
  a 26-127 ms hitch about once a second; one board ~45; four windows ~50. A single window alone had none: the
  hitches are mostly one PC running 2-4 games and one GPU; the real check is the window alone with the bots on the
  Codespaces.
- **Internet tests, pieces 1-7 (2026-11-09, build/internet-p7.log)**: 10 of 12 cases all 0 differs: two, three,
  four (+ this PC's 4th player), vanish, stall, spectator, spectators (a 5th turned away), oneboard, oneboard-rb,
  **mixed-host1 (failing before: passes now)**. FAILED: **mixed** (14-19 hash checks differed, a 7.9 s stall: the
  open one-board bug, as the local autoplay repro), and **relay** on this PC only: it asked Cloudflare's TURN for
  its permissions and never had an answer, so it never joined ("the game didn't start"; the 11-08 run of the same
  case passed with 1P RELAY; relay code untouched by pieces 1-7). SLOW per machine: 1-20 seconds on every board,
  40-120 on one board (the Codespaces are 4-core shared machines). **relay re-run: passes** (1P RELAY 15 ms, 0
  differed): Cloudflare's relay not answering that once. So every case passes but `mixed`.
- **THE REAL-WORLD TEST, RUN (2026-11-09)**: only the window on this PC (sound on, the autoplay presses, hosting 3),
  the bots joining from the UK and India Codespaces (scratchpad realworld.ps1; joins by tools/codespace.sh).
  Every board: all 0 differs; the window 57.18 frames a second over the run, 182 stutters, the longest gap 179 ms
  (at the joins); **a hitch of just over a frame about every 290 frames (~5 s), from the start, before anyone had
  joined** (frames 509, 799, 1089, 1379...): something periodic in the window costs a frame (the backups? a debug
  print? to find). One board: hand-back at 4364, out of step from **4440** (the same bug as the local autoplay
  repro and the internet case `mixed`). Earlier, the window hosting alone (nobody joined, a script quoting slip):
  every board 57.05, one board 57.36 frames a second.
- Later (owner noted 2026-11-09): a Linux arm64 download (Raspberry Pi 5, ARM laptops, Asahi): GitHub's
  ubuntu-24.04-arm runner or sniper's aarch64 SDK, one more release job; the translator never run on ARM Linux.

**Part B** (after A, planned with the owner): the window records, for each other player's input, the local frame at
which it arrived; gamerun --rollback-sim replays those arrivals exactly, so a failing live run repeats offline (same
guesses, same goings back) and the finder (tools/onepc-diff.ps1) reaches the first wrong byte. Shadows in live play come
from the other machine's real board: the recording may need those states too, or a failing run without a shadow after
the hand-back.

Kept aside, not in the code: branch `backup/claude-2026-11-08` (6dac65a: a made record keeps a gone enemy's +8 sum per
cabinet, `Remote::kept_sum` in remote_cabinet.hpp; NOT proven: the window bug is random and the test set wasn't run)
and a git stash (a "late inputs" print in the window, window-autoplay-test.ps1 -BotDelay/-BotsLow/-BotsOneBoard).
The frame drops in window-autoplay-test are the test's own load (docs/netplay.md "Rollback's cost with players pressing
all the time"): don't measure them again on one PC.

## The LAN to-do list (the owner's grouping, 2026-11-09)

Where LAN stands: every board is exact (inputs only, a small input delay); spectators (found by LAN discovery,
tools/spectate-tests.ps1) and late joins (onepc-net-tests late0/late1, coin0/coin1, fourauto) work. The open risk is
a one-board PC on a LAN, alone or in a mixed game: no rollback, so a wrong side-run guess can't be undone (only a
resync from the host's state puts it back). Left:
1. **One board on a LAN from the real link packets** (Mode A, as a cable link): each PC feeds the other cabinets'
   real packets into its own link, with a small input delay; exact, no side runs, no rollback; prediction again only
   when the delay goes over about a frame. Fixes the one-board and mixed LAN risk. (Memory: lan-one-board-real-packets.)
2. **LAN rollback switched on by itself** on a PC with CPU to spare, the first snapshot copied in slices over a few
   frames (no pause, proved byte for byte); one-board hosts and joiners too. Deferred by the owner until after
   another release. (Memory: netplay-all-auto.)
3. DONE (2026-11-09): **the parallel rollback restore back** (threads kept for the game, tools::rollback::Crew;
   going back 3.1-3.9 ms against 4.6-5.0; the whole test set and every internet case passed; docs/netplay.md).
4. **A vanished player holds a no-rollback (LAN) game 3 s** before the others go on (a normal quit doesn't).

## Everything left in the project (2026-11-08), in the owner's order

1. Network play, the owner's plan: (a) the internet case `mixed` (below), (b) one shared network set-up for gamerun
   and the window (recommended before more `mixed` digging), (c) window tests, then the whole test set.
2. Network play, also open: the real-world test (the window alone on this PC, bots on the Codespaces; not run, steps
   below); a gamerun bot's silent crash after being put back in step (below); **LAN one board can't correct a wrong
   guess** (LAN games run without rollback: the plan is to give it the other cabinets' real link packets, as a cable
   link (Mode A) would; the internet keeps prediction + rollback); the parallel rollback restore can probably come
   back (its heap crash is fixed); a vanish in a no-rollback game holds the others 3 s; Codespace join stalls of
   2-5 s; `tools/onepc-tests.ps1 -Only ONE_CASE` fails; bot longplays with 2-4 players through every area, the
   finder on each checkpoint.
3. Players and release: **GitHub issue #1 "8bitdo controller not detected"** (a player's report, 2026-11-08; not
   looked at; answering on GitHub is public: ask the owner first; the player: USB, Windows 11 Pro, 8BitDo Ultimate
   Wireless. LIKELY CAUSE found by reading the code (2026-11-09, not fixed: owner "not now"): the window's pad list
   (spikeout.cpp, `gamepads`) is filled only from SDL_EVENT_GAMEPAD_ADDED, but the screens before the game loop read
   and drop every event: the JIT's "PREPARING THE GAME" loop (keeps only quit) and the ROM picker (opens the pad, never
   lists it). A pad connected before the start is lost; one connected later works (the owner's Bluetooth PS5). Fix:
   at the game loop's start take SDL_GetGamepads(), skip an ADDED for one already open); the JIT release track in CLAUDE.md "Latest"
   (CI with LLVM, macOS on the owner's Mac (docs/building-macos.md), download size, Steam Deck, then the JIT the
   default); the update check's step 2 (the game updating itself); the interpreter for iOS (the window, an iOS
   build, speed). **Player reports (the owner's to-do, 2026-11-08)**: today the released window prints its log only
   to stdout (lost: it is a GUI program), has no crash handler, and GitHub has no issue template. Planned, smallest
   first: (1) always write the log to `spikeout.log` in the data folder (+ `spikeout-prev.log`, size capped);
   (2) a crash report (Windows minidump + last log lines; Linux/macOS crash location in the log; each release keeps
   its debug symbols on our side); (3) Esc menu "Report a problem": a zip of logs, crash file, settings.ini, version,
   OS, GPU and controller names (never ROM files or game code), then GitHub's new-issue page with the version filled
   in, the player attaching it (nothing sent automatically); (4) a "Detailed logging" switch, off by default;
   (5) on a network desync, the session's inputs + a recent state saved to replay in gamerun; (6) an issue template.
   Privacy: network logs hold IP addresses, room codes, names: remove addresses or show the player the contents.
   **DONE 2026-11-09 except (5), on branch `claude/player-reports`** (worktree .claude/worktrees/player-reports, made
   from cf216e9 = pushed main, WITHOUT this branch's network pieces; pushed, not merged into main): its
   docs/player-reports.md has everything. In short: spikeout.log + crash reports (Windows minidump); Esc > Game >
   Report a problem (owner's design: GitHub for logged-in players, a zip for others; no lobby involved): What
   happened (a new menu text page), Detailed logging (moved there), Log in to GitHub (device flow, QR code, the
   owner's GitHub App "spikeout-reports", App ID 5678845, Client ID Iv23lix0ed2ATx6BOHJC built in, installed on
   spikeout-recomp only, Issues: write), Send to GitHub (only logged in + text written), Save as a zip (a page of the
   game's own, no system windows), Last report (the issue and its answers in the game), Log out. Proved: logging costs
   nothing (CPU and every second at full speed, 60/60 hashes); real test issues #2 and #3 sent from the game and
   deleted. The pad bug above is LOGGED (not fixed, owner's choice) so the player's log will show it.
   RELEASES NOW BUILT ON GITHUB (owner, 2026-11-09): .github/workflows/release.yml, started by pushing
   `release-build/<version>`: Windows (WinLibs GCC 16.2.0 r2), Linux (Steam Runtime sniper SDK, GCC 14, SDL and
   OpenSSL 3 built in), macOS arm64 and x64 (13.3+; `-fexperimental-library` for jthread), LLVM cached per runner;
   packed by tools/package-release.sh with debug symbols; a DRAFT release, the owner publishes. Version
   0.1.0-preview2: PUBLISHED 2026-11-09 13:24 UTC (owner), tag v0.1.0-preview2 at 2c41815 (4 downloads, release notes on it; its 4 debug-symbol files moved on 2026-11-09 to the draft release "symbols-v0.1.0-preview2", never to be published: releases now put them there, the owner's choice), Windows and Linux
   downloads tried as players get them (clean PATH; WSL): translated, ran, closed normally, and each one's recording
   replayed 30/30 hashes in the reference build. The Macs built but are untried (no Mac here). README and the
   downloads' readme updated (README on main only when the branch is merged).
4. Later or optional: smooth motion's ghosts (off by default); the map screen's faces layer in widescreen; speed
   (the sound's voice rendering, a 68000 idle skip); the owner's final goal after a first release: one shared world.

## Where things are

- Worktree: `Z:\Emu\spikeout-recomp\.claude\worktrees\lan-play-f60b31`, branch `claude/window-jit`.
- Pushed: **main = claude/window-jit = c95b0df** (this hand-off and its tools) after aa5a6ff (the fixes), 2663956 and
  116b566.
  The owner pushes main as a fast-forward of this branch (`git push origin HEAD:main`) when they ask to commit.
- Committed with this hand-off (c95b0df); nothing else was left uncommitted:
  - `tools/spikeout.cpp`: `SPIKEOUT_RAM_HASHES=B@FROM:FILE` (the window writes board B's RAM page hashes every
    frame, the same format as gamerun `--ram-hashes`, a row each time a frame runs).
  - `tools/window-play-test.ps1`, `tools/window-autoplay-test.ps1` (below), `tools/debug/*` (helper scripts, below),
    this file, and its pointer in CLAUDE.md.
- Never commit: `.claude/`, `net-log*.txt`, `readme-preview.html`, `spikeout-*.png` (the owner's), anything from
  the ROMs, `build*/`.
- Builds: `build\` (Windows, MinGW; `cmake --build build` builds every target incl. the window). WSL copies for
  sanitizers: `~/spikeout-asan` (sources as of aa5a6ff + keep_ram fix), `build-asan` (AddressSanitizer) and
  `build-tsan` (ThreadSanitizer; it and its runs need `setarch x86_64 -R`: TSan can't map memory on this kernel
  otherwise). Start long WSL jobs detached (`setsid nohup ... &`) and poll: a `wsl.exe` call that returns can take
  its session down with it.

## The owner's plan, in this order (agreed 2026-11-08)

1. **Fix the internet case `mixed`** (below).
2. **One shared network set-up for gamerun and the window** (memory note shared-net-setup): most of the last two
   days' lost time came from the two programs wiring network play differently (gamerun had the made-packet
   correction the window never had; tests passed on gamerun code players don't run). The tests must run the
   window's code.
3. **Then test the window** (its own tests: `tools/onepc-window-test`, `spectate-window-test.ps1`,
   `net-window-matrix.ps1`, plus the new `window-autoplay-test.ps1`), and the full set again
   (memory note network-test-suite: every network change passes every suite, every internet case, the window tests).

## The open bug: one board goes out of step after a hand-back

### What is known

- Internet case `mixed` (tools/internet-tests.sh: UK Codespace hosts, India joins on one board, this PC joins on
  every board, bots pressing all the time, rollback 12, input delay auto): fails about one game in two.
  - Failing games: India hands back **only board 2** to side runs (~frame 3775); board 0 (the host's cabinet) stays
    held whole; India's board 1 differs from the others at the next hash check (3840). 
  - Passing game: India handed back **both** cabinets at once (4340).
- **The window reproduces it on this PC**, even with both cabinets handed back together:
  - The owner played in the window (tools/window-play-test.ps1: the window hosts 3 players on one board, two gamerun
    bots join): handed back both at 3873, out of step at 3960 (and again after the next hand-back, 4080).
  - Without a person, `tools\window-autoplay-test.ps1 -Frames 6000 -Seed 1` (the window plays a random press list,
    SPIKEOUT_TEST_PRESS): handed back at 4377, out of step at 4440. Repeatable, a window opens (don't touch it);
    ~3 minutes; it reports each process's first "differs" and hand-back.
- What does NOT reproduce it (all the same on every frame, so not the cause alone):
  - gamerun offline with the finder and the new modes (below): two/three cabinets, every seat staying,
    `--rollback-sim` 3-12 frames, lazy snapshots, a board held whole while the other goes to side runs
    (`ONEBOARD_HOLD_BOARD`), its buttons late too, 2D and sound on.
  - gamerun through the lobby on this PC (`tools\debug\rbbot.ps1 -Seats 3 -OneBoard 1`, also with input delay auto
    and 140 ms): all three the same, both handed back together.
  - So: **the window's own way of running one board differs from gamerun's** in something that matters (the window
    ran at 23 ms a frame then, late; input delay 0; its own rollback loop, its frame pacing, sound on its own thread).
    That is also why step 2 of the plan matters.

### Later the same evening (after c95b0df)

- The window now writes per-frame RAM hashes (SPIKEOUT_RAM_HASHES) and they compare cleanly with gamerun's (all the
  same over 1,401 frames in a run that stayed in step): the comparison works.
- But the window bug is **random**: after it failed 2 of 2 (the owner's play test, built 15:35 with all of
  aa5a6ff's fixes; and autoplay seed 1), 7 automated runs in a row stayed in step
  (`tools\debug\autoplay-until.ps1`, seeds 1-7, 6,000-8,000 frames, hand-backs in 6 of them, one of them
  handing back only one cabinet). Re-running until it fails is slow (2 failures in 9 runs).
- **Recommendation (agreed reasoning with the owner):** move to the plan's step 2, one shared network set-up for
  gamerun and the window, so the window's one-board path runs in gamerun offline, where `--rollback-sim` and the
  finder make it repeatable and find the first wrong byte (as they found the bugs fixed in aa5a6ff). Then come back
  to `mixed` with that.
- The frame drops the owner saw in these tests are mostly the test's own load: this PC ran the window plus two
  gamerun bots, each running all three boards (~9 boards on 6 cores); before the hand-back the window itself runs
  all three boards (held whole until calm): 18-22 ms a frame (12-16 running, 2-4 waiting at its board threads'
  meetings) against 17.4; dips to 10-30 fps when a bot joins (the whole game's state packed and sent). The owner
  asked for a real-world test (only the window on this PC, the bots on the Codespaces) to see the frame rate when
  the PC runs only their game: that test was being set up when this was written (Codespaces synced with KEEP=1;
  the window hosting 3 on one board, bots joining from UK and India with `tools/codespace.sh join CODE --bot N`).
  **It was NOT run**: the session ended while the Codespaces were building; their build was stopped half-way and
  both Codespaces were stopped (nothing left running). To run it (ask the owner first; it uses Codespace time):
  1. `KEEP=1 bash tools/internet-tests.sh sync-only` (copies this checkout's changed sources to both Codespaces and
     builds there; "(no such case)" at the end is expected; KEEP=1 leaves them on).
  2. `tools\window-play-test.ps1 -LocalBots 0` (the window hosts 3 on one board with sound; prints the room code).
  3. Each Codespace joins it (UK and India, names from `gh api user/codespaces`):
     `gh codespace ssh -c NAME -- "cd /workspaces/spikeout-recomp && (nohup tools/codespace.sh join CODE
     --net-rollback 12 --input-delay auto --bot 11 > ~/w.log 2>&1 &)"` (another seed, e.g. 12, for the second).
  4. The owner plays; afterwards stop the bots (`gh codespace ssh -c NAME -- pkill gamerun`) and the Codespaces
     (`gh codespace stop -c NAME`). The window's log: build-jit\shots\play3\window.txt (frames a second, "differs").

### Next steps (exactly)

1. Get the first wrong frame from the window: `tools\window-autoplay-test.ps1 -Frames 6000 -Seed 1 -HashFrom 4300`
   (writes `build-jit\shots\auto\window.hashes` and `bot1.hashes`, `bot2.hashes`: board 0 = the window's own board,
   each frame). The build already has SPIKEOUT_RAM_HASHES (rebuilt 19:17); the script is fixed (Join-Path).
   Compare: `tools\debug\hashcmp.ps1 bot1.hashes window.hashes -Around 3` (first differing frame and pages; is it a
   copy of another frame's RAM) and `tools\debug\hashruns.ps1 bot1.hashes window.hashes FROM TO` (every run of each
   frame in the window, rollbacks included, against the bot's last: shows whether a frame first ran right and a
   re-run went wrong, which pointed at the two bugs fixed today).
2. Then the bytes: the window `SPIKEOUT_RAM_DUMP=DIR@FROM-TO` (whole RAM at the hash-check frames, tools/netplay.hpp
   ram_dump) or add a per-frame dump; gamerun `--ram-dump B:LO-HI@FRAME:N FILE` (rows for re-runs too);
   `tools\debug\dumpcmp.ps1` / `dumpruns.ps1` compare them. Then who writes them: gamerun `--watch-write LO-HI@F:N`.
3. Suspects to check in the window vs gamerun (tools/spikeout.cpp one-board set-up from ~line 1460, its rollback
   loop, its inputs): how its board reads inputs (guess window, `quiet_ahead`, input delay 0), the made boards'
   threads following a going back (`rewound_to`), lazy snapshot `set_local`, the side runs' worker threads (min of
   cores), pacing (frames run late: a frame held back by the link when a made packet is late?).

## Fixed today (aa5a6ff, and 2663956 before it) — all proven

- InputTable::guess re-guessed a frame from inputs that came since; one board's side runs read a frame again in
  later frames, so a frame ran on one guess and was compared with another: wrong guesses never found. Now a frame
  keeps its first guess until set or gone back over.
- Rollback goes back one frame further while a board is made (a cabinet a frame ahead reads its buttons a frame
  early).
- Made-packet correction (NodeCheck step 3) **off by default** in gamerun (`NODE_CORRECT=1` brings it back): it put
  in real packets that weren't theirs; the window never had it. The node check's "whole packet differs" / "bytes
  differ" lines are NOT reliable (it pairs the wrong packets): don't chase them.
- Windows: MinGW double-freed `thread_local` std::vectors at thread exit (watched_memory.cpp): now held by pointer,
  never freed. Very likely the heap crash that dropped the parallel restore (memory note parallel-restore-todo:
  it can probably come back now).
- `keep_ram` read outside its lock (one_board.hpp; ThreadSanitizer).
- 2663956: made links send their own world bytes (a GAME_BYTES_LOG else-slip), the HUD task 0x11CF80-0x11CFFF per
  cabinet, hand-back view bits, done_seed/ram_at trimmed after a going back, kSuspect/kHave/kIdle as urgent
  datagrams (a leaver no longer holds the others 2-5 s), the four-player internet case (4th player on this PC).

## Tools made today

- gamerun `--rollback-force DEPTH:EVERY[:FROM]` (network play's rollback with no network, back DEPTH frames every
  EVERY; the other cabinets' buttons wrong in a first run) and `--rollback-sim LATENCY` (the other seats' buttons put
  in LATENCY frames late: real guesses and goings back). Env: `ROLLBACK_SIM_LAZY=1` (lazy snapshots as in play),
  `ONEBOARD_HOLD_BOARD=N` (that board held whole, the others to side runs), `ROLLBACK_LOG=1` (each going back),
  `ROLLBACK_FORCE_WRONG_ALWAYS=1` (test of the test), `ONEBOARD_NO_SHADOWS=1`.
- tools/onepc-diff.ps1 (the finder): `-TimeoutMin` (default 15: a hung run is stopped and said, exit 3), takes each
  frame's last row (rollback re-runs), works from any folder. Use `-OneBoard @('--one-board-tasks', ...)` as network
  play runs; `-NoShadows` for two or more made boards.
- tools/debug (helper scripts; edit the paths inside if the worktree moves):
  `hashcmp.ps1` (first differing frame of two --ram-hashes files), `hashruns.ps1` (every run of each frame),
  `dumpcmp.ps1`, `dumpruns.ps1` (--ram-dump files), `rbsim-batch.ps1` (finder × latencies × seats),
  `rbforce-batch.ps1`, `suites.ps1` (build, onepc-tests, onepc-net-tests, leaver-first, rbbot repro; each step its
  own PowerShell process with a time limit), `lazy-gdb.ps1` (crash hunt under gdb), `av-test.ps1` (2D and sound on),
  `rbbot.ps1` (gamerun players through the lobby on this PC), `asan-run.sh`, `tsan-run.sh` (WSL).
- tools/window-play-test.ps1 (a person plays: the window hosts 3 players on one board, two bots join; logs in
  build-jit\shots\play3) and tools/window-autoplay-test.ps1 (no person; `-HashFrom F` for per-frame hashes).

## Also found, not fixed

- A gamerun bot (every board) died silently ~80 frames after being put back in step with the host's state
  (window play test, bot1, frame ~4000; build-jit\shots\play3\bot1.txt). The catch-up/resync path; not looked at.
- `tools/onepc-tests.ps1 -Only ONE_CASE` fails (line 97: a single job isn't an array); give two cases.
- Window tests not rerun since the side-run changes of 116b566..aa5a6ff.

## Test results at aa5a6ff

- Local: onepc-tests 14/14 (4 skipped: no cam3.rec), onepc-net-tests all the same, leave3/vanish3 -LeaverFirst
  all the same, rbbot 3 players one board the same on all three (~800 goings back each).
- Simulated network (--rollback-sim) 3, 6, 8, 10, 12 frames × each of three staying: the same every frame.
- ASan (4 runs) and TSan (1,500 frames) in WSL: clean after the keep_ram fix.
- Internet (UK + India Codespaces + this PC): two, relay, three, four (two players on this PC), vanish, stall,
  spectator, spectators, oneboard, oneboard-rb, mixed-host1: all pass. mixed: about one game in two fails.

## Lessons (cost hours today; the memory notes have them too)

- Fail fast: every batch stops at its first failure and every run/wait has a time limit (an until-grep for "same"
  never matched the finder's "SAME:" and looped 11 hours; a finder run hung 40 minutes).
- Don't rebuild gamerun while an internet run uses build\gamerun.exe (a half-written exe broke two cases); don't run
  heavy local tests during internet runs (a relay join failed); run the internet script once at a time (it refuses a
  second: "a run is going already").
- Run the suites as their own PowerShell processes, not PowerShell jobs (jobs changed their encoding and broke them).
- Memory: this PC had 43 of 48 GB committed (CC_Engine_x64 6.5 GB, the Android subsystem's vmmemWSA, other);
  onepc-tests' parallel runs then died with std::bad_alloc: run its cases a few at a time.
- An exit-time crash loses buffered output: a "difference" at a log's or hash file's end may be a crash. Check the
  run reached its last line ("CPU time").
- gdb on Windows turns on the debug heap: a crash only under gdb isn't proof of one without it.
- Codespaces: only two run at once, even on the owner's Pro plan; a third (spikeout-net-useast, US East) exists,
  built, stopped. The four-player case puts two players on this PC.
