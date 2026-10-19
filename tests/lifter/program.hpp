// The guest program the lifter tests run: small PowerPC functions written
// with a tiny assembler. gen_lifter_tests lifts it to C++ at build time and
// lifter_tests runs the result.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lifter_test {

inline constexpr uint32_t kBase = 0x700;      // where the program is loaded (from the 0x700 vector up)
inline constexpr uint32_t kBuffer = 0x10000;  // scratch memory used by the tests

struct Program {
    std::vector<uint32_t> words;
    std::map<std::string, uint32_t> labels;
    std::vector<uint32_t> entries; // function entry points to seed the analysis with
};

const Program& program();

} // namespace lifter_test
