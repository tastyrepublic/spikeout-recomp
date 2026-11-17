// Test aid (docs/reasm.md): a rebuilt program with part of it moved, put in
// with SPIKEOUT_TEST_PROGRAM, and SPIKEOUT_TEST_PROGRAM_MOVE=FROM:BYTES:TO
// saying how: the program's addresses from FROM up to TO (exclusive) moved
// BYTES on. The game's own addresses known to the emulator (GameSpec's
// code entries and hooks) follow, as names would in a build from source.
// Not set (always, but in those tests): every address as it is.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstdio>

namespace m3::test_move {

struct Move {
    uint32_t from = 0, bytes = 0, to = 0;
};

inline const Move& move() {
    static const Move m = [] {
        Move r;
        if (const char* s = std::getenv("SPIKEOUT_TEST_PROGRAM_MOVE"); s && *s) {
            char* e = nullptr;
            const unsigned long f = std::strtoul(s, &e, 0);
            const unsigned long b = *e == ':' ? std::strtoul(e + 1, &e, 0) : 0;
            const unsigned long t = *e == ':' ? std::strtoul(e + 1, &e, 0) : 0;
            if (f < t) {
                r.from = static_cast<uint32_t>(f);
                r.bytes = static_cast<uint32_t>(b);
                r.to = static_cast<uint32_t>(t);
                std::fprintf(stderr, "SPIKEOUT_TEST_PROGRAM_MOVE: %08x-%08x moved %u bytes on\n", r.from, r.to, r.bytes);
            }
        }
        return r;
    }();
    return m;
}

// The original program's address, where the moved program has it.
inline uint32_t moved(uint32_t addr) {
    const Move& m = move();
    return m.bytes && addr >= m.from && addr < m.to ? addr + m.bytes : addr;
}

// The moved program's address, where the original had it.
inline uint32_t original(uint32_t addr) {
    const Move& m = move();
    return m.bytes && addr >= m.from + m.bytes && addr < m.to + m.bytes ? addr - m.bytes : addr;
}

} // namespace m3::test_move
