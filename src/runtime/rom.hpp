// ROM set description and loading.
//
// A game is a list of ROM regions; each region is assembled from one or
// more chip dumps. Chips that share a 64-bit (CROM) or 256-bit (VROM) data
// bus are interleaved: every `chunk_size` bytes of a file are placed
// `stride` bytes apart, starting at the file's offset in the region.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace m3 {

enum class RomRegionId : uint8_t {
    Crom,         // fixed program ROM, mapped at 0xFF800000
    BankedCrom,   // banked program/data ROM, 8 MB window at 0xFF000000
    Vrom,         // Real3D texture ROM
    SoundProgram, // 68000 code for the sound board
    SoundSamples, // SCSP samples
    MpegProgram,  // DSB2 MPEG board 68000 code
    MpegMusic,    // DSB2 MPEG audio stream data
    Count
};

std::string_view rom_region_name(RomRegionId id);

struct RomFile {
    std::string_view name;
    uint32_t offset; // first byte's position inside the region
    uint32_t size;
    uint32_t crc32;
};

struct RomRegionSpec {
    RomRegionId id;
    uint32_t size;
    uint32_t stride;
    uint32_t chunk_size;
    bool byte_swap; // swap each pair of bytes in the file before placing it
    std::span<const RomFile> files;
};

// A block of program ROM that the boot code copies into RAM and then runs
// from there.
struct CodeCopy {
    uint32_t dst;  // RAM address
    uint32_t src;  // CPU address in the program ROMs
    uint32_t size; // bytes
};

struct GameSpec {
    std::string_view id;    // MAME / Supermodel set name
    std::string_view title;
    uint32_t security_key;  // 315-5881 key
    std::span<const RomRegionSpec> regions;
    std::span<const CodeCopy> boot_copies = {};
    // Function entries the control-flow analysis can't find by itself
    // (targets of computed jumps), found by reading the code.
    std::span<const uint32_t> code_entries = {};
    // Blocks whose start calls recomp::Context::hook: game-specific observers
    // that read the game's state and never change it (widescreen's sides
    // from the game's own tile maps: games/spikeofe_hooks.cpp).
    std::span<const uint32_t> hooks = {};
};

// Places one file's data into a region buffer according to `spec`.
// `data.size()` must be a multiple of `spec.chunk_size`.
void place_rom_file(const RomRegionSpec& spec, const RomFile& file, std::span<const uint8_t> data,
                    std::span<uint8_t> region);

uint32_t crc32(std::span<const uint8_t> data);

enum class RomFileStatus : uint8_t { Ok, Missing, WrongSize, WrongChecksum };

struct RomFileReport {
    RomRegionId region;
    std::string name;
    RomFileStatus status;
    uint64_t actual_size = 0;
    uint32_t actual_crc = 0;
    uint32_t expected_crc = 0;
};

// Byte order fix-up applied to the program ROMs so that address A in the
// CPU's view is byte (A - base) in the buffer (big-endian, as the PPC sees it).
enum class WordOrder : uint8_t {
    AsIs,
    Swap16,    // swap the bytes of each 16-bit word
    Swap32,    // swap the 16-bit halves of each 32-bit word
    Reverse32, // reverse the bytes of each 32-bit word
};

std::string_view word_order_name(WordOrder order);
void apply_word_order(std::span<uint8_t> data, WordOrder order);

struct RomSet {
    std::vector<uint8_t> regions[static_cast<size_t>(RomRegionId::Count)];
    std::vector<RomFileReport> report;
    WordOrder program_word_order = WordOrder::AsIs;

    std::vector<uint8_t>& region(RomRegionId id) { return regions[static_cast<size_t>(id)]; }
    const std::vector<uint8_t>& region(RomRegionId id) const { return regions[static_cast<size_t>(id)]; }

    // True when every file was found with the right size. Checksum
    // mismatches are reported but do not prevent loading (e.g. hacks,
    // bad dumps someone wants to compare).
    bool complete() const;

    // Big-endian 32-bit read from the CPU's view of the program ROMs.
    // `bank` selects the 8 MB bank shown at 0xFF000000.
    uint32_t read_program32(uint32_t addr, uint32_t bank = 0) const;
};

// Load every region of `game` from a directory holding the extracted ROM
// files (e.g. the contents of MAME's spikeofe.zip). File name matching is
// case-insensitive. After loading, the program ROM word order is detected
// from the reset vector and normalized.
RomSet load_rom_set(const GameSpec& game, const std::filesystem::path& dir);

// Pick the word order under which the code at the reset vector looks most
// like valid PowerPC code. Exposed for testing.
WordOrder detect_program_word_order(std::span<const uint8_t> fixed_crom);

} // namespace m3
