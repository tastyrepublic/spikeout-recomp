// ppcrecomp: recompile PowerPC code to C++.
//
//   ppcrecomp --rom DIR [--game ID] [--segment NAME]... [--scan-pointers] [--name NAME] -o FILE
//   ppcrecomp --image FILE --base ADDR --entry ADDR... [--name NAME] -o FILE
//
// --rom      a ROM set folder: code is found from the game's vectors, through
//            its boot copies into RAM (see codemap)
// --segment  only recompile functions whose entry is in this segment
//            ("crom" = the boot ROM, "ram" = the game's RAM copy)
// --image    a raw big-endian binary loaded at --base (tests)
// --name     the generated file defines generated::<NAME>_functions()
//
// The output is derived from the ROM: it belongs in the build directory.
#include "analysis/boot_image.hpp"
#include "analysis/control_flow.hpp"
#include "games/games.hpp"
#include "recomp/lifter.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/rom.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ppcrecomp --rom DIR [--game ID] [--segment NAME]... [--scan-pointers] [--name NAME] -o FILE\n"
                 "       ppcrecomp --image FILE --base ADDR --entry ADDR... [--name NAME] -o FILE\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string rom_dir, image_path, out_path, game_id = "spikeofe", name = "guest";
    uint32_t base = 0;
    std::vector<uint32_t> entries;
    std::vector<std::string> segments;
    analysis::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) std::exit(usage());
            return argv[++i];
        };
        auto number = [&]() { return static_cast<uint32_t>(std::strtoul(next(), nullptr, 0)); };
        if (arg == "--rom") rom_dir = next();
        else if (arg == "--image") image_path = next();
        else if (arg == "--base") base = number();
        else if (arg == "--entry") entries.push_back(number());
        else if (arg == "--game") game_id = next();
        else if (arg == "--segment") segments.push_back(next());
        else if (arg == "--scan-pointers") options.scan_pointers = true;
        else if (arg == "--name") name = next();
        else if (arg == "-o") out_path = next();
        else return usage();
    }
    if (out_path.empty() || rom_dir.empty() == image_path.empty()) return usage();

    analysis::BootImage boot;
    std::string source;
    m3::RomSet roms;
    if (!rom_dir.empty()) {
        const m3::GameSpec* game = m3::games::find(game_id);
        if (!game) {
            std::fprintf(stderr, "ppcrecomp: unknown game '%s'\n", game_id.c_str());
            return 2;
        }
        roms = m3::load_rom_set(*game, rom_dir);
        if (!roms.complete()) {
            std::fprintf(stderr, "ppcrecomp: ROM set incomplete; run romcheck for details\n");
            return 1;
        }
        boot = analysis::make_boot_image(*game, roms);
        for (const analysis::Segment& seg : boot.image.segments())
            if (seg.base < m3::kWorkRamSize) options.pointer_segments.push_back(seg.name);
        source = std::string(game->id);
    } else {
        std::ifstream in(image_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "ppcrecomp: cannot open %s\n", image_path.c_str());
            return 1;
        }
        boot.image.add("image", base,
                       std::vector<uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
        options.pointer_segments.push_back("image");
        source = "a test image";
    }
    for (uint32_t addr : entries) boot.entries.push_back({addr, analysis::EntryKind::Seed, ""});

    const analysis::Analysis an = analysis::analyze(boot.image, boot.entries, options);

    recomp::LiftOptions lift;
    lift.name = name;
    lift.source = source;
    for (const auto& [entry, fn] : an.functions) {
        const analysis::Segment* seg = boot.image.segment_at(entry);
        bool wanted = segments.empty();
        for (const std::string& s : segments) wanted |= seg && seg->name == s;
        if (wanted) lift.functions.insert(entry);
    }
    recomp::LiftStats stats;
    const std::string code = recomp::lift(boot.image, an, lift, &stats);

    std::ofstream out(out_path, std::ios::binary);
    out << code;
    if (!out) {
        std::fprintf(stderr, "ppcrecomp: cannot write %s\n", out_path.c_str());
        return 1;
    }
    std::printf("ppcrecomp: %u functions, %u blocks, %u instructions -> %s\n", stats.functions, stats.blocks,
                stats.instructions, out_path.c_str());
    if (!stats.unimplemented.empty()) {
        uint32_t total = 0;
        for (const auto& [mnem, n] : stats.unimplemented) total += n;
        std::printf("  %u instructions not lifted yet (they throw if reached):", total);
        for (const auto& [mnem, n] : stats.unimplemented) std::printf(" %s x%u", mnem.c_str(), n);
        std::printf("\n");
    }
    return 0;
}
