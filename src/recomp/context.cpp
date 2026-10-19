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

void Context::bad_return(uint32_t expected) {
    throw GuestError(cpu.pc, "returned to " + hex(returned_to) + ", expected " + hex(expected));
}

uint32_t Context::read_tbl() {
    cpu.tb += tb_step;
    return static_cast<uint32_t>(cpu.tb);
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

void Context::unimplemented(uint32_t pc, uint32_t raw) {
    throw GuestError(pc, "not implemented: " + ppc::disassemble(ppc::decode(raw), pc));
}

void Context::invalid(uint32_t pc, uint32_t raw) { throw GuestError(pc, "invalid instruction " + hex(raw)); }

void Context::trap(uint32_t pc) { throw GuestError(pc, "trap"); }

void Context::system_call(uint32_t pc) { throw GuestError(pc, "sc (system call exception not implemented)"); }

void Context::unknown_spr(uint32_t pc, uint32_t spr) {
    throw GuestError(pc, "access to SPR " + std::to_string(spr) + " not implemented");
}

} // namespace recomp
