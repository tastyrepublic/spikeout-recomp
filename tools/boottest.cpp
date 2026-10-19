// boottest: run the recompiled boot ROM and check what it did.
//
//   boottest <rom_dir>
//
// Built only when CMake's SPIKEOUT_ROM_DIR is set: the boot ROM is
// recompiled from your ROMs at build time (generated/boot_rom.cpp).
//
// From the reset vector the boot code probes and sets up the MPC106 PCI
// bridge, sizes and clears RAM, copies 1 MB of the program ROM to RAM 0
// with a running checksum, stores the PVR at 0x80 and jumps to 0x100, the
// RAM copy, which isn't recompiled here: reaching it ends the run. The test
// passes when the RAM copy matches the ROM byte for byte.
#include "games/games.hpp"
#include "recomp/context.hpp"
#include "runtime/bus.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/rom.hpp"

#include <cstdio>
#include <span>

namespace generated {
std::span<const recomp::FunctionEntry> boot_rom_functions();
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: boottest <rom_dir>\n");
        return 2;
    }
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const m3::RomSet roms = m3::load_rom_set(game, argv[1]);
    if (!roms.complete()) {
        std::fprintf(stderr, "boottest: ROM set incomplete; run romcheck for details\n");
        return 1;
    }

    m3::Bus bus(roms);
    // RAM holds garbage at power-on; the boot code clears it.
    for (uint8_t& b : bus.ram()) b = 0xA5;
    recomp::Context c(bus, generated::boot_rom_functions());
    constexpr int64_t kBudget = 100'000'000; // blocks
    c.budget = kBudget;
    std::printf("Recompiled boot ROM: %zu functions\n", generated::boot_rom_functions().size());

    uint32_t stopped_at = 0;
    try {
        c.call(c.cpu.pc); // 0xFFF00100
        std::printf("FAIL: the reset code returned\n");
        return 1;
    } catch (const recomp::MissingCode& e) {
        stopped_at = e.addr;
    } catch (const recomp::GuestError& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("Ran %lld blocks, stopped at %08x (no recompiled code there)\n",
                static_cast<long long>(kBudget - c.budget), stopped_at);

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
        failures += !ok;
    };
    check(stopped_at == 0x100, "jumped to the RAM copy at 0x100");

    // The copy: RAM 0..1 MB against the fixed CROM at 0xFFE00000, except the
    // two words stored afterwards.
    const auto& copy = game.boot_copies[0];
    const auto& crom = roms.region(m3::RomRegionId::Crom);
    const auto ram = bus.ram();
    uint32_t mismatches = 0, first_mismatch = 0;
    for (uint32_t off = 0; off < copy.size; ++off) {
        if (off >= 0x80 && off < 0x88) continue;
        if (ram[copy.dst + off] != crom[copy.src - m3::kFixedCromBase + off]) {
            if (mismatches++ == 0) first_mismatch = off;
        }
    }
    if (mismatches) std::printf("    %u bytes differ, first at %08x\n", mismatches, first_mismatch);
    check(mismatches == 0, "RAM 0x000000-0x0FFFFF is a copy of ROM 0xFFE00000-0xFFEFFFFF");
    check(m3::Bus::load_be32(&ram[0x80]) == c.cpu.pvr, "PVR stored at 0x80");
    std::printf("    0x84 = %08x (bridge device ID << 16 | revision << 8)\n", m3::Bus::load_be32(&ram[0x84]));

    // The rest of RAM was cleared (it started out as 0xA5 garbage).
    bool cleared = true;
    for (uint32_t off = copy.size; off < m3::kWorkRamSize; ++off) cleared &= ram[off] == 0;
    check(cleared, "RAM above the copy was cleared");

    // What the boot code changed in the bridge's configuration registers.
    const m3::Mpc106 fresh;
    const auto now = bus.pci().registers(), before = fresh.registers();
    std::printf("\nMPC106 registers written (reg: before -> after, little-endian dwords):\n");
    for (uint32_t reg = 0; reg < 256; reg += 4) {
        uint32_t a = 0, b = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            a |= uint32_t{before[reg + i]} << (8 * i);
            b |= uint32_t{now[reg + i]} << (8 * i);
        }
        if (a != b) std::printf("  %02x: %08x -> %08x\n", reg, a, b);
    }

    std::printf("\nDevices accessed that aren't emulated yet:\n");
    if (bus.unhandled().empty()) std::printf("  none\n");
    for (const auto& [region, u] : bus.unhandled())
        std::printf("  %-16.*s %llu reads, %llu writes (first at %08x)\n", int(region.size()), region.data(),
                    static_cast<unsigned long long>(u.reads), static_cast<unsigned long long>(u.writes),
                    u.first_addr);

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
