// A run played from a save state, to play again exactly (spikebot
// --record-inputs writes it, gamerun --inputs plays it): for each frame, the
// I/O board's inputs during it, the RAM writes made after it ended (the bot's
// --weak-enemies), and now and then main RAM's hash at its end (before those
// writes), to tell that the replay is on the same course.
//
// The file: "SPKINP1\0", then little-endian entries: frame (u64), bank0,
// controls, hash (u64, 0: none), the writes' count (u32), each write's
// address (u32) and 16-bit value (u16).
#pragma once

#include "recording.hpp"

#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tools {

struct InputLog {
    static constexpr char kMagic[8] = {'S', 'P', 'K', 'I', 'N', 'P', '1', '\0'};
    static constexpr uint64_t kHashEvery = 60;

    struct Frame {
        uint8_t bank0 = 0xFF, controls = 0xFF;
        uint64_t hash = 0;
        std::vector<std::pair<uint32_t, uint16_t>> writes;
    };
    std::map<uint64_t, Frame> frames; // by frame number

    // Going back to a state saved after frame `frame`, when it had `writes`
    // writes: what came after it is forgotten.
    void forget_after(uint64_t frame, std::size_t writes) {
        frames.erase(frames.upper_bound(frame), frames.end());
        if (auto it = frames.find(frame); it != frames.end() && it->second.writes.size() > writes) it->second.writes.resize(writes);
    }

    bool save(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fwrite(kMagic, 1, 8, f);
        for (const auto& [frame, e] : frames) {
            const uint32_t n = static_cast<uint32_t>(e.writes.size());
            std::fwrite(&frame, 8, 1, f);
            std::fwrite(&e.bank0, 1, 1, f);
            std::fwrite(&e.controls, 1, 1, f);
            std::fwrite(&e.hash, 8, 1, f);
            std::fwrite(&n, 4, 1, f);
            for (const auto& [addr, value] : e.writes) {
                std::fwrite(&addr, 4, 1, f);
                std::fwrite(&value, 2, 1, f);
            }
        }
        return std::fclose(f) == 0;
    }
    bool load(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char magic[8] = {};
        bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kMagic, 8) == 0;
        uint64_t frame = 0;
        while (ok && std::fread(&frame, 8, 1, f) == 1) {
            Frame e;
            uint32_t n = 0;
            ok = std::fread(&e.bank0, 1, 1, f) == 1 && std::fread(&e.controls, 1, 1, f) == 1 &&
                 std::fread(&e.hash, 8, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1;
            for (uint32_t k = 0; ok && k < n; ++k) {
                uint32_t addr = 0;
                uint16_t value = 0;
                ok = std::fread(&addr, 4, 1, f) == 1 && std::fread(&value, 2, 1, f) == 1;
                e.writes.emplace_back(addr, value);
            }
            if (ok) frames[frame] = std::move(e);
        }
        std::fclose(f);
        return ok;
    }
};

// Linked boards' run (spikebot --boards N --record-inputs, gamerun --boards N
// --parallel --inputs): the boards stop for the bot at a meeting of their
// slices, or where a state put them back (each board at its own point after a
// frame's end), not at frame ends, so each change is kept with where it took
// effect: a board's frame end (the next frame's inputs, and main RAM's hash
// now and then), a meeting (board 0's frame count and line there: every
// board's are the same) or a board's point after a frame's end (the first
// block it can resume at, after a state was loaded). In the order made; going
// back to a state forgets what came after it.
//
// The file: "SPKLNK1\0", then little-endian entries: where (u8: 0 frame end,
// 1 resumed, 2 meeting), board (u8), frame (u64), line (u32), inputs set
// (u8), bank0, controls, hash (u64, 0: none), the writes' count (u32), each
// write's address (u32) and 16-bit value (u16).
struct LinkedInputLog {
    static constexpr char kMagic[8] = {'S', 'P', 'K', 'L', 'N', 'K', '1', '\0'};
    static constexpr uint64_t kHashEvery = 60;

    enum class At : uint8_t { FrameEnd, Resumed, Meeting };
    struct Where {
        At at = At::Resumed;
        uint8_t board = 0;
        uint64_t frame = 0;
        uint32_t line = 0; // (a meeting's)
        auto operator<=>(const Where&) const = default;
    };
    struct Change {
        bool inputs = false;
        uint8_t bank0 = 0xFF, controls = 0xFF;
        uint64_t hash = 0;
        std::vector<std::pair<uint32_t, uint16_t>> writes;
    };
    std::vector<std::pair<Where, Change>> changes;

    // By where they took effect (the later inputs kept, the writes in order).
    std::map<Where, Change> by_where() const {
        std::map<Where, Change> out;
        for (const auto& [where, c] : changes) {
            Change& o = out[where];
            if (c.inputs) o.inputs = true, o.bank0 = c.bank0, o.controls = c.controls;
            if (c.hash) o.hash = c.hash;
            o.writes.insert(o.writes.end(), c.writes.begin(), c.writes.end());
        }
        return out;
    }

    bool save(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fwrite(kMagic, 1, 8, f);
        for (const auto& [where, c] : changes) {
            const uint8_t head[4] = {static_cast<uint8_t>(where.at), where.board, static_cast<uint8_t>(c.inputs), 0};
            const uint32_t n = static_cast<uint32_t>(c.writes.size());
            std::fwrite(&head[0], 1, 2, f);
            std::fwrite(&where.frame, 8, 1, f);
            std::fwrite(&where.line, 4, 1, f);
            std::fwrite(&head[2], 1, 1, f);
            std::fwrite(&c.bank0, 1, 1, f);
            std::fwrite(&c.controls, 1, 1, f);
            std::fwrite(&c.hash, 8, 1, f);
            std::fwrite(&n, 4, 1, f);
            for (const auto& [addr, value] : c.writes) {
                std::fwrite(&addr, 4, 1, f);
                std::fwrite(&value, 2, 1, f);
            }
        }
        return std::fclose(f) == 0;
    }
    bool load(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char magic[8] = {};
        bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kMagic, 8) == 0;
        uint8_t head[2];
        while (ok && std::fread(head, 1, 2, f) == 2) {
            Where where{static_cast<At>(head[0]), head[1]};
            Change c;
            uint8_t inputs = 0;
            uint32_t n = 0;
            ok = std::fread(&where.frame, 8, 1, f) == 1 && std::fread(&where.line, 4, 1, f) == 1 && std::fread(&inputs, 1, 1, f) == 1 &&
                 std::fread(&c.bank0, 1, 1, f) == 1 && std::fread(&c.controls, 1, 1, f) == 1 && std::fread(&c.hash, 8, 1, f) == 1 &&
                 std::fread(&n, 4, 1, f) == 1;
            c.inputs = inputs != 0;
            for (uint32_t k = 0; ok && k < n; ++k) {
                uint32_t addr = 0;
                uint16_t value = 0;
                ok = std::fread(&addr, 4, 1, f) == 1 && std::fread(&value, 2, 1, f) == 1;
                c.writes.emplace_back(addr, value);
            }
            if (ok) changes.emplace_back(where, std::move(c));
        }
        std::fclose(f);
        return ok;
    }
};

} // namespace tools
