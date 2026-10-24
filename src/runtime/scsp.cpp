// SCSP: see scsp.hpp. Adapted from Supermodel's Src/Sound/SCSP.cpp,
// SCSPLFO.cpp and SCSPDSP.cpp:
//
//   Supermodel, a Sega Model 3 Arcade Emulator.
//   Copyright 2003-2026 The Supermodel Team.
//   SCSP emulation by ElSemi; MAME conversion by R. Belmont, ElSemi and
//   Paul Prosser.
//
//   Supermodel is free software: you can redistribute it and/or modify it
//   under the terms of the GNU General Public License as published by the
//   Free Software Foundation, either version 3 of the License, or (at your
//   option) any later version.
//
// Changes from it: one object per chip (the original keeps both chips and
// the MIDI FIFO in globals); RAM in 68000 byte order and 512 KB; register
// fields at their documented widths (Supermodel widens MDL and PLFOS by a
// bit as a tuning hack; FM depth follows MAME's scale for the real field);
// idle slots start released, so the first key-on starts them; reading the
// MIDI status byte doesn't take a byte from the FIFO, and the FIFO's empty
// and full flags are kept; timers reload with all 8 bits; the LFO noise is
// a fixed sequence, so runs repeat; no bounds overruns on odd addresses.
#include "runtime/scsp.hpp"
#include "runtime/state.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace m3 {
namespace {

constexpr int kShift = 12;   // sample position and volume fixed point
constexpr int kEgShift = 16; // envelope fixed point
constexpr int kLfoShift = 8;

// ---- Slot register fields ----
struct Fields {
    const std::array<uint16_t, 16>& d;
    unsigned keyonex() const { return d[0] & 0x1000; }
    unsigned keyonb() const { return d[0] & 0x0800; }
    unsigned sbctl() const { return (d[0] >> 9) & 3; }
    unsigned ssctl() const { return (d[0] >> 7) & 3; }
    unsigned lpctl() const { return (d[0] >> 5) & 3; }
    unsigned pcm8b() const { return d[0] & 0x10; }
    uint32_t sa() const { return (static_cast<uint32_t>(d[0] & 0xF) << 16) | d[1]; }
    uint32_t lsa() const { return d[2]; }
    uint32_t lea() const { return d[3]; }
    unsigned d2r() const { return (d[4] >> 11) & 0x1F; }
    unsigned d1r() const { return (d[4] >> 6) & 0x1F; }
    unsigned eghold() const { return d[4] & 0x20; }
    unsigned ar() const { return d[4] & 0x1F; }
    unsigned lpslnk() const { return d[5] & 0x4000; }
    unsigned krs() const { return (d[5] >> 10) & 0xF; }
    unsigned dl() const { return (d[5] >> 5) & 0x1F; }
    unsigned rr() const { return d[5] & 0x1F; }
    unsigned stwinh() const { return d[6] & 0x200; }
    unsigned sdir() const { return d[6] & 0x100; }
    unsigned tl() const { return d[6] & 0xFF; }
    unsigned mdl() const { return (d[7] >> 12) & 0xF; }
    unsigned mdxsl() const { return (d[7] >> 6) & 0x3F; }
    unsigned mdysl() const { return d[7] & 0x3F; }
    unsigned oct() const { return (d[8] >> 11) & 0xF; }
    unsigned fns() const { return d[8] & 0x3FF; }
    unsigned lfof() const { return (d[9] >> 10) & 0x1F; }
    unsigned plfows() const { return (d[9] >> 8) & 3; }
    unsigned plfos() const { return (d[9] >> 5) & 7; }
    unsigned alfows() const { return (d[9] >> 3) & 3; }
    unsigned alfos() const { return d[9] & 7; }
    unsigned isel() const { return (d[10] >> 3) & 0xF; }
    unsigned imxl() const { return d[10] & 7; }
    unsigned disdl() const { return (d[11] >> 13) & 7; }
    unsigned dipan() const { return (d[11] >> 8) & 0x1F; }
    unsigned efsdl() const { return (d[11] >> 5) & 7; }
    unsigned efpan() const { return d[11] & 0x1F; }
};

// ---- Tables, built once ----
constexpr double kArTimes[64] = {
    100000, 100000, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
    1200.0, 1000.0, 860.0,  760.0,  600.0,  500.0,  430.0,  380.0,  300.0,  250.0,  220.0,  190.0,  150.0,
    130.0,  110.0,  95.0,   76.0,   63.0,   55.0,   47.0,   38.0,   31.0,   27.0,   24.0,   19.0,   15.0,
    13.0,   12.0,   9.4,    7.9,    6.8,    6.0,    4.7,    3.8,    3.4,    3.0,    2.4,    2.0,    1.8,
    1.6,    1.3,    1.1,    0.93,   0.85,   0.65,   0.53,   0.44,   0.40,   0.35,   0.0,    0.0};
constexpr double kDrTimes[64] = {
    100000,  100000,  118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0,
    25300.0, 22200.0, 17700.0,  14800.0,  12700.0, 11100.0, 8900.0,  7400.0,  6300.0,  5500.0,  4400.0,
    3700.0,  3200.0,  2800.0,   2200.0,   1800.0,  1600.0,  1400.0,  1100.0,  920.0,   790.0,   690.0,
    550.0,   460.0,   390.0,    340.0,    270.0,   230.0,   200.0,   170.0,   140.0,   110.0,   98.0,
    85.0,    68.0,    57.0,     49.0,     43.0,    34.0,    28.0,    25.0,    22.0,    18.0,    14.0,
    12.0,    11.0,    8.5,      7.1,      6.1,     5.4,     4.3,     3.6,     3.1};
constexpr float kLfoFreq[32] = {0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f,
                                1.10f, 1.39f, 1.60f, 1.87f, 2.27f, 2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f,
                                8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f};
constexpr float kAScale[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
constexpr float kPScale[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};
constexpr float kSdlDb[8] = {-1000000.0f, -36.0f, -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f};

struct Tables {
    std::array<int, 64> ar{}, dr{};
    std::array<int32_t, 0x400> eg{};
    std::vector<int> lpan, rpan; // by TL | PAN << 8 | SDL << 13
    int plfo[4][256]{}, alfo[4][256]{}; // saw, square, triangle, noise
    int pscales[8][256]{}, ascales[8][256]{};

    Tables() : lpan(0x10000), rpan(0x10000) {
        for (int i = 0; i < 0x400; ++i)
            eg[i] = static_cast<int32_t>(std::pow(10.0, (3.0 * (i - 0x3FF) / 32.0) / 20.0) * (1 << kShift));
        for (int i = 0; i < 0x10000; ++i) {
            const int itl = i & 0xFF, ipan = (i >> 8) & 0x1F, isdl = (i >> 13) & 7;
            float db = 0;
            const float tl_steps[8] = {0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f, 48.0f};
            for (int b = 0; b < 8; ++b)
                if (itl & (1 << b)) db -= tl_steps[b];
            const float tl = std::pow(10.0f, db / 20.0f);
            db = 0;
            const float pan_steps[4] = {3.0f, 6.0f, 12.0f, 24.0f};
            for (int b = 0; b < 4; ++b)
                if (ipan & (1 << b)) db -= pan_steps[b];
            const float pan = (ipan == 0xF || ipan == 0x1F) ? 0.0f : std::pow(10.0f, db / 20.0f);
            const float l = ipan < 0x10 ? pan : 1.0f, r = ipan < 0x10 ? 1.0f : pan;
            const float sdl = isdl ? std::pow(10.0f, kSdlDb[isdl] / 20.0f) : 0.0f;
            lpan[i] = static_cast<int>(static_cast<float>(1 << kShift) * (4.0f * l * tl * sdl));
            rpan[i] = static_cast<int>(static_cast<float>(1 << kShift) * (4.0f * r * tl * sdl));
        }
        constexpr double srate = Scsp::kSampleRate;
        ar[0] = dr[0] = ar[1] = dr[1] = 0; // infinite
        for (int i = 2; i < 64; ++i) {
            ar[i] = kArTimes[i] != 0.0 ? static_cast<int>((1023 * 1000.0) / (srate * kArTimes[i]) * (1 << kEgShift))
                                       : 1024 << kEgShift;
            dr[i] = static_cast<int>((1023 * 1000.0) / (srate * kDrTimes[i]) * (1 << kEgShift));
        }
        uint32_t noise = 0x12345678; // a fixed sequence (Supermodel uses rand())
        for (int i = 0; i < 256; ++i) {
            alfo[0][i] = 255 - i; // saw
            plfo[0][i] = i < 128 ? i : i - 256;
            alfo[1][i] = i < 128 ? 255 : 0; // square
            plfo[1][i] = i < 128 ? 127 : -128;
            alfo[2][i] = i < 128 ? 255 - i * 2 : i * 2 - 256; // triangle
            plfo[2][i] = i < 64 ? i * 2 : i < 128 ? 255 - i * 2 : i < 192 ? 256 - i * 2 : i * 2 - 511;
            noise = noise * 1103515245u + 12345u;
            const int a = static_cast<int>((noise >> 16) & 0xFF);
            alfo[3][i] = a;
            plfo[3][i] = 128 - a;
        }
        for (int s = 0; s < 8; ++s) {
            for (int i = -128; i < 128; ++i)
                pscales[s][i + 128] = static_cast<int>((1 << kLfoShift) * std::exp2((kPScale[s] * static_cast<double>(i) / 128.0) / 1200.0));
            for (int i = 0; i < 256; ++i)
                ascales[s][i] = static_cast<int>((1 << kLfoShift) * std::pow(10.0, (-kAScale[s] * static_cast<double>(i) / 256.0) / 20.0));
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

// An envelope rate with the key-rate scale added. A rate of 0 is no change
// whatever the scale (the SCSP manual: "when AR = 00H the amount of change is
// 0", likewise D1R, D2R, RR); Supermodel and MAME add the scale to it too.
int rate_of(const std::array<int, 64>& table, int base, unsigned r) {
    if (r == 0) return 0;
    return table[static_cast<std::size_t>(std::clamp(base + static_cast<int>(r << 1), 0, 63))];
}

int clip16(int v) { return std::clamp(v, -32768, 32767); }

// DSP memory words: a 4-bit exponent, sign and 11-bit mantissa.
uint16_t pack(int32_t val) {
    const int sign = (val >> 23) & 1;
    uint32_t temp = static_cast<uint32_t>(val ^ (val << 1)) & 0xFFFFFF;
    int exponent = 0;
    for (int k = 0; k < 12; ++k) {
        if (temp & 0x800000) break;
        temp <<= 1;
        ++exponent;
    }
    val = exponent < 12 ? (val << exponent) & 0x3FFFFF : val << 11;
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return static_cast<uint16_t>(val);
}

int32_t unpack(uint16_t val) {
    const int sign = (val >> 15) & 1;
    int exponent = (val >> 11) & 0xF;
    const int mantissa = val & 0x7FF;
    int32_t uval = mantissa << 11;
    if (exponent > 11) {
        exponent = 11;
        uval |= sign << 22;
    } else {
        uval |= (sign ^ 1) << 22;
    }
    uval |= sign << 23;
    uval = static_cast<int32_t>(static_cast<uint32_t>(uval) << 8) >> 8;
    return uval >> exponent;
}

int32_t sign_extend24(int32_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v) << 8) >> 8; }

} // namespace

Scsp::Scsp(std::span<uint8_t> ram, bool master) : ram_(ram), master_(master) { (void)tables(); }

uint16_t Scsp::ram_word(uint32_t word) const {
    const uint32_t a = (word * 2) & (kRamSize - 1);
    return static_cast<uint16_t>(ram_[a] << 8 | ram_[a + 1]);
}

void Scsp::set_ram_word(uint32_t word, uint16_t v) {
    const uint32_t a = (word * 2) & (kRamSize - 1);
    ram_[a] = static_cast<uint8_t>(v >> 8);
    ram_[a + 1] = static_cast<uint8_t>(v);
}

int Scsp::active_slots() const {
    int n = 0;
    for (const Slot& s : slots_) n += s.active ? 1 : 0;
    return n;
}

// ---- Slots ----

void Scsp::compute_eg(Slot& slot) {
    const Fields f{slot.data};
    const int octave = static_cast<int>(f.oct() ^ 8) - 8;
    const int rate = f.krs() != 0xF ? octave + 2 * static_cast<int>(f.krs()) + static_cast<int>((f.fns() >> 9) & 1) : 0;
    const Tables& t = tables();
    slot.eg.volume = 0x17F << kEgShift;
    slot.eg.ar = rate_of(t.ar, rate, f.ar());
    slot.eg.d1r = rate_of(t.dr, rate, f.d1r());
    slot.eg.d2r = rate_of(t.dr, rate, f.d2r());
    slot.eg.rr = rate_of(t.dr, rate, f.rr());
    slot.eg.dl = 0x1F - static_cast<int>(f.dl());
    slot.eg.hold = f.eghold() != 0;
}

void Scsp::compute_lfo(Slot& slot) {
    const Fields f{slot.data};
    const Tables& t = tables();
    const auto step = static_cast<uint32_t>((1 << kLfoShift) * (kLfoFreq[f.lfof()] * 256.0f / kSampleRate));
    if (f.plfos()) {
        slot.plfo.step = step;
        slot.plfo.table = t.plfo[f.plfows()];
        slot.plfo.scale = t.pscales[f.plfos()];
    }
    if (f.alfos()) {
        slot.alfo.step = step;
        slot.alfo.table = t.alfo[f.alfows()];
        slot.alfo.scale = t.ascales[f.alfos()];
    }
}

namespace {
uint32_t pitch_step(const Fields& f) {
    const int octave = static_cast<int>(f.oct() ^ 8) - 8 + kShift - 10;
    uint32_t fn = f.fns() + (1u << 10);
    return octave >= 0 ? fn << octave : fn >> -octave;
}
} // namespace

void Scsp::start_slot(Slot& slot) {
    const Fields f{slot.data};
    slot.active = true;
    slot.back = false;
    slot.nxt = 1u << kShift;
    slot.cur = 0;
    slot.base = f.pcm8b() ? f.sa() : f.sa() & 0x7FFFE;
    slot.step = pitch_step(f);
    compute_eg(slot);
    slot.eg.state = EgState::Attack;
    slot.eg.volume = 0x17F << kEgShift;
    slot.eg.below_dl = false;
    compute_lfo(slot);
}

void Scsp::stop_slot(Slot& slot, bool keyoff) {
    if (keyoff) slot.eg.state = EgState::Release;
    else slot.active = false;
    slot.data[0] &= static_cast<uint16_t>(~0x800);
}

int Scsp::eg_update(Slot& slot) {
    const Fields f{slot.data};
    Eg& eg = slot.eg;
    switch (eg.state) {
    case EgState::Attack:
        eg.volume += eg.ar;
        if (eg.volume >= (0x3FF << kEgShift)) {
            if (!f.lpslnk()) {
                eg.state = EgState::Decay1;
                if (eg.d1r >= (1024 << kEgShift)) eg.state = EgState::Decay2; // skip decay 1
            }
            eg.volume = 0x3FF << kEgShift;
        }
        if (eg.hold) return 0x3FF << (kShift - 10);
        break;
    case EgState::Decay1:
        eg.volume = std::max(eg.volume - eg.d1r, 0);
        if (!eg.below_dl && (eg.volume >> (kEgShift + 5)) <= eg.dl) eg.state = EgState::Decay2;
        break;
    case EgState::Decay2:
        if (f.d2r() == 0) return (eg.volume >> kEgShift) << (kShift - 10);
        eg.volume = std::max(eg.volume - eg.d2r, 0);
        break;
    case EgState::Release:
        eg.volume -= eg.rr;
        if (eg.volume <= 0) {
            eg.volume = 0;
            stop_slot(slot, false);
        }
        break;
    }
    return (eg.volume >> kEgShift) << (kShift - 10);
}

int32_t Scsp::update_slot(Slot& slot, int16_t* ring_out) {
    const Fields f{slot.data};
    const Tables& t = tables();

    uint32_t step = slot.step;
    if (f.plfos()) {
        slot.plfo.phase = static_cast<uint16_t>(slot.plfo.phase + slot.plfo.step);
        const int p = slot.plfo.scale[slot.plfo.table[slot.plfo.phase >> kLfoShift] + 128] << (kShift - kLfoShift);
        step = static_cast<uint32_t>((static_cast<int64_t>(step) * p) >> kShift);
    }

    uint32_t addr1, addr2;
    if (f.pcm8b()) {
        addr1 = slot.cur >> kShift;
        addr2 = slot.nxt >> kShift;
    } else {
        addr1 = (slot.cur >> (kShift - 1)) & 0x7FFFE;
        addr2 = (slot.nxt >> (kShift - 1)) & 0x7FFFE;
    }
    if (f.mdl() != 0 || f.mdxsl() != 0 || f.mdysl() != 0) { // FM: modulation from the ring buffer
        int32_t smp = (ring_[(bufptr_ + f.mdxsl()) & 63] + ring_[(bufptr_ + f.mdysl()) & 63]) / 2;
        smp *= 1 << 10;                                  // a cycle is 1024
        smp >>= 0x1A - static_cast<int>(f.mdl());        // MDL 15: +/- 64 pi (MAME's scale)
        if (!f.pcm8b()) smp *= 2;
        addr1 += static_cast<uint32_t>(smp);
        addr2 += static_cast<uint32_t>(smp);
        const uint32_t mask = f.pcm8b() ? 0x7FFFF : 0x7FFFE;
        addr1 &= mask;
        addr2 &= mask;
    }

    // SSCTL (the SCSP manual): 0 the waveform in memory, 1 noise from the
    // slot's LFO noise oscillator, 2 all zeros, 3 not available. The noise
    // algorithm isn't known (MAME uses random numbers): a fixed sequence
    // here, so runs repeat. The slot otherwise runs as usual (pitch,
    // envelope, levels). Supermodel and our earlier code played nothing
    // and left the slot's envelope stopped.
    int32_t sample;
    const int32_t fpart = static_cast<int32_t>(slot.cur & ((1u << kShift) - 1));
    if (f.ssctl() == 1) {
        noise_ = noise_ * 1103515245u + 12345u;
        sample = static_cast<int16_t>(noise_ >> 16);
    } else if (f.ssctl() != 0) {
        sample = 0;
    } else if (f.pcm8b()) {
        const int32_t p1 = static_cast<int8_t>(ram_[(slot.base + addr1) & (kRamSize - 1)]) * 256;
        const int32_t p2 = static_cast<int8_t>(ram_[(slot.base + addr2) & (kRamSize - 1)]) * 256;
        sample = (p1 * ((1 << kShift) - fpart) + p2 * fpart) >> kShift;
    } else {
        const auto word = [&](uint32_t a) {
            a = (slot.base + a) & (kRamSize - 2);
            return static_cast<int32_t>(static_cast<int16_t>(ram_[a] << 8 | ram_[a + 1]));
        };
        sample = (word(addr1) * ((1 << kShift) - fpart) + word(addr2) * fpart) >> kShift;
    }
    if (f.sbctl() & 1) sample ^= 0x7FFF;
    if (f.sbctl() & 2) sample = static_cast<int16_t>(sample ^ 0x8000);

    if (slot.back) slot.cur -= step;
    else slot.cur += step;
    slot.nxt = slot.cur + (1u << kShift);

    addr1 = slot.cur >> kShift;
    addr2 = slot.nxt >> kShift;
    if (addr1 >= f.lsa() && !slot.back && f.lpslnk() && slot.eg.state == EgState::Attack) {
        // The SCSP manual (EG register, LPSLNK, Fig. 4.11): reaching the loop
        // start under the decay level, the EG decays at D1R and never gets to
        // decay 2.
        slot.eg.state = EgState::Decay1;
        slot.eg.below_dl = (slot.eg.volume >> (kEgShift + 5)) <= slot.eg.dl;
    }

    uint32_t* const pos[2] = {&slot.cur, &slot.nxt};
    const uint32_t addrs[2] = {addr1, addr2};
    const uint32_t lsa = f.lsa() << kShift, lea = f.lea() << kShift;
    for (int i = 0; i < 2; ++i) {
        uint32_t& p = *pos[i];
        const uint32_t a = addrs[i];
        switch (f.lpctl()) {
        case 0: // no loop
            if (a >= f.lsa() && a >= f.lea()) stop_slot(slot, false);
            break;
        case 1: // normal loop
            if (a >= f.lea()) p = lsa + (p - lea);
            break;
        case 2: // reverse loop
            if (a >= f.lsa() && !slot.back) {
                p = lea - (p - lsa);
                slot.back = true;
            } else if ((a < f.lsa() || (p & 0x80000000u)) && slot.back) {
                p = lea - (lsa - p);
            }
            break;
        case 3: // ping-pong
            if (a >= f.lea()) {
                p = lea - (p - lea);
                slot.back = true;
            } else if ((a < f.lsa() || (p & 0x80000000u)) && slot.back) {
                p = lsa + (lsa - p);
                slot.back = false;
            }
            break;
        }
    }

    if (!f.sdir()) {
        if (f.alfos()) {
            slot.alfo.phase = static_cast<uint16_t>(slot.alfo.phase + slot.alfo.step);
            const int a = slot.alfo.scale[slot.alfo.table[slot.alfo.phase >> kLfoShift]] << (kShift - kLfoShift);
            sample = (sample * a) >> kShift;
        }
        if (slot.eg.state == EgState::Attack) sample = (sample * eg_update(slot)) >> kShift;
        else sample = (sample * t.eg[static_cast<std::size_t>(eg_update(slot) >> (kShift - 10))]) >> kShift;
    }
    if (!f.stwinh()) {
        const unsigned enc = (f.sdir() ? 0u : f.tl()) | (7u << 13);
        *ring_out = static_cast<int16_t>((sample * t.lpan[enc]) >> (kShift + 1));
    }
    return sample;
}

void Scsp::sample(int32_t& left, int32_t& right, int16_t ext_left, int16_t ext_right) {
    const Tables& t = tables();
    dsp_.exts = {ext_left, ext_right};
    int32_t l = 0, r = 0;
    for (Slot& slot : slots_) {
        int16_t* ring = &ring_[bufptr_];
        if (slot.active) {
            const int32_t s = update_slot(slot, ring);
            const Fields f{slot.data};
            dsp_.mixs[f.isel()] += (s * t.lpan[f.tl() | (f.imxl() << 13)]) >> (kShift - 2);
            const unsigned enc = f.tl() | (f.dipan() << 8) | (f.disdl() << 13);
            l += (s * t.lpan[enc]) >> kShift;
            r += (s * t.rpan[enc]) >> kShift;
        }
        bufptr_ = static_cast<uint8_t>((bufptr_ + 1) & 63);
    }
    dsp_step();
    for (std::size_t i = 0; i < 16; ++i) {
        const Fields f{slots_[i].data};
        if (!f.efsdl()) continue;
        const unsigned enc = (f.efpan() << 8) | (f.efsdl() << 13);
        l += (dsp_.efreg[i] * t.lpan[enc]) >> kShift;
        r += (dsp_.efreg[i] * t.rpan[enc]) >> kShift;
    }
    for (std::size_t i = 0; i < 2; ++i) { // external inputs: slot 16/17's effect level, or its direct level when that is 0 (MAME)
        const Fields f{slots_[16 + i].data};
        const unsigned enc = f.efsdl() ? (f.efpan() << 8) | (f.efsdl() << 13) : (f.dipan() << 8) | (f.disdl() << 13);
        l += (dsp_.exts[i] * t.lpan[enc]) >> kShift;
        r += (dsp_.exts[i] * t.rpan[enc]) >> kShift;
    }
    // The mix is 18 bits; a 16-bit DAC takes the top 16. With DAC18B (0x400
    // bit 8, which Spikeout sets) the 18 bits come out unscaled, as in
    // Supermodel: four times the 16-bit level. That is Supermodel's balance
    // against the music board; MAME scales it back to the 16-bit range,
    // which leaves the sound effects 12 dB under the music.
    if (regs_[0] & 0x100) {
        left = std::clamp(l, -131072, 131071);
        right = std::clamp(r, -131072, 131071);
    } else {
        left = clip16(l >> 2);
        right = clip16(r >> 2);
    }
}

// ---- Timers, interrupts, MIDI (master) ----

uint8_t Scsp::decode_sci(int irq) const {
    uint8_t level = 0;
    for (int b = 0; b < 3; ++b)
        if (regs_[0x24 / 2 + b] & (1u << irq)) level = static_cast<uint8_t>(level | (1u << b));
    return level;
}

void Scsp::timer_reload(int t) {
    if (master_) timer_[static_cast<std::size_t>(t)] = (regs_[0x18 / 2 + t] & 0xFF) << 8;
}

void Scsp::tick_timers() {
    // Each sample, and the MIDI output FIFO (nothing is sent) always empty.
    if (master_) regs_[0x20 / 2] |= 0x600;
    for (int t = 0; t < 3; ++t) {
        int& count = timer_[static_cast<std::size_t>(t)];
        uint16_t& reg = regs_[0x18 / 2 + t];
        if (count > 0xFF00) continue; // overflowed: stays until reloaded (Supermodel)
        count += 1 << (8 - ((reg >> 8) & 7));
        if (count > 0xFF00) {
            count = 0xFFFF;
            regs_[0x20 / 2] |= static_cast<uint16_t>(0x40 << t);
        }
        reg = static_cast<uint16_t>((reg & 0xFF00) | (count >> 8));
    }
}

// MIDI input is pending while the FIFO holds a byte (MAME), and stops when
// a read empties it. Supermodel dropped the pending bit each time the level
// was worked out, which with one byte at a time let the 68000 take the
// interrupt again and read an empty FIFO.
// The SCSP manual (interrupt control register): a level for each pending
// and enabled source, from SCILV0-2 bit for bit; timers B and C, MIDI out
// and the per-sample interrupt (bits 7-10) all take bit 7's level. Sources:
// 3 MIDI in, 4 DMA end, 5 CPU (written), 6-8 timers A-C, 9 MIDI out empty,
// 10 each sample (0-2 external inputs aren't connected here).
int Scsp::irq_level() {
    if (!master_) return 0;
    if (midi_w_ != midi_r_) regs_[0x20 / 2] |= 8;
    const uint16_t active = regs_[0x20 / 2] & regs_[0x1E / 2] & 0x7FF;
    int level = 0;
    for (int b = 0; b <= 10; ++b)
        if (active & (1u << b)) level = std::max<int>(level, decode_sci(std::min(b, 7)));
    return level;
}

void Scsp::midi_in(uint8_t byte) {
    if (static_cast<uint8_t>(midi_w_ + 1) == midi_r_) { // full
        ++midi_dropped;
        return;
    }
    midi_[midi_w_++] = byte;
}

// ---- Registers ----

void Scsp::update_slot_reg(int s, uint32_t reg) {
    Slot& slot = slots_[static_cast<std::size_t>(s)];
    switch (reg & 0x1E) {
    case 0x00:
        if (Fields{slot.data}.keyonex()) {
            for (Slot& s2 : slots_) {
                const Fields f2{s2.data};
                if (f2.keyonb() && s2.eg.state == EgState::Release) start_slot(s2);
                if (!f2.keyonb()) stop_slot(s2, true);
            }
            slot.data[0] &= static_cast<uint16_t>(~0x1000);
        }
        break;
    case 0x10: slot.step = pitch_step(Fields{slot.data}); break;
    case 0x0A:
        slot.eg.rr = rate_of(tables().dr, 0, Fields{slot.data}.rr());
        slot.eg.dl = 0x1F - static_cast<int>(Fields{slot.data}.dl());
        break;
    case 0x12: compute_lfo(slot); break;
    default: break;
    }
}

void Scsp::update_reg(uint32_t reg) {
    switch (reg & 0x3E) {
    case 0x00:
    case 0x02:
        dsp_.rbl = (8u * 1024) << ((regs_[1] >> 7) & 3); // 8 / 16 / 32 / 64 kwords
        dsp_.rbp = regs_[1] & 0x7F;
        break;
    case 0x08: regs_[4] &= 0xF800; break;
    case 0x18: timer_reload(0); break;
    case 0x1A: timer_reload(1); break;
    case 0x1C: timer_reload(2); break;
    case 0x22: // SCIRE: clear pending interrupts; an overflowed timer stays pending (Supermodel)
        if (master_) {
            regs_[0x20 / 2] &= static_cast<uint16_t>(~regs_[0x22 / 2]);
            for (int t = 0; t < 3; ++t)
                if (timer_[static_cast<std::size_t>(t)] == 0xFFFF) regs_[0x20 / 2] |= static_cast<uint16_t>(0x40 << t);
        }
        break;
    case 0x2A: mcieb_ = regs_[0x2A / 2]; break;
    case 0x2E: mcipd_ &= static_cast<uint16_t>(~regs_[0x2E / 2]); break;
    default: break;
    }
}

// Registers whose read value is made at read time. `low` is false when only
// the high byte of the word is read.
void Scsp::update_reg_read(uint32_t reg) {
    const bool low = (reg & 1) != 0 || (reg & 0x80) != 0; // bit 7 marks a word read
    reg &= 0x3E;
    switch (reg) {
    case 0x04: { // MIDI input: the oldest byte, and the FIFO's flags as it was
        // The flags describe the FIFO before the read, so MIEMPTY says
        // whether the byte read is real (MAME); reading the low byte then
        // takes it.
        uint16_t flags = 0x0800; // MIDI output always empty
        if (midi_r_ == midi_w_) flags |= 0x0100;                        // MIEMPTY
        if (static_cast<uint8_t>(midi_w_ + 1) == midi_r_) flags |= 0x0200; // MIFULL
        uint8_t byte = static_cast<uint8_t>(regs_[2]);
        if (master_) byte = midi_[midi_r_];
        if (low && master_ && midi_r_ != midi_w_) {
            ++midi_r_;
            if (midi_r_ == midi_w_) regs_[0x20 / 2] &= static_cast<uint16_t>(~8); // emptied: no longer pending
        }
        regs_[2] = static_cast<uint16_t>(flags | byte);
        break;
    }
    case 0x08: { // monitor: the slot MSLC selects
        const Slot& slot = slots_[(regs_[4] >> 11) & 0x1F];
        const unsigned ca = (slot.cur >> (kShift + 12)) & 0xF;
        const unsigned sgc = static_cast<unsigned>(slot.eg.state) & 3;
        const unsigned eg = (0x1Fu - static_cast<unsigned>(slot.eg.volume >> (kEgShift + 5))) & 0x1F;
        regs_[4] = static_cast<uint16_t>((regs_[4] & 0xF800) | ca << 7 | sgc << 5 | eg);
        break;
    }
    case 0x2A: regs_[0x2A / 2] = mcieb_; break;
    case 0x2C: regs_[0x2C / 2] = mcipd_; break;
    default: break;
    }
}

uint16_t Scsp::read16(uint32_t addr) {
    addr &= 0xFFE;
    if (addr < 0x400) return slots_[addr / 0x20].data[(addr & 0x1F) / 2];
    if (addr < 0x430) {
        update_reg_read((addr & 0x3F) | 0x80);
        return regs_[(addr & 0x3F) / 2];
    }
    if (addr < 0x600) return 0;
    if (addr < 0x680) return static_cast<uint16_t>(ring_[(addr - 0x600) / 2]);
    if (addr < 0x700) return 0;
    if (addr < 0x780) return static_cast<uint16_t>(dsp_.coef[(addr - 0x700) / 2]);
    if (addr < 0x800) return dsp_.madrs[((addr - 0x780) / 2) & 0x1F];
    if (addr < 0xC00) return dsp_.mpro[(addr - 0x800) / 2];
    const auto half = [&](int32_t v) { return static_cast<uint16_t>((addr & 2) ? v & 0xFFFF : v >> 16); };
    if (addr < 0xE00) return half(dsp_.temp[(addr >> 2) & 0x7F]);
    if (addr < 0xE80) return half(dsp_.mems[(addr >> 2) & 0x1F]);
    if (addr < 0xEC0) return half(dsp_.mixs[(addr >> 2) & 0xF]);
    if (addr < 0xEE0) return static_cast<uint16_t>(dsp_.efreg[(addr - 0xEC0) / 2]);
    if (addr < 0xEE4) return static_cast<uint16_t>(dsp_.exts[(addr - 0xEE0) / 2]);
    return 0;
}

uint8_t Scsp::read8(uint32_t addr) {
    addr &= 0xFFF;
    uint16_t word;
    if (addr >= 0x400 && addr < 0x430) {
        update_reg_read(addr & 0x3F);
        word = regs_[(addr & 0x3E) / 2];
    } else {
        word = read16(addr);
    }
    return static_cast<uint8_t>((addr & 1) ? word : word >> 8);
}

void Scsp::write16(uint32_t addr, uint16_t v) {
    addr &= 0xFFE;
    if (addr < 0x400) {
        slots_[addr / 0x20].data[(addr & 0x1F) / 2] = v;
        update_slot_reg(static_cast<int>(addr / 0x20), addr & 0x1F);
    } else if (addr < 0x430) {
        if ((addr & 0x3E) == 0x20) { // SCIPD: read-only but for bit 5; writing 1 there interrupts the CPU
            regs_[0x20 / 2] = static_cast<uint16_t>(regs_[0x20 / 2] | (v & 0x20));
            return;
        }
        regs_[(addr & 0x3F) / 2] = v;
        update_reg(addr & 0x3F);
    } else if (addr >= 0x600 && addr < 0x680) {
        ring_[(addr - 0x600) / 2] = static_cast<int16_t>(v);
    } else if (addr >= 0x700 && addr < 0xC00) {
        if (addr < 0x780) dsp_.coef[(addr - 0x700) / 2] = static_cast<int16_t>(v);
        else if (addr < 0x800) dsp_.madrs[((addr - 0x780) / 2) & 0x1F] = v; // mirrored twice
        else dsp_.mpro[(addr - 0x800) / 2] = v;
        if (addr == 0xBF0) dsp_start();
    }
}

void Scsp::write8(uint32_t addr, uint8_t v) {
    addr &= 0xFFF;
    if (addr >= 0x400 && addr < 0x430) { // don't read back registers made at read time
        uint16_t& w = regs_[(addr & 0x3E) / 2];
        if ((addr & 0x3E) == 0x20) { // SCIPD: only bit 5 (a CPU interrupt) can be set
            if (addr & 1) w = static_cast<uint16_t>(w | (v & 0x20));
            return;
        }
        w = (addr & 1) ? static_cast<uint16_t>((w & 0xFF00) | v) : static_cast<uint16_t>((w & 0x00FF) | v << 8);
        update_reg(addr & 0x3F);
        return;
    }
    uint16_t w = read16(addr);
    w = (addr & 1) ? static_cast<uint16_t>((w & 0xFF00) | v) : static_cast<uint16_t>((w & 0x00FF) | v << 8);
    write16(addr, w);
}

// ---- DSP ----

void Scsp::dsp_start() {
    dsp_.stopped = false;
    int i = 127;
    for (; i >= 0; --i) {
        const std::size_t s = static_cast<std::size_t>(i) * 4;
        if (dsp_.mpro[s] || dsp_.mpro[s + 1] || dsp_.mpro[s + 2] || dsp_.mpro[s + 3]) break;
    }
    dsp_.last_step = i + 1;
}

void Scsp::dsp_step() {
    Dsp& d = dsp_;
    if (d.stopped) {
        d.mixs.fill(0);
        return;
    }
    int32_t acc = 0, shifted = 0, x = 0, y = 0, b = 0, inputs = 0, memval = 0, frc_reg = 0, y_reg = 0;
    uint32_t adrs_reg = 0;
    d.efreg.fill(0);
    for (int step = 0; step < d.last_step; ++step) {
        const uint16_t* ip = &d.mpro[static_cast<std::size_t>(step) * 4];
        const uint32_t tra = (ip[0] >> 8) & 0x7F, twt = (ip[0] >> 7) & 1, twa = ip[0] & 0x7F;
        const uint32_t xsel = (ip[1] >> 15) & 1, ysel = (ip[1] >> 13) & 3, ira = (ip[1] >> 6) & 0x3F;
        const uint32_t iwt = (ip[1] >> 5) & 1, iwa = ip[1] & 0x1F;
        const uint32_t table = (ip[2] >> 15) & 1, mwt = (ip[2] >> 14) & 1, mrd = (ip[2] >> 13) & 1;
        const uint32_t ewt = (ip[2] >> 12) & 1, ewa = (ip[2] >> 8) & 0xF, adrl = (ip[2] >> 7) & 1;
        const uint32_t frcl = (ip[2] >> 6) & 1, shift = (ip[2] >> 4) & 3, yrl = (ip[2] >> 3) & 1;
        const uint32_t negb = (ip[2] >> 2) & 1, zero = (ip[2] >> 1) & 1, bsel = ip[2] & 1;
        const uint32_t nofl = (ip[3] >> 15) & 1, coef = (ip[3] >> 9) & 0x3F, masa = (ip[3] >> 2) & 0x1F;
        const uint32_t adreb = (ip[3] >> 1) & 1, nxadr = ip[3] & 1;

        if (ira <= 0x1F) inputs = d.mems[ira];
        else if (ira <= 0x2F) inputs = d.mixs[ira - 0x20] * 16; // MIXS is 20 bits
        else if (ira <= 0x31) inputs = d.exts[ira - 0x30] * 256;
        else break; // not an input: stop here (Supermodel returns)
        inputs = sign_extend24(inputs);

        if (iwt) {
            d.mems[iwa] = memval; // read by the previous MRD
            if (ira == iwa) inputs = memval;
        }
        const int32_t temp_r = sign_extend24(d.temp[(tra + d.dec) & 0x7F]);
        if (!zero) {
            b = bsel ? acc : temp_r;
            if (negb) b = -b;
        } else {
            b = 0;
        }
        x = xsel ? inputs : temp_r;
        if (ysel == 0) y = frc_reg;
        else if (ysel == 1) y = d.coef[coef] >> 3; // COEF is 13 bits
        else if (ysel == 2) y = (y_reg >> 11) & 0x1FFF;
        else y = (y_reg >> 4) & 0x0FFF;
        if (yrl) y_reg = inputs;

        if (shift == 0) shifted = std::clamp(acc, -0x800000, 0x7FFFFF);
        else if (shift == 1) shifted = std::clamp(acc * 2, -0x800000, 0x7FFFFF);
        else if (shift == 2) shifted = sign_extend24(acc * 2);
        else shifted = sign_extend24(acc);

        y = static_cast<int32_t>(static_cast<uint32_t>(y) << 19) >> 19;
        acc = static_cast<int32_t>((static_cast<int64_t>(x) * y) >> 12) + b;

        if (twt) d.temp[(twa + d.dec) & 0x7F] = shifted;
        if (frcl) frc_reg = shift == 3 ? shifted & 0x0FFF : (shifted >> 11) & 0x1FFF;

        if (mrd || mwt) {
            uint32_t addr = d.madrs[masa];
            if (!table) addr += d.dec;
            if (adreb) addr += adrs_reg & 0x0FFF;
            if (nxadr) ++addr;
            addr &= table ? 0xFFFF : d.rbl - 1;
            addr += d.rbp << 12;
            if (mrd && (step & 1)) // memory only on odd steps
                memval = nofl ? ram_word(addr) << 8 : unpack(ram_word(addr));
            if (mwt && (step & 1)) set_ram_word(addr, nofl ? static_cast<uint16_t>(shifted >> 8) : pack(shifted));
        }
        if (adrl) adrs_reg = shift == 3 ? (shifted >> 12) & 0xFFF : static_cast<uint32_t>(inputs >> 16);
        if (ewt) d.efreg[ewa] = static_cast<int16_t>(d.efreg[ewa] + (shifted >> 8));
    }
    --d.dec;
    d.mixs.fill(0);
}

// The chip's RAM is the sound board's to save. An LFO's tables are kept
// as the row of the table they point to.
void Scsp::state(State& s) {
    s.tag(master_ ? "scsp1" : "scsp2");
    auto row = [&s](const int*& p, const auto& rows) {
        int32_t i = -1;
        for (std::size_t k = 0; k < std::size(rows) && !s.loading(); ++k)
            if (p == rows[k]) i = static_cast<int32_t>(k);
        s.io(i);
        if (s.loading()) p = i >= 0 && static_cast<std::size_t>(i) < std::size(rows) ? rows[i] : nullptr;
    };
    const Tables& t = tables();
    s.io(regs_);
    for (Slot& slot : slots_) {
        s.io(slot.data), s.io(slot.active), s.io(slot.base), s.io(slot.cur), s.io(slot.nxt), s.io(slot.step);
        Eg& eg = slot.eg;
        s.io(slot.back), s.io(eg.volume), s.io(eg.state), s.io(eg.ar), s.io(eg.d1r), s.io(eg.d2r), s.io(eg.rr);
        s.io(eg.dl), s.io(eg.hold), s.io(eg.below_dl);
        for (Lfo* lfo : {&slot.plfo, &slot.alfo}) s.io(lfo->phase), s.io(lfo->step);
        row(slot.plfo.table, t.plfo), row(slot.plfo.scale, t.pscales);
        row(slot.alfo.table, t.alfo), row(slot.alfo.scale, t.ascales);
    }
    Dsp& d = dsp_;
    s.io(d.rbp), s.io(d.rbl), s.io(d.coef), s.io(d.madrs), s.io(d.mpro), s.io(d.temp), s.io(d.mems), s.io(d.dec);
    s.io(d.mixs), s.io(d.exts), s.io(d.efreg), s.io(d.stopped), s.io(d.last_step);
    s.io(ring_), s.io(bufptr_), s.io(noise_), s.io(timer_), s.io(mcieb_), s.io(mcipd_);
    s.io(midi_), s.io(midi_w_), s.io(midi_r_);
}

} // namespace m3
