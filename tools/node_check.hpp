// One board a PC's check (2026-11-06): the packets a machine makes for the other cabinets (side runs, tools::remote)
// against the real ones. Each machine sends its own board's part of each link packet it sends (its node, the first
// 0x900 bytes of a data frame: the sender's record block) to the others (net::Session::send_node); a machine making
// another board's packets compares its made node with that board's real one, packet by packet (the ring numbers each
// link's packets alike on every machine while they are in step), and counts what differs, byte by byte: what the side
// runs don't make as the real cabinet does. Step 1 of the real-block check (docs/netplay.md): it only tells.
//
// Packed: a byte (0: whole, 1: XORed with the sender's node sent before it), that one's number (8 bytes), then
// net::compress of the 0x900 bytes; whole
// every kWholeEvery packets, so a machine that came later (or missed the start) has a base.
#pragma once

#include "net/compress.hpp"
#include "net/session.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <span>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tools {

class NodeCheck {
public:
    static constexpr std::size_t kNodeBytes = 0x900;
    static constexpr uint64_t kWholeEvery = 8, kKept = 600;
    using Node = std::array<uint8_t, kNodeBytes>;

    // The game's boards, this machine's own (its seat), and whether it makes other boards' packets (one board here:
    // compares; else it only sends its own).
    void set(std::size_t boards, std::size_t local) {
        std::lock_guard guard(lock_);
        boards_ = boards, local_ = local;
    }
    void set_making(bool making) {
        std::lock_guard guard(lock_);
        making_ = making;
    }
    void set_session(net::Session* session) { session_ = session; }
    // With rollback: this machine's own nodes sent only once their frame is settled here (every seat's inputs in, none
    // guessed): a packet from a frame run on guesses, gone back over later, isn't the one its board finally sends, and
    // taken as real by another it put that one out of step (2026-11-07). A packet's frame: its arrival on the ring (in
    // cycles) over `frame_cycles`.
    void set_settled(std::function<bool(uint64_t frame)> settled, uint64_t frame_cycles) {
        std::lock_guard guard(lock_);
        settled_ = std::move(settled), frame_cycles_ = frame_cycles;
    }
    // Step 3 (one board here, with rollback): a made packet's real one put in as it leaves, if it has come (patch);
    // one come after its made one left, and different: `run_again(frame)` (the frame that packet left in: rollback
    // goes back there; run again, the real one is in by then). `frame_now`: the staying board's frame now.
    void set_correcting(std::function<void(uint64_t frame)> run_again, std::function<uint64_t()> frame_now) {
        std::lock_guard guard(lock_);
        run_again_ = std::move(run_again), frame_now_ = std::move(frame_now);
    }

    // The ring's on_patch: a packet board `from` sends (number `seq` on its link), before it leaves.
    void patch(std::size_t from, uint64_t seq, std::vector<uint8_t>& data) {
        if (data.size() < kNodeBytes) return;
        std::lock_guard guard(lock_);
        if (!run_again_ || from == local_) return;
        const auto s = real_.find(from);
        if (s == real_.end()) return;
        const auto r = s->second.find(seq);
        if (r == s->second.end()) return;
        if (!std::equal(r->second.begin(), r->second.end(), data.begin())) ++patched_;
        std::copy(r->second.begin(), r->second.end(), data.begin());
    }

    // The ring's on_departure: every packet sent to board `to` (number `seq` on that link, arriving at `at`), as it leaves.
    void departed(std::size_t to, uint64_t seq, uint64_t at, const std::vector<uint8_t>& data) {
        std::unique_lock guard(lock_);
        if (!boards_) return;
        const std::size_t from = (to + boards_ - 1) % boards_;
        (void)at;
        const bool data_frame = data.size() >= kNodeBytes; // (else a small sync frame: its hash alone)
        const Whole whole_packet{static_cast<uint32_t>(data.size()), hash(data)};
        if (from == local_) { // (our own board's: to the others, once settled)
            Outgoing& out = outgoing_[seq]; // (a frame run again: its packet anew)
            out.frame = frame_cycles_ ? at / frame_cycles_ : 0, out.data_frame = data_frame, out.whole = whole_packet;
            if (data_frame) std::copy(data.begin(), data.begin() + kNodeBytes, out.node.begin());
            // (everything up to the newest one whose frame is settled: a frame settled, all before it are; one from
            // before the inputs began is never "settled" itself)
            uint64_t through = 0;
            bool any = !settled_;
            if (settled_)
                for (auto it = outgoing_.rbegin(); it != outgoing_.rend(); ++it)
                    if (settled_(it->second.frame)) {
                        through = it->first, any = true;
                        break;
                    }
            std::vector<std::pair<uint64_t, std::vector<uint8_t>>> sends;
            while (any && !outgoing_.empty() && (!settled_ || outgoing_.begin()->first <= through)) {
                const auto [s, o] = *outgoing_.begin();
                outgoing_.erase(outgoing_.begin());
                sends.emplace_back(s, pack(s, o));
            }
            while (outgoing_.size() > 4096) outgoing_.erase(outgoing_.begin()); // (never settled: none sent)
            guard.unlock();
            if (session_)
                for (auto& [s, p] : sends) session_->send_node(s, std::move(p));
            return;
        }
        if (!making_) return;
        if (frame_now_) made_frame_[from][seq] = frame_now_(); // (the staying board's frame as it leaves)
        // (+2: a cabinet a frame ahead made it from its own next frame's inputs, maybe still guessed here then)
        if (frame_cycles_) made_at_[from][seq] = at / frame_cycles_ + 2, trim(made_at_[from], seq);
        sweep(); // (those waiting for their frames to settle)
        made_whole_[from][seq] = whole_packet;
        compare_whole(from, seq);
        trim(made_whole_[from], seq);
        if (data_frame) {
            Node node;
            std::copy(data.begin(), data.begin() + kNodeBytes, node.begin());
            made_[from][seq] = node;
            compare(from, seq);
            trim(made_[from], seq);
        }
        trim(made_frame_[from], seq);
        go_back(guard);
    }

    // A real node from seat `seat` (Session::Callbacks::node).
    void received(uint8_t seat, uint64_t seq, const std::vector<uint8_t>& packed) {
        if (packed.size() < 21) return;
        uint64_t base_seq = 0, whole_hash = 0;
        uint32_t whole_size = 0;
        for (int k = 0; k < 8; ++k) base_seq |= uint64_t{packed[1 + static_cast<std::size_t>(k)]} << (8 * k);
        for (int k = 0; k < 4; ++k) whole_size |= uint32_t{packed[9 + static_cast<std::size_t>(k)]} << (8 * k);
        for (int k = 0; k < 8; ++k) whole_hash |= uint64_t{packed[13 + static_cast<std::size_t>(k)]} << (8 * k);
        {
            std::unique_lock guard(lock_);
            if (making_) {
                real_whole_[seat][seq] = Whole{whole_size, whole_hash};
                if (settled_) sweep();
                compare_whole(seat, seq);
                trim(real_whole_[seat], seq);
            }
        }
        if (packed[0] == 2) return; // (a sync frame's: its hash alone)
        const auto unpacked = net::decompress(std::span<const uint8_t>(packed.data() + 21, packed.size() - 21));
        if (!unpacked || unpacked->size() != kNodeBytes) return;
        const std::vector<uint8_t>& raw = *unpacked;
        std::unique_lock guard(lock_);
        Base& b = base_[seat];
        Node node;
        if (packed[0] == 0) std::copy(raw.begin(), raw.end(), node.begin());
        else {
            if (!b.have || b.seq != base_seq) return; // (not the one it was XORed with: the next whole one)
            for (std::size_t i = 0; i < kNodeBytes; ++i) node[i] = static_cast<uint8_t>(raw[i] ^ b.node[i]);
        }
        b.node = node, b.seq = seq, b.have = true;
        b.recent[b.n++ % 3] = node; // (the newest three, for the header's lasting bytes: fix_header)
        if (!making_) return;
        real_[seat][seq] = node;
        compare(seat, seq);
        trim(real_[seat], seq);
        go_back(guard);
    }

    // Step 2: a record made for seat `seat` (its block words, the record at byte `rec`), its bytes that each cabinet
    // keeps for itself (the enemies' entries' +4 bytes, 0x74884: +0x3F02, +0x3F22, +0x34A0, +0x34FC of each, which a
    // cabinet changes on its own player's hits) and any byte seen to differ, taken from its real records: where the
    // newest three it sent all have the same value (a lasting one; one changing each count is left as made). Off with
    // NODE_FIX=1 (tests; off by default since the made records are exact by themselves, 2026-11-07: on, it made them
    // worse).
    void fix_record(std::size_t seat, std::array<uint32_t, 0x240>& next, uint32_t rec) {
        static const bool off = [] {
            const char* v = std::getenv("NODE_FIX");
            return !(v && v[0] == '1');
        }();
        if (off) return;
        std::lock_guard guard(lock_);
        static int traced = 0; // (tests: NODE_FIX_TRACE=N: the first N records it sees, what it took and why)
        static const int trace = std::getenv("NODE_FIX_TRACE") ? std::atoi(std::getenv("NODE_FIX_TRACE")) : 0;
        const bool say = traced < trace;
        const auto b = base_.find(seat);
        if (b == base_.end() || !b->second.have) {
            if (say) ++traced, std::printf("fix: seat %zu record at +%03x: no real node yet\n", seat, rec);
            return;
        }
        const Node& node = b->second.node;
        // (its four records, newest first, by the count at +0xDC; the node's words are byte-reversed)
        auto word = [&](std::size_t at) {
            return uint32_t(node[at + 3]) << 24 | uint32_t(node[at + 2]) << 16 | uint32_t(node[at + 1]) << 8 | node[at];
        };
        std::array<std::pair<uint32_t, std::size_t>, 4> slots;
        for (std::size_t s = 0; s < 4; ++s) slots[s] = {word(0x100 + 0x200 * s + 0xDC), 0x100 + 0x200 * s};
        std::sort(slots.rbegin(), slots.rend());
        auto byte_of = [&](std::size_t slot_at, uint32_t x) { return node[slot_at + (x / 4) * 4 + (3 - x % 4)]; };
        if (say) { // (enemy 2's +7, the one seen wrong: what the real records say, and the made one)
            ++traced;
            const uint32_t x = 0x14 + 12 * 5 + 7, at = rec + x;
            std::printf("fix: seat %zu record at +%03x (count %u): real node %llu, its counts %u %u %u %u; enemy 2's +7 real %02x %02x %02x, "
                        "made %02x, its flags %08x\n",
                        seat, rec, next[(rec + 0xDC) / 4], (unsigned long long)b->second.seq, slots[0].first, slots[1].first,
                        slots[2].first, slots[3].first, byte_of(slots[0].second, x), byte_of(slots[1].second, x),
                        byte_of(slots[2].second, x), static_cast<uint8_t>(next[at / 4] >> (8 * (3 - at % 4))),
                        next[(rec + 0x14 + 12 * 5) / 4]);
        }
        // (the enemies' entries whole, as the real cabinet has them: each cabinet keeps its own of whether one is there
        // (one comes and goes at a slightly different moment on each) and of its +4 bytes. All zero in the newest three:
        // zero; else its flags word and +4 bytes where those three agree; its +8 sum (moving) left as made)
        auto word_of = [&](std::size_t slot_at, uint32_t x) {
            return uint32_t(byte_of(slot_at, x)) << 24 | uint32_t(byte_of(slot_at, x + 1)) << 16 | uint32_t(byte_of(slot_at, x + 2)) << 8 |
                   byte_of(slot_at, x + 3);
        };
        for (uint32_t k = 4; k < 12; ++k) {
            const uint32_t e = 0x14 + 12 * k;
            bool gone = true, same = true;
            for (int s = 0; s < 3; ++s)
                for (uint32_t i = 0; i < 12; i += 4) gone = gone && word_of(slots[static_cast<std::size_t>(s)].second, e + i) == 0;
            for (int s = 1; s < 3; ++s)
                for (uint32_t i = 0; i < 8; i += 4)
                    same = same && word_of(slots[static_cast<std::size_t>(s)].second, e + i) == word_of(slots[0].second, e + i);
            auto put = [&](uint32_t i, uint32_t v) {
                uint32_t& w = next[(rec + e + i) / 4];
                if (w != v) ++fixed_;
                w = v;
            };
            if (gone) put(0, 0), put(4, 0), put(8, 0);
            else if (same) put(0, word_of(slots[0].second, e)), put(4, word_of(slots[0].second, e + 4));
        }
        for (uint32_t x = 0; x < 0x200; ++x) {
            if (!learned_[x]) continue;
            if (x >= 0x14 + 12 * 4 && x < 0x14 + 12 * 12) continue; // (the enemies': above)
            // (a character's entry: only while it's there (its flags word not zero). A gone one's entry is all zero, as
            // the real cabinet sends it; its bytes from older records, where it still was, made one the real never sent)
            if (x >= 0x14 && x < 0x14 + 12 * 12 && next[(rec + 0x14 + 12 * ((x - 0x14) / 12)) / 4] == 0) continue;
            const uint8_t v = byte_of(slots[0].second, x);
            if (byte_of(slots[1].second, x) != v || byte_of(slots[2].second, x) != v) continue;
            const uint32_t at = rec + x, sh = 8 * (3 - at % 4);
            uint32_t& w = next[at / 4];
            if (static_cast<uint8_t>(w >> sh) != v) ++fixed_;
            w = (w & ~(0xFFu << sh)) | uint32_t{v} << sh;
        }
    }

    // And the block's header (its first 0x100 bytes: the link writes it into the network board from its variables):
    // the bytes seen to differ there, where the newest three real nodes agree, put into the made one as it goes
    // (`put(offset in the node, byte)`).
    template <class Put> void fix_header(std::size_t seat, Put&& put) {
        static const bool off = [] {
            const char* v = std::getenv("NODE_FIX");
            return !(v && v[0] == '1');
        }();
        if (off) return;
        std::lock_guard guard(lock_);
        const auto b = base_.find(seat);
        if (b == base_.end() || b->second.n < 3) return;
        const auto& r = b->second.recent;
        for (std::size_t p = 0; p < 0x100; ++p)
            if (learned_header_[p] && r[0][p] == r[1][p] && r[1][p] == r[2][p]) {
                put(p, r[0][p]);
                ++fixed_;
            }
    }

    struct Totals {
        uint64_t compared = 0, differed = 0;
    };
    Totals totals() const {
        std::lock_guard guard(lock_);
        return totals_;
    }
    // The bytes that differed most often (offset in the node: count), for the report.
    std::string report(std::size_t most = 12) const {
        std::lock_guard guard(lock_);
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "%llu packets compared, %llu differed; %llu bytes of made records taken from real ones; %llu made packets "
                      "sent as their real ones, %llu times gone back for one",
                      (unsigned long long)totals_.compared, (unsigned long long)totals_.differed, (unsigned long long)fixed_,
                      (unsigned long long)patched_, (unsigned long long)backs_);
        std::string out = buf;
        std::snprintf(buf, sizeof buf, "; whole packets: %llu compared, %llu differed", (unsigned long long)whole_compared_,
                      (unsigned long long)whole_differed_);
        out += buf;
        std::vector<std::pair<uint64_t, std::size_t>> by;
        for (std::size_t i = 0; i < kNodeBytes; ++i)
            if (counts_[i]) by.emplace_back(counts_[i], i);
        std::sort(by.rbegin(), by.rend());
        if (!by.empty()) out += "; most often:";
        for (std::size_t k = 0; k < by.size() && k < most; ++k) {
            std::snprintf(buf, sizeof buf, " +%03zx (%llu)", by[k].second, (unsigned long long)by[k].first);
            out += buf;
        }
        return out;
    }
    // (tests: NODE_CHECK_LOG=N prints the first N differing packets, bytes and all; else the first 3 of each kind, 20 with
    // detailed logging (SPIKEOUT_NET_DEBUG): 40 such lines filled a player's log, issues #8 and #9; their count is in
    // report(), printed as the game ends)
    static uint64_t log_limit() {
        static const uint64_t n = [] {
            const char* v = std::getenv("NODE_CHECK_LOG");
            return v ? std::strtoull(v, nullptr, 0) : std::getenv("SPIKEOUT_NET_DEBUG") ? uint64_t{20} : uint64_t{3};
        }();
        return n;
    }

private:
    struct Whole { // (a whole packet, data frame or sync frame: its size and a hash of every byte)
        uint32_t size = 0;
        uint64_t hash = 0;
        bool operator==(const Whole&) const = default;
    };
    static uint64_t hash(const std::vector<uint8_t>& data) {
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint8_t v : data) h = (h ^ v) * 0x100000001B3ull;
        return h;
    }
    // (with rollback: a made packet compared only once its frame is settled here; one from a run on guesses is sent
    // again when the frame is run again. The lock held)
    bool waits(std::size_t seat, uint64_t seq) {
        if (!settled_) return false;
        const auto f = made_at_[seat].find(seq);
        if (f == made_at_[seat].end() || settled_(f->second)) return false;
        waiting_[seat].insert(seq);
        while (waiting_[seat].size() > kKept) waiting_[seat].erase(waiting_[seat].begin());
        return true;
    }
    void sweep() { // (the lock held)
        if (!settled_) return;
        for (auto& [seat, seqs] : waiting_)
            for (auto it = seqs.begin(); it != seqs.end();) {
                const auto f = made_at_[seat].find(*it);
                if (f != made_at_[seat].end() && !settled_(f->second)) break; // (in order: the rest are later)
                const uint64_t seq = *it;
                it = seqs.erase(it);
                compare_whole(seat, seq);
                compare(seat, seq);
            }
    }
    void compare_whole(std::size_t seat, uint64_t seq) { // (the lock held)
        if (waits(seat, seq)) return;
        const auto m = made_whole_[seat].find(seq);
        const auto r = real_whole_[seat].find(seq);
        if (m == made_whole_[seat].end() || r == real_whole_[seat].end()) return;
        ++whole_compared_;
        if (!(m->second == r->second) && ++whole_differed_ <= log_limit())
            std::printf("node check: seat %zu packet %llu: the whole packet differs (%u bytes real, %u made)\n", seat + 1,
                        (unsigned long long)seq, r->second.size, m->second.size);
        made_whole_[seat].erase(m);
    }
    struct Base {
        Node node{};
        uint64_t seq = 0;
        bool have = false;
        std::array<Node, 3> recent{};
        uint64_t n = 0;
    };
    void compare(std::size_t seat, uint64_t seq) { // (the lock held)
        if (waits(seat, seq)) return;
        const auto m = made_[seat].find(seq);
        const auto r = real_[seat].find(seq);
        if (m == made_[seat].end() || r == real_[seat].end()) return;
        ++totals_.compared;
        std::string bytes;
        std::size_t n = 0;
        for (std::size_t i = 0; i < kNodeBytes; ++i)
            if (m->second[i] != r->second[i]) {
                ++counts_[i];
                if (i >= 0x100) { // (a record's byte: learned, as a big-endian offset in it)
                    const std::size_t x = (i - 0x100) % 0x200;
                    learned_[(x / 4) * 4 + (3 - x % 4)] = true;
                } else
                    learned_header_[i] = true; // (the header's: as the node has it)
                if (++n <= 12) {
                    char b[32];
                    std::snprintf(b, sizeof b, " +%03zx %02x/%02x", i, r->second[i], m->second[i]);
                    bytes += b;
                }
            }
        if (n && ++totals_.differed <= log_limit()) {
            const auto at = made_at_[seat].find(seq); // (the frame it left in, +2)
            std::printf("node check: seat %zu packet %llu (frame %lld): %zu bytes differ (real/made):%s\n", seat + 1,
                        (unsigned long long)seq, at == made_at_[seat].end() ? -1ll : static_cast<long long>(at->second) - 2, n,
                        bytes.c_str());
        }
        if (n && run_again_) { // (step 3: back to the frame it left in; the real one kept, put in as it's sent again)
            const auto f = made_frame_[seat].find(seq);
            if (f != made_frame_[seat].end()) back_to_ = std::min(back_to_, f->second ? f->second - 1 : 0); // (a frame more: to be sure)
        }
        made_[seat].erase(m); // (the real one kept: patch puts it in when this packet is sent again)
    }
    // (the lock held: a going back asked, called with it let go)
    void go_back(std::unique_lock<std::mutex>& guard) {
        if (back_to_ == ~0ull) return;
        const uint64_t frame = std::exchange(back_to_, ~0ull);
        ++backs_;
        const auto run_again = run_again_;
        static const bool say = std::getenv("NODE_CHECK_DEBUG") != nullptr; // (tests: each going back asked)
        if (say) std::printf("node check: back to frame %llu asked (the board at %llu)\n", (unsigned long long)frame,
                             frame_now_ ? (unsigned long long)frame_now_() : 0ull);
        guard.unlock();
        if (run_again) run_again(frame);
    }
    template <class T> static void trim(std::map<uint64_t, T>& kept, uint64_t seq) {
        while (!kept.empty() && kept.begin()->first + kKept < seq) kept.erase(kept.begin());
    }

    mutable std::mutex lock_;
    net::Session* session_ = nullptr;
    std::size_t boards_ = 0, local_ = 0;
    bool making_ = false;
    Node last_sent_{};
    uint64_t last_sent_seq_ = 0, since_whole_ = 0;
    bool sent_any_ = false;
    struct Outgoing { // (one of our own packets, waiting for its frame to settle)
        uint64_t frame = 0;
        bool data_frame = false;
        Node node{};
        Whole whole;
    };
    std::map<uint64_t, Outgoing> outgoing_;
    std::function<bool(uint64_t)> settled_;
    uint64_t frame_cycles_ = 0;
    std::map<std::size_t, std::map<uint64_t, uint64_t>> made_at_; // (seat: packet number, the frame it arrives in)
    std::map<std::size_t, std::set<uint64_t>> waiting_;           // (seat: packets compared once their frames settle)
    // (its datagram: [kind 0 whole / 1 XOR with the one sent before / 2 hash only][that one's number][size][hash][node])
    std::vector<uint8_t> pack(uint64_t seq, const Outgoing& out) { // (the lock held)
        std::vector<uint8_t> head(21), packed;
        uint8_t kind = 2;
        if (out.data_frame) {
            std::vector<uint8_t> raw(kNodeBytes);
            const bool whole = !sent_any_ || ++since_whole_ >= kWholeEvery; // (else XORed with the one sent before, named)
            if (whole) since_whole_ = 0;
            for (std::size_t i = 0; i < kNodeBytes; ++i) raw[i] = whole ? out.node[i] : static_cast<uint8_t>(out.node[i] ^ last_sent_[i]);
            const uint64_t base_seq = last_sent_seq_;
            last_sent_ = out.node, last_sent_seq_ = seq, sent_any_ = true;
            packed = net::compress(raw);
            kind = whole ? 0 : 1;
            for (int k = 0; k < 8; ++k) head[1 + k] = static_cast<uint8_t>(base_seq >> (8 * k));
        }
        head[0] = kind;
        for (int k = 0; k < 4; ++k) head[9 + k] = static_cast<uint8_t>(out.whole.size >> (8 * k));
        for (int k = 0; k < 8; ++k) head[13 + k] = static_cast<uint8_t>(out.whole.hash >> (8 * k));
        packed.insert(packed.begin(), head.begin(), head.end());
        return packed;
    }
    std::map<std::size_t, Base> base_;
    std::map<std::size_t, std::map<uint64_t, Node>> made_, real_;
    std::map<std::size_t, std::map<uint64_t, uint64_t>> made_frame_; // (seat: packet number, the frame it left in)
    std::map<std::size_t, std::map<uint64_t, Whole>> made_whole_, real_whole_;
    uint64_t whole_compared_ = 0, whole_differed_ = 0;
    std::function<void(uint64_t)> run_again_;
    std::function<uint64_t()> frame_now_;
    uint64_t back_to_ = ~0ull, backs_ = 0, patched_ = 0;
    std::array<uint64_t, kNodeBytes> counts_{};
    Totals totals_;
    // (step 2: the bytes of a record taken from the real ones when lasting; keep_: those known to be each cabinet's own,
    // learned_: those seen to differ (big-endian byte offsets in a record, 0x200 bytes))
    std::array<bool, 0x200> keep_ = [] {
        std::array<bool, 0x200> k{};
        for (uint32_t c = 4; c < 12; ++c)
            for (uint32_t i = 4; i < 8; ++i) k[0x14 + 12 * c + i] = true;
        return k;
    }();
    std::array<bool, 0x200> learned_{};
    std::array<bool, 0x100> learned_header_{};
    uint64_t fixed_ = 0;
};

} // namespace tools
