// The lifter: turns the functions found by the control-flow analysis into
// C++ source that runs against recomp::Context.
//
// Each guest function becomes one C++ function. Basic blocks are labels;
// branches inside the function are gotos, `bl` is a C++ call followed by a
// check that the callee returned to the next instruction, `blr` is a
// return, a jump to another function is a call followed by return (a tail
// call), and jump tables are switches. Anything only known at run time goes
// through Context::call()/jump().
//
// The generated file defines
//     std::span<const recomp::FunctionEntry> generated::<name>_functions();
// listing every function it contains, sorted by address. It is derived from
// the ROM, so it is written into the build directory, never the source tree.
#pragma once

#include "analysis/code_image.hpp"
#include "analysis/control_flow.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace recomp {

struct LiftOptions {
    std::string name = "guest";   // prefix of the generated table function
    std::set<uint32_t> functions; // entries to recompile; empty: every function found
    std::string source;           // where the code came from, for the header comment
};

struct LiftStats {
    uint32_t functions = 0;
    uint32_t blocks = 0;
    uint32_t instructions = 0;
    std::map<std::string, uint32_t> unimplemented; // mnemonic -> count (they throw at run time)
};

std::string lift(const analysis::CodeImage& image, const analysis::Analysis& analysis, const LiftOptions& options,
                 LiftStats* stats = nullptr);

} // namespace recomp
