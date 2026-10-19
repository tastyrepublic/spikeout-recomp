// Main-CPU bus of a Model 3 Step 2.1 board: work RAM, the program ROMs and
// the devices emulated so far. Accesses to anything else are open bus
// (reads return all ones, writes are dropped) and are counted per region so
// the runtime can report what the game touched.
//
// Memory is big-endian, as the CPU sees it: a 32-bit read of RAM returns the
// four bytes at that address with the first one in the top byte.
#pragma once

#include "runtime/memory_map.hpp"
#include "runtime/rom.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace m3 {

constexpr uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

// MPC106 PCI bridge: PCI configuration space through CONFIG_ADDR
// (0xFEC00CF8) and CONFIG_DATA (0xFEE00CFC). Only the bridge's own registers
// (bus 0, device 0) are emulated; other devices read as all ones. Reset
// values and the byte order of the data port follow Supermodel's
// Src/Model3/MPC10x.cpp: the registers are stored little-endian, as on PCI,
// and the data port hands the bytes to the CPU in address order, so the
// boot code reads them with lhbrx/lwbrx.
class Mpc106 {
public:
    Mpc106() { reset(); }
    void reset();

    // CONFIG_ADDR as written by the CPU (byte-reversed with stwbrx, since
    // the register is little-endian).
    void write_addr(uint32_t value) { addr_ = bswap32(value); }
    uint32_t read_addr() const { return bswap32(addr_); }
    uint32_t config_addr() const { return addr_; }

    // Byte `offset` (0-3) of the selected configuration register.
    uint8_t read_data(uint32_t offset) const;
    void write_data(uint32_t offset, uint8_t value);

    std::span<const uint8_t> registers() const { return regs_; }

private:
    bool bridge_selected() const { return ((addr_ >> 8) & 0xFFFF) == 0; } // bus 0, device 0, function 0
    uint32_t addr_ = 0;
    std::array<uint8_t, 256> regs_{};
};

class Bus {
public:
    // `roms` must outlive the bus. Without program ROMs (tests) the ROM
    // window reads as open bus.
    explicit Bus(const RomSet& roms);

    uint8_t read8(uint32_t addr) {
        return addr < kWorkRamSize ? ram_[addr] : read_byte(addr);
    }
    uint16_t read16(uint32_t addr) {
        if (addr < kWorkRamSize - 1) return static_cast<uint16_t>(ram_[addr] << 8 | ram_[addr + 1]);
        return static_cast<uint16_t>(read_byte(addr) << 8 | read_byte(addr + 1));
    }
    uint32_t read32(uint32_t addr) {
        if (addr < kWorkRamSize - 3) return load_be32(&ram_[addr]);
        return read_slow32(addr);
    }
    // 64-bit accesses are two 32-bit ones, high word first (Supermodel).
    uint64_t read64(uint32_t addr) { return uint64_t{read32(addr)} << 32 | read32(addr + 4); }

    void write8(uint32_t addr, uint8_t value) {
        if (addr < kWorkRamSize) ram_[addr] = value;
        else write_slow(addr, value, 1);
    }
    void write16(uint32_t addr, uint16_t value) {
        if (addr < kWorkRamSize - 1) {
            ram_[addr] = static_cast<uint8_t>(value >> 8);
            ram_[addr + 1] = static_cast<uint8_t>(value);
        } else {
            write_slow(addr, value, 2);
        }
    }
    void write32(uint32_t addr, uint32_t value) {
        if (addr < kWorkRamSize - 3) store_be32(&ram_[addr], value);
        else write_slow(addr, value, 4);
    }
    void write64(uint32_t addr, uint64_t value) {
        write32(addr, static_cast<uint32_t>(value >> 32));
        write32(addr + 4, static_cast<uint32_t>(value));
    }

    std::span<uint8_t> ram() { return ram_; }
    std::span<const uint8_t> ram() const { return ram_; }
    Mpc106& pci() { return pci_; }
    uint32_t crom_bank() const { return crom_bank_; }

    // Accesses to regions that aren't emulated yet, by region name
    // ("unmapped" for addresses no device decodes).
    struct Unhandled {
        uint64_t reads = 0;
        uint64_t writes = 0;
        uint32_t first_addr = 0;
    };
    const std::map<std::string_view, Unhandled>& unhandled() const { return unhandled_; }

    static uint32_t load_be32(const uint8_t* p) {
        return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
    }
    static void store_be32(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }

private:
    uint8_t read_byte(uint32_t addr);
    uint32_t read_slow32(uint32_t addr);
    // `value` holds the whole access, `size` bytes, big-endian.
    void write_slow(uint32_t addr, uint32_t value, uint32_t size);
    void count(const Region* region, uint32_t addr, bool write);
    uint8_t device_read(const Region& region, uint32_t addr) const;

    const RomSet& roms_;
    std::vector<uint8_t> ram_;
    Mpc106 pci_;
    uint32_t crom_bank_ = 0;
    uint32_t crom_bank_reg_ = 0xFF;
    std::map<std::string_view, Unhandled> unhandled_;
};

} // namespace m3
