// The lifter's second back end: the same functions, blocks, return sites,
// block resume points and hooks as recomp::lift()'s C++ (src/recomp/lifter.cpp),
// as LLVM IR, for the JIT (src/jit/jit.cpp) to compile from the player's own
// ROM at start-up. Instruction for instruction it does what the C++ does;
// keep the two in step (the JIT's proof is the same RAM every frame).
//
// The generated functions take a recomp::Context* as the C++ ones take a
// Context&. Hot paths (a block's tick, work RAM reads and writes, flags,
// floating point) are written out in IR against the context's fields, at the
// offsets in Layout; everything else calls the runtime's own C++ (the
// spk_* helpers, src/jit/jit.cpp), so it is the same code as the C++ build's.
#pragma once

#include "analysis/code_image.hpp"
#include "analysis/control_flow.hpp"
#include "recomp/lifter.hpp"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace jit {

// Byte offsets of what the IR touches, measured in this build (JitAccess).
struct Layout {
    uint32_t ctx_cpu, ctx_bus, ctx_returned_to, ctx_resume_at, ctx_blocks, ctx_cycles, ctx_next_check;
    uint32_t ctx_recent, ctx_recent_pos, ctx_return_stack, ctx_return_depth, return_stack_size;
    uint32_t bus_ram_data, bus_fast_limit, bus_marks, mark_shift;
    uint64_t hash() const;
};

// One lift: what ppcrecomp does for one --segment.
struct LiftInput {
    std::string prefix; // symbol prefix ("boot_rom", "game_ram")
    const analysis::CodeImage* image = nullptr;
    const analysis::Analysis* analysis = nullptr;
    std::set<uint32_t> functions; // the entries to compile
    std::set<uint32_t> hooks;
};

struct IrPart {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    uint64_t instructions = 0;
};

// The functions spread over `parts` modules by instruction count, and one
// more module (the last) holding the tables: spk_functions / spk_blocks,
// arrays of recomp::FunctionEntry, with spk_functions_n / spk_blocks_n.
// Each module in its own LLVMContext (compiled on its own thread).
std::vector<IrPart> build_ir(const std::vector<LiftInput>& lifts, const Layout& layout, uint32_t parts,
                             const std::string& data_layout, const std::string& triple, recomp::LiftStats* stats);

} // namespace jit
