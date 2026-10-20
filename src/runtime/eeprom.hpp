// 93C46 serial EEPROM: 64 words of 16 bits, Microwire interface (chip
// select, clock, data in, data out). On Model 3 it holds the game's
// settings and is bit-banged through I/O board register 0x00 (CS bit 6,
// CLK bit 7, DI bit 5) and read back in bit 5 of register 0x04, bank 1.
//
// Commands (start bit, 2-bit opcode, 6-bit address, MSB first):
//   READ  1 10 A..A        data follows, D15 first
//   WRITE 1 01 A..A D..D   (needs EWEN)
//   ERASE 1 11 A..A
//   EWEN  1 00 11xxxx      enable writes     EWDS 1 00 00xxxx disable
//   WRAL  1 00 01xxxx D..D ERAL  1 00 10xxxx
//
// From the datasheet, checked against Supermodel's Src/Model3/93C46.cpp:
// after the last address bit of a READ, DO shows a dummy 0 until the next
// clock, which brings D15 (Supermodel leaves the dummy out; shifting in an
// extra 0 bit instead "causes problems", their notes say). Programming time:
// after a write or erase, raising CS shows busy (DO = 0) until the chip's
// self-timed cycle ends, in time on the CPU's clock: MAME's generic 93Cxx
// times (1.75 ms a word, 1 ms an erase, 8 ms for all words), since the
// board's part (a "93C46A") and so its datasheet aren't known; any of them
// takes milliseconds. Supermodel shows busy for 5 DO reads.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace m3 {

class Eeprom93C46 {
public:
    explicit Eeprom93C46(uint64_t cpu_hz = 166'000'000) : cpu_hz_(cpu_hz) { erase_all(); }

    // `now`: the CPU's time in cycles.
    void write_pins(bool cs, bool clk, bool di, uint64_t now = 0);
    bool read_do(uint64_t now = 0);

    // Contents (persisting them across runs is up to the frontend).
    std::span<uint16_t, 64> words() { return words_; }
    std::span<const uint16_t, 64> words() const { return words_; }
    void erase_all() { words_.fill(0xFFFF); } // a blank chip reads all ones

    // Self-timed programming cycles, in microseconds.
    static constexpr uint64_t kWriteUs = 1750, kEraseUs = 1000, kAllUs = 8000;

private:
    std::array<uint16_t, 64> words_{};
    bool cs_ = false, clk_ = false, do_ = true;
    bool writable_ = false;
    bool done_ = false;       // command complete, waiting for CS to drop
    bool sending_ = false;    // shifting data out after a READ
    uint32_t in_ = 0;         // bits shifted in since CS went high
    int in_bits_ = 0;
    uint16_t out_ = 0;        // word being shifted out
    int out_bits_ = 0;
    uint8_t addr_ = 0;
    uint64_t cpu_hz_;
    uint64_t busy_until_ = 0; // cycle the programming cycle ends
    void program(uint64_t now, uint64_t us) { busy_until_ = now + us * cpu_hz_ / 1'000'000; }
};

} // namespace m3
