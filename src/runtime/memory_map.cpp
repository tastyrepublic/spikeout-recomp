#include "runtime/memory_map.hpp"

#include <array>

namespace m3 {

namespace {

// Mirror used by MAME for the 0xF0xxxxxx system devices: they also respond
// at 0xF2xxxxxx, 0xF4xxxxxx, ... 0xFExxxxxx.
constexpr uint32_t kSysMirror = 0x0E000000;

constexpr std::array kRegions = {
    Region{Device::WorkRam,           "work_ram",          0x00000000, 0x007FFFFF, 0, kReadWrite},
    Region{Device::Real3dStatus,      "real3d_status",     0x84000000, 0x8400003F, 0, kRead},
    Region{Device::Real3dCommand,     "real3d_command",    0x88000000, 0x88000007, 0, kWrite},
    Region{Device::Real3dCullingLow,  "real3d_cull_low",   0x8C000000, 0x8C3FFFFF, 0, kWrite},
    Region{Device::Real3dCullingHigh, "real3d_cull_high",  0x8E000000, 0x8E0FFFFF, 0, kWrite},
    Region{Device::Real3dTexturePort, "real3d_tex_port",   0x90000000, 0x900000FF, 0, kWrite},
    Region{Device::Real3dTextureFifo, "real3d_tex_fifo",   0x94000000, 0x940FFFFF, 0, kWrite},
    Region{Device::Real3dPolygonRam,  "real3d_polygon",    0x98000000, 0x983FFFFF, 0, kWrite},
    Region{Device::Real3dConfig,      "real3d_config",     0x9C000000, 0x9CFFFFFF, 0, kWrite},
    Region{Device::NetBoard,          "netboard",          0xC0000000, 0xC003FFFF, 0, kReadWrite},
    Region{Device::Real3dDma,         "real3d_dma",        0xC2000000, 0xC20000FF, 0, kReadWrite},
    Region{Device::Io,                "io",                0xF0040000, 0xF004003F, kSysMirror, kReadWrite},
    Region{Device::SoundUart,         "sound_uart",        0xF0080000, 0xF008FFFF, kSysMirror, kReadWrite},
    Region{Device::BackupRam,         "backup_ram",        0xF00C0000, 0xF00DFFFF, kSysMirror, kReadWrite},
    Region{Device::System,            "system",            0xF0100000, 0xF010003F, kSysMirror, kReadWrite},
    Region{Device::Rtc,               "rtc",               0xF0140000, 0xF014003F, kSysMirror, kReadWrite},
    Region{Device::SecurityRam,       "security_ram",      0xF0180000, 0xF019FFFF, kSysMirror, kReadWrite},
    Region{Device::SecurityCrypt,     "security_crypt",    0xF01A0000, 0xF01A003F, kSysMirror, kReadWrite},
    Region{Device::TileChar,          "tile_char",         0xF1000000, 0xF10F7FFF, 0, kReadWrite},
    Region{Device::TileMap,           "tile_map",          0xF10F8000, 0xF10FFFFF, 0, kReadWrite},
    Region{Device::Palette,           "palette",           0xF1100000, 0xF111FFFF, 0, kReadWrite},
    Region{Device::TileRegs,          "tile_regs",         0xF1180000, 0xF11800FF, 0, kReadWrite},
    Region{Device::PciBridgeRegs,     "mpc106_regs",       0xF8FFF000, 0xF8FFF0FF, 0, kReadWrite},
    Region{Device::PciConfigAddr,     "mpc106_addr",       0xFEC00000, 0xFEDFFFFF, 0, kReadWrite},
    Region{Device::PciConfigData,     "mpc106_data",       0xFEE00000, 0xFEFFFFFF, 0, kReadWrite},
    Region{Device::BankedCrom,        "banked_crom",       0xFF000000, 0xFF7FFFFF, 0, kRead},
    Region{Device::FixedCrom,         "fixed_crom",        0xFF800000, 0xFFFFFFFF, 0, kRead},
};

} // namespace

std::span<const Region> step21_regions() { return kRegions; }

namespace {
const Region* search(uint32_t addr) {
    for (const Region& r : kRegions)
        if (r.contains(addr)) return &r;
    return nullptr;
}
// Each 64 KB block of the address space: the one region that covers it whole (its index + 1), none that touches it
// (kNone), or some only part of it (kMixed: searched). The regions don't overlap, so the same as searching; every
// mirror's bits are above the block's.
constexpr uint8_t kNone = 0xFE, kMixed = 0xFF;
const std::array<uint8_t, 0x10000> kBlocks = [] {
    static_assert(kRegions.size() < kNone);
    std::array<uint8_t, 0x10000> blocks{};
    for (uint32_t b = 0; b < 0x10000; ++b) {
        const uint32_t lo = b << 16, hi = lo | 0xFFFF;
        uint8_t at = kNone;
        for (std::size_t i = 0; i < kRegions.size(); ++i) {
            const Region& r = kRegions[i];
            const uint32_t a = lo & ~r.mirror, z = hi & ~r.mirror;
            if (z < r.start || a > r.end) continue; // (not touched)
            at = a >= r.start && z <= r.end && at == kNone ? static_cast<uint8_t>(i + 1) : kMixed;
            if (at == kMixed) break;
        }
        blocks[b] = at;
    }
    return blocks;
}();
} // namespace

const Region* find_region(uint32_t addr) {
    const uint8_t at = kBlocks[addr >> 16];
    if (at == kNone) return nullptr;
    if (at != kMixed) return &kRegions[at - 1u];
    return search(addr);
}

} // namespace m3
