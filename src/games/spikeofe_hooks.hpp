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
// The game's waits (the vblank wait's loop 0x36E08, the main loop's 0x36EF8, 0x36F24 and 0x36F44: each reading until
// an interrupt changes what it waits for) gone round at once up to the next thing due, rather than spun
// (recomp::Context::skip_loop: the same state, its time not spent). On unless turned off (gamerun and spikeout
// --no-idle-skip, SPIKEOUT_NO_IDLE_SKIP=1); every board of a process alike.
void set_idle_skip(bool on);

} // namespace m3::games
