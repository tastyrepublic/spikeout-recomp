// Spikeout's hooks (m3::GameSpec::hooks): observers in the recompiled game
// code for widescreen's sides (m3::TileSides). They only read the game's
// registers and memory.
#pragma once

namespace recomp {
class Context;
}

namespace m3::games {

// Calls the hooks at the blocks kHooks names (games/spikeofe.cpp).
void install_spikeout_hooks(recomp::Context& c);

} // namespace m3::games
