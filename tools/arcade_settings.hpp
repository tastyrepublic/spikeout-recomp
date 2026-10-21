// Spikeout's own settings (its test menu's GAME ASSIGNMENTS) as it keeps
// them in the settings EEPROM: found by changing each in the test menu and
// comparing the EEPROM after leaving it (the game writes it then, and reads
// it as it starts).
//
//   words 0-2   "M3SEGA"
//   word 3      CRC-16-CCITT (polynomial 0x1021, from 0, MSB first) of
//               words 6-34, high byte first
//   words 6-34  the settings; words 35-63 a second copy of them
//   +0 (word 6) a count of saves; +9 (15) high byte: COMM ID (0 single,
//   1 master, 2 slave); +10 (16): difficulty (0 easiest - 4 hardest);
//   +11 (17) high byte: advertise sound (1 on, 0 off)
//
// The country (EXPORT) isn't among them: the test menu shows it but can't
// change it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tools {

class ArcadeSettings {
public:
    static constexpr std::size_t kFirst = 6, kWords = 29, kChecksum = 3;
    static constexpr std::size_t kCommId = 9, kDifficulty = 10, kSound = 11; // in the block
    static constexpr std::array<const char*, 5> kDifficultyNames = {"Easiest", "Easy", "Normal", "Hard", "Hardest"};

    explicit ArcadeSettings(std::span<uint16_t, 64> words) : w_(words) {}

    // The game has written its settings (a blank EEPROM reads all ones;
    // the game fills it in as it first starts).
    bool valid() const { return w_[0] == 0x4D33 && w_[1] == 0x5345 && w_[2] == 0x4741 && w_[kChecksum] == checksum(); }

    int difficulty() const { return w_[kFirst + kDifficulty]; }
    void set_difficulty(int d) { set(kDifficulty, static_cast<uint16_t>(d)); }
    bool advertise_sound() const { return (w_[kFirst + kSound] >> 8) != 0; }
    void set_advertise_sound(bool on) {
        set(kSound, static_cast<uint16_t>((w_[kFirst + kSound] & 0x00FF) | (on ? 0x0100 : 0)));
    }

    uint16_t checksum() const {
        uint16_t crc = 0;
        for (std::size_t i = kFirst; i < kFirst + kWords; ++i)
            for (int shift = 8; shift >= 0; shift -= 8) {
                crc = static_cast<uint16_t>(crc ^ ((w_[i] >> shift) & 0xFF) << 8);
                for (int bit = 0; bit < 8; ++bit)
                    crc = static_cast<uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
            }
        return crc;
    }

private:
    // A word in both copies, and the checksum again.
    void set(std::size_t at, uint16_t v) {
        w_[kFirst + at] = v;
        w_[kFirst + kWords + at] = v;
        w_[kChecksum] = checksum();
    }
    std::span<uint16_t, 64> w_;
};

} // namespace tools
