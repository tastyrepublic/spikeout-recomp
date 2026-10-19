#include "recomp/frame_timing.hpp"

namespace recomp {

FrameTiming::FrameTiming(uint64_t cpu_hz)
    : frame_cycles_(static_cast<uint64_t>(static_cast<double>(cpu_hz) / kRefreshHz)),
      line_cycles_(frame_cycles_ / kLines),
      vblank_cycles_(line_cycles_ * kVBlankLines) {}

uint64_t FrameTiming::due() const {
    const uint64_t display = frame_start_ + vblank_cycles_;
    switch (next_) {
    case Next::VBlankEnd:  return display;
    case Next::PingPong:   return display + flip_line_ * line_cycles_;
    case Next::DisplayIrq: return display + kDisplayIrqLine * line_cycles_;
    case Next::FrameEnd:   return frame_start_ + frame_cycles_;
    }
    return frame_start_;
}

uint64_t FrameTiming::update(Context& c) {
    while (c.cycles() >= due()) {
        switch (next_) {
        case Next::VBlankEnd:
            c.bus.irq().assert_irq(0x0D);
            flip_line_ = c.bus.tile_reg(0x08);
            next_ = flip_line_ <= kDisplayIrqLine ? Next::PingPong : Next::DisplayIrq;
            break;
        case Next::PingPong:
            c.bus.flip_ping_pong();
            next_ = Next::DisplayIrq;
            break;
        case Next::DisplayIrq:
            c.bus.irq().assert_irq(0x02);
            next_ = Next::FrameEnd;
            break;
        case Next::FrameEnd:
            c.bus.netboard().run_frame(); // after the main board's frame, as Supermodel does
            frame_start_ += frame_cycles_;
            ++frames_;
            next_ = Next::VBlankEnd;
            break;
        }
    }
    return due();
}

} // namespace recomp
