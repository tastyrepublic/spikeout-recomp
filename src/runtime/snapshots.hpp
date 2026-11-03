// Rollback snapshots of one board, a frame at a time (docs/netplay.md,
// "Rollback"). A board's whole state is 27 MB and saving it as a save state
// takes longer than a frame; but a frame writes only about 400 KB of it. So
// the small state (CPU, devices' registers...) is saved whole each frame,
// and the big buffers (those a state walks with State::fixed) are kept by
// page: a copy of each as at the last snapshot, and for each frame kept the
// pages that frame wrote, as they were before. Going back undoes those pages
// frame by frame, newest first.
//
// The pages a frame wrote come from the system (m3::watched: the buffers are
// WatchedVectors), else from comparing each page with the copy.
#pragma once

#include "runtime/state.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <vector>

namespace m3 {

class Snapshots {
public:
    // `depth`: how many frames back one can go.
    explicit Snapshots(std::size_t depth) : depth_(depth) {}
    // Kept sparser further back (rollback's long window: going far back is rare, and the frames after a kept one
    // can be run again): every frame for the newest `dense`, beyond that only frames that are multiples of `every`
    // (the others merged into the one after: going back past them gives the same); `depth` then counts frames, not
    // snapshots. Before the first save.
    void set_sparse(std::size_t dense, std::size_t every) { dense_ = dense, every_ = every; }
    // And `frame` kept as it is once saved, never merged into the next (rollback: the first frame after a switch
    // between side runs and whole: nothing gone back to before it), until it is out of the window.
    void pin(uint64_t frame) { pinned_.insert(frame); }
    // The newest kept frame at or before `frame` (none: ~0).
    uint64_t kept_at_or_before(uint64_t frame) const;
    std::size_t kept() const { return frames_.size(); }
    // What the kept frames hold (their pages and small states), the copy of the big buffers aside.
    std::size_t kept_bytes() const {
        std::size_t n = 0;
        for (const Frame& f : frames_) {
            n += f.small.size();
            for (const Page& p : f.pages) n += p.before.size();
        }
        return n;
    }

    // The board as it is now, as frame `frame` (later than the last, or the
    // history is started again). `walk` is the board's state function (as
    // for save states: tools::board_state).
    void save(uint64_t frame, const std::function<void(State&)>& walk);
    // The board back as it was at `frame` (kept: has()). The frames after it
    // are forgotten; it is the last one now.
    void restore(uint64_t frame, const std::function<void(State&)>& walk);
    bool has(uint64_t frame) const;
    uint64_t last() const { return frames_.empty() ? 0 : frames_.back().frame; }

    // The last save, as it found the board (one board's backups, tools::backup): its small state, each big buffer as
    // it was then, and the pages it found written since the save before (none after starting again: started()).
    const std::vector<uint8_t>& small() const { return frames_.back().small; }
    std::size_t regions() const { return regions_.size(); }
    const std::vector<uint8_t>& region(std::size_t i) const { return regions_[i].copy; }
    bool started() const { return started_; }
    void pages(const std::function<void(std::size_t region, std::size_t page)>& fn) const {
        for (const Page& p : frames_.back().pages) fn(p.region, p.page);
    }
    static constexpr std::size_t kPageBytes = 4096;

    // For measuring: the last save's small state and pages, the time it took.
    std::size_t small_bytes() const { return frames_.empty() ? 0 : frames_.back().small.size(); }
    std::size_t pages_saved() const { return pages_last_; }
    std::size_t big_bytes() const;
    bool watched() const { return watched_; }
    // Tests: compare pages even where the system lists them (as on systems
    // without write watch), from the next start.
    void compare_only(bool on) { compare_only_ = on; }
    // Pages the board's own loading wrote in its big buffers (restore): should
    // stay 0 (a state's loading only sets what it was given).
    std::size_t load_writes() const { return load_writes_; }

private:
    struct Region {
        uint8_t* data = nullptr;
        std::size_t bytes = 0;
        std::vector<uint8_t> copy; // as at the last snapshot
    };
    struct Page {
        uint32_t region;
        uint32_t page;
        std::vector<uint8_t> before; // as at the snapshot before this frame's
    };
    struct Frame {
        uint64_t frame;
        std::vector<uint8_t> small;
        std::vector<Page> pages; // undoing them gives the frame before
    };

    // The regions the board's state walks now, in order.
    std::vector<std::pair<uint8_t*, std::size_t>> walk_regions(const std::function<void(State&)>& walk, std::vector<uint8_t>& small);
    // Pages of a region written since the last snapshot.
    void written(Region& r, std::vector<std::size_t>& pages);
    static std::size_t page_bytes(const Region& r, std::size_t page);

    // A frame kept no longer: its pages into the next one's (where both have a page, the older contents, its).
    void merge_into_next(std::size_t i);

    std::size_t depth_;
    std::size_t dense_ = 0, every_ = 0; // (set_sparse; 0: every frame kept)
    std::set<uint64_t> pinned_;         // (pin)
    std::vector<Region> regions_;
    std::deque<Frame> frames_;
    std::vector<std::vector<uint8_t>> spare_; // page buffers to use again
    std::size_t pages_last_ = 0, load_writes_ = 0;
    bool watched_ = false, compare_only_ = false, started_ = false;
    std::vector<uint8_t> small_spare_;
    std::vector<std::size_t> pages_; // (a region's written pages, reused)
};

} // namespace m3
