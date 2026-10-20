# Notes for working on this repo

- Hardware reference priority: Supermodel first, MAME second. See
  docs/REFERENCES.md. When adapting Supermodel code, keep its copyright
  notice and cite the source file.
- Never commit ROM data, dumps, or anything extracted from them. That
  includes recompiled code: it is generated into build/generated/.
- Build and test: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
  CI builds with warnings as errors on Linux and macOS.

## Hand-off (state as of 2026-10-20, commit 0d7edec)

How the owner works: asks for each commit and push explicitly; wants
fidelity to real Model 3 hardware (Supermodel is the best reference, then
MAME, then datasheets/manuals; change behaviour only with evidence, keep
the old behaviour selectable); docs/fidelity.md is the backlog and records
every such decision. Wanted later: an in-game settings menu (key binding,
network setup, match lobby, sound/music volume, speaker setup with the
front/rear pairs on separate outputs). A decompilation may come later and
must never be committed.

This machine (Windows):
- Toolchain: WinLibs GCC + CMake + Ninja from winget. In PowerShell, refresh
  PATH first: `$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')`.
  Executables need that PATH (run them from PowerShell, not Git Bash).
- ROMs: `build/roms/spikeofe` (unpacked from `Z:\Emu\Sega Supermodel 3\ROMs\spikeofe.zip`;
  git-ignored). `SPIKEOUT_ROM_DIR` in build/CMakeCache.txt points there.
- References outside the repo: `Z:\Emu\refs` (Supermodel and MAME sources,
  the MPC106 and SCSP manuals, comparison scripts in `refs/tools`).
- Window: `build\spikeout.exe build\roms\spikeofe` (slow until the GPU
  renderer exists). Headless: `build\gamerun.exe build\roms\spikeofe --blocks N`
  (`--press`, `--poke`, `--wav`, `--screenshot`, `--text`, `--probe`; see
  the file's header). Game: about 4.3 cycles a block, ~670k blocks a frame.
- Shell edits with sed/perl mangle `\n` in C strings; use the Edit tool.

Where it stands: attract mode, gameplay, sound, music and a linked master
(ring of one: `gamerun --poke 10019E=1@0:2000`) run; 3D is software only.
Next candidates: two network boards linked in one process (settles which
comm RAM bank the main board sees; first step to local multiplayer), the
GPU renderer (the owner wants it before judging the new clock by eye),
then the settings menu.
