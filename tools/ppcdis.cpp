// ppcdis: disassemble a raw big-endian PowerPC binary.
//
//   ppcdis <file> [--base ADDR] [--offset N] [--count N]
//
// --base   address of the first byte of the file (default 0)
// --offset byte offset into the file to start at (default 0)
// --count  number of instructions (default: to end of file)
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr, "usage: ppcdis <file> [--base ADDR] [--offset N] [--count N]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const char* path = nullptr;
    uint64_t base = 0, offset = 0, count = UINT64_MAX;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> uint64_t {
            if (i + 1 >= argc) std::exit(usage());
            return std::strtoull(argv[++i], nullptr, 0);
        };
        if (arg == "--base") base = value();
        else if (arg == "--offset") offset = value();
        else if (arg == "--count") count = value();
        else if (!path) path = argv[i];
        else return usage();
    }
    if (!path) return usage();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "ppcdis: cannot open %s\n", path);
        return 1;
    }
    const std::vector<unsigned char> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    for (uint64_t off = offset & ~uint64_t{3}; off + 4 <= data.size() && count > 0; off += 4, --count) {
        const uint32_t raw = (uint32_t{data[off]} << 24) | (uint32_t{data[off + 1]} << 16) |
                             (uint32_t{data[off + 2]} << 8) | uint32_t{data[off + 3]};
        const uint32_t pc = static_cast<uint32_t>(base + off);
        std::printf("%08x:  %08x  %s\n", pc, raw, ppc::disassemble(ppc::decode(raw), pc).c_str());
    }
    return 0;
}
