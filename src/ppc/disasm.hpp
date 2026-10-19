#pragma once

#include "ppc/instruction.hpp"

#include <string>

namespace ppc {

// Disassemble one instruction located at `pc`, using the common simplified
// mnemonics (li, mr, blr, beq, mflr, ...) where they apply.
std::string disassemble(const Instruction& insn, uint32_t pc);

// Name of a special-purpose register as used on the 603e, or nullptr.
const char* spr_name(uint32_t spr);

} // namespace ppc
