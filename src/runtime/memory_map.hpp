// Main-CPU address map of a Sega Model 3 Step 2.1 board (Spikeout,
// Daytona USA 2, Star Wars Trilogy, ...), with the 315-5881 security board.
//
// Source: MAME's sega/model3.cpp (model3_10_mem, model3_mem,
// model3_5881_mem and init_model3_20). See docs/memory-map.md.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace m3 {

enum class Device : uint8_t {
    WorkRam,          // 8 MB main RAM
    Real3dStatus,     // Real3D Pro-1000 status registers (read)
    Real3dCommand,    // Real3D command port (write)
    Real3dDisplayList,// Real3D culling/display list RAM (write)
    Real3dPolygonRam, // Real3D polygon RAM (write)
    Real3dDma,        // Real3D DMA controller (Step 2.x)
    NetBoard,         // Model 3 comm/network board (cabinet link)
    Io,               // Sega 315-5649 I/O: inputs, EEPROM, outputs
    SoundUart,        // i8251 UART to the sound board (MIDI-style link)
    BackupRam,        // battery-backed SRAM
    System,           // IRQ status/enable, CROM bank select, ...
    Rtc,              // real-time clock
    SecurityRam,      // RAM read by the 315-5881 decrypter
    SecurityCrypt,    // 315-5881 data decryption ports
    TileChar,         // Tilegen character RAM (2D layers)
    TileMap,          // Tilegen tilemaps
    Palette,          // Tilegen palette RAM
    TileRegs,         // Tilegen video registers
    PciConfigAddr,    // MPC106 PCI bridge config address
    PciConfigData,    // MPC106 PCI bridge config data
    PciBridgeRegs,    // MPC106 internal registers
    BankedCrom,       // 8 MB window into the banked program ROMs
    FixedCrom,        // 8 MB fixed program ROM (holds the reset vector)
};

enum Access : uint8_t {
    kRead = 1,
    kWrite = 2,
    kReadWrite = kRead | kWrite,
};

struct Region {
    Device device;
    std::string_view name;
    uint32_t start;
    uint32_t end;    // inclusive
    uint32_t mirror; // address bits ignored by the decoder
    uint8_t access;

    constexpr uint32_t size() const { return end - start + 1; }
    constexpr bool contains(uint32_t addr) const {
        const uint32_t a = addr & ~mirror;
        return a >= start && a <= end;
    }
    // Offset of `addr` inside the region, with mirror bits removed.
    constexpr uint32_t offset(uint32_t addr) const { return (addr & ~mirror) - start; }
};

std::span<const Region> step21_regions();

// Region that decodes `addr`, or nullptr for unmapped addresses.
const Region* find_region(uint32_t addr);

// Fixed constants the runtime relies on.
inline constexpr uint32_t kWorkRamSize = 0x00800000;
inline constexpr uint32_t kFixedCromBase = 0xFF800000;
inline constexpr uint32_t kBankedCromBase = 0xFF000000;
inline constexpr uint32_t kCromWindowSize = 0x00800000;
inline constexpr uint32_t kResetVector = 0xFFF00100; // MSR[IP]=1 system reset

} // namespace m3
