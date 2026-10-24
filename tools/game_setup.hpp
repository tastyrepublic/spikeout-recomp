// What gamerun and the spikeout frontend share: the recompiled functions in
// one table, and the RTC set to the host's clock.
#pragma once

#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/rom.hpp"
#include "runtime/soundboard.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace generated {
std::span<const recomp::FunctionEntry> boot_rom_functions();
std::span<const recomp::FunctionEntry> game_ram_functions();
std::span<const recomp::FunctionEntry> boot_rom_blocks();
std::span<const recomp::FunctionEntry> game_ram_blocks();
} // namespace generated

namespace tools {

// One table, sorted: the RAM program's addresses are all below the boot ROM's.
inline std::vector<recomp::FunctionEntry> game_functions() {
    std::vector<recomp::FunctionEntry> functions;
    for (const auto& f : generated::game_ram_functions()) functions.push_back(f);
    for (const auto& f : generated::boot_rom_functions()) functions.push_back(f);
    std::sort(functions.begin(), functions.end(),
              [](const recomp::FunctionEntry& a, const recomp::FunctionEntry& b) { return a.addr < b.addr; });
    return functions;
}

// Every other block start, sorted, for save states (Context::set_blocks).
inline std::vector<recomp::FunctionEntry> game_blocks() {
    std::vector<recomp::FunctionEntry> blocks;
    for (const auto& b : generated::game_ram_blocks())
        if (b.fn) blocks.push_back(b);
    for (const auto& b : generated::boot_rom_blocks())
        if (b.fn) blocks.push_back(b);
    std::sort(blocks.begin(), blocks.end(),
              [](const recomp::FunctionEntry& a, const recomp::FunctionEntry& b) { return a.addr < b.addr; });
    return blocks;
}

// The RTC shows the host's local time when the run starts; returns it (as
// Rtc72421::set_time's seconds: a recording keeps it, to replay the same).
inline int64_t set_rtc_to_host(m3::Bus& bus) {
    const std::time_t t = std::time(nullptr);
    int64_t seconds = 0;
    if (const std::tm* lt = std::localtime(&t))
        seconds =
            m3::Rtc72421::to_seconds({lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec});
    bus.rtc().set_time(seconds, 0);
    return seconds;
}

// The music board (DSB2) on the sound board's serial line.
inline void attach_music(m3::SoundBoard& board, const m3::RomSet& roms) {
    board.attach_music(std::make_unique<m3::Dsb2>(roms.region(m3::RomRegionId::MpegProgram),
                                                  roms.region(m3::RomRegionId::MpegMusic)));
}

// The sound board, run a frame behind the main board for that frame's share
// of 44.1 kHz samples, each MIDI byte arriving at the same point of its
// frame as it reached the end of the serial line.
class FrameSound {
public:
    explicit FrameSound(const m3::RomSet& roms)
        : board(roms.region(m3::RomRegionId::SoundProgram), roms.region(m3::RomRegionId::SoundSamples)) {
        attach_music(board, roms);
    }

    // Takes the MIDI bytes that arrived during the frame from cycle `start`
    // (`cycles` long; the list is emptied); returns that frame's
    // interleaved stereo samples (766 or 767).
    std::span<const int16_t> frame(std::vector<m3::SerialByte>& midi, uint64_t start, uint64_t cycles) {
        due_ += static_cast<double>(m3::Scsp::kSampleRate) / recomp::FrameTiming::kRefreshHz;
        const auto n = static_cast<std::size_t>(due_);
        due_ -= static_cast<double>(n);
        buffer_.resize(2 * n);
        std::size_t done = 0;
        for (uint8_t b : waiting_) board.midi_in(b); // (a state saved by the window: bytes it hadn't played yet)
        waiting_.clear();
        for (const m3::SerialByte& b : midi) {
            const uint64_t into = b.cycle > start ? b.cycle - start : 0;
            const auto at = static_cast<std::size_t>(std::min<uint64_t>(n, into * n / cycles));
            if (at > done) {
                board.render(buffer_.data() + 2 * done, at - done);
                done = at;
            }
            board.midi_in(b.value);
        }
        midi.clear();
        board.render(buffer_.data() + 2 * done, n - done);
        return buffer_;
    }

    m3::SoundBoard board;

    // Save states: the board, and MIDI bytes that have reached it but not
    // yet been played (none here: each frame plays all of its own). The
    // window's sound, on the audio device's clock, keeps the same.
    void state(m3::State& s) {
        board.state(s);
        s.io(due_);
        s.io(waiting_);
    }

private:
    double due_ = 0;
    std::vector<uint8_t> waiting_;
    std::vector<int16_t> buffer_;
};

// A save state of a board: its CPU and frame timing, the bus with its
// devices (the network board's too) and the sound board (`sound`: as
// FrameSound::state walks it; none for a board run without sound, when a
// state with sound skips it). Saved in Context::request_snapshot's call,
// and loaded before the run, which then starts with
// Context::resume_block(cpu.pc). Throws if a state doesn't fit (another
// build, or another board set-up).
inline void board_state(m3::State& s, recomp::Context& c, recomp::FrameTiming& timing,
                        const std::function<void(m3::State&)>& sound) {
    s.tag("spikeout save state 1");
    c.state(s);
    timing.state(s);
    c.bus.state(s);
    bool with_sound = static_cast<bool>(sound);
    s.io(with_sound);
    if (with_sound && sound) sound(s);
    else if (with_sound) return; // (loading: the rest is the sound, not wanted)
    s.tag("end");
    if (s.loading() && !s.done()) throw std::runtime_error("save state: longer than this build's");
}

inline bool write_file(const std::string& path, std::span<const uint8_t> data) {
    std::ofstream out(path, std::ios::binary);
    return static_cast<bool>(out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())));
}

inline std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Linked boards in one process, each on its own thread, run one at a time
// in order: each runs until it passes the turn on, then waits for it to
// come back. A board that has stopped is skipped. Only one runs at a time,
// so a run with the same inputs is the same every time.
//
// Parallel: all of them run at once, and pass() is a barrier: each waits
// there until the others (those still running) have come to it; the last
// to come runs `between` (the link's packets on: m3::LinkRing::exchange)
// before they all go on. With the link's packets held until then, a run is
// the same every time here too.
class Turns {
public:
    explicit Turns(std::size_t boards, bool parallel = false, std::function<void()> between = {})
        : done_(boards, false), parallel_(parallel), between_(std::move(between)) {}
    std::size_t boards() const { return done_.size(); }

    void wait(std::size_t i) {
        if (parallel_) return; // (all start together)
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return turn_ == i; });
    }
    void pass(std::size_t i) {
        if (parallel_) {
            std::unique_lock lock(mutex_);
            const uint64_t round = round_;
            if (++arrived_ == running()) {
                release();
                lock.unlock();
                changed_.notify_all();
            } else {
                changed_.wait(lock, [&] { return round_ != round; });
            }
            return;
        }
        {
            std::lock_guard lock(mutex_);
            advance(i);
        }
        changed_.notify_all();
        wait(i);
    }
    void finish(std::size_t i) {
        {
            std::lock_guard lock(mutex_);
            done_[i] = true;
            if (parallel_) {
                if (arrived_ > 0 && arrived_ == running()) release(); // the others were waiting on this one
            } else {
                advance(i);
            }
        }
        changed_.notify_all();
    }

private:
    std::size_t running() const { return static_cast<std::size_t>(std::count(done_.begin(), done_.end(), false)); }
    void release() { // (the lock held)
        if (between_) between_();
        arrived_ = 0;
        ++round_;
    }
    void advance(std::size_t i) {
        for (std::size_t k = 1; k <= done_.size(); ++k) {
            const std::size_t next = (i + k) % done_.size();
            if (!done_[next]) {
                turn_ = next;
                return;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t turn_ = 0;
    std::vector<bool> done_;
    bool parallel_ = false;
    std::function<void()> between_;
    std::size_t arrived_ = 0; // parallel: at the barrier this round
    uint64_t round_ = 0;
};

// The link setting (0 stand-alone, 1 master, 2 slave) is the byte at
// 0x10019E, read at start-up from the EEPROM settings; until the EEPROM
// word for it is known, linked runs write it over the first frames.
constexpr uint32_t kLinkSettingAddr = 0x10019E;
constexpr uint64_t kLinkSettingFrames = 3000;

} // namespace tools
