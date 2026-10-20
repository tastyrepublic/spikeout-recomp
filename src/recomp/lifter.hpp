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
// Every instruction after a call is also a return site: a label the
// function can start at when Context::call() resumes it there (resume_at,
// checked at the top of the function). The table lists those sites too.
//
// The generated code defines
//     std::span<const recomp::FunctionEntry> generated::<name>_functions();
// listing every function in it, sorted by address. It is derived from the
// ROM, so it is written into the build directory, never the source tree.
#pragma once

#include "analysis/code_image.hpp"
#include "analysis/control_flow.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace recomp {

struct LiftOptions {
    std::string name = "guest";   // prefix of the generated table function
    std::set<uint32_t> functions; // entries to recompile; empty: every function found
    std::string source;           // where the code came from, for the header comment
    uint32_t files = 1;           // spread the functions over this many files (parallel builds)
};

struct LiftStats {
    uint32_t functions = 0;
    uint32_t blocks = 0;
    uint32_t instructions = 0;
    uint64_t cycles = 0;       // the blocks' 603e cycles, added up (ppc::block_cycles)
    uint32_t return_sites = 0; // places after a call the runtime can resume at
    std::map<std::string, uint32_t> unimplemented; // mnemonic -> count (they throw at run time)
};

// Returns the generated files: the function table first, then
// `options.files` files of functions (with files == 1, a single file holding
// both).
std::vector<std::string> lift(const analysis::CodeImage& image, const analysis::Analysis& analysis,
                              const LiftOptions& options, LiftStats* stats = nullptr);

} // namespace recomp
