// Network play's input delay, chosen automatically (the window's Auto, gamerun --input-delay auto): from the furthest
// player's distance and, with rollback, from how well this machine absorbs its rollbacks (the owner's choice,
// 2026-11-06: each PC raises its own delay as far as it needs to stay at the arcade's speed, never past the far
// player's distance).
//
// Without rollback: the frames the furthest player is away (one way, past a frame's slack): every input there in
// time. With rollback: half of them, two at most, to start (rollback covers the rest); then, once a second, from
// rollback's catching up (tools::rollback::Linked::take_catches): more than a quarter of the second's catch-ups longer
// than a frame (the frame shown late: the game below 57.5) for two seconds running, a frame more (then three
// seconds to see what it did); thirty seconds with none late, a frame less to try; that late again within ten
// seconds, back up, and no lower again for two minutes. One frame at a time either way.
#pragma once

#include "recomp/frame_timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tools {

class DelayTuner {
public:
    // The delay the furthest player's distance asks for (one way, ms; < 0: not known yet).
    static uint64_t base(int one_way, bool rollback) {
        const uint64_t frames = most(one_way);
        return rollback ? std::min<uint64_t>(2, frames / 2) : frames;
    }
    // The most it may go to: every input in time from the furthest player.
    static uint64_t most(int one_way) {
        if (one_way <= 8) return 0;
        return static_cast<uint64_t>(std::ceil((one_way - 8) / (1000.0 / recomp::FrameTiming::kRefreshHz)));
    }

    // Once a second: the furthest player's distance, rollback or not, and (rollback) this second's catch-ups and
    // how many of them took longer than a frame. The delay wanted now.
    uint64_t tick(int one_way, bool rollback, uint64_t catches = 0, uint64_t late = 0) {
        ++seconds_;
        const uint64_t low = base(one_way, rollback), high = std::max(low, most(one_way));
        if (!rollback) return extra_ = 0, low;
        const bool bad = catches >= 2 && late * 4 > catches;
        bad_ = bad ? bad_ + 1 : 0;
        calm_ = late ? 0 : calm_ + 1;
        if (seconds_ < wait_until_) return std::min(high, low + extra_);
        if (bad_ >= 2 && low + extra_ < high) {
            ++extra_, bad_ = 0, wait_until_ = seconds_ + 3;
            if (tried_lower_ && seconds_ < tried_lower_at_ + 10) floor_ = extra_, floor_until_ = seconds_ + 120; // (lower didn't hold)
        } else if (calm_ >= 30 && extra_ > 0 && !(extra_ <= floor_ && seconds_ < floor_until_)) {
            --extra_, calm_ = 0, tried_lower_ = true, tried_lower_at_ = seconds_, wait_until_ = seconds_ + 3;
        }
        return std::min(high, low + extra_);
    }
    uint64_t extra() const { return extra_; } // (frames above the distance's own)

private:
    uint64_t seconds_ = 0, extra_ = 0, bad_ = 0, calm_ = 0, wait_until_ = 0;
    uint64_t tried_lower_at_ = 0, floor_ = 0, floor_until_ = 0;
    bool tried_lower_ = false;
};

} // namespace tools
