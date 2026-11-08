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
   looked at; answering on GitHub is public: ask the owner first); the JIT release track in CLAUDE.md "Latest"
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
   Ask the owner when to schedule it.
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
