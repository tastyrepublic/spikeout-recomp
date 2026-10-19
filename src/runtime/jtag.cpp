// Adapted from Supermodel's Src/Model3/JTAG.cpp:
//   Copyright 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis,
//   Harry Tuttle, and Spindizzi. GPL-3.0-or-later.
#include "runtime/jtag.hpp"

namespace m3 {

namespace {

// Next state for TMS = 0 and TMS = 1 (IEEE 1149.1).
constexpr uint8_t kNext[16][2] = {
    {1, 0},   // Test-Logic-Reset
    {1, 2},   // Run-Test/Idle
    {3, 9},   // Select-DR-Scan
    {4, 5},   // Capture-DR
    {4, 5},   // Shift-DR
    {6, 8},   // Exit1-DR
    {6, 7},   // Pause-DR
    {4, 8},   // Exit2-DR
    {1, 2},   // Update-DR
    {10, 0},  // Select-IR-Scan
    {11, 12}, // Capture-IR
    {11, 12}, // Shift-IR
    {13, 15}, // Exit1-IR
    {13, 14}, // Pause-IR
    {11, 15}, // Exit2-IR
    {1, 2},   // Update-IR
};

constexpr uint32_t kIdCode = 3, kModeword = 8, kMemoryConfig = 9;

} // namespace

Jtag::Jtag() {
    // Step 2.x chain order; the 3D-RAMs only ever take BYPASS here.
    using A = Asic;
    struct Spec {
        bool asic;
        A kind;
        bool first;
        uint32_t id;
    };
    // Step 2.1 ID codes (Supermodel, CJTAG::SetStepping).
    constexpr Spec kChain[17] = {
        {true, A::Jupiter, true, 0x516c7057}, {true, A::Mercury, true, 0x416c3057},
        {true, A::Venus, true, 0x316c4057},   {true, A::Earth, true, 0x516c5057},
        {false, A::Mercury, false, 0},        {true, A::Mars, true, 0x316c6057},
        {true, A::Mars, false, 0x316c6057},   {false, A::Mercury, false, 0},
        {false, A::Mercury, false, 0},        {false, A::Mercury, false, 0},
        {true, A::Earth, false, 0x516c5057},  {false, A::Mercury, false, 0},
        {true, A::Mars, false, 0x316c6057},   {true, A::Mars, false, 0x316c6057},
        {false, A::Mercury, false, 0},        {false, A::Mercury, false, 0},
        {false, A::Mercury, false, 0},
    };
    for (size_t i = 0; i < chain_.size(); ++i) {
        chain_[i].asic = kChain[i].asic;
        chain_[i].kind = kChain[i].kind;
        chain_[i].first_of_kind = kChain[i].first;
        chain_[i].id_code = kChain[i].id;
    }
    reset();
}

void Jtag::reset_devices() {
    for (Device& d : chain_) {
        if (d.asic) { // ASICs come out of reset with IDCODE selected
            d.length = 32;
            d.shift = d.id_code;
            d.instruction = kIdCode;
        } else {      // 3D-RAMs with BYPASS
            d.length = 1;
            d.shift = 0;
            d.instruction = 15;
        }
    }
}

void Jtag::reset() {
    state_ = State::TestLogicReset;
    tdo_ = false;
    reset_devices();
}

void Jtag::capture_dr(Device& d) {
    if (!d.asic) {
        d.length = 1;
        d.shift = 0;
        return;
    }
    switch (d.instruction) {
    case kIdCode:
        d.length = 32;
        d.shift = d.id_code;
        break;
    case kModeword:
        d.length = 32;
        d.shift = d.modeword;
        break;
    case kMemoryConfig:
        if (d.kind == Asic::Mercury && d.first_of_kind) {
            d.length = 57;
            d.shift = uint64_t{mem_config_[0] & 0x7FFFF} << 38 | uint64_t{mem_config_[1] & 0x7FFFF} << 19 |
                      (mem_config_[2] & 0x7FFFF);
            break;
        }
        [[fallthrough]];
    default: // BYPASS
        d.length = 1;
        d.shift = 0;
        break;
    }
}

void Jtag::update_dr(Device& d) {
    if (!d.asic) return;
    if (d.instruction == kModeword) {
        d.modeword = static_cast<uint32_t>(d.shift);
        if (d.first_of_kind) modewords_[static_cast<size_t>(d.kind)] = d.modeword;
    } else if (d.instruction == kMemoryConfig && d.kind == Asic::Mercury && d.first_of_kind) {
        mem_config_[0] = static_cast<uint32_t>(d.shift >> 38) & 0x7FFFF;
        mem_config_[1] = static_cast<uint32_t>(d.shift >> 19) & 0x7FFFF;
        mem_config_[2] = static_cast<uint32_t>(d.shift) & 0x7FFFF;
    }
}

void Jtag::rising_edge(bool tms, bool tdi) {
    switch (state_) {
    case State::TestLogicReset:
        reset_devices();
        break;
    case State::CaptureDR:
        for (Device& d : chain_) capture_dr(d);
        break;
    case State::CaptureIR:
        for (Device& d : chain_) {
            d.length = d.asic ? 5 : 4;
            d.shift = d.asic ? 0b00001 : 0b1001;
        }
        break;
    case State::ShiftDR:
    case State::ShiftIR:
        for (Device& d : chain_) { // TDI goes in at the top, out at bit 0 into the next chip
            const bool out = d.shift & 1;
            d.shift >>= 1;
            if (tdi) d.shift |= uint64_t{1} << (d.length - 1);
            tdi = out;
        }
        break;
    default:
        break;
    }
    state_ = static_cast<State>(kNext[static_cast<size_t>(state_)][tms ? 1 : 0]);
}

void Jtag::falling_edge() {
    switch (state_) {
    case State::ShiftDR:
    case State::ShiftIR:
        tdo_ = chain_.back().shift & 1;
        break;
    case State::UpdateDR:
        for (Device& d : chain_) update_dr(d);
        break;
    case State::UpdateIR:
        for (Device& d : chain_) d.instruction = static_cast<uint8_t>(d.shift);
        break;
    default:
        break;
    }
}

void Jtag::write(bool tck, bool tms, bool tdi, bool trst) {
    if (!trst) { // TRST (active low) resets the chips, not the TAP
        reset_devices();
        return;
    }
    if (tck == last_tck_) return;
    last_tck_ = tck;
    if (tck) rising_edge(tms, tdi);
    else falling_edge();
}

} // namespace m3
