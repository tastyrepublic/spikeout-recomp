// codemap: find the functions, blocks and jump tables reachable from the
// reset and exception vectors, and report what couldn't be followed.
//
//   codemap <rom_dir> [--game ID] [--entry ADDR]... [--functions] [--issues]
//           [--tables] [--scan-pointers] [--disasm FILE] [--max-table N]
//
// --entry     extra entry point (repeatable), e.g. a handler found by hand
// --functions list every function
// --issues    list every issue (default: counts and a few examples)
// --tables    list every jump/call table
// --scan-pointers  also follow code addresses found in the RAM copy's data
//             and in constants the code builds (callbacks): see
//             analysis::Options::scan_pointers
// --disasm    write an annotated disassembly of all code found to FILE.
//             It is derived from the ROMs: keep it out of the repository.
#include "analysis/boot_image.hpp"
#include "analysis/control_flow.hpp"
#include "games/games.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/rom.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr, "usage: codemap <rom_dir> [--game ID] [--entry ADDR]... [--functions] [--issues] [--tables]\n"
                         "               [--scan-pointers] [--disasm FILE] [--max-table N]\n");
    return 2;
}

std::string function_name(const analysis::Function& fn) {
    if (!fn.name.empty()) return fn.name;
    char buf[16];
    std::snprintf(buf, sizeof buf, "fn_%08x", fn.entry);
    return buf;
}

std::string_view region_name(uint32_t addr) {
    const m3::Region* r = m3::find_region(addr);
    return r ? r->name : std::string_view("unmapped");
}

void print_summary(const analysis::BootImage& boot, const analysis::Analysis& a) {
    std::printf("Segments:\n");
    for (const analysis::Segment& seg : boot.image.segments()) {
        uint64_t code = 0;
        for (const auto& [start, block] : a.blocks)
            if (seg.contains(start)) code += block.end - block.start;
        std::printf("  %-6s %08x-%08x  %5zu KB, code found in %5llu KB (%.1f%%)\n", seg.name.c_str(), seg.base,
                    static_cast<uint32_t>(seg.end() - 1), seg.bytes.size() / 1024,
                    static_cast<unsigned long long>(code / 1024), 100.0 * double(code) / double(seg.bytes.size()));
    }

    std::map<analysis::EntryKind, int> kinds;
    for (const auto& [entry, fn] : a.functions) ++kinds[fn.kind];
    std::printf("\nEntry points:  %zu\nFunctions:     %zu  (", boot.entries.size(), a.functions.size());
    const char* sep = "";
    for (const auto& [kind, n] : kinds) {
        const auto name = analysis::entry_kind_name(kind);
        std::printf("%s%d %.*s", sep, n, int(name.size()), name.data());
        sep = ", ";
    }
    std::printf(")\nBlocks:        %zu\nInstructions:  %u\n", a.blocks.size(), a.instruction_count());

    size_t exact = 0, calls = 0, targets = 0;
    for (const auto& t : a.jump_tables) {
        exact += t.exact;
        calls += t.call;
        targets += t.targets.size();
    }
    std::printf("Tables:        %zu  (%zu jump, %zu call; %zu with a known length; %zu targets)\n",
                a.jump_tables.size(), a.jump_tables.size() - calls, calls, exact, targets);
}

void print_issues(const analysis::Analysis& a, bool all) {
    std::map<analysis::IssueKind, std::vector<const analysis::Issue*>> by_kind;
    for (const auto& i : a.issues) by_kind[i.kind].push_back(&i);
    std::printf("\nIssues:\n");
    if (by_kind.empty()) std::printf("  none\n");
    for (const auto& [kind, list] : by_kind) {
        const auto name = analysis::issue_kind_name(kind);
        std::printf("  %-34.*s %6zu  ", int(name.size()), name.data(), list.size());
        const size_t shown = all ? list.size() : std::min<size_t>(list.size(), 4);
        for (size_t k = 0; k < shown; ++k) {
            const auto& i = *list[k];
            if (all && k % 6 == 0) std::printf("\n    ");
            if (kind == analysis::IssueKind::TargetOutsideImage || kind == analysis::IssueKind::BadTableEntry)
                std::printf("%08x->%08x ", i.addr, i.target);
            else
                std::printf("%08x ", i.addr);
        }
        if (!all && list.size() > shown) std::printf("...");
        std::printf("\n");
    }

    // Where code goes that isn't loaded: tells what else needs to be mapped.
    std::map<std::string_view, std::map<uint32_t, int>> outside;
    for (const auto& i : a.issues)
        if (i.kind == analysis::IssueKind::TargetOutsideImage) ++outside[region_name(i.target)][i.target];
    if (!outside.empty()) {
        std::printf("\nTargets outside the image, by region:\n");
        for (const auto& [region, targets] : outside) {
            std::printf("  %-20.*s %5zu distinct  ", int(region.size()), region.data(), targets.size());
            int k = 0;
            for (const auto& [target, n] : targets) {
                if (k++ == 6) {
                    std::printf("...");
                    break;
                }
                std::printf("%08x ", target);
            }
            std::printf("\n");
        }
    }
}

void print_functions(const analysis::Analysis& a) {
    std::printf("\nFunctions:\n  entry     kind           blocks  insns  name\n");
    for (const auto& [entry, fn] : a.functions) {
        const auto kind = analysis::entry_kind_name(fn.kind);
        std::printf("  %08x  %-13.*s %7zu %6u  %s\n", entry, int(kind.size()), kind.data(), fn.blocks.size(),
                    fn.instructions, fn.name.c_str());
    }
}

void print_tables(const analysis::Analysis& a) {
    std::printf("\nTables:\n  site      table     stride  entries  length  kind\n");
    for (const auto& t : a.jump_tables)
        std::printf("  %08x  %08x  %6u  %7zu  %-6s  %s\n", t.site, t.table, t.stride, t.targets.size(),
                    t.exact ? "known" : "guess", t.call ? "call" : "jump");
}

bool write_disasm(const char* path, const analysis::BootImage& boot, const analysis::Analysis& a) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    uint32_t prev_end = 0;
    for (const auto& [start, block] : a.blocks) {
        if (start != prev_end) std::fprintf(f, "\n");
        if (auto fn = a.functions.find(start); fn != a.functions.end()) {
            const auto kind = analysis::entry_kind_name(fn->second.kind);
            std::fprintf(f, "%s:  ; %.*s, %zu blocks, %u instructions\n", function_name(fn->second).c_str(),
                         int(kind.size()), kind.data(), fn->second.blocks.size(), fn->second.instructions);
        } else if (start != prev_end) {
            std::fprintf(f, ".L%08x:\n", start);
        }
        for (uint32_t pc = start; pc < block.end; pc += 4) {
            const uint32_t raw = boot.image.read32(pc).value_or(0);
            std::fprintf(f, "  %08x:  %08x  %s\n", pc, raw, ppc::disassemble(ppc::decode(raw), pc).c_str());
        }
        const bool plain = block.exit == analysis::Exit::Fallthrough || block.exit == analysis::Exit::Jump ||
                           block.exit == analysis::Exit::Branch || block.exit == analysis::Exit::Return;
        if (!plain || !block.calls.empty()) {
            const auto exit = analysis::exit_name(block.exit);
            std::fprintf(f, "                      ; %.*s", int(exit.size()), exit.data());
            if (block.exit == analysis::Exit::JumpTable || block.exit == analysis::Exit::TailCall) {
                std::fprintf(f, " ->");
                for (uint32_t s : block.successors) std::fprintf(f, " %08x", s);
            }
            if (!block.calls.empty()) {
                std::fprintf(f, "; calls");
                for (uint32_t c : block.calls) std::fprintf(f, " %08x", c);
            }
            std::fprintf(f, "\n");
        }
        prev_end = block.end;
    }
    return std::fclose(f) == 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* dir = nullptr;
    const char* disasm_path = nullptr;
    std::string game_id = "spikeofe";
    std::vector<uint32_t> extra;
    bool list_functions = false, list_issues = false, list_tables = false;
    analysis::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) std::exit(usage());
            return argv[++i];
        };
        if (arg == "--game") game_id = next();
        else if (arg == "--entry") extra.push_back(static_cast<uint32_t>(std::strtoul(next(), nullptr, 0)));
        else if (arg == "--functions") list_functions = true;
        else if (arg == "--issues") list_issues = true;
        else if (arg == "--tables") list_tables = true;
        else if (arg == "--scan-pointers") options.scan_pointers = true;
        else if (arg == "--disasm") disasm_path = next();
        else if (arg == "--max-table") options.max_table_entries = static_cast<uint32_t>(std::strtoul(next(), nullptr, 0));
        else if (!dir) dir = argv[i];
        else return usage();
    }
    if (!dir) return usage();

    const m3::GameSpec* game = m3::games::find(game_id);
    if (!game) {
        std::fprintf(stderr, "codemap: unknown game '%s'\n", game_id.c_str());
        return 2;
    }
    const m3::RomSet set = m3::load_rom_set(*game, dir);
    if (!set.complete()) {
        std::fprintf(stderr, "codemap: ROM set incomplete; run romcheck for details\n");
        return 1;
    }

    analysis::BootImage boot = analysis::make_boot_image(*game, set);
    for (uint32_t addr : extra) boot.entries.push_back({addr, analysis::EntryKind::Seed, ""});
    // Scan the RAM copies' data for pointers, not the whole CROM: most of it
    // is graphics and other data full of small numbers that look like RAM
    // addresses.
    for (const analysis::Segment& seg : boot.image.segments())
        if (seg.base < m3::kWorkRamSize) options.pointer_segments.push_back(seg.name);
    const analysis::Analysis a = analysis::analyze(boot.image, boot.entries, options);

    std::printf("%.*s (%.*s)\n\n", int(game->title.size()), game->title.data(), int(game->id.size()), game->id.data());
    print_summary(boot, a);
    print_issues(a, list_issues);
    if (list_tables) print_tables(a);
    if (list_functions) print_functions(a);

    if (disasm_path) {
        if (!write_disasm(disasm_path, boot, a)) {
            std::fprintf(stderr, "codemap: cannot write %s\n", disasm_path);
            return 1;
        }
        std::printf("\nWrote annotated disassembly to %s\n", disasm_path);
    }
    return 0;
}
