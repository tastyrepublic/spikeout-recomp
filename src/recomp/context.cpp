#include "recomp/context.hpp"

#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"

#include <algorithm>
#include <cstdio>

namespace recomp {

namespace {

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

} // namespace

MissingCode::MissingCode(uint32_t a) : std::runtime_error("no recompiled code at " + hex(a)), addr(a) {}

GuestError::GuestError(uint32_t at, const std::string& what) : std::runtime_error(hex(at) + ": " + what), pc(at) {}

OutOfBudget::OutOfBudget(uint32_t at) : GuestError(at, "block budget used up") {}

Context::Context(m3::Bus& b, std::span<const FunctionEntry> functions) : bus(b), functions_(functions) {
    cpu.reset();
    bus.irq().on_change_user = this;
    bus.irq().on_change = [](void* self) { static_cast<Context*>(self)->recheck(); };
}

// ---- Time base and decrementer ----
// TB ticks at the bus clock / 4: 16.5 MHz against a 166 MHz core, 33/332 of
// the CPU clock.
constexpr uint64_t kTbNum = 33, kTbDen = 332;

uint64_t Context::time_base() {
    cpu.tb = tb_offset_ + cycles() * kTbNum / kTbDen;
    return cpu.tb;
}

uint32_t Context::read_tbl() {
    tb_offset_ += tb_step;
    return static_cast<uint32_t>(time_base());
}

void Context::write_tbl(uint32_t v) {
    const uint64_t now = time_base();
    tb_offset_ += ((now & 0xFFFFFFFF00000000u) | v) - now;
}

void Context::write_tbu(uint32_t v) {
    const uint64_t now = time_base();
    tb_offset_ += ((now & 0xFFFFFFFFu) | uint64_t{v} << 32) - now;
}

uint32_t Context::read_dec() { return dec_ - static_cast<uint32_t>(time_base() - dec_tb_); }

// DEC counts down with the time base; going from 0 to -1 (bit 0 turning on)
// requests a decrementer exception, and so does writing a negative value
// over a positive one.
void Context::write_dec(uint32_t v) {
    const uint32_t old = read_dec();
    dec_ = v;
    dec_tb_ = time_base();
    if (!(old >> 31) && (v >> 31)) dec_pending_ = true;
    dec_due_tb_ = dec_tb_ + uint64_t{v} + 1; // reaches -1 (v + 1 ticks; for a negative v, after wrapping)
    if (v >> 31) dec_due_tb_ += uint64_t{1} << 32;
    recheck();
}

uint64_t Context::cycle_of_tb(uint64_t tb) const {
    if (tb <= tb_offset_) return 0;
    return ((tb - tb_offset_) * kTbDen + kTbNum - 1) / kTbNum;
}

// ---- Events and interrupts ----

void Context::slow_tick(uint32_t pc) {
    if (blocks > block_limit_) throw OutOfBudget(pc);

    if (scheduler_ && cycles() >= scheduler_due_) scheduler_due_ = scheduler_->update(*this);
    const uint64_t tb = time_base();
    if (tb >= dec_due_tb_) {
        dec_pending_ = true;
        dec_due_tb_ += uint64_t{1} << 32; // the next time it passes 0
    }

    // Interrupts are taken at the start of a block, which is then run once
    // the handler's rfi comes back to it.
    if (cpu.msr & kMsrEE) {
        if (bus.irq().line()) {
            ++external_interrupts;
            exception(0x500, pc, 0, pc);
        } else if (dec_pending_) {
            dec_pending_ = false;
            ++decrementer_interrupts;
            exception(0x900, pc, 0, pc);
        }
    }

    uint64_t next = block_limit_ + 1;
    if (scheduler_) next = std::min(next, block_of_cycle(scheduler_due_));
    next = std::min(next, block_of_cycle(cycle_of_tb(dec_due_tb_)));
    if ((cpu.msr & kMsrEE) && (bus.irq().line() || dec_pending_)) next = blocks + 1;
    next_check_ = std::max(next, blocks + 1);
}

GuestFunction Context::find(uint32_t addr) const {
    auto it = std::lower_bound(functions_.begin(), functions_.end(), addr,
                               [](const FunctionEntry& e, uint32_t a) { return e.addr < a; });
    return it != functions_.end() && it->addr == addr ? it->fn : nullptr;
}

void Context::call(uint32_t addr) {
    GuestFunction fn = find(addr);
    if (!fn) throw MissingCode(addr);
    fn(*this);
}

void Context::follow_return(uint32_t next) {
    for (int hops = 0; returned_to != next; ++hops) {
        if (hops == 64) bad_return(next);
        jump(returned_to); // that code's blr sets returned_to again
    }
}

void Context::bad_return(uint32_t expected) {
    throw GuestError(cpu.pc, "returned to " + hex(returned_to) + ", expected " + hex(expected));
}

void Context::dcbz(uint32_t ea) {
    const uint32_t line = ea & ~31u;
    for (uint32_t i = 0; i < 32; i += 4) write32(line + i, 0);
}

// Bytes go into the registers from the top down, four per register,
// wrapping from r31 to r0; the last register's unused bytes are zero.
void Context::load_string(uint32_t rd, uint32_t ea, uint32_t nb) {
    uint32_t r = rd;
    for (uint32_t i = 0; i < nb; ++i) {
        const uint32_t shift = 24 - 8 * (i & 3);
        if ((i & 3) == 0) cpu.gpr[r] = 0;
        cpu.gpr[r] |= uint32_t{read8(ea + i)} << shift;
        if ((i & 3) == 3) r = (r + 1) & 31;
    }
}

void Context::store_string(uint32_t rs, uint32_t ea, uint32_t nb) {
    uint32_t r = rs;
    for (uint32_t i = 0; i < nb; ++i) {
        write8(ea + i, static_cast<uint8_t>(cpu.gpr[r] >> (24 - 8 * (i & 3))));
        if ((i & 3) == 3) r = (r + 1) & 31;
    }
}

namespace {

// MSR bits (bit 31 = LSB).
constexpr uint32_t kMsrLE = 1u << 0;
constexpr uint32_t kMsrME = 1u << 12;
constexpr uint32_t kMsrILE = 1u << 16;

// Thrown by an rfi that ends the exception at `depth`; caught by the
// exception() call that entered it. Unwinding instead of returning means
// the rfi may sit any number of calls deep inside the handler.
struct ExceptionReturn {
    size_t depth;
};

} // namespace

void Context::exception(uint32_t offset, uint32_t srr0, uint32_t cause, uint32_t resume) {
    // 603e: SRR1[16-31] = MSR[16-31] (with cause bits above); the new MSR
    // keeps ME, IP and ILE, sets LE from ILE and clears everything else
    // (EE, PR, FP, FE0/1, SE, BE, IR, DR, RI).
    cpu.srr0 = srr0;
    cpu.srr1 = (cpu.msr & 0x0000FFFFu) | cause;
    cpu.msr = (cpu.msr & (kMsrME | ppc::CpuState::kMsrIP | kMsrILE)) | ((cpu.msr & kMsrILE) ? kMsrLE : 0);
    const uint32_t vector = ((cpu.msr & ppc::CpuState::kMsrIP) ? 0xFFF00000u : 0u) + offset;

    resume_.push_back(resume);
    const size_t depth = resume_.size();
    struct Pop {
        std::vector<uint32_t>& stack;
        size_t depth;
        ~Pop() { stack.resize(depth - 1); }
    } pop{resume_, depth};
    try {
        call(vector);
    } catch (const ExceptionReturn& r) {
        if (r.depth != depth) throw; // returns from an outer exception
        return;
    }
    throw GuestError(vector, "exception handler returned without rfi");
}

void Context::rfi() {
    // 603e: MSR[16-23, 25-27, 30-31] = SRR1.
    cpu.msr = (cpu.msr & ~0xFF73u) | (cpu.srr1 & 0xFF73u);
    msr_changed();
    const uint32_t target = cpu.srr0 & ~3u;
    for (size_t i = resume_.size(); i-- > 0;)
        if (resume_[i] == target) throw ExceptionReturn{i + 1};
    jump(target);
}

void Context::unimplemented(uint32_t pc, uint32_t raw) {
    throw GuestError(pc, "not implemented: " + ppc::disassemble(ppc::decode(raw), pc));
}

void Context::invalid(uint32_t pc, uint32_t raw) { throw GuestError(pc, "invalid instruction " + hex(raw)); }

void Context::unknown_spr(uint32_t pc, uint32_t spr) {
    throw GuestError(pc, "access to SPR " + std::to_string(spr) + " not implemented");
}

} // namespace recomp
