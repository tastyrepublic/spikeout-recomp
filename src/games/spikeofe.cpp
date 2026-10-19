// Spikeout: Final Edition (Sega, 1999), Model 3 Step 2.1.
//
// File names, sizes and CRC32s match MAME's "spikeofe" set; the interleave
// layout follows Supermodel's Games.xml. Only checksums live here — the ROM
// data itself has to come from the user's own dump.
#include "games/games.hpp"

#include <array>

namespace m3::games {

namespace {

constexpr uint32_t k2M = 0x200000;
constexpr uint32_t k4M = 0x400000;
constexpr uint32_t k8M = 0x800000;

constexpr std::array kCrom = {
    RomFile{"epr-21656.20", 0, k2M, 0xBD2AAF64},
    RomFile{"epr-21655.19", 2, k2M, 0x68A9E417},
    RomFile{"epr-21654.18", 4, k2M, 0x5BE245A3},
    RomFile{"epr-21653.17", 6, k2M, 0xF4BD9C3C},
};

constexpr std::array kBankedCrom = {
    // CROM0
    RomFile{"mpr-21616.4",  0x0000000, k8M, 0x2900BDD8},
    RomFile{"mpr-21615.3",  0x0000002, k8M, 0x7727A6FC},
    RomFile{"mpr-21614.2",  0x0000004, k8M, 0xE21D619B},
    RomFile{"mpr-21613.1",  0x0000006, k8M, 0xD039E608},
    // CROM1
    RomFile{"mpr-21620.8",  0x2000000, k8M, 0x476F027F},
    RomFile{"mpr-21619.7",  0x2000002, k8M, 0xE1076F47},
    RomFile{"mpr-21618.6",  0x2000004, k8M, 0x633530FA},
    RomFile{"mpr-21617.5",  0x2000006, k8M, 0xA08C6790},
    // CROM2
    RomFile{"mpr-21624.12", 0x4000000, k8M, 0xA158B7DA},
    RomFile{"mpr-21623.11", 0x4000002, k8M, 0xD9301674},
    RomFile{"mpr-21622.10", 0x4000004, k8M, 0x5F5A1563},
    RomFile{"mpr-21621.9",  0x4000006, k8M, 0x551A444D},
    // CROM3
    RomFile{"mpr-21628.16", 0x6000000, k4M, 0xDE3866EA},
    RomFile{"mpr-21627.15", 0x6000002, k4M, 0xEFE94608},
    RomFile{"mpr-21626.14", 0x6000004, k4M, 0x1861652E},
    RomFile{"mpr-21625.13", 0x6000006, k4M, 0x72A34707},
};

constexpr std::array kVrom = {
    RomFile{"mpr-21633.26", 0,  k4M, 0x735FB67D},
    RomFile{"mpr-21634.27", 2,  k4M, 0x876E6788},
    RomFile{"mpr-21635.28", 4,  k4M, 0x093534A8},
    RomFile{"mpr-21636.29", 6,  k4M, 0x2433F21C},
    RomFile{"mpr-21637.30", 8,  k4M, 0xEDB8F2B8},
    RomFile{"mpr-21638.31", 10, k4M, 0x3773A215},
    RomFile{"mpr-21639.32", 12, k4M, 0x313D1872},
    RomFile{"mpr-21640.33", 14, k4M, 0x271366BE},
    RomFile{"mpr-21641.34", 16, k4M, 0x782147E4},
    RomFile{"mpr-21642.35", 18, k4M, 0x844732C9},
    RomFile{"mpr-21643.36", 20, k4M, 0x9E922E9D},
    RomFile{"mpr-21644.37", 22, k4M, 0x617AA65A},
    RomFile{"mpr-21645.38", 24, k4M, 0x71396F52},
    RomFile{"mpr-21646.39", 26, k4M, 0x90FD9C87},
    RomFile{"mpr-21647.40", 28, k4M, 0xCF87991F},
    RomFile{"mpr-21648.41", 30, k4M, 0x30F974A1},
};

constexpr std::array kSoundProgram = {
    RomFile{"epr-21657.21", 0, 0x80000, 0x7242E8FD},
};

constexpr std::array kSoundSamples = {
    RomFile{"mpr-21629.22", 0x000000, k4M, 0xBC9701C4},
    RomFile{"mpr-21630.24", 0x400000, k4M, 0x9F2DEADD},
    RomFile{"mpr-21631.23", 0x800000, k4M, 0x299036C5},
    RomFile{"mpr-21632.25", 0xC00000, k4M, 0xFF162F0D},
};

constexpr std::array kMpegProgram = {
    RomFile{"epr-21658.ic2", 0, 0x20000, 0x50BAD8CB},
};

constexpr std::array kMpegMusic = {
    RomFile{"mpr-21649.ic18", 0x000000, k4M, 0xDAC87F47},
    RomFile{"mpr-21650.ic20", 0x400000, k4M, 0x86D90123},
    RomFile{"mpr-21651.ic22", 0x800000, k4M, 0x81715565},
    RomFile{"mpr-21652.ic24", 0xC00000, k4M, 0xE7C8C9BF},
};

constexpr std::array kRegions = {
    RomRegionSpec{RomRegionId::Crom,         0x0800000, 8,  2, true,  kCrom},
    RomRegionSpec{RomRegionId::BankedCrom,   0x7000000, 8,  2, true,  kBankedCrom},
    RomRegionSpec{RomRegionId::Vrom,         0x4000000, 32, 2, false, kVrom},
    RomRegionSpec{RomRegionId::SoundProgram, 0x0080000, 1,  1, true,  kSoundProgram},
    RomRegionSpec{RomRegionId::SoundSamples, 0x1000000, 1,  1, true,  kSoundSamples},
    RomRegionSpec{RomRegionId::MpegProgram,  0x0020000, 1,  1, true,  kMpegProgram},
    RomRegionSpec{RomRegionId::MpegMusic,    0x1000000, 1,  1, false, kMpegMusic},
};

// The boot code at 0xFFF03090 copies 1 MB of the fixed CROM to the bottom of
// RAM (with a running checksum, routine at 0xFFF033FC) and jumps to 0x100;
// the game runs from that copy with the low exception vectors.
constexpr std::array kBootCopies = {
    CodeCopy{0x00000000, 0xFFE00000, 0x00100000},
};

// PCI configuration accesses jump to `base + size * 12` with size 1, 2 or 4
// (mflr r0; ...; mtlr; blr): one stub per access width, each returning
// through r0. Read and write routines, in the boot ROM and in the game.
constexpr std::array kCodeEntries = {
    // Boot ROM, MPC106 (0xFFF030B4 read, 0xFFF03114 write)
    0xFFF030E4u, 0xFFF030F0u, 0xFFF03108u,
    0xFFF03144u, 0xFFF03150u, 0xFFF03168u,
    // Game, MPC106 (0x4078 read, 0x4130 write)
    0x000040DCu, 0x000040E8u, 0x00004100u,
    0x00004194u, 0x000041A0u, 0x000041B8u,
    // Game, the Real3D DMA device's own config ports at 0xC2000010/14
    0x00002AD0u, 0x00002ADCu, 0x00002AF4u,
    0x00002B44u, 0x00002B50u, 0x00002B68u,
};

} // namespace

const GameSpec kSpikeoutFinalEdition{
    "spikeofe",
    "Spikeout: Final Edition",
    0x29236FC8,
    kRegions,
    kBootCopies,
    kCodeEntries,
};

} // namespace m3::games
