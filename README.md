<p align="center"><img src="assets/icon/icon.png" alt="Spikeout: Final Edition Recomp" width="160"></p>

<h1 align="center">Spikeout: Final Edition Recomp</h1>

<p align="center"><b>Spikeout: Final Edition (SEGA, arcade, Model 3, 1999) as a native PC game: the game's own PowerPC
program translated to native code, the 3D on the GPU at up to 6x the arcade's resolution, widescreen up to 21:9, and
four-player co-op over the internet, one player a PC, as four linked cabinets played it.</b></p>

<p align="center">
  <a href="https://github.com/tastyrepublic/spikeout-recomp/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/tastyrepublic/spikeout-recomp?include_prereleases&label=release"></a>
  <a href="LICENSE"><img alt="Licence: GPL-3.0" src="https://img.shields.io/badge/licence-GPL--3.0-blue"></a>
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%2F%20Steam%20Deck%20%7C%20macOS-informational">
  <a href="https://buymeacoffee.com/donfy"><img alt="Buy me a coffee" src="https://img.shields.io/badge/Buy%20me%20a%20coffee-FFDD00?logo=buymeacoffee&logoColor=black"></a>
</p>

<p align="center">
  <a href="https://github.com/tastyrepublic/spikeout-recomp/releases/latest">Download</a> ·
  <a href="#latest-update">What's new</a> ·
  <a href="#play">Install</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#online-play">Online play</a> ·
  <a href="#faq">FAQ</a> ·
  <a href="#reporting-a-problem">Report a bug</a> ·
  <a href="https://buymeacoffee.com/donfy">Buy me a coffee</a>
</p>

<p align="center">
  <img src="docs/images/rooftop-16x9.jpg" alt="A rooftop fight in 16:9 at 4x render scale with antialiasing" width="800">
</p>

> [!IMPORTANT]
> No game files are included: no ROMs and nothing made from them. You need your own dump of the game as MAME's and
> Supermodel's `spikeofe` set (`spikeofe.zip`). It is read on your computer, and the game's program is translated
> there on the first start. An unofficial fan project, not made, approved or supported by SEGA, and not affiliated
> with it.

## Latest update

**0.1.0-preview5** ([all the details](https://github.com/tastyrepublic/spikeout-recomp/releases/tag/v0.1.0-preview5))

- **Thank you for the reports:** in its first day, preview4's automatic reports brought in about 170 network games
  from players in Hong Kong, Taiwan, Japan, Canada, Australia and the UK, and showed us problems we couldn't see on
  our own machines.
- **The game list shows where each game is:** its stage and area, like "Stage 2 · Warehouse", kept up to date while
  the list is open.
- **Watching:** switching between players is smoother (the picture could stand still for seconds), and a game that
  never answers now gives up after 45 seconds with a reason.
- **Screenshots (F12)** now look like the game on your screen: your render scale, picture shape, CRT colours and
  filter.
- **Esc > Game > Game folder** shows where your settings, save states, screenshots and logs are, and opens it.
- **XInput sticks** always show Xbox button names.
- **Reports that find more:** any crash or freeze, also at the start; what a freeze was waiting for; when each
  player's buttons arrived over the network; how smooth each game was; and the layout of a stick you set up yourself.
  The game asks again once, as the question now covers more.
- **Next: the next update focuses on network play first:** a room freezing when a late player takes their cabinet,
  then a slow PC slowing everyone (see [Roadmap](#roadmap)).

## About

Spikeout was SEGA's four-player beat 'em up for linked arcade cabinets: each player stood at their own cabinet, and
the cabinets' network boards joined them into one game. This project runs the game's own program, written for the
Model 3 board's PowerPC 603e, translated to native code for your PC, and rebuilds the rest of the board in its own
code: the Real3D graphics, the tile generator's 2D layers, the two SCSP sound chips and their 68000, the MPEG music
board, the network board and its link, the inputs and EEPROM. [Supermodel](https://github.com/trzy/Supermodel) is
the reference for how the hardware behaves, and every change to what the game shows or plays is checked against it.

The game isn't emulated an instruction at a time: on its first start, the program in your ROM set is analysed and
translated once with LLVM (about 15-25 seconds on a six-core PC), and kept, so later starts take a moment. That
translation is checked against the project's own ahead-of-time build by recordings of play, RAM hash for RAM hash.

## Status

| Area | State |
| --- | --- |
| The game: attract mode, every stage, the ending, the rankings | Working |
| The 3D on the GPU (Vulkan) or in software | Working: render scale up to 6x, antialiasing, quads drawn whole |
| Widescreen (16:10, 16:9, 21:9) | Working: the fights see more to the sides; the 2D screens (the intro, the title and its doors, the fighter select, the linking screen) widened from the game's own art |
| Sound and music | Working: the cabinet's front and rear speakers apart, on four channels or two outputs |
| Online play: up to four players, each on their own PC | Working, being tested (help wanted; expect some lag and stutters): an internet lobby with room codes, games on your own network found automatically, rollback, spectators (see [Online play](#online-play)) |
| Save states | Working (one player, not in network games) |
| Reporting a problem from the game | Working: a GitHub issue sent from the menu, or a zip (see [Reporting a problem](#reporting-a-problem)) |
| Windows 10 / 11 (64-bit) | Working |
| Linux / Steam Deck (x86-64) | Working; built in Valve's Steam Runtime SDK, for Linux from about 2020 on; players run it on Steam Decks (SteamOS), still being tested by them through the automatic reports |
| macOS 13.3 or later (Apple silicon and Intel) | Working on players' Apple silicon Macs, including network play; still being tested by them through the automatic reports (Intel Macs: please report how they run) |

**This is a preview.** Please report anything that looks or plays differently from the arcade: see
[Reporting a problem](#reporting-a-problem).

## Play

| System | Download |
| --- | --- |
| Windows 10 or 11 (64-bit) | `spikeout-<version>-windows-x64.zip` |
| Linux / Steam Deck (x86-64: glibc 2.31 or later, most Linux from 2020 on) | `spikeout-<version>-linux-x64.tar.gz` |
| macOS 13.3 or later, Apple silicon (M1 and later) | `spikeout-<version>-macos-arm64.zip` |
| macOS 13.3 or later, Intel | `spikeout-<version>-macos-x64.zip` |

**Windows**

1. Unzip it anywhere (not inside Program Files).
2. Run `spikeout.exe`. Windows may say "Windows protected your PC": the program isn't signed. Choose More info, then
   Run anyway.
3. Choose your `spikeofe.zip` when it asks. The game is then prepared from it, once; it remembers where the zip is.

**Linux**

1. Unpack it anywhere: `tar -xzf spikeout-<version>-linux-x64.tar.gz`.
2. Run `./spikeout` in its folder, and choose your `spikeofe.zip` when it asks.
3. Optional: `./add-to-menu.sh` puts it in your desktop's applications menu, with its icon (`spikeout.png`).

**Steam Deck**

1. In Desktop Mode, copy the download to the Deck (e.g. Downloads) and unpack it there: right-click it in the file
   manager > Extract > Extract archive here. (Unpacking it on Windows can lose the file's permission to run.)
2. Steam > Games > Add a Non-Steam Game to My Library > Browse, set the file type to "All files", pick `spikeout` in
   its folder, Add Selected Programs. Leave Compatibility's "Force the use of a specific Steam Play compatibility
   tool" off: it's a native Linux game.
3. Start it once in Desktop Mode to choose your `spikeofe.zip`; after that, play it from Game Mode. The game's menu on
   the Deck: View + Menu together.

The full guide, step by step (controls, the picture, online play, updating): [docs/steam-deck.md](docs/steam-deck.md).

**macOS**

1. Unzip it and move `Spikeout` to Applications (or anywhere).
2. The app isn't signed: the first time, right-click it and choose Open (or open it, then System Settings > Privacy &
   Security > Open Anyway).
3. Choose your `spikeofe.zip` when it asks.

**Where things are kept:** your settings, the cabinets' settings and rankings, the translated game and the game's
log (`spikeout.log`) are in your user data folder:

| System | Folder |
| --- | --- |
| Windows | `C:\Users\<you>\AppData\Roaming\spikeout-recomp\Spikeout` (type `%APPDATA%\spikeout-recomp\Spikeout` in Explorer's address bar) |
| Linux, Steam Deck | `~/.local/share/spikeout-recomp/Spikeout` |
| macOS | `~/Library/Application Support/spikeout-recomp/Spikeout` |

Save states (<kbd>F9</kbd>) go in its `saves` folder, screenshots (<kbd>F12</kbd>) in its `screenshots` folder. Put an
empty file named `portable.txt` next to the program and all of it is kept in the program's folder instead.

## Controls

Change them in the menu (<kbd>Esc</kbd>), under Controls.

| Key | Does |
| --- | --- |
| <kbd>↑</kbd> <kbd>↓</kbd> <kbd>←</kbd> <kbd>→</kbd> | Move |
| <kbd>Z</kbd> <kbd>X</kbd> <kbd>C</kbd> <kbd>V</kbd> | Beat, charge, jump, shift |
| <kbd>5</kbd> / <kbd>1</kbd> | Coin / start |
| <kbd>Esc</kbd> | The menu (a pad: Guide, the touchpad, or Back + Start) |
| <kbd>F9</kbd> / <kbd>F10</kbd> | Save state / load the last (press twice; <kbd>F9</kbd> within 5 seconds undoes it) |
| <kbd>F12</kbd> | A screenshot (in the folder the game was started from) |
| <kbd>Tab</kbd> (held) | As fast as it goes |
| <kbd>F2</kbd> / <kbd>F1</kbd> | The arcade's test and service buttons |

Gamepads: d-pad or left stick, A beat, X charge, B jump, Y shift, Start, Back coin. Free play is the game's own
setting: Esc > Game > Arcade settings.

**How quickly a press shows (from the next version).** The keys and pads are read just before each of the game's
frames starts, the frame is run, and its picture is shown as soon as it is drawn, as Supermodel does it. Measured on
a 100 Hz screen, a press reaches the screen in about **39 ms** (from 29 to 54), where earlier versions took about
**77 ms**. The game's own speed is unchanged: the arcade's 57.52 frames a second.

**The left stick (from the next version)** works as the cabinet's 8-way lever: a light push does nothing (a round dead
zone, 30% of the stick's reach), and past it the direction is one of eight equal slices of 45 degrees, so the
diagonals are as easy to hold as up, down, left and right. The d-pad and the keys are unchanged.

## Features

- **Render scale** up to 6x the arcade's 496 x 384, or Auto to fit the window; antialiasing; quads drawn whole
  (Supermodel's quad rendering).
- **Picture shape**: the arcade's 4:3, the window's, 16:10, 16:9 or 21:9. In widescreen the fights see more to the
  sides, and the game's 2D screens are widened from its own tile maps.
- **CRT colours** as arcade monitors showed them (Supermodel's), and **screen filters**: scanlines, or a CRT's.
- **The cabinet's sound**: the front and rear speakers apart, on a four-channel output or two outputs.
- **The arcade's settings**, as an operator sets them: difficulty, region, free play, the demo's sound, and its test
  menu.
- **Save states**, checked before they load and kept across updates.

<p align="center">
  <img src="docs/images/fight-16x9.jpg" alt="A fight in a hall, 16:9" width="49%">
  <img src="docs/images/boss-16x9.jpg" alt="Here comes the boss, 16:9" width="49%">
</p>
<p align="center">
  <img src="docs/images/gate-21x9.jpg" alt="Break the gate, in 21:9" width="98%">
  <br><sub>21:9: the fights see more to the sides</sub>
</p>
<p align="center">
  <img src="docs/images/title-16x9.jpg" alt="The title's doors, widened to 16:9 from the game's own tile map" width="49%">
  <img src="docs/images/select-16x9.jpg" alt="The fighter select, widened to 16:9" width="49%">
  <br><sub>The 2D screens widened from the game's own tile maps: the title's doors, the fighter select</sub>
</p>
<p align="center">
  <img src="docs/images/crt-4x3.jpg" alt="The arcade's 4:3 with CRT colours and the CRT filter" width="60%">
  <br><sub>The arcade's 4:3, with a Sega arcade monitor's colours and the CRT filter</sub>
</p>

## Online play

Each player plays on their own PC, as each player had their own cabinet. Every PC runs all the cabinets of the game
and only the players' inputs travel, so the game is the same on every screen.

> [!IMPORTANT]
> **Network play is still being tested, and we need your help.** Please play it with friends, on your own network
> and over the internet, and tell us how it went. Expect some rough edges while it's tested: moments of lag, a
> stutter now and then (more often with players far apart), and a short pause when someone joins a game in
> progress. If something goes wrong, or feels worse than that, use **Esc > Game > Report a problem** in the game:
> it collects the logs we need (see [Reporting a problem](#reporting-a-problem)). And when the game asks, please let
> it send [network play reports](#network-play-reports-you-choose): real games are what we need most to find and fix
> network play's bugs.

<p align="center">
  <img src="docs/images/network-play.jpg" alt="A three-player network game: the seat list with each player's connection and board mode" width="800">
  <br><sub>Three players, one PC each, about 80 ms apart: the seat list on the right shows each player's connection and
  how their PC runs the game (ALL BOARDS or 1 BOARD, RB: rollback), and for the others how often their presses were
  guessed wrong here (RB 0, RB 4)</sub>
</p>

- **Host a game** (Esc > Network play): on the internet lobby, or only on your own network. You get a six-character room
  code to share; private rooms are joined by code only.
- **Join a game**: the list shows the games on the lobby and on your own network (no setup, nothing to forward), or
  join by code. Players can join a game already started; their cabinet joins as a new player joins a linked cabinet.
- **No settings to tune**: the input delay and rollback are set automatically from how far apart the players are.
- **Spectators**: up to four can watch a game (Watch in the list, or by code), a moment behind, never slowing the
  players.
- **If the host leaves**, the game goes on and another player hosts it, under the same room code.
- **Weaker PCs** (a handheld, an older laptop) automatically run only their own cabinet in full; there is no setting for it.

### How the arcade linked its cabinets, and why that makes online play hard

**In the arcade**, up to four Spikeout cabinets stood side by side, joined by cables into a ring. Each cabinet is a
whole Model 3 board: its own PowerPC running its own copy of the game, its own 3D, 2D and sound. Each also has a
network board, a separate little computer (a 68000 with its own program) that passes data around the ring. Every
frame, 57.5 times a second, each cabinet's game writes a block of what its player is doing (position, moves, hits,
the camera) to its network board. The ring carries every cabinet's block to all the others, and each game reads
them before it runs its next frame. The four games stay together only because those blocks arrive every frame,
exactly, and within a fraction of a millisecond: that is what a cable between cabinets gives.

**Modern online games are built for the internet** from the start. Each PC runs its own game and sends the others
what matters (where you are, what you hit) a few times a second. The game is designed so that a late or lost message
is fine: the other players' characters are smoothed, predicted or corrected, and a server usually decides who hit
whom. Small differences between the PCs don't matter.

**Spikeout was never built for that.** Over the internet a message takes 10 to 150 ms, not a fraction of one, and
sometimes it's lost. If a block is late, a real cabinet waits for it (the game slows down); if the data in it were
even slightly different from what the other cabinet sent, the games drift apart (enemies in different places,
different hits) and never come back together. We can't change how the game links: we run its own code.

**What we do instead:**

- **Only the buttons travel.** Every PC runs every cabinet's board, network board included, and the ring between them
  is simulated inside the PC. The game always gives the same result for the same buttons, so if every PC has every
  player's buttons for each frame, every PC has exactly the same game, and the blocks on the ring never have to
  cross the internet at all. A frame's buttons are a few bytes.
- **Rollback hides the distance.** A far player's buttons arrive late, so the game doesn't wait: it guesses they're
  still pressing what they pressed last. When their real buttons arrive and the guess was wrong, the game goes back
  to that frame and runs every frame since again with the right buttons, in a few milliseconds, before the next
  picture is shown. You see the right result, a moment late; usually nothing at all.
- **One board for weaker PCs.** Running four whole arcade boards (below) is a lot. A PC that can't keep up runs only
  its own player's board, and works out what the other cabinets would send from their buttons, running just those
  parts of the game's own code on the side (below, "Every board and one board").

**Why speed matters so much.** On every board, a PC runs four complete arcade boards at once: four copies of the game,
four network boards, and the sound and pictures of the one you watch, all within a frame's 17.4 ms. Then rollback
asks for more: when a guess was wrong, several of those frames have to be run again, inside the time of one frame,
before the next picture. A PC that only just keeps up runs fine until the first wrong guess, then stutters. So every
millisecond saved means smoother games with players far apart, more PCs that can run every board, and smaller
devices (handhelds, tablets, later phones) that can play at all. A lot of this project's work goes into it, and it
goes on.

### Every board and one board

Each PC in a network game runs the cabinets' boards in one of two ways, chosen for it automatically (Auto), so one
game can have both:

- **Every board**: the PC runs every player's cabinet, all of them, from the same inputs. Only the button presses
  travel between the PCs, so every PC has exactly the same game, frame for frame. Nothing is guessed except, over the
  internet, the other players' latest presses, which rollback corrects (the game goes back a few frames and runs
  them again with the real presses, too quick to see).
- **One board**: the PC runs only its own player's cabinet in full. The other cabinets aren't run there; what they
  would send over the link is worked out from the game's own code, run on the side for just those parts, and checked
  against the real cabinets' messages.

**Which one is my PC on?** The seat list on the right of the screen says it (it's on by default: Esc > Display >
Network status). Under each player's name is a line of badges, that player's PC's own:

| Badge | What it means |
|---|---|
| **ALL BOARDS** or **1 BOARD** | Every board, or one board, on that player's PC |
| **RB** or **WAIT** | Rollback (over the internet: it guesses the others' latest presses and corrects them), or waiting for every press (on your own network) |
| **AUTO 2** or **DELAY 2** | Its input delay in frames: your presses are used that many frames later, so they reach the other PCs in time. AUTO: set automatically from the furthest player |
| **RB 3** (another player's row) | How often that player's presses were guessed wrong on your PC in the last 10 seconds: green is fine, yellow and red mean many corrections (more chance of stutters) |

**Why one board is needed.** Each cabinet is a whole arcade board: its game, its 2D and 3D, its sound. Running four of
them at full speed takes a fast desktop PC. A handheld, a tablet or an older laptop can run one board but not four,
and without one board it couldn't play a four-player game at all. One board is also the base for later: phones and
tablets, and a direct cable mode where each device runs only its own cabinet.

**Where each one stands** (the version on this page):

| | On your own network (LAN) | Over the internet |
|---|---|---|
| Every board | **Stable.** No guessing at all: each PC waits a moment for the others' presses. | **Stable.** Rollback corrects the guesses; tested between this project's PC and machines in the UK and India. |
| One board | **Still needs work.** A LAN game uses no rollback, so if a one-board PC's guess about another cabinet is ever wrong, it can't undo it; the game puts that PC back in step from the host's copy instead (a short pause). The plan: feed the other cabinets' real link messages in directly, as a cable between cabinets would, so nothing is guessed. | **Working, newer.** Rollback corrects a wrong guess, and the tests over the internet pass, including games mixing every-board and one-board PCs. It has had less play than every board. |

Auto only puts a PC on one board when it is too slow for every board, so on a capable PC you get every board.
Spectators and players joining a game already started work in both modes, on a LAN and over the internet.

What it took to get there is written up in [docs/netplay.md](docs/netplay.md): the linked cabinets running in slow
motion until the network board's ring was modelled, four boards on one PC, and keeping every PC's game identical.

## Reporting a problem

In the game: **Esc > Game > Report a problem**.

1. **What happened**: write what you did, what you expected and what happened (any language).
2. **Detailed logging**: for network or speed problems, turn it on, start the game again and make the problem
   happen first.
3. Then either:
   - **Log in to GitHub** and **Send to GitHub**: the report becomes an issue on this page, as you, so you see the
     answers. Logging in shows a code and a QR code for your phone; GitHub's own page asks for your password, never
     the game. The game may only open issues here (the "Spikeout reports" app). **Last report** then shows the issue
     and its answers in the game.
   - **Save as a zip**, and attach it to a [new issue](https://github.com/tastyrepublic/spikeout-recomp/issues/new/choose)
     yourself.

A report holds the game's log, its settings, your computer's details and controllers, and any crash report; internet
addresses, your user name and game files are left out. Reports sent from the game are public on GitHub.
It also holds your last network game's recording and any problems the game noticed (below), locked so that only the
project can open them: the zip can be posted publicly, and nobody else can read those parts.

If the game crashes, it writes a crash report in its data folder, and says so at the next start.

### Network play reports (you choose)

Network play is the hardest part of this project, and the part that needs the most real games to get right. On our
own test machines a game behaves the same every time; with real players it doesn't: different PCs, different
distances, different networks, players joining, leaving, continuing. Most network bugs only show there, and by the
time a player writes a report, what caused it is long gone. The same goes for crashes and freezes on PCs we don't
have. So the game can help, if you let it.

**The first time you start the game, it asks whether it may send reports.** (If you said yes in 0.1.0-preview4, which
asked about network play only, it asks once more, because it now covers more.) If you say yes:

- **After each network game**, never during one, it sends that game's numbers: how smoothly it ran, the input delay
  Auto chose and what its tries did, how often rollback corrected a guess and how long that took, how long messages
  took between the players, and which problems it noticed. This tells us how network play really behaves out there,
  and whether each change makes it better.
- **The buttons pressed** in that game (every player's, frame by frame). Rollback has to guess the other players'
  next presses before they arrive; real players' presses are what we need to make those guesses better, so the game
  goes back to fix them less often (fewer stutters).
- **Problems the game noticed itself**, with what's needed to find their cause: the games on two PCs drifting apart,
  the game or its start standing still (with where every part of it was stuck), a long stall, a crash, in network play
  or alone. With the logs and the game's recording, we can replay the exact game that went wrong and find the line of
  code responsible, without asking you to make it happen again. A problem's report also holds the last few minutes of
  when each player's buttons arrived over the network, so a freeze can be rebuilt as your PC had it.
- **Your controller's layout, if you set it up yourself** in Controls: its name, its USB IDs and the button you chose
  for each action (nothing about you). That's how a stick the game doesn't know can get the right layout built in,
  for everyone after you.

What's protected:

- The numbers and controller layouts carry no names and no internet addresses. Everything else (the recordings, the presses, logs) is
  locked before it leaves your PC, so that only the project can open it.
- Nothing is sent while you play; the game never waits for it. It goes after the game ends, a little at a time.
- You can change your answer at any time: **Esc > Game > Send reports** (or Network play > Send reports). With it off, problems are still
  saved on your PC (the newest ten, in `saves/`), for Report a problem.

Every report helps: most of the network fixes so far came from a game that went wrong and could be replayed.

## Roadmap

- Network play, next: a room no longer freezing when a late player takes their cabinet; then a slow PC no longer
  slowing everyone (the room showing who holds it and why, a slow PC catching up by skipping pictures, the others
  slowed only as a last resort, weak PCs running their own cabinet only, and the host able to move a player who holds
  everyone to watching)
- Smarter guessing of the other players' buttons, learnt from real games' presses (fewer corrections, fewer stutters)
- macOS: signed and notarized apps
- Smooth motion: pictures made between the game's frames, for high-refresh screens
- HD texture packs: the game's textures replaced by sharper ones you make or share (off by default; packs are never
  part of the downloads)
- A direct cable mode: each PC runs only its own cabinet, the link over your network, for handhelds on one hotspot
- Maybe direct connections by address
- The menu in Chinese and Japanese
- Later: Android, iOS, Xbox, Switch (one player)

## FAQ

**Where do I get the ROM set?** From your own arcade board. The project can't help you find one.

**It says the ROM set's files differ.** The game is translated from your program ROM, so only the known good set is
accepted: MAME's and Supermodel's `spikeofe`.

**Why does the first start take a while?** The game's program is translated from your ROM set, using every core;
it's kept, so that happens once (and again after an update or with another ROM set).

**Can I play with someone on another system?** Yes: Windows, Linux and macOS play together, as long as all have the
same version.

**My controller isn't seen.** Send a report (Esc > Game > Report a problem): the log says which controllers the game
found and whether it can read them.

**How do I know a new version is out?** At start the game asks GitHub whether there's a newer release, and the menu
says so, with a link to its page; the lobby also tells you when a game is on another version. The game doesn't update
itself. The check can be turned off (Esc > Game > Check for updates).

**Is it emulation?** The board's chips are rebuilt in code, as an emulator does, but the game's own program runs as
native code on your CPU, translated once, not instruction by instruction while it plays.

## Building

Requires CMake 3.20+ and a C++20 compiler (GCC 13+, Clang 17+ or Visual Studio 2022; on Windows, MinGW-w64 GCC from WinLibs is what's
used).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The first configure downloads SDL 3, the menu's libraries (RmlUi, FreeType) and the shader compilers. On Linux, SDL
needs the X11 or Wayland and audio development packages; on Debian or Ubuntu (SDL's list, from its
`docs/README-linux.md`), with OpenSSL for the lobby's connection:

```sh
sudo apt-get install libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev libusb-1.0-0-dev \
  libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev libcurl4-openssl-dev libssl-dev
```

**The game as the downloads have it** (translated at start, nothing from the ROM in the program):
`-DSPIKEOUT_LLVM_JIT=ON` builds `spikeout-jit` (the window) and `gamerun-jit`. The first configure downloads LLVM
18.1.8's source and builds it once with your compiler, into `%LOCALAPPDATA%/spikeout-llvm` or
`~/.cache/spikeout-llvm` (about 40 minutes on six cores). The downloads are built on GitHub by the Release workflow
(`.github/workflows/release.yml`: Windows with MinGW GCC, Linux in Valve's Steam Runtime "sniper" SDK, macOS for
Apple silicon and Intel) and packed by `tools/package-release.sh`.

**The game compiled ahead of time** (for development and the proofs): point CMake at your unzipped ROM set,
`-DSPIKEOUT_ROM_DIR=path/to/spikeofe`, and the build recompiles the game into `build/generated/` and builds
`spikeout` and `gamerun`. That code is made from your ROMs: it stays in the build folder and is never committed or
shared.

The tools: `romcheck` checks a ROM set, `ppcdis` and `codemap` disassemble and map the code, `ppcrecomp`
recompiles it, `gamerun` runs the game without a window (screenshots, recordings and replays, save states, sound to
WAV, network play between processes; see its file's header), `spikeout` is the game in a window. How it's built
inside: [docs/recompiler.md](docs/recompiler.md), [docs/control-flow.md](docs/control-flow.md),
[docs/hardware-notes.md](docs/hardware-notes.md), [docs/memory-map.md](docs/memory-map.md),
[docs/netplay.md](docs/netplay.md), [docs/fidelity.md](docs/fidelity.md).

## Authors, and the use of AI

- **Don** ([tastyrepublic](https://github.com/tastyrepublic)): the project, its direction and every decision on how
  the game should look, sound and play; the testing on real PCs, pads and networks.
- **Claude** (Anthropic's AI model, through [Claude Code](https://claude.com/claude-code)): a contributor to every
  commit, writing much of the code and the documentation.

This project is made with AI. Claude writes code, runs the tests and investigates, at Don's direction; Don decides
what's built, plays it, and asks for each change to be committed. What AI writes is held to the same rule as the rest:
a change to what the game shows or plays must be checked against the reference (Supermodel) or proved identical by
hashes (pictures, sound, RAM, recordings, save states), as the docs record. Each commit names Claude as co-author.

## Credits and references

- [Supermodel](https://github.com/trzy/Supermodel): the primary Model 3 hardware reference (ROM layout, address map, Real3D, network board).
- [MAME](https://github.com/mamedev/mame) (`src/mame/sega/model3.cpp`): the second reference, for cross-checking and the ROM set's naming.
- Motorola/IBM *PowerPC 603e RISC Microprocessor User's Manual* and *Programming Environments Manual*.
- [Moira](https://github.com/dirkwhoffmann/Moira) by Dirk W. Hoffmann (MIT, in `third_party/moira`): the 68000 core that runs the network board's firmware and the sound and music boards' programs.
- [minimp3](https://github.com/lieff/minimp3) by lieff (CC0, in `third_party/minimp3`): decodes the music board's MPEG audio.
- [LLVM](https://llvm.org) (Apache 2.0 with LLVM exceptions): translates the game's program at start.
- [SDL 3](https://github.com/libsdl-org/SDL) (zlib licence): the window, the GPU, sound and pads.
- [RmlUi](https://github.com/mikke89/RmlUi) (MIT) and [FreeType](https://freetype.org) (FTL): the in-game menu; RmlUi's SDL GPU renderer is in `third_party/rmlui_sdl_gpu` (MIT).
- [curl](https://curl.se) (curl licence), [OpenSSL](https://www.openssl.org) (Apache 2.0, built into the Linux and macOS downloads) and [Zstandard](https://github.com/facebook/zstd) (BSD): the lobby's connection, and the network game's state compressed.
- [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki (MIT, in `third_party/qrcodegen`): the GitHub login's QR code.
- [glslang](https://github.com/KhronosGroup/glslang) (BSD-3-Clause and others) and [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) (Apache-2.0), at build time, to compile the GPU shaders.
- [vf3-recomp](https://github.com/mr-finster/vf3-recomp) by mr-finster (MIT): the menu's design and style (`src/ui/menu.cpp`, `assets/ui`), and this page's.
- [Barlow Condensed](https://github.com/jpt/barlow) by the Barlow Project Authors (SIL Open Font License, `assets/fonts`): the menu's typeface.
- MAME's 315-5881 decryption by Andreas Naive, Olivier Galibert and David Haywood (BSD-3-Clause; in `src/runtime/crypt315_5881.cpp`, with their notice), as Supermodel also uses it.

See [docs/REFERENCES.md](docs/REFERENCES.md) for how sources are used.

## Support the project

Spikeout: Final Edition Recomp is free, and always will be. It's made in spare time by one person (with an AI
helper), and it has a long road ahead: if it brought back some arcade nights for you, a coffee helps it keep going.

<p align="center">
  <a href="https://buymeacoffee.com/donfy"><img alt="Buy me a coffee" src="https://img.shields.io/badge/Buy%20me%20a%20coffee-donfy-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=black"></a>
</p>

What your support pays for:

- **The online lobby and its relay**, which run on Cloudflare: the relay carries the games of players whose networks
  can't connect directly, and costs more as more people play.
- **Devices to test on**: a Steam Deck, handhelds, pads, so it plays as well on them as on a PC.
- **Time** for what's next: smooth motion, the cable link mode, more systems.

Not in a position to donate? That helps too:

- **Star the repository**, so more Spikeout players find it.
- **Play online and tell others**: a beat 'em up made for four is best with four.
- **Report what looks or plays differently** from the arcade ([Reporting a problem](#reporting-a-problem)): every
  report makes it closer to the real cabinet.
- **Try it on your device** (a Steam Deck, a Mac, another Linux, a laptop) and say how it runs.

## Licence

GPL-3.0-only. See [LICENSE](LICENSE). The downloads' `THIRD-PARTY-NOTICES.txt` lists the libraries built into them
and their licences.
