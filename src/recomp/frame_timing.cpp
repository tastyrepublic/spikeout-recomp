#include "recomp/frame_timing.hpp"

#include <algorithm>

namespace recomp {

FrameTiming::FrameTiming(uint64_t cpu_hz)
    : frame_cycles_(static_cast<uint64_t>(static_cast<double>(cpu_hz) / kRefreshHz)),
      line_cycles_(frame_cycles_ / kLines),
      vblank_cycles_(line_cycles_ * kVBlankLines) {}

uint64_t FrameTiming::due() const {
    const uint64_t display = frame_start_ + vblank_cycles_;
    switch (next_) {
    case Next::Midi:      return frame_start_ + midi_irqs_ * kMidiIrqCycles;
    case Next::VBlankEnd: return display;
    case Next::Line:      return display + line_ * line_cycles_;
    case Next::FrameEnd:  return frame_start_ + frame_cycles_;
    }
    return frame_start_;
}

uint64_t FrameTiming::update(Context& c) {
    for (;;) {
        // The bus devices' events (the sound link's serial chip, Real3D DMA)
        // and the frame's, in time order.
        const uint64_t frame_due = due(), device_due = c.bus.device_due();
        if (c.cycles() < std::min(frame_due, device_due)) break;
        if (device_due < frame_due) {
            c.bus.run_devices(device_due);
            continue;
        }
        c.bus.run_devices(frame_due);
        switch (next_) {
        case Next::Midi: // Supermodel's timing: one more MIDI IRQ, while the game asks for them
            if (c.bus.midi_burst && (c.bus.midi_control() & 0x20) && (c.bus.irq().enable() & 0x40) && midi_irqs_ < kMaxMidiIrqs &&
                (midi_irqs_ + 1) * kMidiIrqCycles < vblank_cycles_) {
                c.bus.irq().assert_irq(0x40);
                ++midi_irqs_;
            } else {
                next_ = Next::VBlankEnd;
            }
            break;
        case Next::VBlankEnd:
            c.bus.irq().assert_irq(0x0D);
            flip_line_ = c.bus.tile_reg(0x08);
            line_ = 0;
            next_ = Next::Line;
            break;
        case Next::Line:
            if (line_ == flip_line_) c.bus.flip_ping_pong();
            if (line_ == kDisplayIrqLine) c.bus.irq().assert_irq(0x02);
            if (on_display_line) on_display_line(line_);
            if (++line_ == kDisplayLines) next_ = Next::FrameEnd;
            break;
        case Next::FrameEnd:
            c.bus.netboard().sync(c.cycles());
            c.bus.netboard().end_frame(); // after the main board's frame, as Supermodel does
            frame_start_ += frame_cycles_;
            ++frames_;
            midi_irqs_ = 0;
            next_ = Next::Midi;
            if (on_frame_end) on_frame_end();
            c.bus.real3d_begin_vblank(); // after the frame is drawn: buffered updates land now
            break;
        }
    }
    return std::min(due(), c.bus.device_due());
}

void FrameTiming::state(m3::State& s) {
    s.tag("frame timing");
    s.io(frame_start_), s.io(frames_), s.io(flip_line_), s.io(line_), s.io(midi_irqs_), s.io(next_);
}

} // namespace recomp
