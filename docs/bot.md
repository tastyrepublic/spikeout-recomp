# spikebot: playing Spikeout headless

`spikebot` plays the recompiled game by itself, so that a stage, a scene or a
bug can be reached without a person playing: from a save state it fights,
walks to the gate, breaks it and goes on, writing screenshots and save states
the window can load (`spikeout --load-state`).

```
spikebot <rom_dir> --load-state FILE [--out DIR] [--frames N] [--no-sound]
         [--play] [--workers N] [--inspect EVERY] [--dump LO-HI]
         [--route D:N,...] [--explore-test] [--moves-test] [--floor-map] [--bench] [--selftest]
```

- `--floor-map` prints the area's floor and walls as the game keeps them,
  and the way planned to the nearest enemy or the gate (below).
- `--route` directions: 0-7 walk (0 up, 2 right, 4 down, 6 left: the lever,
  relative to the camera), 8 nothing, 9 beat taps, 10-17 a tapped jump then
  walk, 20-27 walk with jump held (a higher jump).

- `--route D:N,...` walks direction D (0 up, 2 right, 4 down, 6 left, odd
  ones between; 8 none) for N frames each, printing where the player goes;
  `--explore-test` runs one search for a way to the enemies and walks it.

- `--play` plays for `--frames` frames. With `--out DIR`: a screenshot every
  60 frames (`p-<frame>.png`) and a save state every 300 (`bot-<frame>.state`).
- `--inspect EVERY` runs with no input and prints the player and the
  character slots every EVERY frames; `--dump LO-HI` prints RAM (hex
  addresses) as words and floats; `--selftest` checks that a save state run
  on twice, on the board itself and on the workers, ends the same.
- `--no-sound` leaves out the sound board (faster; states saved so load in
  the window without the sound board's state: the music comes back with the
  next piece the game starts).
- `SPIKEBOT_VERBOSE=1` prints every try's result.

## Runs from stage 1 with the newer bot (2026-10-29, later)

From `build/bot/bot-3500.state` (stage 1's gym), unhelped:

- A hold (an enemy grabbing the player) looked like a scene to the "held"
  check, so the bot waited until time ran out (stage 1's boss): with
  enemies within reach it now struggles (beat taps and the lever wiggled).
- In a boss fight (the phase's boss bit) the boss, the enemy with the most
  health at full, is the target, and damage to him counts three times (the
  owner: "every stage the goal is to kill the boss").
- **`--weak-enemies`** (the owner's request, for getting through the stages):
  each turn every enemy in play is set to 1 health, so any hit knocks it
  out; the game's own knock-out still runs (counts, time bonuses). With it
  the bot uses quick hits only (the 20-frame tries, no charges: the owner).
  Run `weak1`/`weak2`: the shipyard's stairs gate (by the floor grid) and a
  fence gate in Diesel Town passed alone; the hedge maze at frame ~100,500
  (the long run: ~188,000), 1 h 45 min from the start.
- At a gate, nearness counts a little (0.1 a unit): at the fence gate the
  bot beat the bars 1.65 from its door until time ran out.
- Planes flagged 0 (ceilings, it seems) aren't floors or room-takers: taken
  as room-takers they hid the opera house's stairs. A wall is planned as
  jumped only when its top is within 1.0 of the floor (the theatre stage's,
  0.85, was jumped; the walls by the opera house's stairs, with 2.4, sent
  the bot shuttling instead of round by the outdoor area); a jump that gets
  no nearer the point aimed at bars it at once. With these the opera gate
  was broken alone (frame 123023).
- Running (the lever tapped twice, then held, per the guide) didn't start a
  run with taps of 2-8 frames (`--route 30-37`, `SPIKEBOT_RUNTAP`); left out
  (and later strafed instead: "Strafing along a way").
- The run was stopped in the Reception Hall (frame ~128,400) at the owner's
  word: good enough for the next work, the stages and scene changes.

## Two linked boards (2026-10-30)

For the camera events of a two-player game (docs/netplay.md), the bot
drives linked boards: `--boards N` with a linked state (gamerun --boards N
--parallel --save-state, or the window's F9 in network play), `--player N`
the board it plays (default 0). The boards run in one process as gamerun
--parallel does (a `m3::LinkRing`, a meeting every 8 lines; the class
`Rig`), the bot stopping them at the meeting after each frame; a save is
the ring's cut and every board's state, gamerun's format, so the states it
writes load in gamerun. Workers are rigs too. `--selftest` checks that a
state run twice gives every board's RAM the same.

- `--idle-others`: only the bot's player plays; the others stand (continued
  when knocked out). The owner: one player beating the boss and breaking
  the gate is enough to go on, and so it is (player 2 stays behind, and
  appears with the team in the next area).
- The red arrow (0x11DF54) is kept by board 0 only, so the target is read
  there. A fight move's inputs are held on through the boards' frames.
- In a boss fight the hold check counts the nearest enemy's own distance,
  not the boss's (which is all `nearest` gives there).
- `--weak-enemies` writes each board's own copy: the copies of an enemy
  differ between cabinets (at frame 8371 one enemy had 1 health on board 0,
  60 on board 1, the check made on board 0 only), and when player 1 knocked
  it out the cabinets disagreed: "Communication error. Please call service
  staff" on both from frame ~8556, the run waiting for ever (two5). With
  each board checked and written, the same run (two6, from 8047) went on.
- The hold check reads the enemy slots itself: at stage 2's fence gate
  (two7, frame ~38990) a grabber came after the area's last enemy, while
  `enemies()` was empty (the area's phase over) and `apart()` gave the
  gate's distance, so the bot waited as if for a scene until the time ran
  out (game over at ~41700). With the slots read it struggles free, and
  the gate was broken (two8, from 38712: next area at 39601).
- `--record-inputs` is refused with more than one board.
- Start: build/bot/two-start.state, both players joined, frame 5000, made
  from power-on with `gamerun --boards 2 --parallel --poke
  0:10019E=1@0:5000 --poke 1:10019E=2@0:5000 --press 0:coin@1200 --press
  0:start@1300 --press 0:beat@1700 --press 1:coin@2600 --press
  1:start@2700 --press 1:beat@3300 --press 1:beat@3650 --save-state
  build\bot\two-start.state@5000 --blocks 4000000000`. Run: `spikebot
  --boards 2 --load-state build\bot\two-start.state --play --weak-enemies
  --idle-others --workers 5 --out DIR`.
- Recorded (2026-10-30): `--record-inputs FILE` works on linked boards too
  (tools::LinkedInputLog). The boards stop for the bot at a meeting of
  their slices, or where a state put them back (each board just after a
  frame's end), not at a frame's end, so every change is kept with where it
  took effect: a board's frame end (the inputs held), a meeting (board 0's
  frame and line there) or the point a state resumed at (the bot's inputs
  and RAM writes), with each board's RAM hash every 60 frames; a step that
  goes back to a save forgets what came after it. `gamerun --boards N
  --parallel --load-state START --inputs FILE` plays it again at those
  points. Checked: 1,500 frames from two-start.state, the bot playing
  player 1 (25 of 25 hashes on each board), and 3,000 with both players
  fighting (build/bot/bothfight: 50 of 50 on each).
- two8's game over (frame 69468, stage 2's walkways): two enemies paced a
  ledge a floor up (height 5.15) whose floor the grid lists in some cells
  and not others. Over a cell without it, the way's target floor was taken
  as the ground 5.3 below, and the way, out of reach, ended on another
  walkway; it flipped between the two every turn (27 and 41 points) and the
  bot shuttled along the walkway below for 6,000 frames until the time ran
  out. Now a target further than 2.0 from the floor listed nearest it is
  aimed at at its own height (a gate's arrow, 1.5 over its floor, keeps the
  floor). From two8's 61843: both down by 62386, the next area at 63980.

## Strafing along a way (2026-10-30)

The guide's run (the lever tapped twice, then held) still never started:
from fullrec's 10704 (nobody about), 60 frames down, taps of 1-12 frames
and gaps of 1-16 all went less far than holding the lever (at best 3.24
against 3.64). Shift held with the lever (the guide's lock-on, a strafe)
went 5.39: 0.093 a frame from the first frame and straight on, where a walk
starts slowly, wanders and settles at 0.074; shift alone doesn't move. So
the way's legs now hold shift (`SPIKEBOT_WALK=1`: walked, as before), while
the target is further than 5.0: a strafe keeps the way the player faced,
and at stage 1's gate (fullrec4's 81415) he came up to it facing away, no
try broke it and he waited until the 600 frames were over; the last of the
way walked turns him. From fullrec4's 81415 (a 22-unit walk on the flat to
the gate): the gate broken at 81827, walked 81941 (114 frames sooner of
396). On two8's walkways (60328), where every leg chosen is a jump, both
came to the enemies at the same frame: the bot keeps its 12-frame walking
legs and its 4-cell lookahead (longer, 30-40 frames and 8-16 cells, changed
nothing there: the jumps still won).

## Gates, and the unmarked ones (2026-10-30)

The owner's guide (players online): after Victor in stage 2, an unmarked
gate on the far side leads to the sewers (Huey, Dewey and Louie). How the
game has it (Victor's area, `fullrec/bot-31704.state`, the gate called):
- The red arrow is a script action, 0x2D (`022d08MM PPPPPPPP`): P points at
  an x y z in a table of the stage's arrow places (stage 2's at 0xAFFA8);
  M 0 one arrow, 2 two (the next place too), else none.
- The script then registers the gates as objects (action 0x03, sub-byte
  0x0A, an object's number each): Victor's 0x0B and 0x0C (the arrow's) and
  0x05 and 0x06 (no arrow). Condition 0x0C (`010c08NN 04000000`) is object
  NN broken; 0x0B or 0x0C sets the flags word 0x0284's bit 0x10, 0x05 or
  0x06 its 0x20, either ends the gate (0x80, the phase to 8), and 0x90 goes
  on to script 0xAAAF0 then 0xA66F0, 0xA0 to 0xAABB0 then 0xA7CE8.
- The objects: 24 bytes each at *(r13 - 16100) + 2028, the first word
  their flags (a gate 0x01501000; 0x08000000 hit, 0x04000000 broken). A
  gate is walls of the area's collision: an attack hitting a wall takes its
  first word's byte 1 through the byte table at the objects + 24320 to the
  object's number (0x5103C), so each gate's place is its walls'.
- `--gates` lists the area's objects with walls, their flags and where they
  are. Victor's: 0x0B/0x0C at 9.0 22.3 and 9.0 25.3 (the arrow at 7.7
  23.8), 0x05/0x06 at 44.3 36.5 and 42.8 36.5.
- `--unmarked-gate`: when the gate is called, a gate other than the arrow's
  (unbroken, 0x01000000, 5 or more from the arrow; the nearest) is the one
  walked to and broken. From 31704: found at once, broken at 32524 (the
  arrow's took 31874), the next area at 43.9 37.2 (the boss Clint, in
  concrete channels), where the arrow's led to 2.1 23.3.
- Every gate in the scripts (a scan of the program image for action 0x03
  sub-byte 0x0A runs, grouped by the bit each object's break sets): one
  gate with one arrow in all but these: Victor's (0xA9F2C, above); stage 2's
  0xAD054 (and 0xAD910 the same): 0x01 against 0x0B/0x0C, the arrow at
  -7.2 -3.4, an area the bot never reached (on a branch); the opera house
  (0xC4B8C): 0x29/0x2A on the balcony over the doors, the arrow's (15.7
  0.0), against 0x01/0x02 on the ground at 19.8 -25.0. 0xAB844 and
  0xBF1F0 have two gates and two arrows (mode 2): both marked.
- The opera house's (fullrec4's 175591, `--unmarked-gate`): broken at
  176425, then a street by a stone wall (10.5 -27.0, boxers), where the
  arrow's led into the story scene and the reception hall. No 0x3F or 0x18
  in its first 1,800 frames (`build/bot/opera-hidden`, replayed: 30 of 30
  hashes).

## The route, read from the scripts (2026-10-30)

Where each gate leads, from the program image (no play needed):
- Areas: the table at 0xA1D24, 8 bytes an area number (its set-up script,
  its script; 0x5A94C). The area task (0x11BA80) keeps the running script
  at +16 (0x11BA90); the byte at -16365(r13) (0x104013), when set, starts
  that area (0x59C7C) and is cleared.
- Action 0x35 (`023504NN`): go to area NN, through area 36 (0xAF288, the
  between-areas script, whose action 0x36 starts it); action 0x00
  (`020004NN`): area NN at once. Every such record, with the area whose
  script holds it, gives the game's area graph (stage 2's: 6 (Victor) to 1
  or 3, 12 (the two-arrow area) to 9 or 14, 14 to 15, ...; stage starts:
  55 (0xBFC50) to 17 the mall, 57 (0xC00DC) to 24, 59 (0xCC06C) to 29).
- A stage's end: action 0x01 (`020108AA BB`) puts stage-change area BB in
  -16265(r13), which the game flow starts (0x4BB74). Area 84 (0xAF85C,
  after stage 2's last boss) picks by the route bits, the area task's
  +0x288 (0x11BD08): 0x0100 area 93, 0x0200 55 (the mall), 0x0400 57, 0x0001
  91, else 74; area 85 (0xD3DF4): 0x0800 55, 0x1000 57, else 59. Gates set
  those bits as they break (action 0x16 on `02880000`); +0x284 (0x11BD04)
  holds each gate's own bits (condition 0x0E, both words).
- So for the stage starting at area 24 (its set-up 0xB4E78 holds 0x3F's
  0xB4EA0; its change, area 57's 0xC00DC, 0x18's 0xC00FC; it leads on to
  24, 34, 35, 25, 26 and the two-arrow area 44): bit 0x0400, set in area 15
  (0xAD054, after 0xACE9C's gate) by its unmarked gate 0x0B/0x0C (Victor's,
  from the far side, at 9 22.3), where the arrow points at 0x01 (-6.4 -3.4:
  bit 0x0200, the mall). From branch-ab's 61116 with `--unmarked-gate`
  (`build/bot/route57`): broken at 61596, +0x288 0x402.
- `--gate-object N` (hex) goes for gate object N while it stands, a fight
  or not (tried at the basketball court, area 15's last boss: its gates
  were already closed by then; the choice is at the gate before).

## Fighting faster (2026-10-29)

The opera house's hall timed out twice with the old fighting. Now each move
is tried for 55 frames: its own inputs for its length (20 frames; a charge
its hold and 4 more), then beat taps, so moves that take longer to land are
scored fairly; charge held 30 and 45 frames (launcher, stun) were added,
and a charge picked is played whole. From the hall's start
(`run14/bot-219963.state`), 3000 frames: the old way left 6 enemies (640
health, the player at 171); the new had 5 of the 6 down by frame 221778
and was walking to the last, with 15 charges. It costs 1.5 times the time
on the PC (4.2 minutes against 2.8). `SPIKEBOT_OLDFIGHT=1` brings the old
way back.

## How it plays

The game runs on its own thread and stops at the end of every frame for the
bot's next command: run frames with inputs held, save a state in memory, go
back to one, write a screenshot. A save state loaded runs on exactly as
before, so the bot tries things and takes them back. Worker boards (one
thread each, `--workers`, default the cores less one) run the tries in
parallel from the same state.

- **Fighting** (an enemy within 4 units): every 10 frames, each move (wait,
  beat, charge, jump, sweep, homing, special, wiggle, the 8 directions alone
  and the 4 straight ones with beat taps: 21, two rounds of 11 workers) is
  tried for 20 frames; the one that deals the most damage, takes the least
  and ends closest to the nearest enemy is played for 10 frames.
- **Walking** (no enemy within reach, or the gate to reach): each direction
  is tried for 30 frames; if the best gets 1.5 closer (nothing in the way)
  it is walked, else every route of two legs (a direction for 30 frames,
  then another) is tried and the first leg of the one that ends closest is
  walked: it finds its way round furniture and walls one leg at a time.
- **Nobody about** (no enemies, no gate yet: the next ones coming): waits,
  with no search.
- **The gate**: when the game says "break the gate", the bot walks to where
  the red arrow points and tries attacks there for 60 frames, keeping the
  one that breaks it.

## What it reads (stage 1; found by searching RAM across save states)

| Address | What |
|---|---|
| `0x1A0000` + n×`0x4000` | players 1-4 (character objects, `0x4000` bytes each) |
| `0x1B0000` + n×`0x4000` | the area's other characters: enemies, the boss, bystanders |
| object +`0x10`/`+0x14`/`+0x18` | position x, height, z (floats) |
| object +`0x3F02`/`+0x3F04`/`+0x3F06` | health, maximum, the bar's shown value (16-bit) |
| object +`0x44` | state flags (they change all the time) |
| object +`0x35C` on | the skeleton's joints (matrix and position each) |
| `0x11BD22` | frames since the area's fight began (the timer shows 99 - this / 64) |
| `0x11BD5C` | the player's position again; then a list of points (x y z, flags) |
| `0x11DF48`, `0x11DF54` | the red arrow: from the player, to where it points (x y z) |
| `0x111C84` | not the gate: it stayed 0 at the coast road's car park and was 1 in the opera house's hall while 6 enemies fought (taken for "break the gate" in stage 1; no longer read) |
| `0x11BD24` | the area's phase (32-bit), bits: 1 the boss come ("HERE COMES THE BOSS"), 2 the gate to break, 4 an area's start (a few frames), 8 the gate broken (walking on), 16 with 8 the boss down (those left run off), 32 just after a gate, 64 the story scene after a stage and on through stage 3's first areas, 0x400 with 1 Diesel Town's last boss, 0x300 with 8 the bonus round's end. 0 is fighting. `0x11BD22` starts again at each change |
| object +`0x0` | nonzero (1, 3, 9) while the character is in play; 0 once down, and for the slots the last area left behind (they keep their health) |

The player's health is written by code at `0x227B8` (`r14 + 0x3F02`, r14 the
object); the damage taken is in r25 there.

- **Knocked out**: after "K.O." (120 frames), a coin, then start and beat in
  turn (start continues, beat picks the fighter "SELECT A FIGHTER" shows,
  the same one) until the player is back.
- **Stuck** (enemies about, nobody hit and the player no more than a unit
  closer to them for 300 frames): explores. An
  archive of the places reached (cells of 1.5 units, and of 0.5 in height),
  each with its route from a save state (20-frame legs); every round the 8
  places tried least (spreading over the whole area, since the way round
  starts away from the enemies), the nearest them of those first, are each
  walked to once on a worker and saved there, and tried in the 8 directions
  from that save, walking and jumping (16 legs); places above or below
  the start go first among those tried as little. It stops after 6 rounds
  that find nowhere new (or 100 rounds), and walks the route to the place
  nearest an enemy. A player who can't move at all (a scene) is waited
  for, not searched for. `--explore-test` prints a plan of the cells
  reached; `SPIKEBOT_GOAL=x,z` aims it at a point instead,
  `SPIKEBOT_WAIT=N` then waits there N frames printing the slots. Height is
  no guide (a crouching enemy's position is lower than one standing). A
  search that finds nothing is tried again 300 frames later: the way can
  open only after a while.

Ties go to the first move tried, beat taps, which also free the player from
an enemy's hold (every move scores the same while held; waiting there kept
the player held for 1,400 frames).

## How far it gets

To the end: on 2026-10-29 it beat Mikhail and reached the staff credits
(frame ~310800, "The long run" below), with the owner's help at four
places. Before that, from the start of stage 1 (the owner's state after the character select,
frame 1700), 9,000 frames in 14 minutes (5 workers, no sound): it clears the
gym (breaking the gate), the balcony and the road, beats stage 1's boss
(frame ~7400) and goes on into the next stage's coast road, where it ended
circling a truck with three enemies out of its reach.

## Stage 2's coast road

Three enemies crouch on the road behind the kerb (`build/bot/coast-stuck-9500.state`),
1.1 units below the pavement the player is on, and never come up; the
arcade does the same (the owner, 2026-10-25). The way to them (the owner's
waypoints): north along the pavement past z 10.5, west down a slope to the
road, then south to them. That way is closed at first: searches from frame
9500 find 144 places, none past z 10.5; after 400 frames of waiting the same
search walks through. The bot got there by itself on its third search
(frame 10480, a route of 18 legs) and fought them.

## The area's floor, read from the game (2026-10-29)

The owner's idea: read the map instead of walking it. The characters'
movement calls a floor query at `0x4FAB4` for each of the points round
their bodies (found with `gamerun --watch-write` on the player's position and
on object +`0x1ACC`, the floor height under him). Its data is a grid over
the area, which the bot reads from RAM (`read_floor` in spikebot.cpp):

| Where | What |
|---|---|
| `*(r13 - 7832)` | the grid: +0 the cells' table, +12 the header |
| header +8/+12/+16/+20 | the area's bounds: x0, z0, x1, z1 (floats) |
| header +24 | the columns |
| `0x7F768` | cells a unit (2: cells of 0.5) |
| `0x7F770` | k, below |
| a cell's entry | its planes, 24 bytes each until a flags word of -1: flags, d, a, b, c; the height at (fx, fz) within the cell is d + a/(b k) fx + c/(b k) fz |

A cell lists every floor over it: the ground under a crate stack as well as
the stack's top, stairs, the floor above. Flags, as the floor query tests
them: `0x04000000` never a floor (under the opera house's balcony edge: the
bot planned a drop onto it and couldn't); `0x02000000` a floor only while a
switch is on (the byte at `*(r13 - 15084) + 19`); others are floors, bit 31
or not (the way up to Mikhail is `0x01000145`: requiring bit 31, the bot
left it out). `0x00040000` is a solid block (the warehouse's crate stacks):
the character is pushed off it, and the bot doesn't plan to stand on it. A
floor with another plane (a solid block's top too) less than 1.6 above it
is inside something.

Following a way, six turns no nearer the point headed for (against the best
reached for it; aims within a unit count as one) bar that cell and those
round it until the area's phase changes, and the way is planned round them.

**Walls** (the Hedge Maze's hedges, which the floor doesn't stop: found by
watching the push the player gets walking into one, object +`0x1A88`,
written by the wall test at `0x50BBC`): the grid's +8 points to a table of
wall records; a cell's first plane's +20 points to the cell's list of wall
numbers (16-bit, until `0xFFFF`). A wall: flags (bit 31: skipped), its
normal's cos and sin at +4/+8, its ends (x, bottom y, z) at +12 and +24,
its height above each end at +36/+40. It is one-sided: a body coming from
the side the normal faces is pushed out to the wall plus its radius. The
way-finder doesn't cross a wall from its front at a height it covers.

`--floor-map` prints the grid and a map round the player (`.` level, digits
higher, `v`/`-` lower, blank no floor, `#` a wall, `@` the player, `E`
enemies, `*` the way planned, `J` a jump up); it lists the walls in the
player's cell, and `SPIKEBOT_PROBE=x,z` a place's planes.

**The way** (`plan_way`): A* over the cells and their floors; a neighbour's
floor up to 0.35 higher is walked to, up to 1.4 jumped, down to 4 dropped.
Walking to a target out of reach, the bot plans a way there and heads for
a point 4 cells on (or the next step to jump up to); each turn the 8
directions walked and jumped are tried on the workers (12 and 40 frames)
and the one ending nearest that point is played. The lever is relative to
the camera, which turns: trying them all deals with it. A jump keeps the
speed it starts with and no more (straight up from a standstill, 0.8 in 40
frames from a walk), so for a step up it also tries run-ups.

The warehouse's pen: with the solid blocks left out the way is round by
the floor below (72 points), and the bot walked it from the dead end
(frame 65376) and beat both enemies by frame 67264, where the search had
found nothing in 611 places.

## The long run (2026-10-28)

From the coast road (`coast-stuck-9500.state`) with no frame limit (`--frames
2000000`), restarted from its last checkpoint after each fix (runs 1-6 in
`build/bot/run*`, each with its `log.txt`):

- **The car park's gate** (frame 15840): "BREAK THE GATE" showed but
  `0x111C84` stayed 0, so the bot waited where it stood until the time ran
  out. The area's phase (`0x11BD24`, found by diffing RAM across the
  moment) is the bot's guide now; the red arrow moves to the new gate at
  the same frame.
- **The docks' boss** (Phoenix, frame 22760): the enemies left run off and
  keep their health; the bot chased them. Then between stages (the story
  scene, "Mikhail"; then stage 2's "how to see the map") their slots still
  held them, 142 units away. An enemy counts now only while its object's
  first byte is nonzero and the phase has none of the fight-over bits.
- **Stage 2, Diesel Town**: areas, gates and a boss (Elliot) cleared alone;
  knocked out and continued 4 times.
- **The warehouse** (frame 64700): the last two enemies stay in a pen
  behind stacks of crates (state `run3/bot-64726.state`); they never come
  out (5,000 frames waited, beside the pen and where the fight was; the
  clock at 0 does nothing). The others came out over the crates. The
  search now tries a jump at the start of each leg as well (16 ways a
  place, not 8) and tries places above or below the start first (a unit of
  height ranks as 4 nearer), but found no way in (611 places): it climbs
  the first crate (1.4 up) and no further. **The owner got in by hand**
  ("jump up one level, and walk"; their F9 at frame 66082, copied to
  `run4/owner-66082.state`) and the bot went on from there. Not solved:
  the next try is jumps in the middle of a leg (at the crate's edge) and
  legs long enough to land.
- **The stairs** (frame 82800): the camera shows the floor above for 500
  frames while the player can't move, then puts him there. The bot took it
  for being stuck and searched for minutes; now 10 frames' walking each
  straight way first tells a scene (`wait (held)`).
- **Diesel Town's last boss** (Randolph's yard, frame 90214): phase 0x401,
  which the bot read as "nobody about" while 4 soldiers beat it: the phase
  is bits.
- **Bonus 1** (frame 95000): 7 enemies of 1 health each; then stage 3's
  opening scene (phase 64, which stays on through its first fights: not
  "over").
- **Stage 3, the shipyard**: from frame 99700; knocked out 3 times at its
  first boss (7 enemies about), then on.
- **Time up at a gate is game over** (frame ~144700, back to the attract
  screen): the shipyard's gate (frame 142748) is a floor up, by stairs back
  the way the player came; the bot walked into a corner 8.3 short. The
  stuck search now runs for the gate too (after 150 frames no nearer, aimed
  at the arrow), and once nowhere nearer turns up for 10 rounds it tries
  the places furthest from the start first; it still didn't find those
  stairs (921 places). **The owner broke that gate by hand** (their F9 at
  frame 144263, `run7/owner-144263.state`).
- A route walked counted as time stuck, so a second search started at once
  (20 minutes, no log): fixed.
- From run 9 (frame 175477, the shipyard's later areas) the bot walks by
  the floor grid (above).
- **The Hedge Maze** (frame 188000): the hedges aren't in the floor grid;
  the walls are (above). With a body radius of 0.5 the maze's narrow gaps
  were shut and its enemies, idle across a hedge, out of reach; 0.25 opens
  them, and the bot walked round to them (run 11).
- **Outside the opera house** (frame 218337): the gate is on a balcony
  above the doors, its arrow 6 above the player. A gate's distance now
  counts height, the bot hits it from within 2.5 (not 4), nearer is no
  better there (ties go to beat taps, the tries 120 frames: a gate takes
  more hits than a try lands), and a way whose end can't be reached goes to
  the cell reached nearest it, height counted. It broke the gate
  (frame ~219800), and then fought on as if there were still a gate:
  `0x111C84` was 1 in the hall. Only the phase says "gate" now.
- **The opera house's hall** (from frame 219800): enemies below a balcony
  were taken as within reach (height now counts double beyond 1.5); the
  way off the balcony went by a `0x04000000` plane (now not a floor). The
  hall's clock ran out twice (frames ~233000): **time up in an area is game
  over**, not only at a gate. The bot now stops when `0x110000` (1 while a
  game is played) has been 0 for 600 frames, instead of fighting the
  attract mode's demo.
- With the faster fighting the hall still timed out (runs 15-17): the old
  stuck search sent the player 42 legs away from two enemies 1.6 off (it
  now runs only when the target is out of reach and the floor grid has no
  way), and the bot swung for 2,000 frames at enemies across the balcony's
  railing (after 6 turns in which no try hurts anyone it walks the way for
  10). Run 17 lasted 250 seconds, 17 knocked out, and ran out of time.
  **The owner played the hall** (their F9 at frame 243663, the hall's gate
  called: `run18/owner-243663.state`).
- **The theatre** (run 18, frame 255067, the gate called): the gate is
  below the balcony the bot stood on, over the rows of seats; the way-finder
  went through the seat rows' end walls from behind (one-sided walls: now
  walls block both ways in the planner), and found no way down. Time up.
  **The owner broke it** ("jump 1 step up to the stage", "facing the gate
  and make the jump"; their F9 at frame 255756, `run19/owner-255756.state`).
  The planner now crosses a wall no higher than a jump (1.4) as a jump: the
  stage's front walls by the aisle are 0.85 and 0.6 above its floor. But no
  jump the bot tried there crossed them (from the aisle at x -44.8 by the
  wall at -45.6: turning first, run-ups of 2-24 frames, every direction).
  **Jump height depends on how long the button is held** (the owner): the
  bot only tapped it (2 frames: up 1.5, no further). Held, the player
  crouches about 16 frames, takes off, rises 2.75 and carries on forward,
  landing about 80 frames after the press: over that wall onto the stage
  side. The bot now holds jump 20 frames (its way-following jump tries last
  80 frames, the planner allows steps up to 2.4), and crossed the wall by
  itself. On the stage the way ends against the gate itself (a wall 16.6
  high at the stage's front, the arrow 7 behind it): when a gate's way is
  used up (2 points or fewer) the bot now hits it from there.
- **The Reaper Gang** (frame 255836, five bosses, 2900 health) and the
  theatre's crowd (frame 268905, seven, 1900): beaten, with 6 and 7
  continues.
- **Mikhail** (frame 284218, phase 0x445): on a raised walkway the bot
  couldn't plan (its planes are `0x01000145`, without bit 31). The owner
  found the way ("a plane that will show and disappear, that's the path";
  their F9 standing on it: `run19/owner-287156.state`); with the game's own
  flag rules the bot walked up from below (frame 284592) and fought them.
  Knocked off the walkway it lost a long climb back each time; moves that
  drop the player more than 1.5 now score -500 ("stay on the center of the
  stage", the owner). **Mikhail beaten at frame 308158** (run 21, from
  `run20/bot-293596.state`; phase 88), then the ending and the staff
  credits (frame ~310800): **the game cleared**, from stage 1 to the end.

### The whole run, and the owner's help

The bot played every stage itself but for four places, where the owner
played and saved (F9) and the bot went on from their save: the
warehouse's pen (the way in: since found by the floor grid), the
shipyard's stairs gate (stairs back the way it came; the search of the
time didn't find them), the opera house's Reception Hall (time up with the
fighting of the time) and the theatre's gate (a held jump onto the stage;
the bot does it now). Runs and logs: `build/bot/run1` to `run21`
(git-ignored).

## The whole game, from a longplay

A one-credit clear on Supermodel ("Spikeout Final Edition (1999) Arcade
[Full Clear] [Longplay]", youtube.com/watch?v=rKS3cuKpHf8, 92 minutes;
sampled a few frames a chapter, 2026-10-25). Its "jump: buttons 1 and 2" is
its player's own layout: Jump is a bit of its own (Supermodel's Model3.cpp,
bit 3) and lifts the player 1.5 units; beat and charge together is an attack
(kept as a move). On the coast road no button seemed to lift the player
because the tries started mid-throw; jumping while walking doesn't get down
to the road either (`--moves-test` prints each move's lift and each walk's
end, with and without a jump). Chapters (video time) and what each area is:

| Video | Area | What's there |
|---|---|---|
| 4:15 | Stage 1, Spiker's Base | the gym, the balcony, the road (the bot clears it) |
| 6:13 | To The Docks | the coast road's bridge, then a car park with a truck (the stuck spot) |
| 8:23 | Docks | a wooden pier, a boathouse, a kiosk |
| 9:29 | Boss 1, Phoenix | a round deck at the foot of a tower, the sea round it |
| 11:14 | Stage 2, Diesel Town | streets, a fenced alley, a back yard |
| 21:41 | Warehouse | corridors of pillars |
| 24:58 | Industrial Zone | a yard by brick buildings, a walkway with a fence |
| 30:18 | Boss 2, Randolph | a fenced yard, many enemies at once |
| 31:45 | Bonus 1 | a walled courtyard against the clock |
| 32:39 | Stage 3, Shipyard | steel decks, the longest stage (19 minutes) |
| 51:51 | Aboard Ship | wooden rooms and corridors |
| 54:09 | Keiser | a cabin |
| 56:40 | Bonus 2 | |
| 57:07 | Hedge Maze | grass between hedges, gates: a maze, the next hard place for the walking search |
| 1:03:19 | Outside Opera House | a fenced garden gate |
| 1:07:56 | Reception Hall | a red-carpeted hall with balconies |
| 1:13:48 | Theater, Theater 2 | rows of seats on a slope, then the stage and its wings |
| 1:19:38 | Reaper Gang | a red hall |
| 1:23:26 | Final Boss, Mikhail | a hall of columns |

- The map at the bottom right of the screen marks the player (a blue S by
  a blue arrow) and, with a red badge, G: the gate to break (the owner,
  2026-10-25). That is what the bot already walks to once the game asks for
  the gate (`0x11DF54`, while `0x111C84` is 1); before then the target holds
  a gate further on ((39.5, 50) from the gym to the road, (17, 35) all along
  the coast road), so it is no guide while enemies are left. A red S badge
  showed by an enemy on the balcony (frame ~3680); what it means isn't
  known. A list of points after the player's position at `0x11BD5C` (x y z,
  then flags) lies near the gate in both stages.
- For the smooth-motion ghosts: the sea and its birds are around the docks
  and Phoenix's deck (the tower there may be the tower light), and the
  coast road and Diesel Town's streets have the parked cars. States the bot
  saves there are the place to test them.

## From a player's guide

"SpikeOut Guide: Part 1 - The Basics" (Durandal,
testamentenjoyment.net/spikeout/guide-part1/, read 2026-10-25). What the bot
uses or could:

- Moves (arcade buttons): beat and charge together sweep (low, hits enemies
  below the player); beat and jump the homing attack (a leap at the nearest
  enemy faced); beat, charge and jump the special attack (invincible, also
  out of a hold; costs one of the special items, 3 at the start, 5 at most,
  one back for each area cleared); wiggling the lever left and right gets
  out of a hold; walking into an enemy without shift grabs it, beat throws.
  The bot now tries sweep, homing, special and wiggle with the others. From
  the coast road's pavement none of them reached the enemies below (turned
  each way first): the way round is still the way there.
- Charge held longer is a stronger attack: launcher (level 2), stun (3),
  bowling strike (4), shown by the gauge at the top. Not tried: the bot's
  tries last 20 frames.
- Running: the lever tapped twice, then held (the fastest way round; the
  bot only walks). Shift held locks on to the enemy faced and strafes.
- The timer: 99 at most; 55 more for every 8 enemies knocked out, 45 when a
  boss fight starts, 30 for each boss down. Once all are down there are 30
  seconds to break the gate (the arrows; G on the map).
- Some areas have more than one gate (another arrow, or none: secret); the
  gate broken picks the stage's branch.
- The map shows enemies, partners and items (START zooms it): the red S
  badge by an enemy is likely an item.
- Difficulty (Easiest to Hardest, Normal by default; the test menu): lower
  makes the player's health larger and enemies slower to attack.

## Not yet

- In a boss fight only the boss matters (the owner: "every stage the goal
  is to kill the boss"; guards keep coming): the bot should weigh damage to
  the boss (the enemy with the most health by far: Mikhail 1500, his guards
  200) above the rest and head for him. Mikhail took 14,000 frames and
  seven continues without it.

- A search for a way round takes minutes: stage 1's road (frame 5730, a
  route of 33 legs, 307 places) 3 minutes.
- Bystanders can't be told from enemies by any field found so far; the bot
  fights whoever has health until the game asks for the gate.
- About a fifth of real time while fighting (13 frames a second on the
  i7-8700K). What it costs (`--bench`): a frame 7.3 ms on one board, the
  11 workers together about 700 frames a second (6 cores: 6 workers gave
  530, 12 gave 690); a save state is 28 MB, a save 16 ms, a load 10 ms
  alone and about 30 ms on each of 11 workers at once (the memory is
  shared). A fight decision is 420 frames on the workers for 10 played.
