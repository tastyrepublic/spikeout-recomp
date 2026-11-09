// How smoothly the game runs, measured the same way in the window and in gamerun (--pace), for the tests to check
// without anyone watching (the owner, 2026-11-09): at each frame's end, after its pacing, the time since the last
// frame's; a stutter when it is over 1.5 frames (as a player would see it). Each second a line:
//
//   speed: frames F-G: X.X a second, N stutters, the longest gap M ms
//
// and at the end, the run's totals (`summary`). tools/speed-check.ps1 reads them: a run with seconds under 56.5
// frames, or stutters, is SLOW. The lines only with SPIKEOUT_NET_DEBUG (the window's Detailed logging): about one a
// second.
#pragma once

#include "recomp/frame_timing.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tools {

class SpeedReport {
public:
    using clock = std::chrono::steady_clock;
    static constexpr double kStutter = 1.5; // (frames: a gap over this is a stutter)

    // A frame shown (paced): `frame` its number. `restart`: the pacing started again (catching up, a join's load):
    // the gap to it isn't counted.
    void frame_end(uint64_t frame, bool restart = false) {
        const auto now = clock::now();
        if (!started_ || restart) {
            started_ = true, last_ = now, second_at_ = now, second_from_ = frame;
            if (!total_from_set_) total_at_ = now, total_from_ = frame, total_from_set_ = true;
            return;
        }
        const double gap = std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
        if (gap > kStutter * 1000.0 / recomp::FrameTiming::kRefreshHz) ++stutters_, ++total_stutters_;
        worst_ = std::max(worst_, gap), total_worst_ = std::max(total_worst_, gap);
        total_to_ = frame, total_end_ = now;
        const double secs = std::chrono::duration<double>(now - second_at_).count();
        if (secs >= 1.0) {
            if (print_)
                std::printf("speed: frames %llu-%llu: %.1f a second, %llu stutters, the longest gap %.0f ms\n",
                            static_cast<unsigned long long>(second_from_), static_cast<unsigned long long>(frame),
                            static_cast<double>(frame - second_from_) / secs, static_cast<unsigned long long>(stutters_), worst_);
            second_at_ = now, second_from_ = frame, stutters_ = 0, worst_ = 0;
        }
    }
    // The whole run's: frames, seconds, a second's, stutters, the longest gap.
    std::string summary() const {
        const double secs = std::chrono::duration<double>(total_end_ - total_at_).count();
        char text[200];
        std::snprintf(text, sizeof text, "speed: in all, frames %llu-%llu in %.1f s: %.2f a second (the arcade's %.2f), %llu stutters, "
                                         "the longest gap %.0f ms",
                      static_cast<unsigned long long>(total_from_), static_cast<unsigned long long>(total_to_), secs,
                      secs > 0 ? static_cast<double>(total_to_ - total_from_) / secs : 0.0, recomp::FrameTiming::kRefreshHz,
                      static_cast<unsigned long long>(total_stutters_), total_worst_);
        return text;
    }
    bool measured() const { return total_to_ > total_from_; }

private:
    bool print_ = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
    bool started_ = false, total_from_set_ = false;
    clock::time_point last_, second_at_, total_at_, total_end_;
    uint64_t second_from_ = 0, stutters_ = 0, total_from_ = 0, total_to_ = 0, total_stutters_ = 0;
    double worst_ = 0, total_worst_ = 0;
};

} // namespace tools
