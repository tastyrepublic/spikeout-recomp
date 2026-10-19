// Model 3's JTAG test access port: bit-banged by the CPU through system
// register 0x0C (TCK bit 6, TDI bit 5, TMS bit 2, TRST bit 7) and read back
// through 0x10 (TDO in bit 5). It reaches a daisy chain of the video
// board's chips: the Real3D ASICs and the 3D-RAMs. Games read the ASICs' ID
// codes to identify the board and configure them through their mode words.
//
// Adapted from Supermodel's Src/Model3/JTAG.cpp and JTAG.h:
//   Copyright 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis,
//   Harry Tuttle, and Spindizzi. GPL-3.0-or-later.
// IEEE 1149.1 TAP state machine; Step 2.1 chain of 17 devices and its ID
// codes. Not taken over: Venus's polygon memory test access (Model 3 games
// don't use it). Mode words and Mercury's memory configuration are kept
// here until there is a Real3D to hand them to.
#pragma once

#include <array>
#include <cstdint>

namespace m3 {

class Jtag {
public:
    Jtag();

    bool tdo() const { return tdo_; }
    void write(bool tck, bool tms, bool tdi, bool trst);
    void reset();

    enum class Asic : uint8_t { Mercury, Venus, Earth, Mars, Jupiter };
    // Mode word last written to the first ASIC of each kind.
    uint32_t modeword(Asic a) const { return modewords_[static_cast<size_t>(a)]; }
    // Mercury's memory configuration (3 words of 19 bits).
    uint32_t memory_config(uint32_t i) const { return mem_config_[i]; }

private:
    enum class State : uint8_t {
        TestLogicReset, RunTestIdle, SelectDRScan, CaptureDR, ShiftDR, Exit1DR, PauseDR, Exit2DR,
        UpdateDR, SelectIRScan, CaptureIR, ShiftIR, Exit1IR, PauseIR, Exit2IR, UpdateIR,
    };

    // One chip on the chain: a shift register (at most 64 bits are ever
    // used on Model 3) and an instruction register.
    struct Device {
        bool asic = false;
        Asic kind = Asic::Mercury;
        bool first_of_kind = false; // its mode word is the one that counts
        uint32_t id_code = 0;
        uint32_t modeword = 0;
        uint64_t shift = 0;
        uint32_t length = 1;
        uint8_t instruction = 0;
    };

    void rising_edge(bool tms, bool tdi);
    void falling_edge();
    void reset_devices();
    void capture_dr(Device& d);
    void update_dr(Device& d);

    std::array<Device, 17> chain_{};
    State state_ = State::TestLogicReset;
    bool tdo_ = false;
    bool last_tck_ = false;
    std::array<uint32_t, 5> modewords_{};
    std::array<uint32_t, 3> mem_config_{};
};

} // namespace m3
