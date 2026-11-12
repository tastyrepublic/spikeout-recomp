// Network play's timing, kept for problem reports (the owner, 2026-11-12: "a way to reproduce those problems from the
// reports, otherwise a fix won't be sure"). A game's recording (tools/net_record.hpp) has every player's buttons, frame
// by frame, but not when each came over the network; a room standing still is about when. So each machine keeps its
// last few minutes of what came and went, with the time: each datagram of inputs (from whom, whose, which frames),
// each frame's own inputs made here, and the session's turning points (a cabinet taken, a player dropped). A problem
// bundle and Report a problem take it in (net-events.txt); gamerun --net-replay FILE --net-arrivals net-events.txt holds
// each frame's inputs until the time they came here, to run the game again as this machine had it.
//
// Global (one ring for the program, whatever session writes): read from any thread at any time, a session gone or not.
// Cheap: a lock and a few bytes per event; ~200 events a second in a game of four.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace net::events {

enum class Kind : uint8_t {
    inputs_in = 1, // from `from`: seat `seat`'s inputs, frames a .. a + b - 1 (as sent: some may be had already)
    local = 2,     // this machine's own inputs made, for frame a (seat `seat`)
    taken = 3,     // seat `seat`'s cabinet a player's from frame a
    idle = 4,      // seat `seat` dropped: its cabinet idle from frame a
    started = 5,   // the game began here: seats a, this machine's seat `seat` (0xFF: a spectator), from frame b
    note = 6,      // a turning point said in words (the session's log line): `text`
};

struct Event {
    double t = 0; // seconds since the first event of the run
    Kind kind{};
    uint8_t from = 0xFF, seat = 0xFF;
    uint64_t a = 0, b = 0;
    std::string text;
};

namespace detail {
struct Ring {
    std::mutex lock;
    std::vector<Event> events; // (a ring: kKept, the oldest overwritten)
    std::size_t next = 0;
    bool wrapped = false;
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
};
inline Ring& ring() {
    static Ring* r = new Ring; // (never destroyed: sessions may still write at exit)
    return *r;
}
constexpr std::size_t kKept = 60000; // (about 5 minutes of a four-player game)
} // namespace detail

inline void add(Kind kind, uint8_t from, uint8_t seat, uint64_t a, uint64_t b = 0, std::string text = {}) {
    detail::Ring& r = detail::ring();
    Event e{std::chrono::duration<double>(std::chrono::steady_clock::now() - r.origin).count(), kind, from, seat, a, b, std::move(text)};
    std::lock_guard guard(r.lock);
    if (r.events.size() < detail::kKept) r.events.push_back(std::move(e));
    else r.events[r.next] = std::move(e), r.wrapped = true;
    r.next = (r.next + 1) % detail::kKept;
}

// The events of the last `seconds`, oldest first, a line each (empty: none):
//   <seconds> in <from> <seat> <first> <count> | local <seat> <frame> | taken <seat> <frame> | idle <seat> <frame> |
//   started <seats> <seat> <frame> | note <text>
inline std::string text(double seconds = 300) {
    detail::Ring& r = detail::ring();
    std::vector<Event> all;
    {
        std::lock_guard guard(r.lock);
        if (r.events.empty()) return {};
        if (r.wrapped) all.insert(all.end(), r.events.begin() + static_cast<std::ptrdiff_t>(r.next), r.events.end());
        all.insert(all.end(), r.events.begin(), r.events.begin() + static_cast<std::ptrdiff_t>(r.wrapped ? r.next : r.events.size()));
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.origin).count();
    std::string out = "# net-events 1: seconds since the run's first event; seats from 1; see src/net/event_log.hpp\n";
    char line[256];
    for (const Event& e : all) {
        if (e.t < now - seconds) continue;
        switch (e.kind) {
        case Kind::inputs_in:
            std::snprintf(line, sizeof line, "%.4f in %u %u %llu %llu\n", e.t, e.from + 1u, e.seat + 1u, static_cast<unsigned long long>(e.a),
                          static_cast<unsigned long long>(e.b));
            break;
        case Kind::local: std::snprintf(line, sizeof line, "%.4f local %u %llu\n", e.t, e.seat + 1u, static_cast<unsigned long long>(e.a)); break;
        case Kind::taken: std::snprintf(line, sizeof line, "%.4f taken %u %llu\n", e.t, e.seat + 1u, static_cast<unsigned long long>(e.a)); break;
        case Kind::idle: std::snprintf(line, sizeof line, "%.4f idle %u %llu\n", e.t, e.seat + 1u, static_cast<unsigned long long>(e.a)); break;
        case Kind::started:
            std::snprintf(line, sizeof line, "%.4f started %llu %u %llu\n", e.t, static_cast<unsigned long long>(e.a), e.seat + 1u,
                          static_cast<unsigned long long>(e.b));
            break;
        case Kind::note: std::snprintf(line, sizeof line, "%.4f note %s\n", e.t, e.text.c_str()); break;
        }
        out += line;
    }
    return out;
}

} // namespace net::events
