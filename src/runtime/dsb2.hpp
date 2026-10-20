// Sega Digital Sound Board 2 (837-12273), the music board: a 68EC000 at
// 11 MHz (33 MHz / 3) runs a 128 KB program and drives an MPEG audio
// decoder that plays Layer II streams at 32 kHz from 16 MB of ROM. It
// listens on the same MIDI-rate serial line from the main board as the
// sound board. The 68000's map (Supermodel's Src/Model3/DSB.cpp and MAME's
// sega/dsb2.cpp agree):
//
//   000000  program ROM, 128 KB
//   C00001  8251 serial chip: received byte (C00003: status; a received
//           byte raises IRQ 1 until it is read)
//   D00001  system control (bit 3: MPEG ROM bank on boards with 32 MB)
//   E00003  the decoder's command port: start and end addresses, play,
//           stop, stereo mode and volume (Supermodel's decoding of them)
//   E80001  decoder status (reads 1)
//   F00000  RAM, 128 KB
//
// IRQ 2 is a 1 kHz timer (Supermodel: "fixes music fade outs").
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace m3 {

class Dsb2 {
public:
    static constexpr uint32_t kCpuHz = 11'000'000;
    static constexpr int kMpegRate = 32'000;
    static constexpr int kOutputRate = 44'100;
    static constexpr uint32_t kByteCycles = kCpuHz * 10 / 31'250; // a byte on the 31,250-baud line
    static constexpr uint32_t kTimerCycles = kCpuHz / 1000;

    Dsb2(std::span<const uint8_t> program, std::span<const uint8_t> mpeg);
    ~Dsb2();

    void reset();
    // A byte from the main board's serial line.
    void midi_in(uint8_t byte) { line_.push_back(byte); }
    // Runs the board for `frames` more output samples (44.1 kHz), writing its
    // music to `out` (interleaved stereo).
    void render(int16_t* out, std::size_t frames);

    // ---- Inspection ----
    uint32_t pc() const;
    bool playing() const { return playing_; }
    uint64_t tracks_started = 0, commands = 0;
    int music_peak = 0; // levels of the music, for reports
    double music_energy = 0;
    std::array<uint64_t, 8> irqs_taken{};
    std::string describe() const;

private:
    class Cpu;
    friend class Cpu;
    struct Decoder;

    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);
    void run_cpu(uint64_t until);
    void update_ipl();
    void interrupted(int level);

    void mpeg_command(uint8_t byte);
    void play(uint32_t start, uint32_t end, bool loop);
    bool decode();
    void next_sample(int32_t out[2]);

    std::span<const uint8_t> program_, mpeg_;
    std::vector<uint8_t> ram_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Decoder> decoder_;

    // Serial line and interrupts
    std::deque<uint8_t> line_;
    uint8_t rx_ = 0;
    bool rx_ready_ = false, timer_irq_ = false;
    uint64_t next_rx_ = 0, next_timer_ = kTimerCycles;
    uint8_t control_ = 0;

    // Decoder commands
    int state_ = 0;
    uint32_t start_ = 0, end_ = 0;
    enum class Stereo : uint8_t { Stereo, MonoLeft, MonoRight } stereo_ = Stereo::Stereo;
    std::array<uint8_t, 2> volume_{0xFF, 0xFF};

    // Player: the stream being decoded, its PCM, and 32 to 44.1 kHz
    bool playing_ = false, loop_ = false;
    uint32_t play_start_ = 0, play_end_ = 0, pos_ = 0;
    std::vector<int16_t> pcm_;
    int pcm_count_ = 0, pcm_pos_ = 0, pcm_channels_ = 2;
    int stream_layer_ = 0, stream_hz_ = 0; // of the last frame decoded
    std::array<int32_t, 2> prev_{}, next_{};
    uint32_t frac_ = 0; // 16.16 between prev_ and next_
    uint64_t samples_ = 0;
};

} // namespace m3
