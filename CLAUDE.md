# Notes for working on this repo

- Hardware reference priority: Supermodel first, MAME second. See
  docs/REFERENCES.md. When adapting Supermodel code, keep its copyright
  notice and cite the source file.
- Never commit ROM data, dumps, or anything extracted from them. That
  includes recompiled code: it is generated into build/generated/.
- Build and test: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
  CI builds with warnings as errors on Linux and macOS.

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
  ~670k blocks a frame, ~160M blocks/s.
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
  Not yet: linked boards (LinkRing) in a state; a recording from a state.
- Record/replay: spikeout --record, gamerun --replay (66/66 hashes).
- Smooth motion (frame interpolation, m3::interpolate, spikeout --smooth /
  menu setting, default OFF): ghosts remain (tower light, car wheels, gate,
  sea birds, sea level); to be revisited with replays and states.
- Next: the owner asked Claude to learn to play the game (a bot driving
  gamerun with save states and --press) so bugs can be reached without the
  owner playing; then the smooth-motion ghosts; online play later
  (docs/netplay.md).

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
