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
    Region{Device::Real3dDisplayList, "real3d_display",    0x8E000000, 0x8E0FFFFF, 0, kWrite},
    Region{Device::Real3dPolygonRam,  "real3d_polygon",    0x98000000, 0x980FFFFF, 0, kWrite},
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

const Region* find_region(uint32_t addr) {
    for (const Region& r : kRegions)
        if (r.contains(addr)) return &r;
    return nullptr;
}

} // namespace m3
