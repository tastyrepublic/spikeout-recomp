// A Moira 68000 that can be saved: the boards' CPUs derive from this
// instead of moira::Moira (its state is in protected fields). Everything
// an instruction or an interrupt can change, as vAmiga's CPU saves it.
#pragma once

#include "runtime/state.hpp"

#include <Moira.h>

namespace m3 {

class SavedMoira : public moira::Moira {
public:
    void cpu_state(State& s) {
        s.tag("68000");
        s.io(clock);
        s.io(reg.pc), s.io(reg.pc0), s.io(reg.sr), s.io(reg.r), s.io(reg.usp), s.io(reg.isp), s.io(reg.msp);
        s.io(reg.ipl), s.io(reg.vbr), s.io(reg.sfc), s.io(reg.dfc), s.io(reg.cacr), s.io(reg.caar);
        s.io(queue), s.io(irqMode), s.io(ipl), s.io(fcl), s.io(fcSource);
        s.io(exception), s.io(cp), s.io(loopModeDelay), s.io(readBuffer), s.io(writeBuffer), s.io(flags);
    }
};

} // namespace m3
