// The Model 3 sound board: a 68000 at 11.2896 MHz (45.1584 MHz / 4) with two
// SCSPs at 22.5792 MHz, each with 512 KB of sound RAM it plays samples from.
// The main board talks to it over a MIDI-rate serial line into the first
// SCSP. The 68000's memory map (Supermodel's Src/Model3/SoundBoard.cpp and
// MAME's model3.cpp agree):
//
//   000000  SCSP 1 RAM          100000  SCSP 1 registers
//   200000  SCSP 2 RAM          300000  SCSP 2 registers
//   400001  control: bit 4 selects the upper 8 MB of sample ROM
//   600000  program ROM, 512 KB
//   800000  sample ROM, an 8 MB window into the 16 MB
//
// At reset the first 16 bytes of the program (the 68000's vectors) are in
// SCSP 1 RAM at 0. Mirrors within each 1 MB block are a choice (MAME maps
// only the parts listed).
#pragma once

#include "runtime/scsp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace m3 {

class SoundBoard {
public:
    static constexpr uint32_t kCpuHz = 11'289'600;
    static constexpr int kCyclesPerSample = kCpuHz / Scsp::kSampleRate; // 256

    SoundBoard(std::span<const uint8_t> program, std::span<const uint8_t> samples);
    ~SoundBoard();

    void reset();
    // A byte from the main board's MIDI port.
    void midi_in(uint8_t byte);
    // Runs the board for `frames` output samples at 44.1 kHz, writing
    // interleaved stereo (both SCSPs mixed).
    void render(int16_t* out, std::size_t frames);

    // ---- Inspection ----
    uint32_t pc() const;
    uint64_t cpu_cycles() const;
    uint64_t samples() const { return samples_; }
    uint64_t midi_bytes = 0;
    std::array<uint64_t, 8> irqs_taken{};
    int active_slots() const { return scsp1_.active_slots() + scsp2_.active_slots(); }
    uint64_t midi_dropped() const { return scsp1_.midi_dropped; }
    std::size_t midi_waiting() const { return scsp1_.midi_pending(); }
    std::span<const uint8_t> ram(int chip) const { return chip ? ram2_ : ram1_; }
    std::string describe() const;

private:
    class Cpu;
    friend class Cpu;

    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);
    const uint8_t* sample_bank() const;

    std::span<const uint8_t> program_, samples_rom_;
    std::vector<uint8_t> ram1_, ram2_;
    Scsp scsp1_, scsp2_;
    std::unique_ptr<Cpu> cpu_;
    uint8_t ctrl_ = 0;
    uint64_t samples_ = 0;
};

} // namespace m3
