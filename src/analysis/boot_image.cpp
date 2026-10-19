#include "analysis/boot_image.hpp"

#include "ppc/instruction.hpp"
#include "runtime/memory_map.hpp"

#include <array>
#include <string>

namespace analysis {

namespace {

constexpr std::array kVectors = {
    ExceptionVector{0x0100, "system_reset"},
    ExceptionVector{0x0200, "machine_check"},
    ExceptionVector{0x0300, "dsi"},
    ExceptionVector{0x0400, "isi"},
    ExceptionVector{0x0500, "external_interrupt"},
    ExceptionVector{0x0600, "alignment"},
    ExceptionVector{0x0700, "program"},
    ExceptionVector{0x0800, "fp_unavailable"},
    ExceptionVector{0x0900, "decrementer"},
    ExceptionVector{0x0C00, "system_call"},
    ExceptionVector{0x0D00, "trace"},
    ExceptionVector{0x1000, "itlb_miss"},
    ExceptionVector{0x1100, "dtlb_load_miss"},
    ExceptionVector{0x1200, "dtlb_store_miss"},
    ExceptionVector{0x1300, "instruction_breakpoint"},
    ExceptionVector{0x1400, "system_management"},
};

constexpr uint32_t kHighVectorBase = 0xFFF00000;

// A vector is used if its first word is a real instruction.
void add_vectors(const CodeImage& image, uint32_t base, std::string_view prefix, std::vector<Entry>& out) {
    for (const ExceptionVector& v : kVectors) {
        const uint32_t addr = base + v.offset;
        const auto word = image.read32(addr);
        if (!word || !ppc::decode(*word).valid()) continue;
        out.push_back({addr, EntryKind::Seed, std::string(prefix) + std::string(v.name)});
    }
}

} // namespace

std::span<const ExceptionVector> exception_vectors() { return kVectors; }

BootImage make_boot_image(const m3::GameSpec& game, const m3::RomSet& set) {
    BootImage boot;
    boot.image.add("crom", m3::kFixedCromBase, set.region(m3::RomRegionId::Crom));
    add_vectors(boot.image, kHighVectorBase, "", boot.entries);

    int n = 0;
    for (const m3::CodeCopy& copy : game.boot_copies) {
        std::vector<uint8_t> bytes(copy.size);
        for (uint32_t off = 0; off + 4 <= copy.size; off += 4) {
            const uint32_t word = set.read_program32(copy.src + off);
            bytes[off] = static_cast<uint8_t>(word >> 24);
            bytes[off + 1] = static_cast<uint8_t>(word >> 16);
            bytes[off + 2] = static_cast<uint8_t>(word >> 8);
            bytes[off + 3] = static_cast<uint8_t>(word);
        }
        const std::string name = n++ == 0 ? "ram" : "ram" + std::to_string(n);
        boot.image.add(name, copy.dst, std::move(bytes));
        if (copy.dst == 0) add_vectors(boot.image, 0, "ram_", boot.entries);
    }
    return boot;
}

} // namespace analysis
