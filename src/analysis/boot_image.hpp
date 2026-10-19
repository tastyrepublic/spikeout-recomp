// The code a game can run right after boot, set up for the control-flow
// analysis: the fixed CROM at 0xFF800000, each RAM copy the boot code makes
// (GameSpec::boot_copies), and the exception vectors of both as entry points
// (high vectors at 0xFFF00000 while MSR[IP] = 1, low vectors at 0 after).
#pragma once

#include "analysis/code_image.hpp"
#include "analysis/control_flow.hpp"
#include "runtime/rom.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace analysis {

struct ExceptionVector {
    uint32_t offset; // from the vector base
    std::string_view name;
};

// The 603e's exception vectors.
std::span<const ExceptionVector> exception_vectors();

struct BootImage {
    CodeImage image;
    std::vector<Entry> entries;
};

// `set` must hold a complete program ROM.
BootImage make_boot_image(const m3::GameSpec& game, const m3::RomSet& set);

} // namespace analysis
