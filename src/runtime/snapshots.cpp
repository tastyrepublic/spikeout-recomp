#include "runtime/snapshots.hpp"

#include "runtime/watched_memory.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace m3 {

namespace {
constexpr std::size_t kPage = watched::kPage;

// The big buffers a state walks, in order.
struct Collect : State::Regions {
    std::vector<std::pair<uint8_t*, std::size_t>> found;
    void region(void* data, std::size_t bytes) override { found.emplace_back(static_cast<uint8_t*>(data), bytes); }
};
} // namespace

std::size_t Snapshots::page_bytes(const Region& r, std::size_t page) {
    return std::min(kPage, r.bytes - page * kPage);
}

std::size_t Snapshots::big_bytes() const {
    std::size_t n = 0;
    for (const Region& r : regions_) n += r.bytes;
    return n;
}

std::vector<std::pair<uint8_t*, std::size_t>> Snapshots::walk_regions(const std::function<void(State&)>& walk,
                                                                       std::vector<uint8_t>& small) {
    Collect collect;
    State s;
    s.reuse(std::move(small_spare_));
    s.set_regions(&collect);
    walk(s);
    small = std::move(s.data());
    return std::move(collect.found);
}

void Snapshots::written(Region& r, std::vector<std::size_t>& pages) {
    if (watched_ && watched::take_written(r.data, r.bytes, pages)) return;
    pages.clear(); // (not watched: each page that differs from the copy)
    for (std::size_t p = 0, n = (r.bytes + kPage - 1) / kPage; p < n; ++p)
        if (std::memcmp(r.data + p * kPage, r.copy.data() + p * kPage, page_bytes(r, p)) != 0) pages.push_back(p);
}

void Snapshots::save(uint64_t frame, const std::function<void(State&)>& walk) {
    Frame f{frame, {}, {}};
    const auto found = walk_regions(walk, f.small);
    bool same = !frames_.empty() && frame > frames_.back().frame && found.size() == regions_.size();
    for (std::size_t i = 0; same && i < found.size(); ++i)
        same = found[i].first == regions_[i].data && found[i].second == regions_[i].bytes;
    if (!same) { // (the first, or the buffers moved: start again, a copy of each)
        frames_.clear();
        regions_.clear();
        watched_ = watched::available() && !compare_only_;
        pages_last_ = 0;
        for (const auto& [data, bytes] : found) {
            regions_.push_back({data, bytes, std::vector<uint8_t>(data, data + bytes)});
            watched::forget_written(data, bytes);
            pages_last_ += (bytes + kPage - 1) / kPage;
        }
        frames_.push_back(std::move(f));
        return;
    }
    pages_last_ = 0;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        Region& r = regions_[i];
        written(r, pages_);
        for (std::size_t p : pages_) {
            const std::size_t at = p * kPage, n = page_bytes(r, p);
            std::vector<uint8_t> before;
            if (!spare_.empty()) {
                before = std::move(spare_.back());
                spare_.pop_back();
            }
            before.assign(r.copy.begin() + static_cast<std::ptrdiff_t>(at), r.copy.begin() + static_cast<std::ptrdiff_t>(at + n));
            std::memcpy(r.copy.data() + at, r.data + at, n);
            f.pages.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(p), std::move(before)});
        }
        pages_last_ += pages_.size();
    }
    frames_.push_back(std::move(f));
    while (frames_.size() > depth_ + 1) { // (the oldest's pages go back to a frame no longer kept)
        for (Page& pg : frames_.front().pages) spare_.push_back(std::move(pg.before));
        small_spare_ = std::move(frames_.front().small);
        frames_.pop_front();
    }
}

bool Snapshots::has(uint64_t frame) const {
    return std::any_of(frames_.begin(), frames_.end(), [&](const Frame& f) { return f.frame == frame; });
}

void Snapshots::restore(uint64_t frame, const std::function<void(State&)>& walk) {
    if (!has(frame)) throw std::runtime_error("snapshots: frame " + std::to_string(frame) + " isn't kept");
    // Back to the last snapshot: the pages written since.
    for (Region& r : regions_) {
        written(r, pages_);
        for (std::size_t p : pages_) std::memcpy(r.data + p * kPage, r.copy.data() + p * kPage, page_bytes(r, p));
    }
    // Then each frame after `frame` undone, the newest first.
    while (frames_.back().frame != frame) {
        for (Page& pg : frames_.back().pages) {
            Region& r = regions_[pg.region];
            const std::size_t at = static_cast<std::size_t>(pg.page) * kPage;
            std::memcpy(r.data + at, pg.before.data(), pg.before.size());
            std::memcpy(r.copy.data() + at, pg.before.data(), pg.before.size());
            spare_.push_back(std::move(pg.before));
        }
        frames_.pop_back();
    }
    for (Region& r : regions_) watched::forget_written(r.data, r.bytes);
    // The small state (the big buffers' bytes are in place already: as a
    // state loads them, in the walk, before what follows them).
    Collect check;
    State s(frames_.back().small);
    s.set_regions(&check);
    walk(s);
    if (!s.done()) throw std::runtime_error("snapshots: the small state is longer than the board's");
    bool same = check.found.size() == regions_.size();
    for (std::size_t i = 0; same && i < regions_.size(); ++i)
        same = check.found[i].first == regions_[i].data && check.found[i].second == regions_[i].bytes;
    if (!same) throw std::runtime_error("snapshots: the board's buffers moved");
    // Its loading should have left them alone; if not, what it wrote stays
    // (counted), the copy kept equal to the memory.
    for (Region& r : regions_) {
        written(r, pages_);
        for (std::size_t p : pages_)
            if (std::memcmp(r.data + p * kPage, r.copy.data() + p * kPage, page_bytes(r, p)) != 0) {
                std::memcpy(r.copy.data() + p * kPage, r.data + p * kPage, page_bytes(r, p));
                ++load_writes_;
            }
    }
}

} // namespace m3
