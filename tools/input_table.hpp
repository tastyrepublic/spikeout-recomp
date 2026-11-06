// Each board's inputs, by frame: the meeting point between where inputs come
// from (keys and pads, --press, a recording, other players over the
// network) and the boards that read them. A board reads at the end of each
// frame the inputs for the next, as recordings keep them (tools::Recording:
// the inputs set at the end of frame F), so a run is decided by the table
// alone, not by when an input happened to come in.
//
// Each board's frames are set in order (frame F only after F - 1, gaps
// filled with the last inputs set). A frame not yet set can be waited for
// (delay-based network play: a board holds its frame until every player's
// inputs for it are in). Thread-safe: sources and boards on threads of
// their own.
#pragma once

#include "recording.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace tools {

class InputTable {
public:
    using Inputs = Recording::Inputs;
    static constexpr uint64_t kGuessHeld = 8; // (rollback: frames a missing player is guessed to hold what they held)

    explicit InputTable(std::size_t boards) : boards_(boards) {}
    std::size_t boards() const { return boards_.size(); }

    // Board `board`'s inputs set at the end of `frame` (and of the frames
    // since the last set, if any were skipped). A frame already set keeps
    // what it had.
    void set(std::size_t board, uint64_t frame, const Inputs& in) {
        {
            std::lock_guard guard(lock_);
            Board& b = boards_[board];
            if (b.known && frame < b.next) return;
            if (!b.known) b.next = frame; // (the first: frames before it are never read)
            while (b.next < frame) settle(b, b.frames.empty() ? Inputs{} : b.frames.back()); // (gaps: the last held)
            settle(b, in);
        }
        changed_.notify_all();
    }

    // The inputs set at the end of `frame`, if they have been.
    std::optional<Inputs> get(std::size_t board, uint64_t frame) const {
        std::lock_guard guard(lock_);
        return find(boards_[board], frame);
    }

    // Waits until they have been (or the table is closed: nothing then).
    std::optional<Inputs> wait(std::size_t board, uint64_t frame) {
        std::unique_lock guard(lock_);
        changed_.wait(guard, [&] { return closed_ || find(boards_[board], frame); });
        return find(boards_[board], frame);
    }

    // Rollback (tools/rollback.hpp): the inputs set at the end of `frame`; or, not set yet and fewer than `ahead`
    // frames past the last set, a guess, remembered (`guessed` true): what this board's player held last (a cabinet
    // held: as held). Set later to something else, it was guessed wrong (wrong_from). Further ahead: waited for, as
    // wait() (nothing: the table closed).
    // `ahead_quiet`: how far for a board whose player's machine has gone quiet (set_quiet): the long window, so the
    // game runs on with them standing still (as the drop will have them) rather than waiting for the drop.
    std::optional<Inputs> guess(std::size_t board, uint64_t frame, uint64_t ahead, bool& guessed, uint64_t ahead_quiet = 0) {
        std::unique_lock guard(lock_);
        Board& b = boards_[board];
        changed_.wait(guard, [&] {
            return closed_ || find(b, frame) || !b.known || frame < b.next + (b.quiet ? std::max(ahead, ahead_quiet) : ahead);
        });
        guessed = false;
        if (const auto in = find(b, frame)) return in;
        if (closed_) return std::nullopt;
        // (held for kGuessHeld frames past the last in; further: none, a player gone quiet as the drop will have it)
        const Inputs g = b.known && !b.frames.empty() && frame < b.next + kGuessHeld ? b.frames.back() : Inputs{};
        b.guesses[frame] = g;
        guessed = true;
        return g;
    }
    // Rollback's lazy snapshots (tools::rollback::Linked::wants): nothing guessed and not settled yet, no wrong guess
    // still to go back for, and `frame`'s inputs in for every board but those this machine fills itself (`local`: its
    // own, and the empty cabinets when it's the authority: never guessed). Then the frame before `frame` needn't be
    // kept: nothing will go back to it.
    bool settled_but(uint64_t frame, std::size_t boards, const std::vector<bool>& local) const { // (the first `boards`)
        std::lock_guard guard(lock_);
        if (wrong_ != ~0ull) return false;
        for (std::size_t i = 0; i < boards && i < boards_.size(); ++i) {
            if (!boards_[i].guesses.empty()) return false;
            if (!(i < local.size() && local[i]) && !find(boards_[i], frame)) return false;
        }
        return true;
    }
    // Board `board`'s player's machine quiet (nothing heard from it for a while, the session says) or heard again.
    void set_quiet(std::size_t board, bool on) {
        {
            std::lock_guard guard(lock_);
            if (board >= boards_.size()) return;
            boards_[board].quiet = on;
        }
        changed_.notify_all(); // (a guess waiting for it may go on)
    }
    // The first frame guessed wrong since the last asked (~0: none).
    uint64_t take_wrong() {
        std::lock_guard guard(lock_);
        return std::exchange(wrong_, ~0ull);
    }
    uint64_t wrong_from() const {
        std::lock_guard guard(lock_);
        return wrong_;
    }
    // Every board to run again from `frame` on, as if a guess there had been wrong (one board a PC: a shadow's state
    // come after its frame, loaded there: tools::one_board).
    void run_again_from(uint64_t frame) {
        std::lock_guard guard(lock_);
        wrong_ = std::min(wrong_, frame);
    }
    // Going back to the end of `frame`: the guesses after it made again as the frames run again, and the wrong ones
    // after it so dealt with. One found wrong at or before `frame` meanwhile (another board's inputs come in while the
    // boards were put back: with three players or more, one's inputs behind another's) is kept, to go back for next:
    // cleared with the rest, that machine stayed out of step for good (2026-11-06, bots pressing all the time).
    void forget_guesses_after(uint64_t frame) {
        std::lock_guard guard(lock_);
        for (Board& b : boards_) b.guesses.erase(b.guesses.upper_bound(frame), b.guesses.end());
        if (wrong_ != ~0ull && wrong_ > frame) wrong_ = ~0ull;
    }
    // Every board's inputs up to `frame` set (none guessed or still to be).
    bool all_set(uint64_t frame, std::size_t boards, std::size_t but = ~std::size_t{0}) const { // (the first `boards`: those
                                                                                           // in the game; `but`: ours)
        std::lock_guard guard(lock_);
        for (std::size_t i = 0; i < boards && i < boards_.size(); ++i)
            if (i != but && !find(boards_[i], frame)) return false;
        return true;
    }
    // And waited for (nothing: the table closed).
    bool wait_all(uint64_t frame, std::size_t boards, std::size_t but = ~std::size_t{0}) { // (`but`: this machine's own, set after)
        std::unique_lock guard(lock_);
        changed_.wait(guard, [&] {
            if (closed_) return true;
            for (std::size_t i = 0; i < boards && i < boards_.size(); ++i)
                if (i != but && !find(boards_[i], frame)) return false;
            return true;
        });
        return !closed_;
    }

    // Rollback, for the overlay: how often a board's inputs were guessed wrong (runs of frames one after another
    // counted once: each run, most likely, once gone back for) since `frame` (kept 20 s).
    std::size_t wrongs_since(std::size_t board, uint64_t frame) const {
        std::lock_guard guard(lock_);
        if (board >= boards_.size()) return 0;
        std::size_t n = 0;
        for (const Board::Wrong& w : boards_[board].wrong) n += w.last >= frame;
        return n;
    }
    // The first frame still kept (forget_before).
    uint64_t first(std::size_t board) const {
        std::lock_guard guard(lock_);
        return boards_[board].first;
    }
    // The first frame not yet set (0: none).
    uint64_t next(std::size_t board) const {
        std::lock_guard guard(lock_);
        return boards_[board].next;
    }

    // Frames before `frame` won't be read again: let them go.
    void forget_before(std::size_t board, uint64_t frame) {
        std::lock_guard guard(lock_);
        Board& b = boards_[board];
        while (!b.frames.empty() && b.first < frame && b.first + 1 < b.next) {
            b.frames.pop_front();
            ++b.first;
        }
    }

    // From `frame` on, every frame's inputs are `in` (a player who has
    // dropped: none held, the character standing idle).
    void hold_from(std::size_t board, uint64_t frame, const Inputs& in) {
        {
            std::lock_guard guard(lock_);
            Board& b = boards_[board];
            b.hold_from = frame;
            b.hold_until.reset();
            b.held = in;
            for (auto it = b.guesses.lower_bound(frame); it != b.guesses.end(); it = b.guesses.erase(it)) // (guessed: held now)
                if (!same(it->second, in)) wrong_ = std::min(wrong_, it->first);
        }
        changed_.notify_all();
    }
    // And up to `frame` only (a player taking the cabinet from there: theirs, as they are set).
    void hold_until(std::size_t board, uint64_t frame) {
        std::lock_guard guard(lock_);
        Board& b = boards_[board];
        if (b.hold_from) b.hold_until = frame;
    }

    // Every board's frames let go, and open again after a close (a run
    // starting again from a state, its boards stopped).
    void reset() {
        std::lock_guard guard(lock_);
        for (Board& b : boards_) b = Board{};
        closed_ = false;
        wrong_ = ~0ull;
    }

    // Wakes every wait (a session ending).
    void close() {
        {
            std::lock_guard guard(lock_);
            closed_ = true;
        }
        changed_.notify_all();
    }

private:
    struct Board {
        std::deque<Inputs> frames; // frames first .. next - 1
        uint64_t first = 0, next = 0;
        bool known = false; // any set yet
        std::optional<uint64_t> hold_from, hold_until;
        Inputs held;
        std::map<uint64_t, Inputs> guesses; // (rollback: frames read before they were set, as guessed)
        bool quiet = false; // (its player's machine not heard from for a while: guessed as far as the long window)
        struct Wrong {
            uint64_t first, last; // (a run of frames guessed wrong, one after another: one going back, most likely)
        };
        std::deque<Wrong> wrong; // (wrongs_since)
    };
    static constexpr uint64_t kWrongKept = 1200; // (frames: 20 s)
    static bool same(const Inputs& a, const Inputs& b) {
        return a.bank0 == b.bank0 && a.bank1 == b.bank1 && a.controls == b.controls && a.controls2 == b.controls2;
    }
    // A frame set: a guess of it checked (the lock held).
    void settle(Board& b, const Inputs& in) {
        if (const auto it = b.guesses.find(b.next); it != b.guesses.end()) {
            const bool held = b.hold_from && b.next >= *b.hold_from && (!b.hold_until || b.next < *b.hold_until);
            if (!held && !same(it->second, in)) {
                wrong_ = std::min(wrong_, b.next);
                if (b.wrong.empty() || b.wrong.back().last + 1 != b.next) b.wrong.push_back({b.next, b.next});
                else b.wrong.back().last = b.next;
                while (b.wrong.size() > 1 && b.wrong.front().last + kWrongKept < b.next) b.wrong.pop_front();
            }
            b.guesses.erase(it);
        }
        push(b, in);
    }
    static void push(Board& b, const Inputs& in) {
        if (!b.known) b.first = b.next;
        b.known = true;
        b.frames.push_back(in);
        ++b.next;
    }
    static std::optional<Inputs> find(const Board& b, uint64_t frame) {
        if (b.hold_from && frame >= *b.hold_from && (!b.hold_until || frame < *b.hold_until)) return b.held;
        if (!b.known || frame >= b.next || frame < b.first) return std::nullopt;
        return b.frames[static_cast<std::size_t>(frame - b.first)];
    }

    mutable std::mutex lock_;
    std::condition_variable changed_;
    std::vector<Board> boards_;
    bool closed_ = false;
    uint64_t wrong_ = ~0ull; // (rollback: the first frame guessed wrong)
};

} // namespace tools
