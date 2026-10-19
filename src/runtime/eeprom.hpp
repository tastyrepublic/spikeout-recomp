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
// after a write, raising CS shows busy (DO = 0) for a while, then ready;
// here that is a number of DO reads (as in Supermodel), not real time.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace m3 {

class Eeprom93C46 {
public:
    Eeprom93C46() { erase_all(); }

    void write_pins(bool cs, bool clk, bool di);
    bool read_do();

    // Contents (persisting them across runs is up to the frontend).
    std::span<uint16_t, 64> words() { return words_; }
    std::span<const uint16_t, 64> words() const { return words_; }
    void erase_all() { words_.fill(0xFFFF); } // a blank chip reads all ones

    static constexpr int kBusyReads = 5; // DO reads of "busy" after a write or erase

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
    int busy_ = 0;
};

} // namespace m3
