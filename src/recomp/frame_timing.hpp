// Model 3 frame timing, after Supermodel's RunMainBoardFrame (Model3.cpp):
// 57.524160 Hz, 424 lines per frame. A frame starts with 40 lines of
// VBlank, which end with IRQ 0x0D, then 384 display lines; IRQ 0x02 comes
// at the start of line 383.
//
// Not modelled yet: running VBlank until IRQ 0x02 is acknowledged, the
// sound board's MIDI IRQ (0x40), and the Real3D ping-pong flip.
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
    enum class Next { VBlankEnd, DisplayIrq, FrameEnd } next_ = Next::VBlankEnd;
};

} // namespace recomp
