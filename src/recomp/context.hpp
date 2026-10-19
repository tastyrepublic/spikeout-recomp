// What recompiled code runs against: the CPU state, the bus, and a table of
// the recompiled functions for indirect calls and jumps.
//
// Recompiled functions are plain C++ functions taking the context. `bl` is a
// C++ call, `blr` a C++ return; anything the code can't follow statically
// (function pointers, computed jumps, rfi) goes through call()/jump(), which
// look the target up in the table. Reaching code that wasn't recompiled, an
// unimplemented instruction, or a return to the wrong place throws, so a run
// stops at the first thing the recompiler got wrong instead of drifting.
#pragma once

#include "ppc/cpu_state.hpp"
#include "runtime/bus.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace recomp {

class Context;
using GuestFunction = void (*)(Context&);

struct FunctionEntry {
    uint32_t addr;
    GuestFunction fn;
};

// Execution reached an address with no recompiled function.
struct MissingCode : std::runtime_error {
    explicit MissingCode(uint32_t a);
    uint32_t addr;
};

// Something the runtime can't handle (yet): stops the run.
struct GuestError : std::runtime_error {
    GuestError(uint32_t at, const std::string& what);
    uint32_t pc;
};

// The block budget ran out (an endless loop, or a test's limit).
struct OutOfBudget : GuestError {
    explicit OutOfBudget(uint32_t at);
};

class Context {
public:
    // `functions` must be sorted by address.
    Context(m3::Bus& bus, std::span<const FunctionEntry> functions);

    ppc::CpuState cpu;
    m3::Bus& bus;

    // Where the last `blr` returned to; checked after every call.
    uint32_t returned_to = 0;
    // Basic blocks left to run before OutOfBudget is thrown.
    int64_t budget = INT64_MAX;
    // How far the time base moves on each read of TBL. Recompiled code
    // doesn't count cycles; this keeps delay loops finite and deterministic.
    uint32_t tb_step = 64;

    uint8_t read8(uint32_t addr) { return bus.read8(addr); }
    uint16_t read16(uint32_t addr) { return bus.read16(addr); }
    uint32_t read32(uint32_t addr) { return bus.read32(addr); }
    uint64_t read64(uint32_t addr) { return bus.read64(addr); }
    void write8(uint32_t addr, uint8_t v) { bus.write8(addr, v); }
    void write16(uint32_t addr, uint16_t v) { bus.write16(addr, v); }
    void write32(uint32_t addr, uint32_t v) { bus.write32(addr, v); }
    void write64(uint32_t addr, uint64_t v) { bus.write64(addr, v); }

    // Start of every basic block.
    void tick(uint32_t pc) {
        cpu.pc = pc;
        recent[recent_pos++ & (recent.size() - 1)] = pc;
        if (--budget < 0) throw OutOfBudget(pc);
    }

    // The last blocks run, oldest first: where a stopped run came from.
    std::array<uint32_t, 16> recent{};
    uint32_t recent_pos = 0;
    std::array<uint32_t, 16> recent_blocks() const {
        std::array<uint32_t, 16> out{};
        for (uint32_t i = 0; i < out.size(); ++i) out[i] = recent[(recent_pos + i) & (recent.size() - 1)];
        return out;
    }

    GuestFunction find(uint32_t addr) const;
    // Run the function at `addr` (an indirect call; the caller sets LR).
    void call(uint32_t addr);
    // Continue at `addr` (an indirect jump or tail call). Same as call():
    // the C++ stack only unwinds when the guest code returns.
    void jump(uint32_t addr) { call(addr); }

    // The callee's blr went somewhere other than the instruction after the call.
    [[noreturn]] void bad_return(uint32_t expected);

    uint32_t read_tbl();
    uint32_t read_tbu() { return static_cast<uint32_t>(cpu.tb >> 32); }
    void write_tbl(uint32_t v) { cpu.tb = (cpu.tb & 0xFFFFFFFF00000000u) | v; }
    void write_tbu(uint32_t v) { cpu.tb = (cpu.tb & 0xFFFFFFFFu) | uint64_t{v} << 32; }

    void dcbz(uint32_t ea);
    void load_string(uint32_t rd, uint32_t ea, uint32_t nb);  // lswi/lswx
    void store_string(uint32_t rs, uint32_t ea, uint32_t nb); // stswi/stswx

    // Exceptions. exception() enters a handler the way the 603e does (SRR0,
    // SRR1, MSR, vector at 0 or 0xFFF00000 by MSR[IP]) and runs it; it
    // returns when the handler's rfi goes back to `resume`, the address where
    // the recompiled code that raised it carries on. An rfi anywhere else is
    // a jump (the boot code enters the game that way).
    void exception(uint32_t offset, uint32_t srr0, uint32_t cause, uint32_t resume);
    void rfi();
    // Exceptions being handled, innermost last.
    size_t exception_depth() const { return resume_.size(); }

    void system_call(uint32_t pc) { exception(0xC00, pc + 4, 0, pc + 4); }
    // Program exception, trap type. SRR0 points at the trap; handlers step
    // past it, so the code resumes after it.
    void trap(uint32_t pc) { exception(0x700, pc, kSrr1Trap, pc + 4); }

    [[noreturn]] void unimplemented(uint32_t pc, uint32_t raw);
    [[noreturn]] void invalid(uint32_t pc, uint32_t raw);
    [[noreturn]] void missing(uint32_t addr) { throw MissingCode(addr); }
    [[noreturn]] void unknown_spr(uint32_t pc, uint32_t spr);

    static constexpr uint32_t kSrr1Trap = 1u << 17; // SRR1 bit 14

private:
    std::span<const FunctionEntry> functions_;
    std::vector<uint32_t> resume_;
};

} // namespace recomp
