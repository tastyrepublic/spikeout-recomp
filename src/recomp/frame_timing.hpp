// Model 3 frame timing, after Supermodel's RunMainBoardFrame (Model3.cpp):
// 57.524160 Hz, 424 lines per frame. A frame starts with 40 lines of
// VBlank, which end with IRQ 0x0D, then 384 display lines. The Real3D's
// ping-pong bit flips at the start of the display line in Tilegen register
// 0x08 (read when VBlank ends), and IRQ 0x02 comes at the start of line 383.
// At the end of the frame the network board runs its frame.
//
// Not modelled yet: running VBlank until IRQ 0x02 is acknowledged, and the
// sound board's MIDI IRQ (0x40).
#pragma once

#include "recomp/context.hpp"

#include <cstdint>

namespace recomp {

class FrameTiming : public Scheduler {
public:
    static constexpr double kRefreshHz = 57.524160;
    static constexpr uint32_t kLines = 424;
    static constexpr uint32_t kVBlankLines = 40;
    static constexpr uint32_t kDisplayIrqLine = 383; // of the display lines

    explicit FrameTiming(uint64_t cpu_hz = ppc::CpuState::kCoreClockHz);

    uint64_t update(Context& c) override;

    uint64_t frames() const { return frames_; } // completed
    uint64_t frame_cycles() const { return frame_cycles_; }
    uint64_t line_cycles() const { return line_cycles_; }

private:
    uint64_t due() const;

    uint64_t frame_cycles_, line_cycles_, vblank_cycles_;
    uint64_t frame_start_ = 0;
    uint64_t frames_ = 0;
    uint32_t flip_line_ = 0; // display line of this frame's ping-pong flip (>= 384: none)
    enum class Next { VBlankEnd, PingPong, DisplayIrq, FrameEnd } next_ = Next::VBlankEnd;
};

} // namespace recomp
