// gamerun: run the recompiled boot ROM into the recompiled game and report
// how far it gets.
//
//   gamerun <rom_dir> [--blocks N]
//
// Built only when CMake's SPIKEOUT_ROM_DIR is set. A run ends at the first
// thing the runtime can't do yet (code that wasn't found, an exception, a
// device the game waits on), or after N basic blocks (default 200 million).
// That stop, the blocks before it and the devices the game touched are
// what to work on next.
#include "games/games.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "recomp/context.hpp"
#include "runtime/bus.hpp"
#include "runtime/rom.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace generated {
std::span<const recomp::FunctionEntry> boot_rom_functions();
std::span<const recomp::FunctionEntry> game_ram_functions();
} // namespace generated

int main(int argc, char** argv) {
    const char* dir = nullptr;
    int64_t blocks = 200'000'000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--blocks" && i + 1 < argc) blocks = std::strtoll(argv[++i], nullptr, 0);
        else if (!dir) dir = argv[i];
        else dir = nullptr, i = argc;
    }
    if (!dir) {
        std::fprintf(stderr, "usage: gamerun <rom_dir> [--blocks N]\n");
        return 2;
    }
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const m3::RomSet roms = m3::load_rom_set(game, dir);
    if (!roms.complete()) {
        std::fprintf(stderr, "gamerun: ROM set incomplete; run romcheck for details\n");
        return 1;
    }

    // One table, sorted: the RAM program's addresses are all below the boot ROM's.
    std::vector<recomp::FunctionEntry> functions;
    for (const auto& f : generated::game_ram_functions()) functions.push_back(f);
    for (const auto& f : generated::boot_rom_functions()) functions.push_back(f);
    std::sort(functions.begin(), functions.end(),
              [](const recomp::FunctionEntry& a, const recomp::FunctionEntry& b) { return a.addr < b.addr; });

    m3::Bus bus(roms);
    for (uint8_t& b : bus.ram()) b = 0xA5;
    recomp::Context c(bus, functions);
    c.budget = blocks;
    std::printf("%zu recompiled functions; running up to %lld blocks from %08x\n\n", functions.size(),
                static_cast<long long>(blocks), c.cpu.pc);

    const auto start = std::chrono::steady_clock::now();
    std::string why;
    try {
        c.call(c.cpu.pc);
        why = "the reset code returned";
    } catch (const recomp::OutOfBudget&) {
        why = "block limit reached";
    } catch (const recomp::MissingCode& e) {
        why = e.what();
    } catch (const recomp::GuestError& e) {
        why = e.what();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const long long ran = static_cast<long long>(blocks - c.budget);

    std::printf("Stopped: %s\n", why.c_str());
    std::printf("Ran %lld blocks in %.2f s (%.0f million blocks/s)\n", ran, seconds,
                static_cast<double>(ran) / seconds / 1e6);
    std::printf("PC %08x  LR %08x  CTR %08x  MSR %08x  CR %08x\n", c.cpu.pc, c.cpu.lr, c.cpu.ctr, c.cpu.msr, c.cpu.cr);

    std::printf("\nLast blocks:\n");
    for (uint32_t pc : c.recent_blocks())
        if (pc) {
            const uint32_t raw = bus.read32(pc);
            std::printf("  %08x  %s\n", pc, ppc::disassemble(ppc::decode(raw), pc).c_str());
        }

    std::printf("\nDevices accessed that aren't emulated yet:\n");
    if (bus.unhandled().empty()) std::printf("  none\n");
    for (const auto& [region, u] : bus.unhandled())
        std::printf("  %-16.*s %llu reads, %llu writes (first at %08x)\n", int(region.size()), region.data(),
                    static_cast<unsigned long long>(u.reads), static_cast<unsigned long long>(u.writes),
                    u.first_addr);
    return 0;
}
