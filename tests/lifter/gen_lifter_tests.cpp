// Build step for the lifter tests: lifts the test program (program.cpp) to
// C++ the same way ppcrecomp lifts a game.
//
//   gen_lifter_tests OUT.cpp
#include "analysis/code_image.hpp"
#include "analysis/control_flow.hpp"
#include "lifter/program.hpp"
#include "recomp/lifter.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gen_lifter_tests OUT.cpp\n");
        return 2;
    }
    const lifter_test::Program& p = lifter_test::program();
    std::vector<uint8_t> bytes;
    for (uint32_t w : p.words)
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(w >> shift));
    analysis::CodeImage image;
    image.add("test", lifter_test::kBase, std::move(bytes));

    std::vector<analysis::Entry> entries;
    for (const auto& [name, addr] : p.labels)
        if (std::find(p.entries.begin(), p.entries.end(), addr) != p.entries.end())
            entries.push_back({addr, analysis::EntryKind::Seed, name});
    const analysis::Analysis an = analysis::analyze(image, entries);

    recomp::LiftOptions options;
    options.name = "lifter_test";
    options.source = "the lifter test program";
    std::ofstream out(argv[1], std::ios::binary);
    out << recomp::lift(image, an, options).at(0);
    return out ? 0 : 1;
}
