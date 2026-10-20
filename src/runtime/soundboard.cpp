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

// The music board's output goes to the front with SCSP 1 (Supermodel: "the
// default DSB board is supposed to be plugged and mixed with the front
// output"), at the decoder's own volume. Not through the SCSPs' external
// inputs: Spikeout never sets slots 16/17's effect levels, which carry
// them, so the music would be silent.
void SoundBoard::render(int16_t* out, std::size_t frames, int channels) {
    auto clamp16 = [](int32_t v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); };
    music_.assign(2 * frames, 0);
    if (dsb_) dsb_->render(music_.data(), frames);
    for (std::size_t i = 0; i < frames; ++i) {
        int32_t fl, fr, rl, rr;
        scsp1_.sample(fl, fr);
        scsp2_.sample(rl, rr);
        if (sound_volume_ != 100) {
            fl = fl * sound_volume_ / 100;
            fr = fr * sound_volume_ / 100;
            rl = rl * sound_volume_ / 100;
            rr = rr * sound_volume_ / 100;
        }
        fl += music_[2 * i] * music_volume_ / 100;
        fr += music_[2 * i + 1] * music_volume_ / 100;
        if (channels == 4) {
            int16_t* o = out + 4 * i;
            o[0] = clamp16(fl);
            o[1] = clamp16(fr);
            o[2] = clamp16(rl);
            o[3] = clamp16(rr);
        } else {
            out[2 * i] = clamp16((fl + rl) / 2);
            out[2 * i + 1] = clamp16((fr + rr) / 2);
        }
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

// An access to the interrupt registers or the MIDI FIFO can change the
// interrupt line at once (an acknowledge, the last byte read), not only at
// the next sample: a handler that returns within the sample would otherwise
// be taken again.
void SoundBoard::after_scsp1_access(uint32_t a) {
    if ((a & 0xFFF) >= 0x400 && (a & 0xFFF) < 0x430) cpu_->setIPL(static_cast<moira::u8>(scsp1_.irq_level()));
}

uint8_t SoundBoard::read8(uint32_t a) {
    a &= 0xFFFFFF;
    switch (a >> 20) {
    case 0x0: return ram1_[a & (Scsp::kRamSize - 1)];
    case 0x1: {
        const uint8_t v = scsp1_.read8(a);
        after_scsp1_access(a);
        return v;
    }
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
    if ((a >> 20) == 0x1) {
        const uint16_t v = scsp1_.read16(a);
        after_scsp1_access(a);
        return v;
    }
    if ((a >> 20) == 0x3) return scsp2_.read16(a);
    return static_cast<uint16_t>(read8(a) << 8 | read8(a + 1));
}

void SoundBoard::write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    switch (a >> 20) {
    case 0x0: ram1_[a & (Scsp::kRamSize - 1)] = v; break;
    case 0x1:
        scsp1_.write8(a, v);
        after_scsp1_access(a);
        break;
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
    case 0x1:
        scsp1_.write16(a, v);
        after_scsp1_access(a);
        break;
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
