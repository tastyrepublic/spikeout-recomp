// The input delay's Auto (tools::DelayTuner::step): fed made-up seconds of hitches, what it decides. With rollback it
// tries a frame more when hitches are frequent and keeps it only if they come down; tries a frame less once calm.
#include "../tools/delay_tuner.hpp"
#include "test.hpp"

#include <cstdint>
#include <functional>

namespace {

// `seconds` of play: each second's hitches from `hitches(delay)`; the delay as the tuner moves it.
uint64_t play(tools::DelayTuner& t, int one_way, uint64_t& delay, int seconds, const std::function<uint64_t(uint64_t)>& hitches) {
    uint64_t moves = 0;
    for (int s = 0; s < seconds; ++s) {
        const uint64_t next = t.step(one_way, true, hitches(delay), delay);
        moves += next != delay;
        delay = next;
    }
    return moves;
}

} // namespace

TEST_CASE("auto delay: a close player with frequent hitches gets a frame when it halves them") {
    tools::DelayTuner t;
    uint64_t delay = tools::DelayTuner::base(5, true);
    CHECK_EQ(delay, uint64_t{0}); // (5 ms one way: the distance asks for none)
    play(t, 5, delay, 30, [](uint64_t d) { return d == 0 ? uint64_t{3} : uint64_t{1}; });
    CHECK_EQ(delay, uint64_t{1});
}

TEST_CASE("auto delay: a frame that doesn't help is given back, and not tried again at once") {
    tools::DelayTuner t;
    uint64_t delay = 0;
    const uint64_t moves = play(t, 5, delay, 25, [](uint64_t) { return uint64_t{3}; });
    CHECK_EQ(delay, uint64_t{0});
    CHECK_EQ(moves, uint64_t{2}); // (up once, back once: then resting)
}

TEST_CASE("auto delay: calm for long, a frame less tried, and kept when still calm") {
    tools::DelayTuner t;
    uint64_t delay = 0;
    play(t, 5, delay, 20, [](uint64_t d) { return d == 0 ? uint64_t{3} : uint64_t{0}; });
    CHECK_EQ(delay, uint64_t{1});
    // (the hitches gone at either delay now: the other player's line got better)
    play(t, 5, delay, 60, [](uint64_t) { return uint64_t{0}; });
    CHECK_EQ(delay, uint64_t{0});
}

TEST_CASE("auto delay: a frame less that brings the hitches back goes back up, and stays a while") {
    tools::DelayTuner t;
    uint64_t delay = 0;
    play(t, 5, delay, 20, [](uint64_t d) { return d == 0 ? uint64_t{3} : uint64_t{0}; });
    CHECK_EQ(delay, uint64_t{1});
    play(t, 5, delay, 50, [](uint64_t d) { return d == 0 ? uint64_t{3} : uint64_t{0}; });
    CHECK_EQ(delay, uint64_t{1});
    const uint64_t moves = play(t, 5, delay, 90, [](uint64_t d) { return d == 0 ? uint64_t{3} : uint64_t{0}; });
    CHECK_EQ(delay, uint64_t{1});
    CHECK_EQ(moves, uint64_t{0}); // (no lower again for 2 minutes)
}

TEST_CASE("auto delay: no rollback, the distance's frames, after 3 seconds asking") {
    tools::DelayTuner t;
    uint64_t delay = 0;
    for (int s = 0; s < 2; ++s) delay = t.step(60, false, 0, delay);
    CHECK_EQ(delay, uint64_t{0});
    delay = t.step(60, false, 0, delay);
    CHECK_EQ(delay, uint64_t{1});
    for (int s = 0; s < 20; ++s) delay = t.step(60, false, 0, delay);
    CHECK_EQ(delay, tools::DelayTuner::most(60));
}

TEST_CASE("auto delay: never past the distance's frames plus two") {
    tools::DelayTuner t;
    uint64_t delay = 0;
    play(t, 5, delay, 400, [](uint64_t d) { return uint64_t{8} >> d; }); // (each frame halves them)
    CHECK_EQ(delay, uint64_t{2});
}
