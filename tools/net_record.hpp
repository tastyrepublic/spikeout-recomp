// A network game's recording (the owner's, 2026-11-11: a bug seen in a network game run again here, exactly): every
// board's state at a frame (the network F9 save: a linked state file, NAME.state, which gamerun --boards N --parallel
// --load-state runs on from) and, from that frame, every seat's inputs frame by frame as they settle (NAME.inputs).
// Each machine of a network game runs the same boards on the same inputs, so its game is decided by these alone:
// gamerun --net-replay NAME.inputs runs it again, every board, to the frame (docs/netplay.md, "A network game
// recorded"). Written as the game goes, in the data folder's saves/ (the last kKeep kept); no more than the table has.
//
// NAME.inputs: "SPKNETIN", version (u32, 1), the boards (u32), the first frame (u64: the state's), then for each frame
// from there, each board's inputs as set at the end of that frame (tools::InputTable: read for the next frame), 4 bytes
// a board (bank0, bank1, controls, controls2). Little-endian.
#pragma once

#include "recording.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace tools::net_record {

using Inputs = Recording::Inputs;
inline constexpr char kMagic[8] = {'S', 'P', 'K', 'N', 'E', 'T', 'I', 'N'};
inline constexpr std::size_t kKeep = 5; // (recordings kept: the newest)

inline std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

// Written on one thread (the shown board's), frame after frame.
class Writer {
public:
    bool start(const std::string& path, std::size_t boards, uint64_t first) {
        out_.open(u8path(path), std::ios::binary | std::ios::trunc);
        if (!out_) return false;
        path_ = path, boards_ = boards, next_ = first;
        out_.write(kMagic, sizeof kMagic);
        put32(1), put32(static_cast<uint32_t>(boards)), put64(first);
        return static_cast<bool>(out_);
    }
    bool open() const { return out_.is_open(); }
    uint64_t next() const { return next_; } // (the frame to write next)
    const std::string& path() const { return path_; }
    void frame(const std::vector<Inputs>& in) {
        for (std::size_t b = 0; b < boards_; ++b) {
            const Inputs i = b < in.size() ? in[b] : Inputs{};
            const uint8_t bytes[4] = {i.bank0, i.bank1, i.controls, i.controls2};
            out_.write(reinterpret_cast<const char*>(bytes), 4);
        }
        ++next_;
        if (next_ % 60 == 0) out_.flush(); // (a crash loses a second at most)
    }
    void close() {
        if (out_.is_open()) out_.close();
    }
    ~Writer() { close(); }

private:
    void put32(uint32_t v) {
        for (int i = 0; i < 4; ++i) out_.put(static_cast<char>(v >> (8 * i)));
    }
    void put64(uint64_t v) {
        for (int i = 0; i < 8; ++i) out_.put(static_cast<char>(v >> (8 * i)));
    }
    std::ofstream out_;
    std::string path_;
    std::size_t boards_ = 0;
    uint64_t next_ = 0;
};

struct Reader {
    std::size_t boards = 0;
    uint64_t first = 0;
    std::vector<uint8_t> data; // (frame after frame, 4 bytes a board)
    // "" if loaded, else why not
    std::string load(const std::string& path) {
        std::ifstream in(u8path(path), std::ios::binary);
        if (!in) return "can't read " + path;
        std::vector<uint8_t> all((std::istreambuf_iterator<char>(in)), {});
        if (all.size() < 24 || std::memcmp(all.data(), kMagic, 8) != 0) return path + " isn't a network game's recording";
        auto u32 = [&](std::size_t at) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t{all[at + i]} << (8 * i); return v; };
        auto u64 = [&](std::size_t at) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{all[at + i]} << (8 * i); return v; };
        if (u32(8) != 1) return path + ": a recording of another version";
        boards = u32(12), first = u64(16);
        if (boards == 0 || boards > 4) return path + ": " + std::to_string(boards) + " boards?";
        data.assign(all.begin() + 24, all.end());
        data.resize(data.size() / (4 * boards) * (4 * boards)); // (a frame cut short at the end: left out)
        return {};
    }
    uint64_t frames() const { return boards ? data.size() / (4 * boards) : 0; }
    uint64_t end() const { return first + frames(); } // (the first frame not recorded)
    std::optional<Inputs> get(std::size_t board, uint64_t frame) const {
        if (board >= boards || frame < first || frame >= end()) return std::nullopt;
        const std::size_t at = static_cast<std::size_t>((frame - first) * boards + board) * 4;
        return Inputs{data[at], data[at + 1], data[at + 2], data[at + 3]};
    }
};

// The folder's recordings beyond the newest `keep` deleted (each NAME.inputs with its NAME.state)
inline void prune(const std::string& folder, std::size_t keep = kKeep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(u8path(folder), ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("net-game-", 0) == 0 && e.path().extension() == ".inputs") found.push_back(e.path());
    }
    std::sort(found.begin(), found.end()); // (named by the clock: oldest first)
    for (std::size_t i = 0; i + keep < found.size(); ++i) {
        fs::remove(found[i], ec);
        fs::path state = found[i];
        state.replace_extension(".state");
        fs::remove(state, ec);
    }
}

} // namespace tools::net_record
