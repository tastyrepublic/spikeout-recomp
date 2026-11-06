// What gamerun and the spikeout frontend share: the recompiled functions in
// one table, and the RTC set to the host's clock.
#pragma once

#include "games/spikeofe_hooks.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/netboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/soundboard.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
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
#include <optional>
#include <span>
#include <cstring>
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
// A save state file (one board's, the window's F9): a header, then the state (board_state's). The header (the owner's
// choice, 2026-11-04: saves kept across updates where they can be): "SPKSTATE", its version (2), the state's format
// (kStateFormat) and the recompiled game code's fingerprint (code_id), its frame. Same format and code: it loads as is.
// Either different (an update that changed a state's layout or the game's code), or none known (a file from before the
// header): it may still load, offered with a warning, tried on a spare board first (a resume point that moved can't be
// seen there: the undo then). The rest of an update (the window, network play, the menu) keeps saves as they are.
// kStateFormat: raised whenever what board_state writes changes shape (a device's state, a section).
// 2: widescreen's tile sides 26 columns a side (21:9), 12 before (format 1's still load: m3::TileSides::state).
inline constexpr uint64_t kStateFormat = 2;
inline uint64_t code_id() {
    static const uint64_t id = [] {
        uint64_t h = 0xCBF29CE484222325ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
        for (const auto& table : {game_functions(), game_blocks()}) {
            mix(table.size());
            for (const recomp::FunctionEntry& e : table) mix(e.addr), mix(e.resume);
        }
        return h;
    }();
    return id;
}
struct StateFile {
    std::optional<uint64_t> format, code; // (none: made before the header, or its first version's)
    uint64_t frame = 0;
    std::vector<uint8_t> state;
    bool matches() const { return format == kStateFormat && code == code_id(); } // (loads as it is)
};
inline constexpr char kStateMagic[8] = {'S', 'P', 'K', 'S', 'T', 'A', 'T', 'E'};
// (the state's checksum, in the header: damage anywhere in it found, its RAM's too, where any bytes would load)
inline uint64_t state_checksum(std::span<const uint8_t> state) {
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0;
    std::size_t i = 0;
    for (; i + 8 <= state.size(); i += 8) {
        uint64_t v = 0;
        std::memcpy(&v, state.data() + i, 8);
        a = (a ^ v) * 0x100000001B3ull, b += a;
    }
    for (; i < state.size(); ++i) a = (a ^ state[i]) * 0x100000001B3ull, b += a;
    return a ^ (b * 0xC2B2AE3D27D4EB4Full) ^ state.size();
}
inline std::vector<uint8_t> state_file_bytes(uint64_t frame, const std::vector<uint8_t>& state) {
    std::vector<uint8_t> out(kStateMagic, kStateMagic + 8);
    auto u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    u64(2), u64(kStateFormat), u64(code_id()), u64(frame), u64(state_checksum(state));
    out.insert(out.end(), state.begin(), state.end());
    return out;
}
inline StateFile read_state_file(std::vector<uint8_t> bytes) {
    StateFile f;
    if (bytes.size() < 32 || !std::equal(kStateMagic, kStateMagic + 8, bytes.begin())) {
        f.state = std::move(bytes);
        return f;
    }
    auto u64 = [&](std::size_t at) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t{bytes[at + static_cast<std::size_t>(i)]} << (8 * i);
        return v;
    };
    const uint64_t version = u64(8);
    std::size_t at = 0;
    if (version == 1) { // (the first: its build, then its frame: neither format nor code known)
        f.frame = u64(24), at = 32;
    } else if (version == 2 && bytes.size() >= 48) {
        f.format = u64(16), f.code = u64(24), f.frame = u64(32), at = 48;
        if (state_checksum(std::span<const uint8_t>(bytes).subspan(at)) != u64(40))
            throw std::runtime_error("the save state is damaged (its checksum differs)");
    } else {
        throw std::runtime_error("a save state of a newer version");
    }
    f.state.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    return f;
}

// About a board's state, with its sound: the room a save makes for it at once.
constexpr std::size_t kBoardStateBytes = 30u << 20;

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
    // Whether board i is the only one still running.
    bool alone(std::size_t i) {
        std::lock_guard lock(mutex_);
        return !done_[i] && running() == 1;
    }
    // Whether board i has stopped.
    bool finished(std::size_t i) {
        std::lock_guard lock(mutex_);
        return done_[i];
    }
    // Parallel: whether board i has come to the barrier the others are on their way to (or stopped): its part
    // before it done.
    bool waiting(std::size_t i) {
        if (!parallel_) return true; // (taking turns: only the board whose turn it is runs)
        std::lock_guard lock(mutex_);
        return done_[i] || at_barrier_[i];
    }

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
                at_barrier_[i] = true;
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
        std::fill(at_barrier_.begin(), at_barrier_.end(), false);
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
    std::vector<bool> at_barrier_ = std::vector<bool>(done_.size(), false); // (waiting)
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

// A save state of linked boards (gamerun --save-state / --load-state, and
// network play's states for a player joining a game in progress): one file
// for all of them,
// each board's state (saved at its own point, as a board alone) and the
// ring's part, the packets on their way between those points
// (m3::LinkRing::cut_state), written once every board has saved and all
// are stopped: at the slices' meeting when running at once, else by the
// last board to save (the others waiting their turn).
struct LinkedState {
    m3::LinkRing* ring = nullptr;
    std::string path;
    std::mutex lock;
    std::vector<std::vector<uint8_t>> boards; // each board's, as it saves (or as the file has them)
    std::vector<std::vector<uint8_t>> loaded; // --load-state: each board's to load
    std::size_t saved = 0;
    bool written = false;
    // Network play, the host: every board's state at the end of
    // `capture_frame`, as a machine joining the game (or out of step) asked
    // for it, handed to `sink` (again each time asked).
    std::atomic<uint64_t> capture_frame{0};
    std::function<void(uint64_t, std::vector<uint8_t>)> sink;
    // Or its parts, put together by the receiver (on another thread: 80 MB copied once more, on the game thread a
    // pause at each join): the ring's cut and each board's state, for `assemble`.
    std::function<void(uint64_t, std::vector<uint8_t>, std::vector<std::vector<uint8_t>>)> parts_sink;
    // The state those parts make (as io writes it).
    static std::vector<uint8_t> assemble(const std::vector<uint8_t>& cut_bytes, std::vector<std::vector<uint8_t>>& parts) {
        std::size_t total = cut_bytes.size() + 4096;
        for (const auto& b : parts) total += b.size() + 8;
        m3::State s(total);
        s.tag("spikeout linked save state 1");
        s.bytes(const_cast<uint8_t*>(cut_bytes.data()), cut_bytes.size());
        for (auto& b : parts) s.io(b);
        s.tag("end");
        return std::move(s.data());
    }
    // The window's F9 in network play: this capture written to `path`
    // rather than handed to `sink` (then ready for the next capture). Set
    // before capture_frame.
    bool to_file = false;

    // (rollback: the ring's part of the frame, cut where the boards met: tools::rollback::Linked::between)
    std::vector<uint8_t> cut;

    void finish() {
        {
            std::lock_guard guard(lock);
            if (written || saved < boards.size() || (!ring->cut_complete() && cut.empty())) return;
            written = true;
        }
        if (parts_sink && !to_file) { // (put together elsewhere: the ring's cut taken here, where the boards met)
            if (cut.empty()) {
                m3::State c;
                ring->cut_state(c);
                cut = std::move(c.data());
            }
            parts_sink(capture_frame, std::exchange(cut, {}), std::exchange(boards, std::vector<std::vector<uint8_t>>(boards.size())));
            std::lock_guard guard(lock);
            capture_frame = 0, saved = 0, written = false;
            return;
        }
        std::size_t total = cut.size() + 4096; // (the whole of it at once: grown, 80 MB were copied over and over)
        for (const auto& b : boards) total += b.size() + 8;
        m3::State s(total);
        io(s);
        if (sink && !to_file) {
            sink(capture_frame, std::move(s.data()));
            std::lock_guard guard(lock);
            capture_frame = 0, saved = 0, written = false;
            boards.assign(boards.size(), {});
            return;
        }
        if (tools::write_file(path, s.data()))
            std::printf("Saved %s: %zu linked boards, %zu bytes\n", path.c_str(), boards.size(), s.data().size());
        else
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
        if (sink) { // (the window: ready for the next capture)
            std::lock_guard guard(lock);
            to_file = false;
            capture_frame = 0, saved = 0, written = false;
            boards.assign(boards.size(), {});
        }
        std::fflush(stdout);
    }
    // The file: saving or loading (before the boards run).
    void io(m3::State& s) {
        s.tag("spikeout linked save state 1");
        if (!s.loading() && !cut.empty()) s.bytes(cut.data(), cut.size()); // (cut already)
        else ring->cut_state(s);
        cut.clear();
        for (auto& b : boards) s.io(b);
        s.tag("end");
        if (s.loading() && !s.done()) throw std::runtime_error("save state: longer than this build's");
    }
};

} // namespace tools
