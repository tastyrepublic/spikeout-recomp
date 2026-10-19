// What gamerun and the spikeout frontend share: the recompiled functions in
// one table, and the RTC set to the host's clock.
#pragma once

#include "recomp/context.hpp"
#include "runtime/bus.hpp"

#include <algorithm>
#include <ctime>
#include <span>
#include <vector>

namespace generated {
std::span<const recomp::FunctionEntry> boot_rom_functions();
std::span<const recomp::FunctionEntry> game_ram_functions();
} // namespace generated

namespace tools {

// One table, sorted: the RAM program's addresses are all below the boot ROM's.
inline std::vector<recomp::FunctionEntry> game_functions() {
    std::vector<recomp::FunctionEntry> functions;
    for (const auto& f : generated::game_ram_functions()) functions.push_back(f);
    for (const auto& f : generated::boot_rom_functions()) functions.push_back(f);
    std::sort(functions.begin(), functions.end(),
              [](const recomp::FunctionEntry& a, const recomp::FunctionEntry& b) { return a.addr < b.addr; });
    return functions;
}

// The RTC shows the host's local time when the run starts.
inline void set_rtc_to_host(m3::Bus& bus) {
    const std::time_t t = std::time(nullptr);
    if (const std::tm* lt = std::localtime(&t))
        bus.rtc().set_time(
            m3::Rtc72421::to_seconds({lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec}),
            0);
}

} // namespace tools
