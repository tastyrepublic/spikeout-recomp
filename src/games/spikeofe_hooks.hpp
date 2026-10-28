// Spikeout's hooks (m3::GameSpec::hooks): observers in the recompiled game
// code for widescreen's sides (m3::TileSides). They only read the game's
// registers and memory.
#pragma once

#include <cstdint>

namespace recomp {
class Context;
}

namespace m3::games {

// Calls the hooks at the blocks kHooks names (games/spikeofe.cpp).
void install_spikeout_hooks(recomp::Context& c);
// The hooks themselves, for a tool's own hook to pass the others on to.
void spikeout_hook(recomp::Context& c, uint32_t pc, void* user);

} // namespace m3::games
