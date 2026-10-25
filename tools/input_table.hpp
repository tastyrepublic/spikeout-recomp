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

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace tools {

class InputTable {
public:
    using Inputs = Recording::Inputs;

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
            while (b.next < frame) push(b, b.frames.empty() ? Inputs{} : b.frames.back()); // (gaps: the last held)
            push(b, in);
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
    };
    static void push(Board& b, const Inputs& in) {
        if (!b.known) b.first = b.next;
        b.known = true;
        b.frames.push_back(in);
        ++b.next;
    }
    static std::optional<Inputs> find(const Board& b, uint64_t frame) {
        if (!b.known || frame >= b.next || frame < b.first) return std::nullopt;
        return b.frames[static_cast<std::size_t>(frame - b.first)];
    }

    mutable std::mutex lock_;
    std::condition_variable changed_;
    std::vector<Board> boards_;
    bool closed_ = false;
};

} // namespace tools
