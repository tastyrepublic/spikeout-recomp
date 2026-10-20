// DSB2 music board: see dsb2.hpp. The decoder's command set follows
// Supermodel's Src/Model3/DSB.cpp (GPL-3.0-or-later, the Supermodel Team;
// based on code by R. Belmont); MPEG decoding is minimp3 (CC0).
#include "runtime/dsb2.hpp"

#include <Moira.h>
#include <minimp3.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace m3 {

struct Dsb2::Decoder {
    mp3dec_t dec;
};

class Dsb2::Cpu : public moira::Moira {
public:
    explicit Cpu(Dsb2& board) : board_(board) {}
    moira::u8 read8(moira::u32 a) const override { return board_.read8(a); }
    moira::u16 read16(moira::u32 a) const override { return board_.read16(a); }
    void write8(moira::u32 a, moira::u8 v) const override { board_.write8(a, v); }
    void write16(moira::u32 a, moira::u16 v) const override { board_.write16(a, v); }
    void willInterrupt(moira::u8 level) override { board_.interrupted(level & 7); }

private:
    Dsb2& board_;
};

Dsb2::Dsb2(std::span<const uint8_t> program, std::span<const uint8_t> mpeg)
    : program_(program), mpeg_(mpeg), ram_(0x20000, 0), cpu_(std::make_unique<Cpu>(*this)),
      decoder_(std::make_unique<Decoder>()), pcm_(MINIMP3_MAX_SAMPLES_PER_FRAME, 0) {
    reset();
}

Dsb2::~Dsb2() = default;

void Dsb2::reset() {
    const moira::i64 now = cpu_->getClock();
    cpu_->reset();
    cpu_->setClock(now);
    rx_ready_ = timer_irq_ = false;
    next_timer_ = static_cast<uint64_t>(now) + kTimerCycles;
    state_ = 0;
    start_ = end_ = 0;
    stereo_ = Stereo::Stereo;
    volume_ = {0xFF, 0xFF}; // full until the game sets it (Supermodel)
    playing_ = false;
    pcm_count_ = pcm_pos_ = 0;
    update_ipl();
}

uint32_t Dsb2::pc() const { return cpu_->getPC(); }

std::string Dsb2::describe() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "68000 PC %06x; %llu command bytes to the decoder, %llu tracks started, %s at %06x; "
                  "MPEG layer %d, %d Hz, %d channel(s)",
                  pc(), static_cast<unsigned long long>(commands), static_cast<unsigned long long>(tracks_started),
                  playing_ ? (loop_ ? "looping" : "playing") : "stopped", pos_, stream_layer_, stream_hz_, pcm_channels_);
    return buf;
}

// ---- 68000 side ----

uint8_t Dsb2::read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x20000) return a < program_.size() ? program_[a] : 0;
    if (a >= 0xF00000 && a < 0xF20000) return ram_[a & 0x1FFFF];
    switch (a) {
    case 0xC00001: // received byte
        rx_ready_ = false;
        update_ipl();
        return rx_;
    case 0xC00003: return static_cast<uint8_t>(0x05 | (rx_ready_ ? 0x02 : 0)); // TxRDY, TxEMPTY, RxRDY
    case 0xE80001: return 0x01;                                              // decoder status
    default: return 0;
    }
}

uint16_t Dsb2::read16(uint32_t a) {
    a &= 0xFFFFFE;
    return static_cast<uint16_t>(read8(a) << 8 | read8(a + 1));
}

void Dsb2::write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xF00000 && a < 0xF20000) {
        ram_[a & 0x1FFFF] = v;
        return;
    }
    switch (a) {
    case 0xD00001: control_ = v; break;
    case 0xE00003: mpeg_command(v); break;
    default: break; // 0xC00001/3: the serial chip's transmitter and set-up
    }
}

void Dsb2::write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    write8(a, static_cast<uint8_t>(v >> 8));
    write8(a + 1, static_cast<uint8_t>(v));
}

void Dsb2::update_ipl() {
    cpu_->setIPL(static_cast<moira::u8>(timer_irq_ ? 2 : rx_ready_ ? 1 : 0));
}

void Dsb2::interrupted(int level) {
    ++irqs_taken[static_cast<std::size_t>(level)];
    if (level == 2) { // the timer's line is released when the 68000 takes it
        timer_irq_ = false;
        update_ipl();
    }
}

void Dsb2::run_cpu(uint64_t until) {
    for (;;) {
        const auto now = static_cast<uint64_t>(cpu_->getClock());
        if (now >= next_timer_) {
            timer_irq_ = true;
            next_timer_ += kTimerCycles;
            update_ipl();
        }
        if (!rx_ready_ && !line_.empty() && now >= next_rx_) { // the next byte off the line
            rx_ = line_.front();
            line_.pop_front();
            rx_ready_ = true;
            next_rx_ = now + kByteCycles;
            update_ipl();
        }
        if (now >= until) return;
        uint64_t stop = std::min(until, next_timer_);
        if (!rx_ready_ && !line_.empty()) stop = std::min(stop, std::max(next_rx_, now + 1));
        cpu_->executeUntil(static_cast<moira::i64>(stop));
    }
}

// ---- Decoder commands (Supermodel's decoding) ----

namespace {
enum : int {
    kIdle,
    kStart0, kStart1, kStart2, // 0x14/0x15: start (or, while playing, loop) address
    kEnd0, kEnd1, kEnd2,       // 0x24/0x25: end address
    kMonoLeft,                 // 0xA0 n: left channel on both sides (n != 0)
    kMonoRight,                // 0xB1 n
    kVolumeLeft,               // 0xB0 / 0xB6 n
    kVolumeRight,              // 0xA1 / 0xA7 n
    kPlayIf75,                 // 0xA4 0x75: play
    kStopIf96,                 // 0xB4 0x96: stop
    kSkip,                     // 0xA3, 0xA5, 0xB2, 0xB5: one byte, meaning unknown
};
} // namespace

void Dsb2::play(uint32_t start, uint32_t end, bool loop) {
    end = std::min<uint32_t>(end, static_cast<uint32_t>(mpeg_.size()));
    if (start >= end) {
        playing_ = false;
        return;
    }
    mp3dec_init(&decoder_->dec);
    play_start_ = start;
    play_end_ = end;
    pos_ = start;
    loop_ = loop;
    playing_ = true;
    pcm_count_ = pcm_pos_ = 0;
    ++tracks_started;
}

void Dsb2::mpeg_command(uint8_t byte) {
    ++commands;
    switch (state_) {
    case kIdle:
        switch (byte) {
        case 0x14: case 0x15: state_ = kStart0; break;
        case 0x24: case 0x25: state_ = kEnd0; break;
        case 0x74: case 0x75: play(start_, end_, false); break;
        case 0x84: case 0x85: playing_ = false; break;
        case 0xA0: state_ = kMonoLeft; break;
        case 0xB1: state_ = kMonoRight; break;
        case 0xB0: case 0xB6: state_ = kVolumeLeft; break;
        case 0xA1: case 0xA7: state_ = kVolumeRight; break;
        case 0xA4: state_ = kPlayIf75; break;
        case 0xB4: state_ = kStopIf96; break;
        case 0xA3: case 0xA5: case 0xB2: case 0xB5: state_ = kSkip; break;
        default: break;
        }
        return;
    case kStart0: start_ = (start_ & 0x00FFFF) | static_cast<uint32_t>(byte) << 16; ++state_; return;
    case kStart1: start_ = (start_ & 0xFF00FF) | static_cast<uint32_t>(byte) << 8; ++state_; return;
    case kStart2:
        start_ = (start_ & 0xFFFF00) | byte;
        if (playing_) { // a new start while playing: the loop, from there to the end address
            play_start_ = start_;
            play_end_ = std::max(start_, std::min<uint32_t>(end_, static_cast<uint32_t>(mpeg_.size())));
            loop_ = true;
        }
        break;
    case kEnd0: end_ = (end_ & 0x00FFFF) | static_cast<uint32_t>(byte) << 16; ++state_; return;
    case kEnd1: end_ = (end_ & 0xFF00FF) | static_cast<uint32_t>(byte) << 8; ++state_; return;
    case kEnd2:
        end_ = (end_ & 0xFFFF00) | byte;
        stereo_ = Stereo::Stereo;
        break;
    case kMonoLeft: stereo_ = byte ? Stereo::MonoLeft : Stereo::Stereo; break;
    case kMonoRight: stereo_ = byte ? Stereo::MonoRight : Stereo::Stereo; break;
    case kVolumeLeft: volume_[0] = byte; break;
    case kVolumeRight: volume_[1] = byte; break;
    case kPlayIf75:
        if (byte == 0x75) play(start_, end_, false);
        break;
    case kStopIf96:
        if (byte == 0x96) playing_ = false;
        break;
    default: break;
    }
    state_ = kIdle;
}

// ---- Playback ----

bool Dsb2::decode() {
    for (int tries = 0; playing_ && tries < 8; ++tries) {
        if (pos_ + 4 >= play_end_) { // end of the stream
            if (!loop_) break;
            pos_ = play_start_;
        }
        mp3dec_frame_info_t info{};
        const int n = mp3dec_decode_frame(&decoder_->dec, mpeg_.data() + pos_, static_cast<int>(play_end_ - pos_),
                                          pcm_.data(), &info);
        if (info.frame_bytes <= 0) { // no frame in what is left
            if (!loop_) break;
            pos_ = play_start_;
            continue;
        }
        pos_ += static_cast<uint32_t>(info.frame_bytes);
        if (n > 0) {
            pcm_count_ = n;
            pcm_pos_ = 0;
            pcm_channels_ = std::max(1, info.channels);
            stream_layer_ = info.layer;
            stream_hz_ = info.hz;
            return true;
        }
    }
    playing_ = false;
    return false;
}

void Dsb2::next_sample(int32_t out[2]) {
    if (pcm_pos_ >= pcm_count_ && !(playing_ && decode())) {
        out[0] = out[1] = 0;
        return;
    }
    const int16_t* s = &pcm_[static_cast<std::size_t>(pcm_pos_ * pcm_channels_)];
    ++pcm_pos_;
    const int32_t l = s[0], r = s[pcm_channels_ - 1];
    switch (stereo_) {
    case Stereo::Stereo:
        out[0] = l * volume_[0] / 255;
        out[1] = r * volume_[1] / 255;
        break;
    case Stereo::MonoLeft: out[0] = out[1] = l * volume_[0] / 255; break;
    case Stereo::MonoRight: out[0] = out[1] = r * volume_[1] / 255; break;
    }
}

void Dsb2::render(int16_t* out, std::size_t frames) {
    constexpr uint32_t kStep = static_cast<uint32_t>((uint64_t{kMpegRate} << 16) / kOutputRate);
    for (std::size_t i = 0; i < frames; ++i) {
        ++samples_;
        run_cpu(samples_ * kCpuHz / kOutputRate);
        for (int c = 0; c < 2; ++c) {
            const int32_t v = prev_[static_cast<std::size_t>(c)] +
                              static_cast<int32_t>((static_cast<int64_t>(next_[static_cast<std::size_t>(c)] -
                                                                         prev_[static_cast<std::size_t>(c)]) *
                                                    frac_) >> 16);
            music_peak = std::max(music_peak, std::abs(v));
            music_energy += static_cast<double>(v) * v;
            out[2 * i + static_cast<std::size_t>(c)] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }
        frac_ += kStep;
        while (frac_ >= 0x10000) { // 32 kHz to 44.1 kHz, linear
            frac_ -= 0x10000;
            prev_ = next_;
            next_sample(next_.data());
        }
    }
}

} // namespace m3
