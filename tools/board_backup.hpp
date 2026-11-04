// One board a PC (docs/netplay.md, "A player who drops, one board a PC"): each machine's own board backed up on every
// other, so that a player who goes without a parting (a crash, the network lost) leaves a board the others can still
// run whole. Every kEvery frames a machine takes its board by pages (m3::Snapshots: under a millisecond) and sends
// each other machine the pages written since its last backup (tens of KB), or the whole board (5 MB packed) to one
// that has none of it yet. Each machine keeps, for every other board, its last backup (Copy), the packets sent to
// it since (PacketLog: its network board takes them by number) and its player's inputs (in tools::NetPlay). Its
// player gone, the board is run on here from its backup (Replay: a board of its own, on its own thread, the packets
// and inputs as they were, behind the game until it has caught up) to a frame ahead, where it goes in as a shadow
// from a real board would (Game::lost).
#pragma once

#include "game_setup.hpp"
#include "input_table.hpp"
#include "net/compress.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/netboard.hpp"
#include "runtime/snapshots.hpp"
#include "runtime/state.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace tools::backup {

constexpr uint64_t kEvery = 120; // frames between a board's backups (about 2 seconds; rollback: at the first settled frame from then)

namespace detail {
struct Out {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void bytes(const uint8_t* p, std::size_t n) { b.insert(b.end(), p, p + n); }
};
struct In {
    std::span<const uint8_t> b;
    std::size_t at = 0;
    bool ok = true;
    bool need(std::size_t n) { return ok = ok && n <= b.size() - at; }
    uint8_t u8() { return need(1) ? b[at++] : 0; }
    uint32_t u32() {
        uint32_t v = 0;
        if (need(4))
            for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[at++]) << (8 * i);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        if (need(8))
            for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[at++]) << (8 * i);
        return v;
    }
    const uint8_t* bytes(std::size_t n) {
        if (!need(n)) return nullptr;
        const uint8_t* p = b.data() + at;
        at += n;
        return p;
    }
};
enum : uint8_t { kWhole = 0, kChanges = 1 };
} // namespace detail

// This machine's own board's backups. take() at the snapshot point after a frame (Context::request_snapshot), with
// the board's packets' counts then (LinkRing::taken / sent); then whole() or changes() for each machine to send to.
class Maker {
public:
    // (pages compared with its copy, not listed from the memory's record of writes: that record is read once and
    // starts again, and rollback's snapshots read it every frame; each taking the other's pages, a backup's changes
    // missed most of them (a board from them wrong), and the frames' snapshots at a backup missed its)
    Maker() { snaps_.compare_only(true); }
    void take(uint64_t frame, const std::function<void(m3::State&)>& walk, uint64_t taken, uint64_t sent) {
        base_ = frame_;
        snaps_.save(frame, walk);
        frame_ = frame, taken_ = taken, sent_ = sent;
        if (snaps_.started()) base_ = 0; // (nothing to change from: whole for every machine)
    }
    uint64_t frame() const { return frame_; }
    bool changes_possible() const { return base_ != 0; }
    // (not packed yet: net::compress, best off the game's thread)
    std::vector<uint8_t> whole() const {
        detail::Out o;
        std::size_t bytes = 64 + snaps_.small().size();
        for (std::size_t r = 0; r < snaps_.regions(); ++r) bytes += 8 + snaps_.region(r).size();
        o.b.reserve(bytes);
        header(o, detail::kWhole);
        for (std::size_t r = 0; r < snaps_.regions(); ++r) {
            const std::vector<uint8_t>& region = snaps_.region(r);
            o.u64(region.size());
            o.bytes(region.data(), region.size());
        }
        small(o);
        return std::move(o.b);
    }
    std::vector<uint8_t> changes() const {
        detail::Out o;
        header(o, detail::kChanges);
        uint32_t n = 0;
        snaps_.pages([&](std::size_t, std::size_t) { ++n; });
        o.u32(n);
        snaps_.pages([&](std::size_t region, std::size_t page) {
            const std::vector<uint8_t>& r = snaps_.region(region);
            const std::size_t at = page * m3::Snapshots::kPageBytes;
            o.u32(static_cast<uint32_t>(region)), o.u32(static_cast<uint32_t>(page));
            o.bytes(r.data() + at, std::min(m3::Snapshots::kPageBytes, r.size() - at));
        });
        small(o);
        return std::move(o.b);
    }

private:
    void header(detail::Out& o, uint8_t kind) const {
        o.u8(kind), o.u64(frame_), o.u64(base_), o.u64(taken_), o.u64(sent_);
        o.u32(static_cast<uint32_t>(snaps_.regions()));
    }
    void small(detail::Out& o) const {
        o.u64(snaps_.small().size());
        o.bytes(snaps_.small().data(), snaps_.small().size());
    }
    m3::Snapshots snaps_{0};
    uint64_t frame_ = 0, base_ = 0, taken_ = 0, sent_ = 0;
};

// Another machine's board as its last backup has it: its state at the end of `frame` (the big buffers whole, the
// small rest), the packets it had taken and sent then.
struct Copy {
    uint64_t frame = 0, taken = 0, sent = 0;
    std::vector<std::vector<uint8_t>> regions;
    std::vector<uint8_t> small;
    bool valid() const { return frame != 0; }

    // A backup received (packed). False: changes from a backup this isn't (the whole wanted, Session::want_backup),
    // or not one.
    bool apply(std::span<const uint8_t> packed) {
        const auto unpacked = net::decompress(packed);
        if (!unpacked) return false;
        detail::In in{*unpacked};
        const uint8_t kind = in.u8();
        const uint64_t f = in.u64(), base = in.u64(), t = in.u64(), s = in.u64();
        const uint32_t n = in.u32();
        if (!in.ok) return false;
        if (kind == detail::kWhole) {
            std::vector<std::vector<uint8_t>> rs(n);
            for (auto& r : rs) {
                const uint64_t bytes = in.u64();
                const uint8_t* p = in.bytes(static_cast<std::size_t>(bytes));
                if (!p) return false;
                r.assign(p, p + bytes);
            }
            std::vector<uint8_t> sm;
            if (!read_small(in, sm)) return false;
            regions = std::move(rs), small = std::move(sm);
        } else if (kind == detail::kChanges) {
            if (!valid() || base != frame || n != regions.size()) return false;
            const uint32_t pages = in.u32();
            struct Change {
                uint8_t* to;
                const uint8_t* from;
                std::size_t bytes;
            };
            std::vector<Change> changes; // (read through first: applied whole or not at all)
            for (uint32_t i = 0; i < pages; ++i) {
                const uint32_t region = in.u32(), page = in.u32();
                if (!in.ok || region >= regions.size()) return false;
                const std::size_t at = static_cast<std::size_t>(page) * m3::Snapshots::kPageBytes;
                if (at >= regions[region].size()) return false;
                const std::size_t bytes = std::min(m3::Snapshots::kPageBytes, regions[region].size() - at);
                const uint8_t* p = in.bytes(bytes);
                if (!p) return false;
                changes.push_back({regions[region].data() + at, p, bytes});
            }
            std::vector<uint8_t> sm;
            if (!read_small(in, sm)) return false;
            for (const Change& c : changes) std::memcpy(c.to, c.from, c.bytes);
            small = std::move(sm);
        } else {
            return false;
        }
        frame = f, taken = t, sent = s;
        return true;
    }

private:
    static bool read_small(detail::In& in, std::vector<uint8_t>& out) {
        const uint64_t n = in.u64();
        const uint8_t* p = in.bytes(static_cast<std::size_t>(n));
        if (!p) return false;
        out.assign(p, p + n);
        return in.at == in.b.size();
    }
};

// The packets sent to one board (its link's), by their number, from those it had not taken at its last backup.
class PacketLog {
public:
    struct Packet {
        uint64_t seq = 0, at = 0;
        std::vector<uint8_t> data;
    };
    void add(uint64_t seq, uint64_t at, const std::vector<uint8_t>& data) {
        std::lock_guard guard(lock_);
        if (!packets_.empty() && seq != packets_.back().seq + 1) packets_.clear(); // (numbered again: a load; from here)
        packets_.push_back({seq, at, data});
    }
    void forget_before(uint64_t seq) {
        std::lock_guard guard(lock_);
        while (!packets_.empty() && packets_.front().seq < seq) packets_.pop_front();
    }
    std::optional<Packet> get(uint64_t seq) const {
        std::lock_guard guard(lock_);
        if (packets_.empty() || seq < packets_.front().seq || seq - packets_.front().seq >= packets_.size()) return std::nullopt;
        return packets_[static_cast<std::size_t>(seq - packets_.front().seq)];
    }
    // The first packet kept (none: ~0).
    uint64_t first() const {
        std::lock_guard guard(lock_);
        return packets_.empty() ? ~0ull : packets_.front().seq;
    }

private:
    mutable std::mutex lock_;
    std::deque<Packet> packets_;
};

// What this machine keeps of every other board (network play, tools::NetPlay): its last backup, the packets to it
// since, its player's inputs since.
class Keeper {
public:
    explicit Keeper(std::size_t seats) : copies_(seats), older_(seats), history_(seats) {
        for (std::size_t s = 0; s < seats; ++s) logs_.push_back(std::make_unique<PacketLog>());
    }
    // Seat `seat`'s backup (packed). False: changes from one this hasn't (its whole wanted).
    bool backup(std::size_t seat, std::span<const uint8_t> packed) {
        std::lock_guard guard(lock_);
        if (seat >= copies_.size()) return false;
        Copy before = keep_ > 1 && copies_[seat].valid() ? copies_[seat] : Copy{};
        if (!copies_[seat].apply(packed)) return false;
        if (before.valid()) { // (kept: a spectator runs behind the players, its backups' frames ahead of it)
            older_[seat].push_back(std::move(before));
            while (older_[seat].size() + 1 > keep_) older_[seat].pop_front();
        }
        const Copy& oldest = older_[seat].empty() ? copies_[seat] : older_[seat].front();
        logs_[seat]->forget_before(oldest.taken);
        history_.forget_before(seat, oldest.frame);
        return true;
    }
    // How many of each board's backups are kept (1: the last; a spectator on one board, a few: Session::watch).
    void set_keep(std::size_t n) {
        std::lock_guard guard(lock_);
        keep_ = std::max<std::size_t>(1, n);
    }
    // The newest backup of seat `seat` at or before `frame` (none: not valid).
    Copy copy_for(std::size_t seat, uint64_t frame) const {
        std::lock_guard guard(lock_);
        if (seat >= copies_.size()) return {};
        if (copies_[seat].valid() && copies_[seat].frame <= frame) return copies_[seat];
        for (auto it = older_[seat].rbegin(); it != older_[seat].rend(); ++it)
            if (it->frame <= frame) return *it;
        return {};
    }
    void packet(std::size_t to, uint64_t seq, uint64_t at, const std::vector<uint8_t>& data) {
        if (to < logs_.size()) logs_[to]->add(seq, at, data);
    }
    void input(std::size_t seat, uint64_t frame, const InputTable::Inputs& in) {
        if (seat < copies_.size()) history_.set(seat, frame, in);
    }
    void hold_from(std::size_t seat, uint64_t frame) { // (its player gone: none, as InputTable::hold_from)
        if (seat < copies_.size()) history_.hold_from(seat, frame, {});
    }
    void hold_until(std::size_t seat, uint64_t frame) {
        if (seat < copies_.size()) history_.hold_until(seat, frame);
    }
    std::optional<uint64_t> frame(std::size_t seat) const {
        std::lock_guard guard(lock_);
        if (seat >= copies_.size() || !copies_[seat].valid()) return std::nullopt;
        return copies_[seat].frame;
    }
    Copy copy(std::size_t seat) const {
        std::lock_guard guard(lock_);
        return copies_[seat];
    }
    const PacketLog& log(std::size_t seat) const { return *logs_[seat]; }
    // Its inputs set at the end of `frame`, waited for a few seconds at most.
    std::optional<InputTable::Inputs> inputs(std::size_t seat, uint64_t frame) const {
        for (int k = 0; k < 5000; ++k) {
            if (auto in = history_.get(seat, frame)) return in;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    }

private:
    mutable std::mutex lock_;
    std::vector<Copy> copies_;
    std::vector<std::deque<Copy>> older_; // (the ones before the last, set_keep)
    std::size_t keep_ = 1;
    std::vector<std::unique_ptr<PacketLog>> logs_;
    InputTable history_;
};

// A board's end of the link in a replay: the packets it took then, from the log, each once it reaches the time it
// could arrive; what it sends counted (the others had it already).
class ReplayLink : public m3::NetLink {
public:
    ReplayLink(const PacketLog& log, uint64_t taken) : log_(log), next_(taken) {}
    void send(const uint8_t*, std::size_t) override { ++sent_; }
    std::vector<uint8_t> receive() override {
        if (!pending_) pending_ = log_.get(next_);
        if (!pending_ || pending_->at > now_) return {};
        std::vector<uint8_t> data = std::move(pending_->data);
        pending_.reset();
        ++next_;
        return data;
    }
    bool loopback() const override { return false; }
    void set_time(uint64_t main_cycles) override { now_ = main_cycles; }
    void state(m3::State& s) override { s.tag("ring port"); }
    uint64_t taken() const { return next_; }
    uint64_t sent() const { return sent_; }

private:
    const PacketLog& log_;
    uint64_t next_, sent_ = 0, now_ = 0;
    std::optional<PacketLog::Packet> pending_;
};

// A board run on from its backup, on its own (the caller's thread): its state at the end of `until` with its
// packets' counts then, packed as a shadow's from its own machine (one_board::pack_remote's parts).
struct Replay {
    const m3::RomSet& roms;
    const m3::GameSpec& game;
    m3::CommBoardOptions link;
    std::function<void(m3::Bus&)> configure; // (the frontend's bus settings: midi_burst, dma_timed)
    std::function<void(recomp::Context&)> hooks;
    uint32_t slice = 8;                        // lines between the boards' meetings (the network board catches up)
    const Copy& copy;
    const PacketLog& log;
    // Its inputs set at the end of a frame (waits for them; none: the game is over, stop); and waits until the game
    // here has run a frame (all the packets that frame's end needs are in the log by then; false: stop).
    std::function<std::optional<InputTable::Inputs>(uint64_t frame)> inputs;
    std::function<bool(uint64_t frame)> wait_frame;
    std::function<void(uint64_t frame, m3::Bus& bus)> each_frame; // (tests: at each frame's end, before its inputs)

    struct Result {
        std::vector<uint8_t> state;
        uint64_t taken = 0, sent = 0;
        double seconds = 0;
    };
    std::optional<Result> run(uint64_t until) {
        struct Stop {};
        const auto started = std::chrono::steady_clock::now();
        m3::Bus bus(roms);
        bus.set_security_key(game.security_key);
        if (configure) configure(bus);
        auto port = std::make_unique<ReplayLink>(log, copy.taken);
        ReplayLink& link_port = *port;
        bus.use_netboard(std::make_unique<m3::CommBoard>(std::move(port), link));
        const std::vector<recomp::FunctionEntry> functions = game_functions(), blocks = game_blocks();
        recomp::Context c(bus, functions);
        c.set_blocks(blocks);
        if (hooks) hooks(c);
        recomp::FrameTiming timing;
        c.set_scheduler(&timing);
        const std::function<void(m3::State&)> walk = [&](m3::State& s) { board_state(s, c, timing, {}); };
        // (the backup into it: its big buffers copied in, then the small rest loaded around them, as Snapshots::restore)
        struct Collect : m3::State::Regions {
            std::vector<std::pair<uint8_t*, std::size_t>> found;
            void region(void* data, std::size_t bytes) override { found.emplace_back(static_cast<uint8_t*>(data), bytes); }
        } here;
        {
            m3::State s;
            s.set_regions(&here);
            walk(s);
        }
        if (here.found.size() != copy.regions.size()) throw std::runtime_error("backup: not this board's buffers");
        for (std::size_t r = 0; r < here.found.size(); ++r) {
            if (here.found[r].second != copy.regions[r].size()) throw std::runtime_error("backup: a buffer's size differs");
            std::memcpy(here.found[r].first, copy.regions[r].data(), here.found[r].second);
        }
        {
            Collect check;
            m3::State s(copy.small);
            s.set_regions(&check);
            walk(s);
        }
        timing.on_display_line = [&](uint32_t line) {
            if (line % slice == slice - 1) bus.netboard().sync(c.cycles());
        };
        std::optional<Result> result;
        bool over = false;
        timing.on_frame_end = [&] {
            const uint64_t frame = timing.frames();
            if (each_frame) each_frame(frame, bus);
            const auto in = inputs(frame);
            if (!in) {
                over = true;
                throw Stop{};
            }
            bus.inputs.bank0 = in->bank0, bus.inputs.bank1 = in->bank1;
            bus.inputs.controls = in->controls, bus.inputs.controls2 = in->controls2;
            if (frame == until) {
                c.request_snapshot([&](uint32_t) {
                    m3::State s;
                    walk(s);
                    result = Result{std::move(s.data()), link_port.taken(), copy.sent + link_port.sent(), 0};
                    throw Stop{};
                });
                return;
            }
            if (!wait_frame(frame + 1)) { // (the next frame's packets in the log)
                over = true;
                throw Stop{};
            }
        };
        if (!wait_frame(timing.frames() + 1)) return std::nullopt;
        try {
            c.resume_block(c.cpu.pc);
            c.call(c.cpu.pc);
        } catch (const Stop&) {
        }
        if (over || !result) return std::nullopt;
        result->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return result;
    }
};

} // namespace tools::backup
