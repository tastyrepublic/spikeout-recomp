// The interpreter (prototype): the game's PowerPC code run from the player's
// own ROM with no machine code made at all, for systems that allow no JIT
// (iOS). The same analysis as ppcrecomp and the JIT at start (well under two
// seconds); each function's blocks are then decoded once into a list of
// ready steps (a pre-decoded interpreter: registers, constants, masks and
// branch targets worked out in advance), run with exactly the lifter's
// semantics (src/recomp/lifter.cpp, case for case: keep the three in step):
// the same blocks and cycles, calls, returns, tail calls and jump tables, so
// a run gives the same state, pictures and sound as the recompiled C++.
//
// Linking this library gives generated::game_ram_functions() and the other
// table functions game_setup.hpp declares (the boot ROM's are in the same
// tables), valid after prepare().
#pragma once

#include "recomp/context.hpp"
#include "runtime/rom.hpp"

#include <span>

namespace interp {

struct Report {
    double analysis_s = 0, decode_s = 0, total_s = 0;
    size_t functions = 0, blocks = 0, instructions = 0, steps = 0, bytes = 0;
};

// Throws std::runtime_error when it can't.
Report prepare(const m3::GameSpec& game, const m3::RomSet& roms, bool verbose = true);

std::span<const recomp::FunctionEntry> functions(); // sorted
std::span<const recomp::FunctionEntry> blocks();    // sorted

} // namespace interp
