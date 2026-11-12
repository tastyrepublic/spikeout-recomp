# Player reports (2026-11-09)

How a player tells us about a problem, and what the owner sets up once for it.

## What the player has

- `spikeout.log` in the data folder (next to settings.ini): everything the window prints, each line with its time;
  the run before kept as `spikeout-prev.log`; 20 MB at most. Its first lines: the version, the system, the CPU, the
  displays and every controller SDL sees (name, maker and product IDs, GUID, whether SDL can read it as a gamepad,
  its mapping), then each one as it comes and goes, and when the game takes it into its list of pads.
  (The first report, issue #1, found with it: a pad plugged in before the game started was never in the list, its
  "added" event taken by the start-up screens; now every pad connected at the start is added there.)
- A crash writes `crash-<time>.txt` (what, where as module+offset, the stack, the log's last 400 lines; Windows
  also `crash-<time>.dmp`, a minidump); the next start says so at the bottom.
- Esc > Game > **Report a problem** (the owner's design: GitHub for players logged in, a zip for the others):
  - **What happened**: free text (any language through the system's input; Steam Deck: its on-screen keyboard; the
    menu's font shows Latin letters only, other scripts are sent fine). Shift+Enter: a new line; Ctrl+V: paste.
  - **Detailed logging**: the network and timing lines (`SPIKEOUT_NET_DEBUG`) in the log too, from the next start.
  - **Log in to GitHub** (not logged in): GitHub's device flow: the game shows the address, the code and a QR code
    of the address (Project Nayuki's QR Code generator, third_party/qrcodegen, MIT) for a phone; "Open the page here"
    opens this computer's browser only if chosen. GitHub's own page asks for the password (GitHub allows no other
    way); the game never sees it. Back to the report by itself once logged in.
  - **Send to GitHub**: chosen only logged in and with something written (else shown greyed: "log in first",
    "write what happened first"). A public issue on tastyrepublic/spikeout-recomp as the player: their words, the
    version, the computer and its controllers, the newest crash's first lines; the logs, the crash report and the
    settings as comments (each its last 60,000 characters). Internet addresses and the user name (in folder paths)
    are taken out; an `@` the player typed mentions nobody. A send that fails says why at the bottom.
  - **Save as a zip**: the same files, the words (what-happened.txt) and the minidump, in the data folder; a page of
    the game's own says its name and folder, **Copy its path**, and on a computer **Open its folder** (no window of
    the system's: the same on every device; a phone's or tablet's share sheet can come there later).
  - **Last report** (only once one was sent): its number; Enter: a page of the game's own (no browser): open or
    closed, its title, each answer (who, its first words; selected: up to 700 characters of it below), Refresh.
  - **Log out of GitHub** (logged in): the login (github-login.txt in the data folder) forgotten.

Code: tools/player_report.hpp (log, crash reports, zip, the issue's text), src/net/report.{hpp,cpp} (logging in,
sending), src/ui/menu.cpp (the text page, Menu::replace), tools/spikeout.cpp (the pages).

## What the owner sets up once: a GitHub App (only for the login)

1. GitHub > Settings > Developer settings > GitHub Apps > **New GitHub App** (https://github.com/settings/apps/new):
   - Name: e.g. `Spikeout reports`. Homepage URL: https://github.com/tastyrepublic/spikeout-recomp
   - Callback URL: empty. **Expire user authorization tokens: off** (a player's login stays). **Enable Device
     Flow: on**. Webhook: **Active off**.
   - Permissions > Repository permissions > **Issues: Read and write** (nothing else).
   - Where can this GitHub App be installed: **Only on this account**. Create.
2. Copy the **Client ID** (Iv23...: public, not a secret). No private key is needed.
3. Install App (left menu) > your account > **Only select repositories: spikeout-recomp** (a player's login can
   open issues only where the app is installed).
4. The Client ID is in CMakeLists.txt as SPIKEOUT_GITHUB_CLIENT_ID's default (public, as the lobby's address is):
   the owner's app "Spikeout reports", App ID 5678845, Client ID Iv23lix0ed2ATx6BOHJC (made 2026-11-09). Empty: the
   game offers no login, a zip only.

The lobby is not involved.

## Tests (no real GitHub)

- The window against a fake GitHub (2026-11-09): `node fake-github.mjs` (port 8791: users, issues, comments, the
  device flow, approving by itself on its second ask), then the window with `SPIKEOUT_GITHUB_CLIENT_ID=test-id
  SPIKEOUT_GITHUB_WEB=http://127.0.0.1:8791 SPIKEOUT_GITHUB_API=http://127.0.0.1:8791`: logged in by the device flow,
  the issue and its comments sent with the player's token; Save as a zip: the zip and the game's own page.
  Env switches: SPIKEOUT_LOG=0 (no log file), SPIKEOUT_REPORT_REPO.
- Measured (Windows, 3,600 frames each, twice): no log / log / log + detailed logging: CPU 25-27 / 24 / 24-25 s, every
  second at full speed with detailed logging, every recording 60 of 60 hashes in gamerun.

## Problems the game notices itself, and sending them (2026-11-11, branch claude/problem-reports)

The owner's choice: "auto collect their network play data to improve and find bugs before it happens". Two parts.

Part 1, in the game (tools/problem_report.hpp): when something goes wrong in network play, what is needed to find out
why is saved there and then, in the data folder's saves/, locked to the owner's key (tools/report_lock.hpp):
`problem-<time>-<kind>.zip.locked` (`gamerun --unlock FILE --report-key KEY` gives the zip back). Kinds:

- desync: this machine's boards differ from another's (the hash checks; tools::NetPlay's desync callback);
- node: a made packet differs from its real one (one board a PC; tools::NodeCheck, through NetPlay::problem);
- hang: no frame finished for 10 s while a network game runs (problems::Watchdog: the shown board's thread beats in
  pace(); its stack taken by stopping it (Windows: SuspendThread, walked as a crash report's, and a minidump of every
  thread) or by a signal (Linux, macOS: SIGUSR2, the thread's own backtrace));
- stall: a frame over a second late (tools::SpeedReport::on_long_gap);
- crash: the run before crashed (its report and minidump).

In the zip: problem.txt (what, the frame, the version, this computer, input delay and board mode), the log's last
lines (scrubbed), hang-stacks.txt / hang.dmp, and the newest network game's recording (saves/net-game-*.inputs and
.state, gamerun --net-replay). Report a problem takes the newest three in. gamerun prints "problem: KIND at frame N" for
the same (tests). Never on the game's time: note() queues; a thread of its own waits 200 ms for the log, zips, locks and
writes. At most one a kind a minute, ten a run, the newest ten kept.
A one-board machine records its games too: the recording starts as the game does (and again after being put back in
step), while every board still runs whole, before the hand-over to one board (checked in the owner's four games of
2026-11-11, all one board: each recorded). Its replay (gamerun --net-replay) runs every board whole: the game the
players had, not the side runs' own mistakes (tools/onepc-diff.ps1 -MadeBoard for those).

Part 2, sent by the game when the player allows it (settings.ini send_reports: -1 not asked, 0 no, 1 yes to network
play's only, 2 yes to every problem). Asked once, at the first start since 2026-11-12 ("Help improve the game?"; before,
the first time Network play opened, "Help improve network play?", for network play's only: a 1 is asked again); Game >
Send reports and Network play > Send reports change it (to 0 or 2). A 1 sends every kind but startup and hang-alone
(Sender::send's `everything`). With yes:
after each network game, its numbers (a JSON line: version, OS, CPU, seats, one board or every, rollback, input delay,
seconds, frames, round trips, the rollback's and the node check's reports, the problems by kind; no names, no
addresses) wait in saves/outbox/; then, and at each start, a thread of its own sends them and the problem bundles not
sent yet (saves/sent.txt) to the lobby (net::report::upload: POST https://LOBBY/v1/stats and /v1/problem). Never
while a game runs.

The lobby (lobby/src/index.js, "Reports sent by the game itself"): the stats and the files' list in a database of
its own (the Reports Durable Object, SQLite); the files (bundles and inputs) in an R2 bucket, spikeout-reports (the
owner switched R2 on, 2026-11-11). Never past R2's free 10 GB: the database adds up every file's size and lets the
oldest go once 8 GB is passed (deleted from the bucket too); and the bucket's own rule (expire-90d) deletes anything
older than 90 days whatever the count. Files refused unless locked ("SPKLOCK1") and over 24 MB (bundles) / 8 MB
(inputs); each address's day limited (60 stats, 12 bundles, 30 inputs; counted by a hash of the address, never kept);
the owner's lists and downloads behind ADMIN_TOKEN (a secret, compared trimmed: a piped one ends in a line break).
tools/fetch-reports.ps1 fetches what's new, unlocks it and prints this month's use. R2's free operations too: the
lobby counts each month (UTC) its class A (a file stored; deletes, free, counted too) and class B (a file read) and
refuses past half the free amounts (500,000 A, 5,000,000 B: "full for this month"; the game keeps the file to send
another time); GET /v1/admin/usage shows them.
Deployed 2026-11-11 (version 9cc08b9e); ADMIN_TOKEN set from %USERPROFILE%\spikeout-admin-token.txt (the fetch
script's). A stats row and a 50 KB bundle, version "deploy-check", are the deploy's own checks.
Tests: lobby `npm test` (9: the numbers kept and read back with the key only, bundles locked only and fetched back
whole, the day's limit, inputs listed apart, a 2.5 MB file back whole, the cap: past REPORTS_KEPT the oldest gone).

Inputs too (the owner, 2026-11-11, for smarter rollback guesses): with the same yes, each network game's recorded
inputs (NAME.inputs: every seat's buttons, frame by frame; no state, no names; about 1 MB for 20 minutes of four)
locked and sent after the game (saves/outbox/inputs-*.locked; POST /v1/inputs, the lobby's database, 30 a day an address).
tools/fetch-reports.ps1 fetches them into inputs/ and unlocks them; tools/guess-study.cpp reads them. A window closed
during a game keeps its numbers and inputs too, sent at the next start.

Tests: SPIKEOUT_TEST_HANG=FRAME:SECONDS (the shown board's thread stood still that long at that frame of a network
game: a hang over 10 s), SPIKEOUT_TEST_STALL=FRAME:MS (a frame that late), SPIKEOUT_TEST_POKE (a byte changed on one
machine: out of step), NODE_SPOIL (a made packet spoiled: a node difference); SPIKEOUT_TEST_REPORTS=1 (a --frames run
sends too), SPIKEOUT_UPLOAD_URL=http://127.0.0.1:PORT/v1 (a local lobby: `npx wrangler dev`).

## Better logs and reports (2026-11-12)

The owner's request after issues 16 and 18 ("we still need better debug logs and reports, both automatic and by hand"):
what those two reports lacked, added.

- **Every message the player sees is in the log.** The network room page's (NetGame::set_status, "net: shown: ...";
  a failure "net: failed: ..."), the notices at the bottom (Shared::say, "notice: ..."). Issue 18's second watch failed
  with its reason on the screen only.
- **A join or a watch the host never answers** (Session::greeted: no welcome yet) says how long it has waited every
  15 s and gives up after 45 s, with a reason. It waited for ever before, silently.
- **The start's steps** ("start: making the window", "reading the ROM set", "translating the game", "closing the start
  screen", "handing the window to the GPU", "the GPU is ready", "setting up the sound", "starting the game", "the first
  picture shown"), each with the log's time, and **watched**: the problem saver and the hang watchdog start first thing
  now, and a step standing still 60 s (120 s for the translation, beaten by each part done) is a "startup" problem
  bundle, with the step, every thread's stack and a minidump (issue 16's first start standing still at 90%+ left
  nothing). SPIKEOUT_TEST_START_HANG=SECONDS holds the first step that long (tested: a bundle at 60 s, the start then
  going on).
- **A game alone is watched too**: the watchdog on while its frames run (not with the menu open, nor once a board has
  stopped), its hangs "hang-alone".
- **Every thread's stack in a hang** (Watchdog::other_stacks): Windows lists the process's threads (Toolhelp) and walks
  each, stopped, into a fixed array (nothing allocated while it's stopped: it may hold the heap's lock), Linux asks each
  of /proc/self/task by SIGUSR2 (SA_RESTART), macOS the stuck one only. Threads named for it (report::name_thread kept
  by the system's id): the window, the boards, the translation, the log, the saver, the sender, the watchdog.
- **The last five logs** (spikeout.log, spikeout-prev.log, spikeout-prev2.log ... prev4); Report a problem takes them
  all (the zip; the GitHub issue's comments while there's room).
- **Each board's state once a minute in network play** without detailed logging (its logic count, inputs, network
  board; "shown", "on side runs").
- **Reading a report's stacks**: tools/symbolize.ps1 -File FILE [-Version V] downloads the release's program with its
  symbols (the draft release symbols-vV, which the release workflow makes) and gives each "spikeout.exe+0x..." its
  function. Its first use found issue 18's freeze (below).

### Issue 18: a spectator's freeze at a seat switch, found and fixed

A spectator's every board paces itself only while shown (run_game's pace()). A board never shown since the join kept
its pacing's start from then; joined in progress, it had caught up fast (466 frames in 1.1 s in issue 18's log, 1,266
in 4.3 s in a Taiwan report), so once shown, it thought itself that far ahead and held each frame until the clock caught
up: 7 s in issue 18 ("a frame 7023 ms late"), over 10 s in the Taiwan one (its hang bundle's stack, symbolized: the
game thread in pace()'s wait, steady_clock::now). And each board's speed measure counted the time it was out of view as
one frame that late (the "806,251 ms" and "85,059 ms" stalls, 1,078 stutters). Now a board shown again starts its
pacing and its measure afresh (repace). Tested with two windows, a spectator switching seats 60 times: every switch
gave a 0.5-0.8 s "stutter" before, none over 150 ms after. A spectator's first frames from a game's start (waiting
kWatchLag behind the players by design, ~2 s) aren't a stall either.
