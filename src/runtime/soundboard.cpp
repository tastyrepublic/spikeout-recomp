// Model 3 sound board: see soundboard.hpp. The memory map and reset follow
// Supermodel's Src/Model3/SoundBoard.cpp (GPL-3.0-or-later, the Supermodel
// Team), with MAME's 512 KB of RAM per SCSP.
#include "runtime/soundboard.hpp"

#include <Moira.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace m3 {

class SoundBoard::Cpu : public moira::Moira {
public:
    explicit Cpu(SoundBoard& board) : board_(board) {}
    moira::u8 read8(moira::u32 a) const override { return board_.read8(a); }
    moira::u16 read16(moira::u32 a) const override { return board_.read16(a); }
    void write8(moira::u32 a, moira::u8 v) const override { board_.write8(a, v); }
    void write16(moira::u32 a, moira::u16 v) const override { board_.write16(a, v); }
    void willInterrupt(moira::u8 level) override { ++board_.irqs_taken[level & 7]; }

private:
    SoundBoard& board_;
};

SoundBoard::SoundBoard(std::span<const uint8_t> program, std::span<const uint8_t> samples)
    : program_(program), samples_rom_(samples), ram1_(Scsp::kRamSize, 0), ram2_(Scsp::kRamSize, 0),
      scsp1_(ram1_, true), scsp2_(ram2_, false), cpu_(std::make_unique<Cpu>(*this)) {
    reset();
}

SoundBoard::~SoundBoard() = default;

void SoundBoard::reset() {
    std::copy_n(program_.begin(), std::min<std::size_t>(16, program_.size()), ram1_.begin()); // the vectors
    ctrl_ = 0;
    cpu_->reset(); // fetches SSP and PC from address 0
    cpu_->setClock(static_cast<moira::i64>(samples_ * kCyclesPerSample));
}

uint32_t SoundBoard::pc() const { return cpu_->getPC(); }
uint64_t SoundBoard::cpu_cycles() const { return static_cast<uint64_t>(cpu_->getClock()); }

std::string SoundBoard::describe() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "68000 PC %06x, %.1f s run; %llu MIDI bytes in; IRQs taken by level %llu/%llu/%llu/%llu/%llu/%llu",
                  pc(), static_cast<double>(samples_) / Scsp::kSampleRate, static_cast<unsigned long long>(midi_bytes),
                  static_cast<unsigned long long>(irqs_taken[1]), static_cast<unsigned long long>(irqs_taken[2]),
                  static_cast<unsigned long long>(irqs_taken[3]), static_cast<unsigned long long>(irqs_taken[4]),
                  static_cast<unsigned long long>(irqs_taken[5]), static_cast<unsigned long long>(irqs_taken[6]));
    return buf;
}

void SoundBoard::midi_in(uint8_t byte) {
    ++midi_bytes;
    scsp1_.midi_in(byte);
    if (dsb_) dsb_->midi_in(byte);
}

void SoundBoard::render(int16_t* out, std::size_t frames) {
    // The music board's output goes into both SCSPs' external inputs, where
    // the game sets its level (MAME: "should be chained with SCSP EXTS").
    music_.assign(2 * frames, 0);
    if (dsb_) dsb_->render(music_.data(), frames);
    for (std::size_t i = 0; i < frames; ++i) {
        int32_t l1, r1, l2, r2;
        scsp1_.sample(l1, r1, music_[2 * i], music_[2 * i + 1]);
        scsp2_.sample(l2, r2, music_[2 * i], music_[2 * i + 1]);
        out[2 * i] = static_cast<int16_t>(std::clamp(l1 + l2, -32768, 32767));
        out[2 * i + 1] = static_cast<int16_t>(std::clamp(r1 + r2, -32768, 32767));
        scsp1_.tick_timers();
        cpu_->setIPL(static_cast<moira::u8>(scsp1_.irq_level()));
        cpu_->executeUntil(static_cast<moira::i64>((samples_ + 1) * kCyclesPerSample));
        ++samples_;
    }
}

// ---- 68000 side ----

const uint8_t* SoundBoard::sample_bank() const {
    const std::size_t offset = (ctrl_ & 0x10) ? 0x800000 : 0;
    return samples_rom_.data() + (samples_rom_.size() > offset ? offset : 0);
}

uint8_t SoundBoard::read8(uint32_t a) {
    a &= 0xFFFFFF;
    switch (a >> 20) {
    case 0x0: return ram1_[a & (Scsp::kRamSize - 1)];
    case 0x1: return scsp1_.read8(a);
    case 0x2: return ram2_[a & (Scsp::kRamSize - 1)];
    case 0x3: return scsp2_.read8(a);
    case 0x6: return program_.empty() ? 0 : program_[a & (program_.size() - 1)];
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF: {
        const std::size_t off = a & 0x7FFFFF;
        const std::size_t base = static_cast<std::size_t>(sample_bank() - samples_rom_.data());
        return base + off < samples_rom_.size() ? sample_bank()[off] : 0;
    }
    default: return 0;
    }
}

uint16_t SoundBoard::read16(uint32_t a) {
    a &= 0xFFFFFE;
    if ((a >> 20) == 0x1) return scsp1_.read16(a);
    if ((a >> 20) == 0x3) return scsp2_.read16(a);
    return static_cast<uint16_t>(read8(a) << 8 | read8(a + 1));
}

void SoundBoard::write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    switch (a >> 20) {
    case 0x0: ram1_[a & (Scsp::kRamSize - 1)] = v; break;
    case 0x1: scsp1_.write8(a, v); break;
    case 0x2: ram2_[a & (Scsp::kRamSize - 1)] = v; break;
    case 0x3: scsp2_.write8(a, v); break;
    case 0x4:
        if (a == 0x400001) ctrl_ = v;
        break;
    default: break;
    }
}

void SoundBoard::write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    switch (a >> 20) {
    case 0x1: scsp1_.write16(a, v); break;
    case 0x3: scsp2_.write16(a, v); break;
    case 0x4:
        if (a == 0x400000) ctrl_ = static_cast<uint8_t>(v);
        break;
    default:
        write8(a, static_cast<uint8_t>(v >> 8));
        write8(a + 1, static_cast<uint8_t>(v));
        break;
    }
}

} // namespace m3
