// Network play's steps at the end of a board's frame, shared by the window (tools/spikeout.cpp) and gamerun, so that
// the tests run the code players run (docs/handoff.md, the owner's plan step 2: one shared network set-up). Each step
// is the window's, as it was; gamerun's test modes add their own around them.
#pragma once

#include "board_backup.hpp"
#include "games/games.hpp"
#include "games/spikeofe_hooks.hpp"
#include "input_table.hpp"
#include "net/compress.hpp"
#include "netplay.hpp"
#include "one_board.hpp"
#include "recording.hpp"
#include "rollback.hpp"
#include "recomp/context.hpp"
#include "runtime/netboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace tools::net_frame {

// The machines' check that they agree: every kHashEvery frames each board's main RAM hash goes to the others. Without
// rollback at once; with it kept until that frame is settled here (every board's inputs to it in, none guessed wrong):
// a frame that won't be run again.
constexpr uint64_t kHashEvery = 120;

struct Hashes {
    std::map<uint64_t, uint64_t> pending; // (rollback: frame -> hash, waiting to be settled)

    // At the end of `frame`: its hash taken (`hashed`: this board's RAM is the real one here: not a shadow's, which is
    // exact only in what it sends), then those now settled sent.
    void frame_end(NetPlay& net, const InputTable& inputs, bool rollback, std::size_t boards, std::size_t index, uint64_t frame,
                   std::span<const uint8_t> ram, bool hashed) {
        if (hashed && frame % kHashEvery == 0) {
            const uint64_t h = Recording::ram_hash(ram);
            if (rollback) pending[frame] = h;
            else net.hash(frame, index, h);
            ram_dump(index, frame, ram.data(), ram.size());
        }
        while (!pending.empty() && pending.begin()->first <= frame && inputs.all_set(pending.begin()->first, boards) &&
               inputs.wrong_from() > pending.begin()->first) {
            net.hash(pending.begin()->first, index, pending.begin()->second);
            pending.erase(pending.begin());
        }
    }
};

// The inputs a board's next frame runs on, at the end of `frame`: with rollback and guesses allowed, the ones in or a
// guess (a heard player's `ahead` frames past the last in at most, a quiet one's `quiet_ahead`); else waited for. The
// frames that may run again keep theirs (`keep` back). Nothing: the table closed (the game ends).
struct Read {
    bool guess = false;       // (rollback, past its first frame, and no switch of one board's around this frame)
    uint64_t ahead = 0;       // (rollback's guess window for a player heard from)
    uint64_t quiet_ahead = 0; // (and for one gone quiet: rollback::quiet_ahead of its history)
    uint64_t keep = 0;        // (the frames kept in the table behind this one)
};

inline std::optional<InputTable::Inputs> read_inputs(InputTable& table, std::size_t index, uint64_t frame, const Read& r) {
    bool guessed = false;
    const auto got = r.guess ? table.guess(index, frame, r.ahead, guessed, r.quiet_ahead) : table.wait(index, frame);
    if (got && frame > r.keep) table.forget_before(index, frame - r.keep);
    return got;
}

// This board's backup on every other machine now and then, for one board a PC's (any of them may run this board on
// side runs): the pages changed since the last, or whole to a machine with none of it (`bases`: the seats sent one);
// packed and sent off this thread (tools::backup). Due at the end of `frame` (rollback: a settled frame's), taken at
// the snapshot point (backup_now; a shadow's request too: the backups first).
inline bool backup_due(NetPlay& net, const backup::Maker& backups, const InputTable& inputs, bool rollback, std::size_t boards,
                       uint64_t frame) {
    return frame >= backups.frame() + backup::kEvery && net.session().seated() && // (a machine still joining sends none)
           (!rollback || (inputs.all_set(frame, boards) && inputs.wrong_from() > frame));
}

struct BackupSent {
    bool taken = false;                 // (false: taken already this frame)
    std::size_t changes = 0, whole = 0; // (the sizes sent, unpacked)
};

inline BackupSent backup_now(NetPlay& net, backup::Maker& backups, std::set<uint8_t>& bases, std::size_t boards, std::size_t index,
                             m3::LinkRing* ring, uint64_t at, const std::function<void(m3::State&)>& walk) {
    BackupSent sent;
    if (backups.frame() == at) return sent; // (taken already: the backups' turn this frame)
    for (const uint8_t s : net.take_whole_wanted()) bases.erase(s);
    backups.take(at, walk, ring ? ring->taken(index) : 0, ring ? ring->sent(index) : 0);
    if (ring) ring->abandon_cut(index); // (one board's state alone: no cut under way)
    sent.taken = true;
    std::vector<uint8_t> to_changes, to_whole;
    std::vector<uint8_t> to = net.session().backup_watchers(); // (spectators on one board too)
    for (uint8_t s = 0; s < boards; ++s) // (a player still joining: none yet, Session::joining)
        if (s == index) continue;
        else if (net.session().joining(s)) bases.erase(s); // (whole once seated)
        else to.push_back(s);
    for (const uint8_t s : to) {
        (backups.changes_possible() && bases.count(s) ? to_changes : to_whole).push_back(s);
        bases.insert(s);
    }
    if (!to_changes.empty()) {
        std::vector<uint8_t> raw = backups.changes();
        sent.changes = raw.size();
        net.send_backup(at, std::move(raw), std::move(to_changes));
    }
    if (!to_whole.empty()) {
        std::vector<uint8_t> raw = backups.whole();
        sent.whole = raw.size();
        net.send_backup(at, std::move(raw), std::move(to_whole));
    }
    return sent;
}

// One board a PC: the seats whose shadow of this board wants its state at the end of `frame`; leaving (`parting`, its
// frame): every other one. One state for all of them, taken at the snapshot point (board_state_now).
inline std::vector<uint8_t> board_requests(NetPlay& net, std::size_t boards, std::size_t index, uint64_t frame, bool parting) {
    std::vector<uint8_t> seats = net.take_board_requests(frame);
    if (parting)
        for (uint8_t s = 0; s < boards; ++s)
            if (s != index && std::find(seats.begin(), seats.end(), s) == seats.end()) seats.push_back(s);
    return seats;
}

// At the snapshot point: may those seats have this board's state of `frame` now? With rollback only a frame that won't
// be run again: the inputs it ran on waited for (those up to the frame before: a frame's are set at its end, for the
// next; the machine that asked sets its own of this frame only once its shadow is loaded); one guessed wrong: asked
// again (false) and taken when the frame is run again, from the next meeting. Nor while a shadow here waits for a state
// of a frame before it, come late (`game`, one board here): every board goes back there, this frame run again with that
// cabinet whole. Leaving: always.
inline bool board_state_now(NetPlay& net, InputTable& inputs, bool rollback, std::size_t boards, std::size_t index, uint64_t frame,
                            const std::vector<uint8_t>& seats, bool parting, const one_board::Game* game) {
    if (rollback && !parting && frame &&
        (!inputs.wait_all(frame - 1, boards, index) || inputs.wrong_from() < frame || (game && game->late_before(frame)))) {
        for (const uint8_t seat : seats) net.board_request_again(seat, frame);
        return false;
    }
    return true;
}

// Then: as its backup of this frame (backup_now first: the pages written since the last one, sent to every machine off
// this thread), "my backup" to those who asked ...
inline void board_by_backup(NetPlay& net, uint64_t frame, const std::vector<uint8_t>& seats, const char* tag) {
    std::printf("%s: this board's state at frame %llu to %zu seat(s): its backup\n", tag, static_cast<unsigned long long>(frame),
                seats.size());
    net.send_board_after_backups(frame, one_board::pack_remote(0, 0, {}), seats);
}

// ... or (leaving, or no backups) the whole state, sent at once.
inline void board_whole(NetPlay& net, m3::LinkRing* ring, std::size_t index, uint64_t frame, const std::vector<uint8_t>& seats,
                        const std::function<void(m3::State&)>& walk, const char* tag) {
    const auto t0 = std::chrono::steady_clock::now();
    m3::State s;
    walk(s);
    const uint64_t taken = ring ? ring->taken(index) : 0, sent = ring ? ring->sent(index) : 0;
    if (ring) ring->abandon_cut(index); // (one board's state alone: no cut under way)
    const auto t1 = std::chrono::steady_clock::now();
    const std::vector<uint8_t> packed = one_board::pack_remote(taken, sent, ::net::compress(s.data()));
    const auto t2 = std::chrono::steady_clock::now();
    for (const uint8_t seat : seats) {
        std::printf("%s: this board's state at frame %llu to seat %u (%zu KB packed, %zu KB; saved in %.1f ms, compressed in "
                    "%.1f ms)\n",
                    tag, static_cast<unsigned long long>(frame), seat + 1u, packed.size() / 1024, s.data().size() / 1024,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    std::chrono::duration<double, std::milli>(t2 - t1).count());
        net.session().send_board(seat, frame, packed);
    }
}

// One board here (from the hand-over): players leaving after a frame (their boards' states to come), players gone,
// seats taken. `late` (gamerun's test NET_PARTINGS_LATE): each parting taken only once its frame has passed, as one held
// up on the way.
struct SeatNews {
    std::vector<std::pair<uint8_t, uint64_t>> held; // (late: the partings not taken yet)

    void frame_end(NetPlay& net, one_board::Game& game, uint64_t frame, bool late = false) {
        for (const auto& p : net.take_partings()) held.push_back(p);
        for (auto it = held.begin(); it != held.end();)
            if (!late || frame > it->second) {
                game.parting(it->first, it->second);
                it = held.erase(it);
            } else {
                ++it;
            }
        for (const auto& [seat, from] : net.take_gone()) game.seat_gone(seat, from);
        for (const uint8_t seat : net.take_taken()) game.seat_taken(seat);
    }
};

// Every board's state for a player joining or a spectator (or a machine out of step), at the end of a frame every board
// is yet to finish: `capture_frame` (the linked boards'; 0: none under way). With rollback, a frame whose inputs are all
// in already, so taking it waits for none (the host's game held 0.1-0.15 s for a far player's at each join); until then
// (`armed`) the game a little slower (pace), the far players' inputs coming up; after 2 s (one stalled) at the next
// frame, as before. One board here (`alone`, from the hand-over): every other board runs whole first (a shadow from its
// machine's real board, asked for: Game::want_whole), then saved; a board whose player went without a parting can't be.
struct Capture {
    std::atomic<bool>& armed; // (read by the pacing too)
    std::chrono::steady_clock::time_point& armed_at;
    bool& state_pending;  // (asked for: once every board runs whole here)
    uint64_t& whole_until; // (and kept whole until then)

    // True: every board came whole here (one board) and their state is to be taken.
    bool frame_end(NetPlay& net, bool rollback, std::size_t index, uint64_t frame, std::atomic<uint64_t>& capture_frame,
                   one_board::Game* alone, const char* tag) {
        const auto capture_at = [&](uint64_t at) {
            if (!rollback) {
                capture_frame = at;
            } else if (!armed) {
                armed = true;
                armed_at = std::chrono::steady_clock::now();
            }
        };
        // (every other player's inputs here past the frame to be taken (players only: the empty cabinets are filled
        // with ours, the authority's): taking it then waits for none)
        if (armed && capture_frame == 0 &&
            (players_inputs_past(net.session(), index, frame + 2) ||
             std::chrono::steady_clock::now() - armed_at > std::chrono::seconds(2))) {
            capture_frame = frame + 2;
            armed = false;
        }
        if (capture_frame == 0 && !armed && net.take_state_request()) {
            if (!alone || alone->all_whole()) {
                capture_at(frame + 2);
            } else if (alone->can_be_whole()) {
                std::printf("%s: every board's state asked for at frame %llu: every board whole here first\n", tag,
                            static_cast<unsigned long long>(frame));
                alone->want_whole(true);
                state_pending = true;
            } else {
                std::printf("%s: every board's state asked for at frame %llu: a board whose player went is had nowhere whole, "
                            "none sent\n",
                            tag, static_cast<unsigned long long>(frame));
            }
        }
        bool saved = false;
        if (alone && state_pending && capture_frame == 0 && alone->all_whole()) {
            capture_at(frame + 2);
            state_pending = false, whole_until = frame + 10;
            saved = true;
        }
        if (alone && whole_until && frame >= whole_until) alone->want_whole(false), whole_until = 0;
        return saved;
    }
};

// Joined in progress (run on from a state): caught up with the others (as far as the furthest of their inputs: one
// seat alone may be a cabinet whose player left, its inputs stopped where they did); our cabinet then (the caller:
// Session::take_seat, rollback's guesses and snapshots from frame + 2). Not gaining on them after a few seconds (a PC
// that can't run every board faster than the game): the cabinet taken anyway; the others then wait for this machine as
// for any slow player. A spectator runs net::kWatchLag behind every seat.
struct CatchUp {
    uint64_t from = 0; // (the first frame run here; 0: not started)
    std::chrono::steady_clock::time_point at{};
    uint64_t start_behind = 0; // (frames behind then)
    uint64_t behind = 0;       // (and now)

    // True: caught up (or taken anyway).
    bool frame_end(NetPlay& net, std::size_t boards, std::size_t me, bool spectating, uint64_t frame, const char* tag) {
        uint64_t front = 0;
        for (std::size_t s = 0; s < boards; ++s)
            if (s != me || spectating) front = std::max(front, net.session().inputs_known(static_cast<uint8_t>(s)));
        if (spectating) front = front > ::net::kWatchLag ? front - ::net::kWatchLag : 0; // (it runs that far behind them)
        behind = front > frame ? front - frame : 0;
        // (tests: SPIKEOUT_TEST_CATCHUP_MS=N, each frame caught up N ms longer: a slow PC's join, to see it)
        static const int test_catchup_ms = std::getenv("SPIKEOUT_TEST_CATCHUP_MS") ? std::atoi(std::getenv("SPIKEOUT_TEST_CATCHUP_MS")) : 0;
        if (test_catchup_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(test_catchup_ms));
        const auto now = std::chrono::steady_clock::now();
        if (!from) { // (how long it takes: from the first frame run here to the others)
            from = frame, at = now, start_behind = behind;
            std::printf("%s: catching up from frame %llu, the others at %llu\n", tag, static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(front));
        }
        const double so_far = std::chrono::duration<double>(now - at).count();
        const bool given_up = so_far > 5 && front > frame + 4 && front - frame + 30 >= start_behind;
        if (given_up)
            std::printf("%s: not gaining on the others (%llu frames behind after %.0f s): the cabinet taken anyway\n", tag,
                        static_cast<unsigned long long>(front - frame), so_far);
        if (frame + 4 < front && !given_up) return false;
        std::printf("%s: caught up at frame %llu: %llu frames in %.1f s (%.2fx the game's speed)\n", tag,
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(frame - from), so_far,
                    so_far > 0 ? static_cast<double>(frame - from) / so_far / recomp::FrameTiming::kRefreshHz : 0.0);
        from = 0;
        return true;
    }
};

// Rollback, at the very end of `frame`: the boards this machine fills the inputs of told first, by its own board
// (`tell`): those aren't waited for. Then this board's snapshot after the frame (the last asked: another state taken
// here leaves no cut), lazily: only the frames a going back may need (Linked::wants, the same for every board), the
// first from `from` always (none while catching up: every input of those frames is in, none guessed; `from` ~0 then).
inline void rollback_frame_end(rollback::Linked& rb, NetPlay* net, bool tell, std::size_t boards, std::size_t index, uint64_t frame,
                               uint64_t from, recomp::Context& c) {
    if (tell && net) {
        std::vector<bool> local(boards);
        for (std::size_t s = 0; s < boards; ++s) local[s] = net->session().fills_inputs(static_cast<uint8_t>(s), frame + 1);
        rb.set_local(std::move(local));
    }
    if (frame >= from && (frame == from || rb.wants(frame))) c.request_snapshot([&rb, index, frame](uint32_t) { rb.save(index, frame); });
}

// Then each board saves its state at the end of `capture_frame` (the last to save sends them all). With rollback only a
// frame that won't be run again: every input of it in (waited for here; this machine's own set already), none guessed
// wrong; else when it is.
inline bool capture_now(InputTable& inputs, bool rollback, std::size_t boards, std::size_t me, uint64_t frame, uint64_t capture_frame) {
    return capture_frame != 0 && frame == capture_frame && (!rollback || (inputs.wait_all(frame, boards, me) && inputs.wrong_from() > frame));
}

// One board a PC (piece 6): set up at a meeting of the boards (every one stopped), once it is wanted, as the window
// always did; gamerun's network play the same since 2026-11-09. Wanted: no guesses meanwhile, so that a frame settles
// soon (running ahead on guesses, none would). Due: not while every board's state is being saved (`capture_frame`); with
// rollback not where going back (`back`, Linked::between's answer at this meeting), and only on a frame that won't be
// run again, every input of it in. `frame`: the staying board's frames done.
inline bool one_board_due(one_board::Game& game, bool rollback, uint64_t capture_frame, uint64_t back, uint64_t frame,
                          uint64_t rollback_from, InputTable& inputs, std::size_t boards) {
    if (rollback) game.hold_guesses(~0ull);
    if (capture_frame != 0) return false;
    return !rollback || (back == 0 && frame >= rollback_from && inputs.all_set(frame, boards) && inputs.wrong_from() > frame);
}

// What the set-up needs of the program around it.
struct OneBoardSetUp {
    NetPlay& net;
    std::vector<one_board::Board*> parts{}; // every board's part, by index
    std::size_t local = 0;                // the board that stays (this machine's; a spectator's: the watched seat's)
    uint32_t slice = 8;                   // lines between the boards' meetings
    m3::LinkRing* ring = nullptr;
    // A board whose player went without a parting, and a spectator's shadows: a backup run on here (the real network
    // board only, the ROM set at hand): the link and bus settings this machine's boards have.
    bool real_netboard = true;
    const m3::RomSet* roms = nullptr;
    m3::CommBoardOptions link{};
    std::function<void(m3::Bus&)> configure{};
    bool spectating = false;
    rollback::Linked* rollback = nullptr;
    uint64_t guess_ahead = 0; // (rollback's window, in frames)
    InputTable* inputs = nullptr;
    std::function<void(std::size_t seat, uint64_t frame, m3::Bus& bus)> replay_frame{}; // (tests: a backup run on, each frame)
    const char* tag = "spikeout";
};

// The set-up itself: every other board handed over, run whole until the rules find the game calm; their shadows from
// their machines' real boards (Session::want_board), or from their players' backups run on here. Returns the frame.
inline uint64_t set_up_one_board(one_board::Game& game, const OneBoardSetUp& u) {
    one_board::Board& stays = *u.parts[u.local];
    const std::size_t boards = u.parts.size();
    one_board::Game::Settings s;
    s.stays = u.local;
    for (std::size_t b = 0; b < boards; ++b)
        if (b != u.local) s.going.push_back(b);
    s.frame = stays.timing.frames();
    s.tasks = s.rules = true;
    s.slice = u.slice;
    s.net = true;
    // (a shadow from the other machine's real board, asked of it: docs/netplay.md)
    NetPlay* net = &u.net;
    const char* tag = u.tag;
    s.ask_board = [net](std::size_t seat, uint64_t frame) { net->session().want_board(static_cast<uint8_t>(seat), frame); };
    s.wait_board = [net, tag](std::size_t seat, uint64_t frame) -> std::optional<one_board::RemoteBoard> {
        const auto t0 = std::chrono::steady_clock::now();
        auto packed = net->wait_board(static_cast<uint8_t>(seat), frame, std::chrono::seconds(30));
        if (!packed) return std::nullopt;
        const auto t1 = std::chrono::steady_clock::now();
        auto board = one_board::unpack_remote(*packed, [](std::span<const uint8_t> d) { return ::net::decompress(d); });
        const bool backup = board && board->state.empty();
        if (backup) { // ("my backup of that frame": from the copy kept here)
            auto copy = net->backup_at(static_cast<uint8_t>(seat), frame);
            if (!copy) {
                std::printf("%s: board %zu's backup of frame %llu not here\n", tag, seat, (unsigned long long)frame);
                return std::nullopt;
            }
            board->regions = std::move(copy->regions), board->small = std::move(copy->small);
            board->taken = copy->taken, board->sent = copy->sent;
        }
        std::printf("%s: board %zu's state for frame %llu%s: waited %.1f ms for it, unpacked in %.1f ms\n", tag, seat,
                    (unsigned long long)frame, backup ? " (its backup)" : "", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
        return board;
    };
    s.rewind_link = [ring = u.ring](std::size_t seat, const one_board::RemoteBoard& b) {
        if (ring)
            std::printf("one board: board %zu's port there: taken %llu, sent %llu; here: taken %llu, sent %llu (board 0 here: "
                        "taken %llu, sent %llu)\n",
                        seat, (unsigned long long)b.taken, (unsigned long long)b.sent, (unsigned long long)ring->taken(seat),
                        (unsigned long long)ring->sent(seat), (unsigned long long)ring->taken(0), (unsigned long long)ring->sent(0));
        if (!ring || (ring->taken(seat) == b.taken && ring->sent(seat) == b.sent)) return;
        ring->rewind(seat, b.taken);
        ring->unsend(seat, b.sent);
    };
    // (a board whose player went without a parting: its last backup run on here, the packets to each board and its
    // player's inputs kept since its backup for that: tools::backup)
    if (u.real_netboard && u.roms) {
        s.backup_frame = [net](std::size_t seat) { return net->keeper().frame(seat); };
        s.replay = [net, tag, roms = u.roms, link = u.link, configure = u.configure, slice = u.slice, spectating = u.spectating,
                    each = u.replay_frame](std::size_t seat, uint64_t until,
                                           std::function<bool(uint64_t)> live) -> std::optional<one_board::RemoteBoard> {
            // (a spectator: the newest of the backups kept at or before the frame: its players' are taken ahead of it)
            const backup::Copy copy = spectating ? net->keeper().copy_for(seat, until) : net->keeper().copy(seat);
            if (!copy.valid() || copy.frame > until) {
                std::printf("%s: board %zu: no backup of it at or before frame %llu here\n", tag, seat, (unsigned long long)until);
                return std::nullopt;
            }
            std::function<void(uint64_t, m3::Bus&)> each_frame;
            if (each) each_frame = [each, seat](uint64_t frame, m3::Bus& bus) { each(seat, frame, bus); };
            backup::Replay r{*roms, m3::games::kSpikeoutFinalEdition, link, configure,
                             [](recomp::Context& c) { m3::games::install_spikeout_hooks(c); }, slice, copy,
                             net->keeper().log(seat),
                             [net, seat](uint64_t frame) { return net->keeper().inputs(seat, frame); }, std::move(live),
                             std::move(each_frame)};
            try {
                auto result = r.run(until);
                if (!result) return std::nullopt;
                std::printf("%s: board %zu run on from its backup at frame %llu to %llu in %.1f s\n", tag, seat,
                            static_cast<unsigned long long>(copy.frame), static_cast<unsigned long long>(until), result->seconds);
                return one_board::RemoteBoard{std::move(result->state), result->taken, result->sent, {}, {}};
            } catch (const std::exception& e) {
                std::printf("%s: board %zu's backup can't be run on: %s\n", tag, seat, e.what());
                return std::nullopt;
            }
        };
        if (u.ring)
            u.ring->on_departure([net](std::size_t to, uint64_t seq, uint64_t at, const std::vector<uint8_t>& data) {
                net->keeper().packet(to, seq, at, data);
                net->node_check().departed(to, seq, at, data);
            });
        s.record_fix = [net](std::size_t seat, remote::Block& next, uint32_t rec) { net->node_check().fix_record(seat, next, rec); };
        s.header_fix = [net](std::size_t seat, const std::function<void(std::size_t, uint8_t)>& put) { net->node_check().fix_header(seat, put); };
        // A spectator on one board (docs/netplay.md, "Spectators on one board"): no machine to ask for a cabinet's state;
        // its shadow is its player's backup run on here to the frame (the backups sent to it from now on, within the
        // players' spare upload: Session::set_watch_one_board; a few kept, its frames behind theirs).
        if (u.spectating) {
            net->keeper().set_keep(3);
            net->session().set_watch_one_board(true);
            s.ask_board = [](std::size_t, uint64_t) {};
            s.wait_board = [replay = s.replay](std::size_t seat, uint64_t frame) -> std::optional<one_board::RemoteBoard> {
                return replay(seat, frame, [](uint64_t) { return true; }); // (its packets in the log: this board met it there)
            };
        }
    }
    // (rollback: the switches only on frames that won't be run again, Game::may_guess; a board on side runs no part of
    // its own, its network board in this one's snapshots; the module's state kept and put back with this board's)
    if (rollback::Linked* rb = u.rollback) {
        s.guess_ahead = u.guess_ahead;
        InputTable* table = u.inputs;
        s.settled = [table, boards](uint64_t frame) { return table->all_set(frame, boards) && table->wrong_from() > frame; };
        s.wait_settled = [table, boards, stays = u.local](uint64_t frame) { // (this machine's own never guessed)
            return table->wait_all(frame, boards, stays) && table->wrong_from() > frame;
        };
        s.part_whole = [rb](std::size_t board, bool whole, uint64_t) { rb->set_active(board, whole); };
        s.rewound = [rb](std::size_t board) { return rb->rewound_to(board); };
        s.pin = [rb](uint64_t frame) { rb->pin(frame); };
        s.has_board = [net](std::size_t seat, uint64_t frame, uint32_t ms) {
            return net->has_board(static_cast<uint8_t>(seat), frame, std::chrono::milliseconds(ms));
        };
        s.run_again_from = [table](uint64_t frame) { table->run_again_from(frame); };
    }
    game.set_up(u.parts, std::move(s));
    for (std::size_t b = 0; b < boards; ++b)
        if (b != u.local) game.hand_over(*u.parts[b], true);
    if (rollback::Linked* rb = u.rollback) {
        remote::Cabinets* cab = stays.cabinets.get();
        rb->attach_extra(
            u.local, [cab] { return std::shared_ptr<const void>(std::make_shared<remote::Cabinets::Kept>(cab->keep())); },
            [cab](const void* k) { cab->put_back(*static_cast<const remote::Cabinets::Kept*>(k)); });
        // (a switch: set up only on a settled frame, no guesses through the next, nothing gone back to before that one:
        // the module's state kept from its save on)
        game.hold_guesses(stays.timing.frames() + 1);
        rb->pin(stays.timing.frames() + 1);
    }
    return stays.timing.frames();
}

} // namespace tools::net_frame
