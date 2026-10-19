#include "runtime/bus.hpp"

namespace m3 {

namespace {

// System register: CROM bank select (bank = ~value & 0xF; reset writes 0xFF).
constexpr uint32_t kSysCromBank = 0x08;

// Whether `addr` in `region` is emulated (anything else is open bus).
bool emulated(const Region& region, uint32_t addr) {
    switch (region.device) {
    case Device::FixedCrom:
    case Device::BankedCrom:
    case Device::PciConfigAddr:
    case Device::PciConfigData:
        return true;
    case Device::System:
        return region.offset(addr) == kSysCromBank;
    default:
        return false;
    }
}

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

Bus::Bus(const RomSet& roms) : roms_(roms), ram_(kWorkRamSize, 0) {}

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
        return static_cast<uint8_t>(crom_bank_reg_);
    default:
        return 0xFF;
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
    case Device::System: // CROM bank select: the byte at 0xF0100008
        crom_bank_reg_ = (value >> (8 * (size - 1))) & 0xFF;
        crom_bank_ = ~crom_bank_reg_ & 0xF;
        return;
    default:
        count(region, addr, true);
        return;
    }
}

} // namespace m3
