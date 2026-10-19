#include "runtime/bus.hpp"

namespace m3 {

namespace {

// System registers (Supermodel's Model3.cpp, Read/WriteSystemRegister).
constexpr uint32_t kSysCromBank = 0x08;  // CROM bank select: bank = ~value & 0xF (reset writes 0xFF)
constexpr uint32_t kSysIrqEnable = 0x14;
constexpr uint32_t kSysIrqState = 0x18;  // read: pending; write: acknowledge the written bits

// Whether `addr` in `region` is emulated (anything else is open bus).
bool emulated(const Region& region, uint32_t) {
    switch (region.device) {
    case Device::FixedCrom:
    case Device::BankedCrom:
    case Device::PciConfigAddr:
    case Device::PciConfigData:
    case Device::System:
    case Device::TileRegs:
    case Device::TileChar:
    case Device::TileMap:
    case Device::Palette:
    case Device::BackupRam:
        return true;
    default:
        return false;
    }
}

constexpr uint32_t kTileIrqAck = 0x10;

} // namespace

void Mpc106::reset() {
    addr_ = 0;
    regs_.fill(0);
    auto put = [&](uint32_t reg, uint32_t value) { // little-endian, as on PCI
        for (uint32_t i = 0; i < 4; ++i) regs_[reg + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    // Supermodel, Src/Model3/MPC10x.cpp, CMPC10x::Reset() for the MPC106.
    put(0x00, 0x00021057); // vendor 0x1057 (Motorola), device 0x0002 (MPC106)
    put(0x04, 0x00800006); // command, status
    put(0x08, 0x00060000); // revision 0, class: host bridge
    put(0x0C, 0x00000800);
    put(0x70, 0x00CD0000);
    put(0xA8, 0x0010FF00);
    put(0xAC, 0x060C000C);
    put(0xB8, 0x04000000);
    put(0xC0, 0x00000100);
    put(0xE0, 0x00420FFF);
    put(0xE8, 0x00200000);
    put(0xF0, 0x0000FF02);
    put(0xF4, 0x00030000);
    put(0xFC, 0x00000010);
}

uint8_t Mpc106::read_data(uint32_t offset) const {
    if (!bridge_selected()) return 0xFF; // no other PCI device is emulated
    return regs_[((addr_ & 0xFC) + offset) & 0xFF];
}

void Mpc106::write_data(uint32_t offset, uint8_t value) {
    if (bridge_selected()) regs_[((addr_ & 0xFC) + offset) & 0xFF] = value;
}

Bus::Bus(const RomSet& roms)
    : roms_(roms), ram_(kWorkRamSize, 0), tile_ram_(kTileRamSize, 0), backup_ram_(kBackupRamSize, 0) {}

void Bus::count(const Region* region, uint32_t addr, bool write) {
    Unhandled& u = unhandled_[region ? region->name : std::string_view("unmapped")];
    if (u.reads == 0 && u.writes == 0) u.first_addr = addr;
    ++(write ? u.writes : u.reads);
}

uint8_t Bus::device_read(const Region& region, uint32_t addr) const {
    switch (region.device) {
    case Device::FixedCrom: {
        const auto& crom = roms_.region(RomRegionId::Crom);
        const uint32_t off = addr - kFixedCromBase;
        return off < crom.size() ? crom[off] : 0xFF;
    }
    case Device::BankedCrom: {
        const auto& banked = roms_.region(RomRegionId::BankedCrom);
        const size_t off = size_t{crom_bank_} * kCromWindowSize + (addr - kBankedCromBase);
        return off < banked.size() ? banked[off] : 0xFF;
    }
    case Device::PciConfigAddr:
        return static_cast<uint8_t>(pci_.read_addr() >> (24 - 8 * (addr & 3)));
    case Device::PciConfigData:
        return pci_.read_data(addr & 3);
    case Device::System:
        return system_read(region.offset(addr));
    case Device::TileChar:
    case Device::TileMap:
    case Device::Palette:
        return tile_ram_[addr - kTileRamBase];
    case Device::BackupRam:
        return backup_ram_[region.offset(addr) % kBackupRamSize];
    default:
        return 0xFF; // Tilegen registers: only 32-bit reads reach them
    }
}

uint8_t Bus::system_read(uint32_t reg) const {
    switch (reg) {
    case kSysCromBank:  return static_cast<uint8_t>(crom_bank_reg_);
    case 0x10:          return 0; // JTAG TDO (bit 5): no JTAG chain emulated yet
    case kSysIrqEnable: return static_cast<uint8_t>(irq_.enable());
    case kSysIrqState:  return static_cast<uint8_t>(irq_.state());
    default:            return 0xFF; // 0x1C: games expect bits set
    }
}

void Bus::system_write(uint32_t reg, uint8_t value) {
    switch (reg) {
    case kSysCromBank:
        crom_bank_reg_ = value;
        crom_bank_ = ~crom_bank_reg_ & 0xF;
        break;
    case kSysIrqEnable: irq_.set_enable(value); break;
    case kSysIrqState:  irq_.deassert(value); break;
    default: break; // 0x0C JTAG (Real3D configuration), 0x0D-0x0F and 0x1C LEDs
    }
}

uint8_t Bus::read_byte(uint32_t addr) {
    if (addr < kWorkRamSize) return ram_[addr];
    const Region* region = find_region(addr);
    if (!region || !emulated(*region, addr)) {
        count(region, addr, false);
        return 0xFF;
    }
    return device_read(*region, addr);
}

uint32_t Bus::read_slow32(uint32_t addr) {
    if (addr < kWorkRamSize) { // straddles the end of RAM
        uint32_t v = 0;
        for (uint32_t i = 0; i < 4; ++i) v = v << 8 | read_byte(addr + i);
        return v;
    }
    const Region* region = find_region(addr);
    if (!region || !emulated(*region, addr)) {
        count(region, addr, false);
        return 0xFFFFFFFF;
    }
    if (region->device == Device::TileRegs) return bswap32(tile_reg(region->offset(addr)));
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v = v << 8 | device_read(*region, addr + i);
    return v;
}

void Bus::write_slow(uint32_t addr, uint32_t value, uint32_t size) {
    if (addr < kWorkRamSize) { // straddles the end of RAM
        for (uint32_t i = 0; i < size; ++i)
            if (addr + i < kWorkRamSize) ram_[addr + i] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        return;
    }
    const Region* region = find_region(addr);
    if (!region || !emulated(*region, addr) || !(region->access & kWrite)) {
        count(region, addr, true);
        return;
    }
    switch (region->device) {
    case Device::PciConfigAddr:
        if (size == 4) pci_.write_addr(value);
        else count(region, addr, true);
        return;
    case Device::PciConfigData:
        for (uint32_t i = 0; i < size; ++i)
            pci_.write_data(((addr & 3) + i) & 3, static_cast<uint8_t>(value >> (8 * (size - 1 - i))));
        return;
    case Device::TileChar:
    case Device::TileMap:
    case Device::Palette:
        for (uint32_t i = 0; i < size; ++i)
            if (addr + i - kTileRamBase < kTileRamSize)
                tile_ram_[addr + i - kTileRamBase] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        return;
    case Device::BackupRam:
        for (uint32_t i = 0; i < size; ++i)
            backup_ram_[(region->offset(addr) + i) % kBackupRamSize] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        return;
    case Device::TileRegs: { // 32-bit only; narrower writes don't reach the registers
        if (size != 4) {
            count(region, addr, true);
            return;
        }
        const uint32_t reg = region->offset(addr) & 0xFC, v = bswap32(value);
        tile_regs_[reg / 4] = v;
        if (reg == kTileIrqAck) irq_.deassert(v & 0xFF);
        return;
    }
    case Device::System: // byte registers; wider writes reach several of them
        for (uint32_t i = 0; i < size; ++i)
            system_write(region->offset(addr) + i, static_cast<uint8_t>(value >> (8 * (size - 1 - i))));
        return;
    default:
        count(region, addr, true);
        return;
    }
}

} // namespace m3
