// Rollback's guess of a player's inputs not come yet (tools::InputTable::guess): what they held last (as GGPO), or,
// "learned run lengths" (2026-11-11, docs/netplay.md "Smarter guesses"): each button of each player kept as runs (held
// so many frames, let go so many), and a frame d frames past the last come guessed held only while that player's runs of
// that kind, as long as this one is already, more often than not go on d frames more. A tap (beat, jump) is so guessed
// let go after the player's usual tap; a stick or charge held is held. Learned as the game goes, from the inputs come
// (every machine its own: a guess needn't be the same on every machine, the frames are run again once the real ones
// come). Measured on recordings first (tools/guess-study.cpp).
#pragma once

#include "recording.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace tools {

class InputGuess {
public:
    using Inputs = Recording::Inputs;
    static constexpr int kBits = 32;
    static constexpr uint32_t kLongest = 240; // (runs counted up to this, longer as this)

    // A frame's inputs come (in order, frame after frame).
    void add(const Inputs& in) {
        const uint32_t w = word(in);
        if (!started_) {
            started_ = true, last_ = w;
            run_.fill(1);
            return;
        }
        for (int b = 0; b < kBits; ++b) {
            const bool was = (last_ >> b) & 1u, is = (w >> b) & 1u;
            if (was == is) {
                if (run_[b] < kLongest) ++run_[b];
            } else {
                ends_[b][was].push_back(run_[b]); // (a run of `was` over, so long)
                if (ends_[b][was].size() > kKept) ends_[b][was].erase(ends_[b][was].begin());
                run_[b] = 1;
            }
        }
        last_ = w;
    }
    // The frame `ahead` frames past the last come (1: the next).
    Inputs guess(uint64_t ahead) const {
        if (!started_) return Inputs{};
        uint32_t w = last_;
        for (int b = 0; b < kBits; ++b) {
            const bool now = (last_ >> b) & 1u;
            const auto& ended = ends_[b][now];
            if (ended.size() < kMinRuns) continue; // (too little known: held)
            // (of the runs of this kind that got as long as this one is, how many went on `ahead` more)
            const uint32_t r = run_[b], to = static_cast<uint32_t>(std::min<uint64_t>(r + ahead, kLongest));
            uint32_t reached = 1, past = 1; // (the run now counted as one that goes on: a lean to holding)
            for (uint32_t len : ended)
                if (len >= r) {
                    ++reached;
                    if (len >= to) ++past;
                }
            if (r >= kLongest || past * 2 > reached) continue; // (more often than not on: held)
            w ^= 1u << b;
        }
        return unword(w);
    }

private:
    static constexpr std::size_t kKept = 64, kMinRuns = 4;
    static uint32_t word(const Inputs& in) {
        return uint32_t{in.bank0} | uint32_t{in.bank1} << 8 | uint32_t{in.controls} << 16 | uint32_t{in.controls2} << 24;
    }
    static Inputs unword(uint32_t w) {
        return Inputs{static_cast<uint8_t>(w), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 24)};
    }
    bool started_ = false;
    uint32_t last_ = 0;
    std::array<uint32_t, kBits> run_{};
    std::array<std::array<std::vector<uint32_t>, 2>, kBits> ends_{};
};

} // namespace tools
