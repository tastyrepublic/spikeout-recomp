// Model 3 frame timing, after Supermodel's RunMainBoardFrame (Model3.cpp):
// 57.524160 Hz, 424 lines per frame. A frame starts with 40 lines of
// VBlank, which end with IRQ 0x0D, then 384 display lines. At the start of
// each display line: the Real3D's ping-pong bit flips on the line in
// Tilegen register 0x08 (read when VBlank ends), IRQ 0x02 comes on line
// 383, and the tile generator draws the line (`on_display_line`, after the
// other two, as in Supermodel). At the end of the frame the network board
// runs its frame, then `on_frame_end`.
//
// MIDI: the sound link's serial chip (`m3::Usart8251` in the bus) has its
// own events, taken in time order with the frame's. With the bus's
// `midi_burst` (Supermodel's timing) instead: at the start of VBlank, while
// the sound port asks for it (control bit 0x20) and IRQ 0x40 is enabled,
// IRQ 0x40 is raised every 1000 cycles, at most 129 times (Supermodel: it
// runs 1000 PowerPC cycles after each), and the game sends the sound board
// a byte for each.
//
// Not modelled yet: running VBlank until IRQ 0x02 is acknowledged (which
// Supermodel does before the MIDI IRQs).
#pragma once

#include "recomp/context.hpp"

#include <cstdint>
#include <functional>

namespace recomp {

class FrameTiming : public Scheduler {
public:
    static constexpr double kRefreshHz = 57.524160;
    static constexpr uint32_t kLines = 424;
    static constexpr uint32_t kVBlankLines = 40;
    static constexpr uint32_t kDisplayLines = 384;
    static constexpr uint32_t kDisplayIrqLine = 383; // of the display lines
    static constexpr uint64_t kMidiIrqCycles = 1000;
    static constexpr uint32_t kMaxMidiIrqs = 129;

    explicit FrameTiming(uint64_t cpu_hz = ppc::CpuState::kCoreClockHz);

    uint64_t update(Context& c) override;

    uint64_t frames() const { return frames_; } // completed
    uint64_t frame_cycles() const { return frame_cycles_; }
    uint64_t line_cycles() const { return line_cycles_; }
    uint64_t frame_start(uint64_t frame) const { return frame * frame_cycles_; } // frame 0 starts at cycle 0

    void state(m3::State& s); // save states (runtime/state.hpp)
    // Test aid (gamerun --wait-seat): `frames` more frames counted as gone by, as if the board
    // had been stopped that long between two frames.
    void move_on(uint64_t frames) {
        frame_start_ += frames * frame_cycles_;
        frames_ += frames;
    }

    // Called from inside the run (on the thread running the game).
    std::function<void(uint32_t line)> on_display_line; // start of display line 0-383
    std::function<void()> on_frame_end;

private:
    uint64_t due() const;

    uint64_t frame_cycles_, line_cycles_, vblank_cycles_;
    uint64_t frame_start_ = 0;
    uint64_t frames_ = 0;
    uint32_t flip_line_ = 0; // display line of this frame's ping-pong flip (>= 384: none)
    uint32_t line_ = 0;      // next display line
    uint32_t midi_irqs_ = 0; // raised this VBlank
    enum class Next { Midi, VBlankEnd, Line, FrameEnd } next_ = Next::Midi;
};

} // namespace recomp
