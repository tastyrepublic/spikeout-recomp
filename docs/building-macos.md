# Building on macOS (hand-off, 2026-11-05)

Windows and Linux are done (the downloads, 0.1.0 preview 1); this is what's needed to bring the Mac to the same
point, written for the owner building on their own Mac (and for a Claude session there). Read CLAUDE.md's "Latest"
first, then "First Mac build" just below: what has been done since.

## First Mac build (2026-11-05, Intel)

Built from main 076b485 on the owner's Mac mini 2014 (Macmini7,1: Intel Core i5-4278U, 2 cores / 4 threads, 16 GB;
Intel Iris 5100; macOS 15.7.9; Apple Clang 17.0.0). An Intel Mac, not Apple silicon: what's below is x86_64 only.

- **It builds as it is.** Steps 1 and 2 below, no source change: `spikeout-jit` and `gamerun-jit`. Warnings (not
  errors here; CI's `-Werror` not tried): unused lambda captures `o` and `m` (tools/gamerun.cpp:1328, 2229), the
  unused `local_` (tools/remote_cabinet.hpp:585), and ld's duplicate spirv-cross libraries.
- **Tools differed from step 1.** Homebrew has no CMake bottle for this Mac (macOS 15 on Intel) and built it from
  source (~15 min, stopped); CMake 4.4 and Ninja from pip (`pip3 install cmake ninja`) work. SDL3 (3.4.14) and
  openssl@3 came from Homebrew (SDL3 found by CMake as installed, not fetched).
- **LLVM 18.1.8** built once in about 62 minutes on 4 threads (1,980 steps), 175 MB installed in
  `~/.cache/spikeout-llvm/18.1.8-AppleClang-17.0.0.17000604-x86_64-host`; then the game's targets in 11 minutes.
- **The JIT works on macOS x86_64.** `gamerun-jit` translated the ROM (2,900 functions, 10.6 MB of objects) in
  117 s (96 s in the window; LLVM's compile is ~95% of it, on 4 threads) and ran attract for 3,000 frames: 41,494
  interrupts taken, so C++ exceptions do unwind through JIT code here (section 4's item 1 didn't happen on x86_64;
  Apple silicon's JITLink arm64 is still untried). The attract's picture right (software renderer).
- **The window** (`spikeout-jit path/to/spikeofe`) translates, then draws with Metal and runs.
- **The GPU on older Macs.** This Iris is Metal's MTLGPUFamilyMac1, which SDL refuses unless asked; main asks
  (`SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN` in src/gpu/present.cpp, since bfe0f32 with the
  game thread's stack fix, tools/game_thread.hpp). Without it: SDL's renderer, software 3D and no menu.

**OPEN: the characters' shadows are missing in the window, and maybe other effects** (the owner's report, playing
the JIT window on this Mac with Metal). Not looked into yet. First find which part is at fault:
- the renderer: `gamerun-jit ... --renderer compare` over a fight (a saved state from the window, F9) compares the
  GPU's picture with the software renderer's (FILE-<frame>-gpu.png beside each). Only the GPU's missing them points
  at Metal (this MTLGPUFamilyMac1 GPU, or the MSL made from the shaders: check on an Apple silicon Mac too);
- the JIT: the same state through the C++ build's gamerun (step 3, never built from main here yet) with
  `--renderer gpu`; the hash proofs of section 5 also show whether the JIT runs the same game here.

Not done yet: the hash proofs (section 5), the C++ reference build from main, the app bundle and download
(section 6), an Apple silicon build.

## What the Mac download should be

The same as Windows and Linux: the window built with the LLVM JIT (`spikeout-jit`), which translates the game from
the player's own `spikeofe.zip` on the first start, so the download holds nothing made from the ROM. Apple silicon
first; Intel Macs later (a universal app needs LLVM built for both and `lipo`).

## 1. Tools

- Xcode's command line tools: `xcode-select --install` (Apple's Clang, the macOS SDK).
- CMake 3.20+ and Ninja, and OpenSSL 3 for the lobby's connection: `brew install cmake ninja openssl@3` (CMake
  looks in Homebrew's openssl@3 by itself). LLVM is not taken from Homebrew: the build makes its own, the same
  version as Windows and Linux.
- The repository: `git clone https://github.com/tastyrepublic/spikeout-recomp.git` (private: `gh auth login`
  first), branch `main`.
- The ROM set: `spikeofe.zip`, and unzipped into a folder for the ahead-of-time build (step 3).

## 2. The JIT build (what the download is)

```sh
cmake -S . -B build-jit -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIKEOUT_LLVM_JIT=ON
cmake --build build-jit --target spikeout-jit gamerun-jit
```

The first configure downloads LLVM 18.1.8's source (130 MB, SHA-256 checked) and builds it once into
`~/.cache/spikeout-llvm/18.1.8-AppleClang-<version>-arm64-host` (Windows took 38 minutes on 6 jobs; an M-series Mac
should be quicker; `-DSPIKEOUT_LLVM_JOBS=N` for fewer jobs if memory runs short). Its log is beside it if it fails.

Then:

```sh
build-jit/gamerun-jit path/to/spikeofe --blocks 2000000000 --no-sound   # translates (~15-25 s), then runs attract
build-jit/spikeout-jit                                                   # the window: choose spikeofe.zip
```

## 3. The ahead-of-time build (the reference, for the proofs)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIKEOUT_ROM_DIR=path/to/unzipped/spikeofe
cmake --build build
ctest --test-dir build --output-on-failure
```

This recompiles the game into `build/generated/` (from the ROM: never commit it).

## 4. What may need fixing (in order of likelihood)

1. **C++ exceptions through the JIT's code.** The game's `rfi` and missing-code cases throw C++ exceptions that
   unwind through translated code. Windows needed its own memory manager registering each object's unwind tables
   (`SehMemoryManager` in `src/jit/jit.cpp`); Linux worked as is (LLJIT's JITLink registers `.eh_frame`). On macOS
   LLJIT uses JITLink for Mach-O arm64 and registers `__eh_frame` too, but Apple's unwinder prefers compact unwind:
   if `gamerun-jit` dies or hangs at its first exception (early in the boot), this is why. Look at LLJIT's
   `EHFrameRegistrationPlugin` / `setPlatformSetUp`, or emit `-fno-compact-unwind`-style objects (the target
   machine's `MCOptions`), so the frames registered are the ones used.
2. **Executable memory.** Running from Terminal, unsigned, macOS lets JIT memory be written then made executable.
   A signed app with the hardened runtime (needed to notarize) needs the `com.apple.security.cs.allow-jit`
   entitlement and memory mapped with `MAP_JIT` (LLVM's in-process memory manager; check what JITLink's default
   mapper does on Darwin). Not needed for a first test.
3. **The cache key** includes the CPU name and features (`jtmb.getCPU()`), so an M1 and an M3 translate separately:
   fine.
4. **Warnings as errors**: CI builds the ahead-of-time build on macOS with `-Werror` (Apple Clang); the JIT library
   may need what GCC needed (`-Wno-...` for LLVM's headers, see `spikeout_jit` in CMakeLists.txt), for Clang's
   names.
5. **The window**: the menu needs SDL_GPU (Metal: the shaders are compiled to MSL already). The window's icon is
   set at run time (the Dock shows it); `assets/icon/spikeout.icns` is ready for an app bundle.

## 5. Proving it's the same game

The JIT must give the same game as the ahead-of-time build, by RAM hashes (as Windows and Linux were proved):

```sh
build-jit/spikeout-jit path/to/spikeofe --mute --frames 1800 --record mac.rec   # the window, recorded
build/gamerun path/to/spikeofe --replay mac.rec --no-sound                       # "30 of 30 hashes matched"
```

Then the same recording replayed on Windows (`build\gamerun.exe ... --replay mac.rec`) proves the Mac and Windows
run the same game, which network play between them needs. Also: `build-jit/gamerun-jit` against `build/gamerun`
on the attract with sound (`--wav`, the files equal), and a save state from one loaded in the other. The PowerShell
test scripts (`tools/onepc-tests.ps1`, `-Exe build-jit/gamerun-jit`) run under `pwsh` (`brew install powershell`).

## 6. Packaging (after it runs)

- An app bundle: `Spikeout.app/Contents/MacOS/spikeout`, `Contents/Resources/spikeout.icns`, an `Info.plist`
  (CFBundleIconFile spikeout, the version). CMake can make it (`MACOSX_BUNDLE`, `MACOSX_BUNDLE_ICON_FILE`), set on
  the spikeout-jit target for Apple only.
- The data folder on macOS is `~/Library/Application Support/spikeout-recomp/Spikeout` (SDL's), as the other systems
  have theirs.
- Unsigned: players open it once, then System Settings > Privacy & Security > Open Anyway (say so in the README, as
  vf3-recomp does). Signing and notarizing need an Apple developer account.
- The download: `spikeout-<version>-macos-arm64.zip` with the app, `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt` and
  `README.txt`, as `tools/make-release.ps1` makes them for Windows and Linux (a shell version of it, or `pwsh`).
- Then the README: macOS in the status table, the platforms badge, a Mac install guide.

## 7. When done

Commit on a branch, push, and write what was found and changed into this file and CLAUDE.md's "Latest" (the owner
asks for each commit and push).
