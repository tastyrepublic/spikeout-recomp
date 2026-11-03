// Rollback snapshots (m3::Snapshots): a made-up board with big buffers (by
// page) and small fields, written a frame at a time; going back must give
// each frame exactly as it was. Run with the system's page lists (Windows)
// and with comparing, as elsewhere.
#include "runtime/snapshots.hpp"
#include "runtime/watched_memory.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

namespace {

struct Board {
    uint32_t counter = 0;
    uint64_t other = 0;
    m3::WatchedVector<uint8_t> big = m3::WatchedVector<uint8_t>(5 * 4096 + 123, 0);
    m3::WatchedVector<uint32_t> words = m3::WatchedVector<uint32_t>(3000, 0); // (not a whole number of pages)

    void walk(m3::State& s) {
        s.tag("board");
        s.io(counter);
        s.fixed(big);
        s.io(other);
        s.fixed(words);
    }
    void frame(std::mt19937& rng) { // a frame's writes: a few, here and there
        ++counter;
        other = rng();
        for (int i = 0; i < 20; ++i) big[rng() % big.size()] = static_cast<uint8_t>(rng());
        if (rng() % 2) words[rng() % words.size()] = static_cast<uint32_t>(rng());
    }
    struct Copy {
        uint32_t counter;
        uint64_t other;
        std::vector<uint8_t> big;
        std::vector<uint32_t> words;
        bool operator==(const Copy&) const = default;
    };
    Copy copy() const {
        return {counter, other, std::vector<uint8_t>(big.begin(), big.end()), std::vector<uint32_t>(words.begin(), words.end())};
    }
};

// With the board's memory watched by `how` (none: comparing pages).
void run(m3::watched::Method how) {
    const m3::watched::Method was_method = m3::watched::method();
    if (!m3::watched::use(how)) return; // (not on this system)
    std::printf("    (%s)\n", m3::watched::name(how));
    Board b; // (its memory allocated now, watched so)
    m3::watched::use(was_method);
    m3::Snapshots snaps(6);
    const bool compare = how == m3::watched::Method::none;
    snaps.compare_only(compare);
    const auto walk = [&](m3::State& s) { b.walk(s); };
    std::mt19937 rng(static_cast<unsigned>(how) + 3);
    std::map<uint64_t, Board::Copy> was;
    uint64_t frame = 0;
    for (int round = 0; round < 40; ++round) {
        for (int k = 0; k < 5; ++k) { // frames on
            ++frame;
            b.frame(rng);
            snaps.save(frame, walk);
            was[frame] = b.copy();
        }
        b.frame(rng); // (written after the last snapshot: undone too)
        const uint64_t back = frame - rng() % std::min<uint64_t>(6, frame); // (0 to 5 frames back, frame 1 the first)
        CHECK(snaps.has(back));
        snaps.restore(back, walk);
        CHECK(b.copy() == was[back]);
        CHECK_EQ(snaps.last(), back);
        frame = back;
    }
    for (int k = 0; k < 10; ++k) {
        ++frame;
        b.frame(rng);
        snaps.save(frame, walk);
    }
    CHECK(snaps.has(frame - 6)); // (6 back is the most kept)
    CHECK(!snaps.has(frame - 7));
    CHECK_EQ(snaps.load_writes(), std::size_t{0});
    CHECK_EQ(snaps.watched(), !compare);
}

} // namespace

TEST_CASE("snapshots: back to any frame kept, exactly (each way this system has to list pages written)") {
    using M = m3::watched::Method;
    for (M how : {M::write_watch, M::pagemap_scan, M::write_protect}) run(how);
    std::printf("    (the best here: %s)\n", m3::watched::name(m3::watched::method()));
}

TEST_CASE("snapshots: back to any frame kept, exactly (comparing pages)") { run(m3::watched::Method::none); }

TEST_CASE("snapshots: kept sparsely further back, every kept frame exactly as it was") {
    Board b;
    m3::Snapshots snaps(60); // (60 frames back: the newest 8 each, then every 4th)
    snaps.set_sparse(8, 4);
    const auto walk = [&](m3::State& s) { b.walk(s); };
    std::mt19937 rng(11);
    std::map<uint64_t, Board::Copy> was;
    uint64_t frame = 0;
    for (int round = 0; round < 30; ++round) {
        for (int k = 0; k < 20; ++k) {
            ++frame;
            b.frame(rng);
            snaps.save(frame, walk);
            was[frame] = b.copy();
        }
        // (the newest 8 each kept; beyond, multiples of 4 only, the oldest kept aside (the start, until gone by))
        uint64_t oldest = 0;
        for (uint64_t f = frame > 70 ? frame - 70 : 1; f <= frame && !oldest; ++f)
            if (snaps.has(f)) oldest = f;
        for (uint64_t f = oldest; f <= frame; ++f) {
            if (frame - f <= 8) CHECK(snaps.has(f));
            else if (snaps.has(f)) CHECK(f % 4 == 0 || f == oldest);
        }
        CHECK(snaps.kept() < 8 + 60 / 4 + 3);
        b.frame(rng);
        // (back somewhere: to the nearest kept at or before it, exactly)
        const uint64_t want = frame - rng() % std::min<uint64_t>(55, frame);
        const uint64_t back = snaps.kept_at_or_before(want);
        CHECK(back <= want);
        snaps.restore(back, walk);
        CHECK(b.copy() == was[back]);
        frame = back;
    }
}
