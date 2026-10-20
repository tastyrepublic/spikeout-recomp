// PowerPC 603e instruction timing, for the recompiler's clock.
//
// The 603e dispatches up to two instructions a cycle, in order, to five
// units: one integer unit (IU), the FPU, the load/store unit (LSU), the
// system register unit (SRU) and the branch unit (BPU). Each instruction
// here has a unit, a latency (cycles until its result can be used) and a
// busy time (cycles until its unit takes the next one), after the MPC603e
// user's manual (chapter 6, "Instruction Timing"); data-dependent ones
// (multiply, divide) take a typical value.
//
// block_cycles() runs a basic block through that model: an instruction
// dispatches when its source registers are ready, its unit is free and the
// cycle has a dispatch slot left; serializing instructions (sync, isync,
// mtmsr, rfi, sc, segment and TLB updates) wait for everything before them.
// A block costs the cycles until its last instruction dispatches and its
// units are free. Not modelled: caches (every access hits), the bus to
// devices, results that cross into the next block, branch prediction.
#pragma once

#include "ppc/instruction.hpp"

#include <cstdint>
#include <span>

namespace ppc {

enum class Unit : uint8_t { IU, FPU, LSU, SRU, BPU };

struct OpTiming {
    Unit unit;
    uint8_t latency; // until the result can be used
    uint8_t busy;    // until the unit takes another
    bool serial = false;
};

OpTiming timing(const Instruction& in);

// Cycles a basic block takes (at least 1).
uint32_t block_cycles(std::span<const Instruction> block);

} // namespace ppc
