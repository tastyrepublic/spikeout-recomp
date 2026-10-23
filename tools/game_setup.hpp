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
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace generated {
std::span<const recomp::FunctionEntry> boot_rom_functions();
std::span<const recomp::FunctionEntry> game_ram_functions();
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

// The RTC shows the host's local time when the run starts.
inline void set_rtc_to_host(m3::Bus& bus) {
    const std::time_t t = std::time(nullptr);
    if (const std::tm* lt = std::localtime(&t))
        bus.rtc().set_time(
            m3::Rtc72421::to_seconds({lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec}),
            0);
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

private:
    double due_ = 0;
    std::vector<int16_t> buffer_;
};

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
