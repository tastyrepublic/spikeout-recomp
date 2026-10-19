#include "test.hpp"

#include "games/games.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/rom.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace m3;

TEST_CASE("memory map: known addresses") {
    CHECK_EQ(find_region(0x00000000)->device, Device::WorkRam);
    CHECK_EQ(find_region(0x007FFFFF)->device, Device::WorkRam);
    CHECK(find_region(0x00800000) == nullptr);
    CHECK_EQ(find_region(0x84000010)->device, Device::Real3dStatus);
    CHECK_EQ(find_region(0xC0001000)->device, Device::NetBoard);
    CHECK_EQ(find_region(0xF0040000)->device, Device::Io);
    CHECK_EQ(find_region(0xFE040000)->device, Device::Io);      // mirror
    CHECK_EQ(find_region(0xF20C0000)->device, Device::BackupRam); // mirror
    CHECK_EQ(find_region(0xF10F8000)->device, Device::TileMap);
    CHECK_EQ(find_region(0xFEC00000)->device, Device::PciConfigAddr);
    CHECK_EQ(find_region(0xFF000000)->device, Device::BankedCrom);
    CHECK_EQ(find_region(kResetVector)->device, Device::FixedCrom);
    CHECK_EQ(find_region(0xFE0C0004)->offset(0xFE0C0004), 4u);
}

TEST_CASE("memory map: regions never overlap") {
    // Probe both ends of every region (and of every mirror copy); each probe
    // must decode to exactly one region.
    for (const Region& r : step21_regions()) {
        CHECK(r.start <= r.end);
        std::vector<uint32_t> probes = {r.start, r.end};
        if (r.mirror) {
            for (uint32_t m = 0; m <= 7; ++m) {
                probes.push_back(r.start | (m << 25));
                probes.push_back(r.end | (m << 25));
            }
        }
        for (uint32_t addr : probes) {
            int hits = 0;
            for (const Region& other : step21_regions()) hits += other.contains(addr);
            if (hits != 1) std::fprintf(stderr, "  address 0x%08x hits %d regions\n", addr, hits);
            CHECK_EQ(hits, 1);
        }
    }
}

TEST_CASE("crc32: reference value") {
    const char* s = "123456789";
    CHECK_EQ(crc32({reinterpret_cast<const uint8_t*>(s), 9}), 0xCBF43926u);
}

TEST_CASE("rom: interleaved placement with byte swap") {
    // Two chips sharing a 32-bit bus, 16 bits each, stored byte-swapped.
    const std::array files = {RomFile{"a", 0, 4, 0}, RomFile{"b", 2, 4, 0}};
    const RomRegionSpec spec{RomRegionId::Crom, 8, 4, 2, true, files};
    std::vector<uint8_t> region(8, 0);
    const std::vector<uint8_t> a = {0x11, 0x00, 0x33, 0x22};
    const std::vector<uint8_t> b = {0x55, 0x44, 0x77, 0x66};
    place_rom_file(spec, files[0], a, region);
    place_rom_file(spec, files[1], b, region);
    CHECK((region == std::vector<uint8_t>{0x00, 0x11, 0x44, 0x55, 0x22, 0x33, 0x66, 0x77}));
}

TEST_CASE("rom: word order transforms") {
    std::vector<uint8_t> d = {1, 2, 3, 4};
    apply_word_order(d, WordOrder::Swap16);
    CHECK((d == std::vector<uint8_t>{2, 1, 4, 3}));
    d = {1, 2, 3, 4};
    apply_word_order(d, WordOrder::Swap32);
    CHECK((d == std::vector<uint8_t>{3, 4, 1, 2}));
    d = {1, 2, 3, 4};
    apply_word_order(d, WordOrder::Reverse32);
    CHECK((d == std::vector<uint8_t>{4, 3, 2, 1}));
}

TEST_CASE("rom: word order detection at the reset vector") {
    // Synthetic boot code: a typical PPC reset handler prologue.
    const std::array<uint32_t, 8> code = {
        0x7C0000A6, // mfmsr r0
        0x3C20FFF0, // lis r1, 0xfff0
        0x60210000, // ori r1, r1, 0
        0x7C0802A6, // mflr r0
        0x7C1A03A6, // mtsrr0 r0
        0x38600000, // li r3, 0
        0x4C00012C, // isync
        0x4C000064, // rfi
    };
    std::vector<uint8_t> crom(kCromWindowSize, 0);
    const size_t base = kResetVector - kFixedCromBase;
    for (size_t i = 0; i < 64; ++i) {
        const uint32_t w = code[i % code.size()];
        for (size_t b = 0; b < 4; ++b) crom[base + i * 4 + b] = static_cast<uint8_t>(w >> (24 - 8 * b));
    }
    CHECK_EQ(detect_program_word_order(crom), WordOrder::AsIs);

    for (WordOrder order : {WordOrder::Swap16, WordOrder::Swap32, WordOrder::Reverse32}) {
        std::vector<uint8_t> scrambled = crom;
        apply_word_order(scrambled, order); // all three transforms are self-inverse
        CHECK_EQ(detect_program_word_order(scrambled), order);
    }
}

TEST_CASE("games: Spikeout Final Edition spec is consistent") {
    const GameSpec* g = games::find("spikeofe");
    CHECK(g != nullptr);
    if (!g) return;

    std::set<std::string> names;
    size_t file_count = 0;
    for (const RomRegionSpec& spec : g->regions) {
        for (const RomFile& f : spec.files) {
            ++file_count;
            names.insert(std::string(f.name));
            CHECK(f.size % spec.chunk_size == 0);
            const uint64_t last = f.offset + uint64_t(f.size / spec.chunk_size - 1) * spec.stride + spec.chunk_size;
            CHECK(last <= spec.size);
        }
    }
    CHECK_EQ(file_count, names.size()); // no duplicate file names
    CHECK_EQ(file_count, size_t{46});
}

TEST_CASE("rom: loading a directory with no files reports everything missing") {
    const GameSpec* g = games::find("spikeofe");
    const RomSet set = load_rom_set(*g, std::filesystem::temp_directory_path() / "spikeout-recomp-nonexistent");
    CHECK(!set.complete());
    CHECK_EQ(set.report.size(), size_t{46});
    for (const auto& r : set.report) CHECK(r.status == RomFileStatus::Missing);
}

TEST_CASE("rom: load_rom_set reads, verifies and interleaves files from disk") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "spikeout-recomp-test-roms";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::vector<uint8_t> a = {0x11, 0x00, 0x33, 0x22};
    const std::vector<uint8_t> b = {0x55, 0x44, 0x77, 0x66};
    auto write = [&](const char* name, const std::vector<uint8_t>& data) {
        std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), 4);
    };
    write("CHIP_A.BIN", a); // upper case on disk: matching must ignore case
    write("chip_b.bin", b);

    const std::array files = {
        RomFile{"chip_a.bin", 0, 4, crc32(a)},
        RomFile{"chip_b.bin", 2, 4, 0xDEADBEEF}, // deliberately wrong CRC
        RomFile{"chip_c.bin", 0, 4, 0},          // not present
    };
    const std::array regions = {RomRegionSpec{RomRegionId::Vrom, 8, 4, 2, true, files}};
    const GameSpec game{"test", "Test", 0, regions};

    const RomSet set = load_rom_set(game, dir);
    CHECK_EQ(set.report.size(), size_t{3});
    CHECK(set.report[0].status == RomFileStatus::Ok);
    CHECK(set.report[1].status == RomFileStatus::WrongChecksum);
    CHECK_EQ(set.report[1].actual_crc, crc32(b));
    CHECK(set.report[2].status == RomFileStatus::Missing);
    CHECK(!set.complete());
    CHECK((set.region(RomRegionId::Vrom) == std::vector<uint8_t>{0x00, 0x11, 0x44, 0x55, 0x22, 0x33, 0x66, 0x77}));

    fs::remove_all(dir);
}
