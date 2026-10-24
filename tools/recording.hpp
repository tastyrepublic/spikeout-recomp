// Recordings: a cabinet's run from power-on, its inputs frame by frame, to
// play again exactly (gamerun --replay). The game is deterministic: from the
// same settings EEPROM, backup RAM and clock, the same inputs on the same
// frames give the same run. Every kHashEvery frames the recording also keeps
// a hash of main RAM, so a replay can tell it's still on the same course and
// where it parts if not.
//
// The file: "SPKREC1\0", the clock's start (seconds, as Rtc72421::set_time),
// the EEPROM (64 words), the backup RAM's size and bytes; then entries,
// little-endian: 'I', frame (u32), bank0, bank1, controls, controls2 (the
// inputs set at the end of that frame, for the next); 'H', frame (u32), hash
// (u64) of main RAM at the end of that frame.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace tools {

struct Recording {
    static constexpr char kMagic[8] = {'S', 'P', 'K', 'R', 'E', 'C', '1', '\0'};
    static constexpr uint64_t kHashEvery = 60;

    struct Inputs {
        uint8_t bank0 = 0xFF, bank1 = 0xFF, controls = 0xFF, controls2 = 0xFF;
    };
    int64_t clock = 0;
    std::array<uint16_t, 64> eeprom{};
    std::vector<uint8_t> backup;
    std::vector<std::pair<uint32_t, Inputs>> inputs; // (frame, the inputs set at its end)
    std::vector<std::pair<uint32_t, uint64_t>> hashes; // (frame, main RAM's hash at its end)

    // Main RAM's hash: FNV-1a a 64-bit word at a time.
    static uint64_t ram_hash(std::span<const uint8_t> ram) {
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i + 8 <= ram.size(); i += 8) {
            uint64_t w;
            std::memcpy(&w, ram.data() + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
        }
        return h;
    }

    bool load(const std::string& path, std::string& error) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            error = "can't open " + path;
            return false;
        }
        auto get = [&](void* p, size_t n) { return std::fread(p, 1, n, f) == n; };
        char magic[8] = {};
        uint32_t backup_size = 0;
        bool ok = get(magic, 8) && std::memcmp(magic, kMagic, 8) == 0 && get(&clock, 8) && get(eeprom.data(), 128) &&
                  get(&backup_size, 4) && backup_size <= (1u << 24);
        if (ok) {
            backup.resize(backup_size);
            ok = get(backup.data(), backup_size);
        }
        while (ok) {
            int type = std::fgetc(f);
            if (type == EOF) break;
            uint32_t frame = 0;
            if (!get(&frame, 4)) break;
            if (type == 'I') {
                uint8_t b[4];
                if (!get(b, 4)) break;
                inputs.push_back({frame, {b[0], b[1], b[2], b[3]}});
            } else if (type == 'H') {
                uint64_t h = 0;
                if (!get(&h, 8)) break;
                hashes.push_back({frame, h});
            } else {
                ok = false;
            }
        }
        std::fclose(f);
        if (!ok) error = path + " isn't a recording (or is damaged)";
        return ok;
    }
};

// Writes a recording as the run goes (each entry as it happens, so a run
// that stops still leaves what it had).
class RecordingWriter {
public:
    bool open(const std::string& path, int64_t clock, std::span<const uint16_t> eeprom, std::span<const uint8_t> backup) {
        close();
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        const uint32_t backup_size = static_cast<uint32_t>(backup.size());
        std::array<uint16_t, 64> words{};
        std::memcpy(words.data(), eeprom.data(), std::min<size_t>(eeprom.size_bytes(), 128));
        std::fwrite(Recording::kMagic, 1, 8, f_);
        std::fwrite(&clock, 8, 1, f_);
        std::fwrite(words.data(), 1, 128, f_);
        std::fwrite(&backup_size, 4, 1, f_);
        std::fwrite(backup.data(), 1, backup.size(), f_);
        return true;
    }
    bool is_open() const { return f_ != nullptr; }
    void inputs(uint32_t frame, const Recording::Inputs& in) {
        if (!f_) return;
        std::fputc('I', f_);
        std::fwrite(&frame, 4, 1, f_);
        const uint8_t b[4] = {in.bank0, in.bank1, in.controls, in.controls2};
        std::fwrite(b, 1, 4, f_);
    }
    void hash(uint32_t frame, uint64_t h) {
        if (!f_) return;
        std::fputc('H', f_);
        std::fwrite(&frame, 4, 1, f_);
        std::fwrite(&h, 8, 1, f_);
    }
    void close() {
        if (f_) std::fclose(f_);
        f_ = nullptr;
    }
    ~RecordingWriter() { close(); }

private:
    std::FILE* f_ = nullptr;
};

} // namespace tools
