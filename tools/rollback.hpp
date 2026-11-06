// Rollback for network play (docs/netplay.md, "Rollback in play"): the boards run on with a guess for any player's
// inputs not in yet (InputTable::guess: what they held last), a few frames ahead at most; each board's state is kept
// after every frame by its written pages (m3::Snapshots), and the link ring's part of every frame, cut where the boards
// meet; when a guess turns out wrong, every board and the ring go back to the frame before it, together, where they
// meet, and run the frames since again with the inputs as they were, drawing and playing nothing (the sound of a
// frame heard once, the first time: the owner's choice), on to where they were. As gamerun --rollback-test proves on
// linked boards, frame for frame.
//
// A board: save() at its snapshot point after each frame (Context::request_snapshot, the last asked that frame:
// another state taken there leaves no cut under way, LinkRing::abandon_cut); between() where the boards meet, every
// one stopped (Turns' between, after the ring's exchange); rewind() as each leaves the meeting: true once after
// going back, the board then going on from its own snapshot (throw Rewound, caught where it runs: resume_block).
#pragma once

#include "input_table.hpp"
#include "runtime/memory_free.hpp"
#include "runtime/netboard.hpp"
#include "runtime/snapshots.hpp"
#include "runtime/state.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tools::rollback {

struct Rewound {}; // (thrown by a board leaving the meeting after going back: it goes on from its snapshot)

// The window (the owner's choice, 2026-11-02): longer than a silent player takes to be dropped (net::kDropAfter, 3
// seconds), so that the game runs on meanwhile, guessing them as the drop will have them (none, after a few frames
// held); the frames since run again once, when they come back or are dropped. About 0.4-0.7 MB of pages a board a frame.
constexpr uint64_t kWindow = 210;
// Kept sparsely beyond the newest kDense frames: every kEvery-th (going further back, rare, runs up to kEvery - 1
// frames more again); about 5 times less memory over the window.
constexpr std::size_t kDense = 16, kEvery = 8;
// Lazy snapshots (Linked::wants): a frame kept at least this often, all else aside.
constexpr uint64_t kAlways = 32;

// The window this machine can keep: `wanted`, shorter where the kept frames of `boards` boards wouldn't fit in a
// third of the memory free now (`free_bytes`, m3::memory_free_bytes; 0: not known, as wanted), kLeast frames at
// the least. Measured (2026-11-02, kept sparsely): about 0.44 MB a board a frame of the window (175 MB for two
// boards over 210 frames, 370 MB for four), and a copy of each board's big buffers, about 32 MB, beside. Each
// machine its own: the window is how far its guesses run ahead, nothing the others share; a shorter one only
// waits sooner (a silent player: the game holds before the drop, as delay-based play does).
constexpr uint64_t kLeast = 30;
inline uint64_t fit_window(uint64_t wanted, std::size_t boards, uint64_t free_bytes) {
    constexpr uint64_t kBoardCopy = 32ull << 20, kFrameBytes = 450ull << 10;
    if (!free_bytes || !boards || wanted <= kLeast) return wanted;
    const uint64_t each = free_bytes / 3 / boards;
    const uint64_t fits = each > kBoardCopy ? (each - kBoardCopy) / kFrameBytes : 0;
    return std::min(wanted, std::max(kLeast, fits));
}
// How far a quiet player's inputs may be guessed (InputTable::guess's ahead_quiet) with a history of `depth` frames:
// short of it by a sparse step, so the frame before the first of those guesses is still kept to go back to.
inline uint64_t quiet_ahead(uint64_t depth) { return depth > kEvery + 2 ? depth - kEvery - 2 : 0; }
// The memory free for it now (tests: ROLLBACK_FREE_MB=N, as if that much were free).
inline uint64_t free_for_window() {
    if (const char* mb = std::getenv("ROLLBACK_FREE_MB")) return std::strtoull(mb, nullptr, 0) << 20;
    return m3::memory_free_bytes();
}

class Linked {
public:
    // `depth`: frames a guess may run ahead (InputTable::guess's `ahead`); kept one more than that.
    Linked(std::size_t boards, uint64_t depth, m3::LinkRing* ring, InputTable& table)
        : depth_(depth), ring_(ring), table_(table), parts_(boards) {
        for (Part& p : parts_) {
            p.snaps = std::make_unique<m3::Snapshots>(depth + 1);
            if (!std::getenv("ROLLBACK_EVERY_FRAME")) p.snaps->set_sparse(kDense, kEvery); // (every frame for the newest few, every 8th beyond; tests: all)
        }
    }
    uint64_t depth() const { return depth_; }
    // Each board's state as its snapshots walk it (tools::board_state, without the sound board: it hears a frame once).
    void attach(std::size_t board, std::function<void(m3::State&)> walk) { parts_[board].walk = std::move(walk); }
    // And state of its own beside the board's (one board a PC: the cabinet module's), kept with each kept frame and put
    // back with it.
    void attach_extra(std::size_t board, std::function<std::shared_ptr<const void>()> keep,
                      std::function<void(const void*)> put_back) {
        parts_[board].keep = std::move(keep), parts_[board].put_back = std::move(put_back);
    }
    // One board a PC: a board on side runs (made by another's module, its main board idle) is no part of its own: its
    // network board is in the staying board's walk; its thread asks rewound_to() as it takes its turns. Made whole
    // again (a shadow), a part from its next frame on (its history from there). Called where that board's thread is
    // (no meeting under way).
    void set_active(std::size_t board, bool on) {
        Part& p = parts_[board];
        if (p.active == on) return;
        p.active = on, p.fresh = on; // (made whole: a part once it has saved a frame)
        p.snaps = std::make_unique<m3::Snapshots>(depth_ + 1);
        if (!std::getenv("ROLLBACK_EVERY_FRAME")) p.snaps->set_sparse(kDense, kEvery);
        p.kept.clear();
        p.pins_seen = 0; // (its new history takes them all again)
    }
    bool active(std::size_t board) const { return parts_[board].active; }
    // (a board on side runs, its turns taken frame by frame: the frame gone back to since last asked, 0 none; its
    // turns then go on from the frame after it)
    uint64_t rewound_to(std::size_t board) { return std::exchange(parts_[board].back_to, 0); }
    // A switch between side runs and whole: `frame` kept as it is on every board (m3::Snapshots::pin), never merged
    // into a later one, so that nothing is gone back to before it (every frame up to it settled first). Any thread:
    // each board takes it at its next save.
    void pin(uint64_t frame) {
        std::lock_guard guard(pins_lock_);
        pins_.push_back(frame);
    }

    // Lazy snapshots (2026-11-05): whether `frame` is to be kept, asked by every board at its snapshot point (the first
    // to ask decides for all: the boards must keep the same frames). Kept while anything is guessed and not settled,
    // or a wrong guess is still to be gone back for, or the next frame's inputs aren't all in (it may be guessed: going
    // back is to the frame before the first wrong guess); and pinned frames. Else not: nothing will go back to it (a
    // game on a network whose inputs come in time keeps none at all). ROLLBACK_EVERY_FRAME_SAVE=1: every frame, as before.
    bool wants(uint64_t frame) {
        std::lock_guard guard(decide_lock_);
        if (const auto it = decided_.find(frame); it != decided_.end()) return it->second;
        static const bool every = std::getenv("ROLLBACK_EVERY_FRAME_SAVE") != nullptr;
        // (and every kAlways-th frame: a guess not foreseen (the boards filled here told late) goes back to the newest
        // kept before it, never far: its inputs still kept, a few frames more run again)
        bool want = every || frame % kAlways == 0 || !table_.settled_but(frame + 1, parts_.size(), local_);
        if (!want) {
            std::lock_guard pins(pins_lock_);
            want = std::find(pins_.begin(), pins_.end(), frame) != pins_.end();
        }
        decided_[frame] = want;
        if (frame > 256) decided_.erase(decided_.begin(), decided_.lower_bound(frame - 256));
        ++(want ? kept_frames_ : skipped_frames_);
        return want;
    }
    // The boards whose inputs this machine fills itself (its own; the empty cabinets when it's the authority): never
    // guessed, so not waited for by wants(). Set as it changes (a player taking a cabinet, one dropped).
    void set_local(std::vector<bool> local) {
        std::lock_guard guard(decide_lock_);
        local_ = std::move(local);
    }

    // Before the boards run (every one stopped where it starts, at the end of `frame`: power-on, or a state loaded):
    // that frame kept too, the first guess's frame before.
    void start(uint64_t frame) {
        for (std::size_t b = 0; b < parts_.size(); ++b)
            if (parts_[b].active) save(b, frame);
        m3::State s;
        ring_->cut_state(s);
        cuts_[frame] = std::move(s.data());
    }
    // A board's snapshot point after frame `frame`.
    void save(std::size_t board, uint64_t frame) {
        Part& p = parts_[board];
        const auto t0 = std::chrono::steady_clock::now();
        {
            std::lock_guard guard(pins_lock_);
            for (; p.pins_seen < pins_.size(); ++p.pins_seen) p.snaps->pin(pins_[p.pins_seen]);
        }
        p.snaps->save(frame, p.walk);
        p.saved = frame, p.fresh = false;
        if (p.keep) { // (its own state too, for the frames kept)
            p.kept[frame] = p.keep();
            for (auto it = p.kept.begin(); it != p.kept.end();) it = p.snaps->has(it->first) ? std::next(it) : p.kept.erase(it);
        }
        if (frame % 60 == 0) // (how much its kept frames hold, now and then: the most so far; each board's on its own thread)
            p.peak_bytes = std::max(p.peak_bytes, p.snaps->kept_bytes()), p.kept_frames = p.snaps->kept();
        p.save_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++p.saves;
    }

    // Where the boards meet, every one stopped: the ring's part of the frame every board has saved (kept, and handed
    // to `cut` when asked: a linked save state of that frame); and back to the frame before a guess found wrong. The
    // frame gone back to (0: none).
    uint64_t between(std::vector<uint8_t>* cut = nullptr) {
        if (!ring_->cut_complete()) return 0;
        const Part* first = nullptr;
        for (const Part& p : parts_)
            if (p.active && !p.fresh && !first) first = &p;
        if (!first) return 0;
        const uint64_t frame = first->saved;
        for (std::size_t b = 0; b < parts_.size(); ++b)
            if (const Part& p = parts_[b]; p.active && !p.fresh && p.saved != frame) {
                if (!mismatched_) // (once a stretch of them: every meeting else)
                    std::fprintf(stderr, "rollback: the boards saved different frames (board %zu %llu, board %zu %llu)\n",
                                 static_cast<std::size_t>(first - parts_.data()), (unsigned long long)frame, b,
                                 (unsigned long long)p.saved);
                mismatched_ = true;
                return 0;
            }
        mismatched_ = false;
        m3::State s;
        ring_->cut_state(s);
        if (cut) *cut = s.data();
        cuts_[frame] = std::move(s.data());
        if (frame > depth_ + 1) cuts_.erase(cuts_.begin(), cuts_.lower_bound(frame - depth_ - 1));
        const uint64_t wrong = table_.wrong_from();
        if (wrong > frame) return 0; // (none, or for a frame not saved yet: at a later meeting)
        // (the frame before it, or the nearest kept before that: kept sparsely further back, the frames between run
        // again too)
        const uint64_t back = first->snaps->kept_at_or_before(wrong - 1);
        if (back == ~0ull || !cuts_.count(back)) {
            std::fprintf(stderr, "rollback: frame %llu guessed wrong, too far back to go (at %llu)\n", (unsigned long long)wrong,
                         (unsigned long long)frame);
            table_.take_wrong();
            return 0;
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (Part& p : parts_) { // (each board's walk marks its ring port saved: the ring's load after clears it)
            if (!p.active || p.fresh) { // (its network board put back in the staying board's)
                p.back_to = back;
                continue;
            }
            p.snaps->restore(back, p.walk);
            if (p.put_back && p.kept.count(back)) p.put_back(p.kept.at(back).get());
            p.kept.erase(p.kept.upper_bound(back), p.kept.end());
            p.rewind = true;
            p.quiet_through = frame;
        }
        m3::State load(std::vector<uint8_t>(cuts_.at(back)));
        ring_->cut_state(load);
        cuts_.erase(cuts_.upper_bound(back), cuts_.end());
        table_.forget_guesses_after(back); // (and the wrong guesses after `back`: one before it, found meanwhile, kept)
        ++backs_;
        frames_again_ += frame - back;
        back_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return back;
    }
    // A board leaving the meeting: true once after going back.
    bool rewind(std::size_t board) { return std::exchange(parts_[board].rewind, false); }
    // A frame being run again (drawn and heard nothing).
    bool quiet(std::size_t board, uint64_t frame) const { return frame <= parts_[board].quiet_through; }

    // How it went: times gone back, frames run again, the average snapshot (a board) and going back (all of them).
    std::string report() const {
        double save = 0;
        uint64_t saves = 0;
        std::size_t peak = 0, kept = 0; // (the boards' peaks added: at most that at once)
        for (const Part& p : parts_) save += p.save_ms, saves += p.saves, peak += p.peak_bytes, kept = std::max(kept, p.kept_frames);
        char buf[400];
        std::snprintf(buf, sizeof buf,
                      "went back %llu times, %llu frames run again; a snapshot %.2f ms, going back %.2f ms; %zu frames kept, at most %.1f MB; "
                      "snapshots taken for %llu frames, %llu not needed",
                      (unsigned long long)backs_, (unsigned long long)frames_again_, saves ? save / static_cast<double>(saves) : 0.0,
                      backs_ ? back_ms_ / static_cast<double>(backs_) : 0.0, kept, static_cast<double>(peak) / 1048576.0,
                      (unsigned long long)kept_frames_, (unsigned long long)skipped_frames_);
        return buf;
    }

private:
    struct Part {
        std::function<void(m3::State&)> walk;
        std::unique_ptr<m3::Snapshots> snaps;
        uint64_t saved = 0, quiet_through = 0, saves = 0;
        bool rewind = false;
        bool active = true, fresh = false;
        uint64_t back_to = 0;
        std::function<std::shared_ptr<const void>()> keep;
        std::function<void(const void*)> put_back;
        std::map<uint64_t, std::shared_ptr<const void>> kept;
        double save_ms = 0;
        std::size_t peak_bytes = 0, kept_frames = 0;
        std::size_t pins_seen = 0;
    };
    uint64_t depth_;
    m3::LinkRing* ring_;
    InputTable& table_;
    std::vector<Part> parts_;
    std::map<uint64_t, std::vector<uint8_t>> cuts_; // the ring's part, by frame
    std::mutex pins_lock_;
    std::vector<uint64_t> pins_; // (pin: every one asked, in order)
    bool mismatched_ = false;    // (between: the boards' saved frames differed at the last meeting)
    std::mutex decide_lock_;     // (wants)
    std::map<uint64_t, bool> decided_;
    std::vector<bool> local_;
    uint64_t kept_frames_ = 0, skipped_frames_ = 0;
    uint64_t backs_ = 0, frames_again_ = 0;
    double back_ms_ = 0;
};

} // namespace tools::rollback
