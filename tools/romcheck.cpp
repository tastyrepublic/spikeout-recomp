// romcheck: verify a ROM set and show the code at the CPU reset vector.
//
//   romcheck <rom_dir> [--game spikeofe] [--count N] [--dump-crom FILE]
//
// <rom_dir> is a folder with the extracted ROM files (unzip the MAME set).
// --dump-crom writes the normalized fixed CROM (as the CPU sees it at
// 0xFF800000) so it can be inspected with ppcdis.
#include "games/games.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/rom.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

int usage() {
    std::fprintf(stderr, "usage: romcheck <rom_dir> [--game ID] [--count N] [--dump-crom FILE]\n");
    return 2;
}

const char* status_text(m3::RomFileStatus s) {
    switch (s) {
    case m3::RomFileStatus::Ok:            return "ok";
    case m3::RomFileStatus::Missing:       return "MISSING";
    case m3::RomFileStatus::WrongSize:     return "WRONG SIZE";
    case m3::RomFileStatus::WrongChecksum: return "BAD CRC";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    const char* dir = nullptr;
    std::string game_id = "spikeofe";
    std::string dump_path;
    unsigned count = 16;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) std::exit(usage());
            return argv[++i];
        };
        if (arg == "--game") game_id = next();
        else if (arg == "--count") count = static_cast<unsigned>(std::strtoul(next(), nullptr, 0));
        else if (arg == "--dump-crom") dump_path = next();
        else if (!dir) dir = argv[i];
        else return usage();
    }
    if (!dir) return usage();

    const m3::GameSpec* game = m3::games::find(game_id);
    if (!game) {
        std::fprintf(stderr, "romcheck: unknown game '%s'\n", game_id.c_str());
        return 2;
    }

    const m3::RomSet set = m3::load_rom_set(*game, dir);
    std::printf("%.*s (%.*s)\n\n", int(game->title.size()), game->title.data(), int(game->id.size()), game->id.data());

    int bad = 0;
    for (const auto& r : set.report) {
        const auto region = m3::rom_region_name(r.region);
        std::printf("  %-16s %-14.*s %-10s", r.name.c_str(), int(region.size()), region.data(), status_text(r.status));
        if (r.status == m3::RomFileStatus::WrongChecksum)
            std::printf("  crc %08x, expected %08x", r.actual_crc, r.expected_crc);
        if (r.status == m3::RomFileStatus::WrongSize)
            std::printf("  %llu bytes", static_cast<unsigned long long>(r.actual_size));
        std::printf("\n");
        bad += r.status != m3::RomFileStatus::Ok;
    }
    std::printf("\n%zu files, %d with problems\n", set.report.size(), bad);

    if (set.region(m3::RomRegionId::Crom).empty() || !set.complete()) {
        std::printf("Program ROMs incomplete; not disassembling.\n");
        return 1;
    }

    const auto order = m3::word_order_name(set.program_word_order);
    std::printf("Program ROM word order: %.*s\n\n", int(order.size()), order.data());
    std::printf("Reset vector (0x%08x):\n", m3::kResetVector);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t pc = m3::kResetVector + i * 4;
        const uint32_t raw = set.read_program32(pc);
        std::printf("  %08x:  %08x  %s\n", pc, raw, ppc::disassemble(ppc::decode(raw), pc).c_str());
    }

    if (!dump_path.empty()) {
        const auto& crom = set.region(m3::RomRegionId::Crom);
        std::ofstream out(dump_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(crom.data()), static_cast<std::streamsize>(crom.size()));
        std::printf("\nWrote fixed CROM to %s (base 0x%08x)\n", dump_path.c_str(), m3::kFixedCromBase);
    }
    return bad == 0 ? 0 : 1;
}
