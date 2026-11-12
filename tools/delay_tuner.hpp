// Network play's input delay, chosen automatically (the window's Auto, gamerun --input-delay auto), the same rule for
// both (tools::DelayTuner::step, once a second).
//
// Without rollback: the frames the furthest player is away (one way, past a frame's slack): every input there in time;
// moved a frame once the distance has asked for the same for 3 seconds (a moment's spike doesn't move it).
//
// With rollback (2026-11-11, docs/netplay.md "Auto delay that tries": the owner's wish, a smarter and real reflection,
// nothing forced): half the distance's frames, two at most, to start (rollback covers the rest); then tried, not
// predicted. A hitch: a catch-up after going back that took longer than a frame (the frame shown late; rollback::Linked::
// take_catches). Hitches often (kTryAt a second over the last kLook seconds): a frame more is tried; kSettle seconds to
// settle, kTrial to measure; kept only if the hitches came down to kKeep of what they were, else back, and no try again
// for kRest seconds (twice as long each time it didn't help, up to 4 minutes). Kept, kCalm seconds with few: a frame
// less tried the same way, kept unless the hitches rise past what they were, and if they did, not lower for 2 minutes.
// No distance rule above (a close player's frame of delay can halve the hitches: measured 2026-11-11, delay 0 against 1
// on this PC, rollbacks over two frames -62%); at most the distance's frames plus kAbove.
#pragma once

#include "recomp/frame_timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <numeric>

namespace tools {

class DelayTuner {
public:
    static constexpr int kLook = 5, kSettle = 2, kTrial = 5, kCalm = 30;
    static constexpr double kTryAt = 1.0, kKeep = 0.7;
    static constexpr uint64_t kAbove = 2, kRest = 30;

    // The delay the furthest player's distance asks for (one way, ms; < 0: not known yet).
    static uint64_t base(int one_way, bool rollback) {
        const uint64_t frames = most(one_way);
        return rollback ? std::min<uint64_t>(2, frames / 2) : frames;
    }
    // Every input in time from the furthest player.
    static uint64_t most(int one_way) {
        if (one_way <= 8) return 0;
        return static_cast<uint64_t>(std::ceil((one_way - 8) / (1000.0 / recomp::FrameTiming::kRefreshHz)));
    }
    // The highest a try may go.
    static uint64_t top(int one_way, bool rollback) { return rollback ? most(one_way) + kAbove : most(one_way); }

    // Once a second in play: the furthest player's distance (< 0: not known), rollback or not, this second's hitches
    // (catch-ups longer than a frame), and the delay in use. The delay to use from now (a frame from `have` at most).
    uint64_t step(int one_way, bool rollback, uint64_t late, uint64_t have) {
        ++seconds_;
        if (one_way < 0) return have;
        const uint64_t low = base(one_way, rollback), high = std::max(low, top(one_way, rollback));
        if (!rollback) { // (the distance's, once asked for 3 seconds)
            asked_ = low == have ? 0 : asked_ + 1;
            if (asked_ < 3) return have;
            asked_ = 0;
            return low > have ? have + 1 : have - 1;
        }
        hitches_.push_back(late);
        while (hitches_.size() > 120) hitches_.pop_front();
        // (the distance moved past it: back within at once, a trial dropped)
        if (have < low || have > high) {
            trial_ = Trial::none, since_ = seconds_;
            return have < low ? have + 1 : have - 1;
        }
        switch (trial_) {
        case Trial::none: {
            if (seconds_ < since_ + kLook) return have; // (a look at this delay first)
            const double now = rate(kLook);
            if (now >= kTryAt && have < high && seconds_ >= rest_until_) {
                start(Trial::up, now, have);
                say("a frame more tried", have + 1, now);
                return have + 1;
            }
            const double calm = rate(kCalm);
            if (have > low && seconds_ >= since_ + kCalm && calm < kTryAt / 4 && seconds_ >= floor_until_) {
                start(Trial::down, calm, have);
                say("a frame less tried", have - 1, calm);
                return have - 1;
            }
            return have;
        }
        case Trial::up:
        case Trial::down: {
            if (seconds_ < trial_at_ + kSettle + kTrial) return have; // (settling, then measuring)
            const double tried = rate(kTrial);
            const bool up = trial_ == Trial::up;
            trial_ = Trial::none, since_ = seconds_;
            if (up) {
                if (tried <= before_ * kKeep) {
                    rest_ = kRest;
                    say("kept", have, tried);
                    return have;
                }
                rest_until_ = seconds_ + rest_, rest_ = std::min<uint64_t>(rest_ * 2, 240);
                say("no better: back", from_, tried);
                return from_;
            }
            if (tried > std::max(before_ * 1.5, kTryAt / 2)) {
                floor_until_ = seconds_ + 120;
                say("worse: back up", from_, tried);
                return from_;
            }
            say("kept", have, tried);
            return have;
        }
        }
        return have;
    }
    bool trying() const { return trial_ != Trial::none; }

private:
    enum class Trial { none, up, down };
    // Hitches a second over the last `n` seconds.
    double rate(int n) const {
        const std::size_t k = std::min<std::size_t>(hitches_.size(), static_cast<std::size_t>(n));
        if (!k) return 0;
        const uint64_t sum = std::accumulate(hitches_.end() - static_cast<std::ptrdiff_t>(k), hitches_.end(), uint64_t{0});
        return static_cast<double>(sum) / static_cast<double>(k);
    }
    void start(Trial t, double before, uint64_t from) { trial_ = t, before_ = before, from_ = from, trial_at_ = seconds_; }
    static void say(const char* what, uint64_t delay, double rate) {
        std::printf("auto delay: %s (delay %llu, %.1f hitches a second)\n", what, static_cast<unsigned long long>(delay), rate);
    }

    uint64_t seconds_ = 0, since_ = 0, trial_at_ = 0, from_ = 0, rest_until_ = 0, rest_ = kRest, floor_until_ = 0;
    int asked_ = 0;
    double before_ = 0;
    Trial trial_ = Trial::none;
    std::deque<uint64_t> hitches_;
};

} // namespace tools
