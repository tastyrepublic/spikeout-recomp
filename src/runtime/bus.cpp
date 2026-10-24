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
    case Device::NetBoard:
    case Device::Real3dDma:
    case Device::Real3dStatus:
    case Device::Io:
    case Device::SecurityRam:
    case Device::SecurityCrypt:
    case Device::Rtc:
    case Device::SoundUart:
    case Device::Real3dCommand:
    case Device::Real3dCullingLow:
    case Device::Real3dCullingHigh:
    case Device::Real3dTextureFifo:
    case Device::Real3dPolygonRam:
    case Device::Real3dConfig:
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
    // Reset values from the MPC106 user's manual (MPC106UM/AD Rev. 2, Table
    // 3-10, the register tables, and the addendum's ODCR), as register values.
    // Supermodel's (MPC10x.cpp) are the same numbers stored the other way
    // round: halves swapped, and some single bytes one register off.
    put(0x00, 0x00021057); // vendor 0x1057 (Motorola), device 0x0002 (MPC106)
    put(0x04, 0x00800006); // command, status
    put(0x08, 0x06000000); // revision (chip-specific: unknown, 0), class 0x06 bridge, subclass 0 host
    put(0x0C, 0x00000008); // cache line size 8 words
    put(0x70, 0xCD000000); // PMCR1 0, PMCR2 0, ODCR (0x73) 0xCD
    put(0xA8, 0xFF000010); // PICR1
    put(0xAC, 0x000C060C); // PICR2
    put(0xB8, 0x00040000); // ECC counter, trigger 0; alternate OS-visible 1 (0xBA) XIO_MODE
    put(0xC0, 0x00000001); // ErrEnR1: 60x bus error enable
    put(0xE0, 0x0FFF0042); // ESCR1
    put(0xE8, 0x00000020); // ESCR2
    put(0xF0, 0xFF020000); // MCCR1 (bits 19-16 from reset configuration pins: 0 here)
    put(0xF4, 0x00000003); // MCCR2
    put(0xFC, 0x00100000); // MCCR4
}

uint8_t Mpc106::read_data(uint32_t offset) const {
    if (((addr_ >> 8) & 0xFFFF) == kReal3dDevice << 3) { // bus 0, device 13, function 0
        constexpr uint32_t kReal3dPciId = 0x178611DB;     // little-endian, like the bridge's registers
        const uint32_t byte = (addr_ & 0xFC) + offset;
        return byte < 4 ? static_cast<uint8_t>(kReal3dPciId >> (8 * byte)) : 0;
    }
    if (!bridge_selected()) return 0xFF; // nothing else on the bus
    return regs_[((addr_ & 0xFC) + offset) & 0xFF];
}

void Mpc106::write_data(uint32_t offset, uint8_t value) {
    if (bridge_selected()) regs_[((addr_ & 0xFC) + offset) & 0xFF] = value;
}

Bus::Bus(const RomSet& roms)
    : roms_(roms), ram_(kWorkRamSize, 0), netboard_(std::make_unique<SimNetBoard>()),
      real3d_(roms.region(RomRegionId::Vrom)), security_ram_(kSecurityRamSize, 0), tile_ram_(kTileRamSize, 0), backup_ram_(kBackupRamSize, 0) {
    crypt_.set_reader([this](uint32_t word) -> uint16_t { // the top half of the RAM's 32-bit word `word`
        return word < kSecurityRamSize / 4 ? static_cast<uint16_t>(load_be32(&security_ram_[word * 4]) >> 16) : 0;
    });
}

void Bus::count(const Region* region, uint32_t addr, bool write) {
    Unhandled& u = unhandled_[region ? region->name : std::string_view("unmapped")];
    if (u.reads == 0 && u.writes == 0) u.first_addr = addr;
    ++(write ? u.writes : u.reads);
}

uint8_t Bus::device_read(const Region& region, uint32_t addr) {
    switch (region.device) {
    case Device::FixedCrom: {
        const auto& crom = roms_.region(RomRegionId::Crom);
        const uint32_t off = addr - kFixedCromBase;
        return off < crom.size() ? crom[off] : 0xFF;
    }
    case Device::BankedCrom: {
        const auto& banked = roms_.region(RomRegionId::BankedCrom);
        const std::size_t off = std::size_t{crom_bank_} * kCromWindowSize + (addr - kBankedCromBase);
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
    case Device::NetBoard:
        return net()->read8(addr);
    case Device::Io:
        return io_read(region.offset(addr));
    case Device::SoundUart: // nothing is received: data 0x00 (Supermodel: a region lock in Magical Truck)
        if ((region.offset(addr) & 0xF) != 0x4) return 0x00;
        if (midi_burst) return 0x83; // Supermodel
        {
            const uint8_t s = midi_uart_.status(now());
            run_midi(now());
            return s;
        }
    case Device::Rtc: { // a register in the top byte of each word; byte 1: battery OK
        const uint32_t off = region.offset(addr);
        if ((off & 3) == 0) return rtc_.read(off >> 2, now());
        return (off & 3) == 1 ? 0x03 : 0x00;
    }
    case Device::SecurityRam:
        return security_ram_[region.offset(addr) % kSecurityRamSize];
    case Device::SecurityCrypt: // narrow reads see the status (0); data only as 32 bits
        return region.offset(addr) < 4 ? 0 : 0xFF;
    case Device::Real3dDma:
        return dma_read8(addr & 0xFF);
    default:
        return 0xFF; // Tilegen registers: only 32-bit reads reach them
    }
}

// ---- I/O board (Supermodel's Model3.cpp, ReadInputs / WriteInputs) ----

uint8_t Bus::io_read(uint32_t reg) {
    switch (reg & 0x3F) {
    case 0x00: return io_bank_;
    case 0x04:
        if ((io_bank_ & 1) == 0) return inputs.bank0;
        return static_cast<uint8_t>((inputs.bank1 & ~0x20) | (eeprom_.read_do(now()) ? 0x20 : 0));
    case 0x08: return inputs.controls;
    case 0x0C: return inputs.controls2;
    default:   return 0xFF; // nothing pressed, no analog inputs
    }
}

void Bus::io_write(uint32_t reg, uint8_t value) {
    if ((reg & 0x3F) == 0x00) {
        eeprom_.write_pins(value & 0x40, value & 0x80, value & 0x20, now()); // CS, CLK, DI
        io_bank_ = value;
    }
    // 0x10 drive board, 0x14 lamps, 0x24 serial: outputs, nothing to do yet
}

void Bus::midi_write(uint32_t reg, uint8_t value) {
    if (reg == 0x4) midi_ctrl_ = value;
    if (midi_burst) {
        if (reg == 0x0) { // data: to the sound board
            midi_out_.push_back({value, now()});
            ++midi_bytes_;
            irq_.deassert(0x40);
        } else if (reg == 0x4 && !(value & 0x20)) {
            irq_.deassert(0x40);
        }
        return;
    }
    if (reg == 0x0) midi_uart_.write_data(value, now());
    else if (reg == 0x4) midi_uart_.write_control(value, now());
    else return;
    run_midi(now());
    if (on_due_change) on_due_change(on_due_change_user);
}

// Acknowledging clears pending bits; IRQ 0x40 is a level (the serial chip's
// TxRDY pin) and stays pending while the pin is up. The game turns the
// transmitter off when it has nothing to send, which is what a level needs.
void Bus::ack_irq(uint32_t bits) {
    irq_.deassert(bits);
    if (!midi_burst && (bits & 0x40) && midi_uart_.txrdy_pin()) irq_.assert_irq(0x40);
}

void Bus::run_midi(uint64_t cycle) {
    midi_uart_.run_to(cycle);
    auto& sent = midi_uart_.sent();
    midi_out_.insert(midi_out_.end(), sent.begin(), sent.end());
    midi_bytes_ += sent.size();
    sent.clear();
    // IRQ 0x40 is the TxRDY pin.
    const bool up = midi_uart_.txrdy_pin();
    if (up != ((irq_.state() & 0x40) != 0)) {
        if (up) irq_.assert_irq(0x40);
        else irq_.deassert(0x40);
    }
}

// ---- Security board (Supermodel's Model3.cpp, ReadSecurity / WriteSecurity) ----

uint32_t Bus::security_read32(uint32_t reg) {
    switch (reg & 0x3C) {
    case 0x00: return 0; // status: never busy
    case 0x1C:
        if (security_first_read_) {
            security_first_read_ = false;
            return 0xFFFF0000;
        }
        return uint32_t{crypt_.decrypt()} << 16;
    default: return 0xFFFFFFFF;
    }
}

void Bus::security_write32(uint32_t reg, uint32_t value) {
    switch (reg & 0x3C) {
    case 0x10:
    case 0x14: // start again from address 0
        crypt_.set_address_low(0);
        crypt_.set_address_high(0);
        security_first_read_ = true;
        return;
    case 0x18: { // sequence key, byte-swapped in the top half
        const uint16_t key = static_cast<uint16_t>(value >> 16);
        crypt_.set_subkey(static_cast<uint16_t>(key >> 8 | key << 8));
        return;
    }
    default: return;
    }
}

uint8_t Bus::system_read(uint32_t reg) const {
    switch (reg) {
    case kSysCromBank:  return static_cast<uint8_t>(crom_bank_reg_);
    case 0x10:          return static_cast<uint8_t>(jtag_.tdo() << 5); // JTAG TDO
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
    case kSysIrqState:  ack_irq(value); break;
    case 0x0C: // JTAG: TCK bit 6, TMS bit 2, TDI bit 5, TRST bit 7
        jtag_.write(value & 0x40, value & 0x04, value & 0x20, value & 0x80);
        break;
    default: break; // 0x0D-0x0F and 0x1C: LEDs
    }
}

// ---- Real3D DMA device (adapted from Supermodel's Real3D.cpp:
//      Copyright 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis,
//      Harry Tuttle, and Spindizzi. GPL-3.0-or-later.) ----

constexpr uint32_t kDmaIrq = 0x100;       // non-maskable
constexpr uint32_t kMercuryPciId = 0x16C311DB;
constexpr uint64_t kPciHz = 33'000'000;   // one 32-bit word a clock at best

void Bus::run_dma(uint64_t cycle) {
    if (!dma_.busy || cycle < dma_.done_at) return;
    dma_.busy = false;
    if (dma_.config & 1) { // IRQ on completion only when asked for
        dma_.status |= 1;
        irq_.assert_irq(kDmaIrq);
    }
}

uint32_t Bus::real3d_register(uint32_t reg) const {
    if (reg == 0) return 0xFDFFFFFF | (ping_pong_ ? 0x02000000u : 0u);
    if (reg >= 20 && reg <= 32) return 0; // line of sight: from the renderer, none yet
    return 0xFFFFFFFF;
}

uint8_t Bus::dma_read8(uint32_t reg) const {
    switch (reg) {
    case 0x0C: return static_cast<uint8_t>(dma_.status | (dma_.busy ? 0x80 : 0)); // bit 7 busy, bit 0 IRQ pending
    case 0x0E: return dma_.config;
    default:   return 0;
    }
}

void Bus::dma_write8(uint32_t reg, uint8_t value) {
    switch (reg) {
    case 0x0D: // IRQ acknowledge
        if (value & 1) {
            dma_.status &= static_cast<uint8_t>(~1u);
            irq_.deassert(kDmaIrq);
        }
        break;
    case 0x0E: dma_.config = value; break;
    default: break;
    }
}

uint32_t Bus::dma_read32(uint32_t reg) const { return reg == 0x14 ? dma_.data : 0; }

void Bus::dma_write32(uint32_t reg, uint32_t value) {
    switch (reg) {
    case 0x00: dma_.src = value; break;
    case 0x04: dma_.dest = value; break;
    case 0x08: { // length in words: the data moves now, the copy completes when the bus would have carried it
        for (uint32_t n = value; n != 0; --n) {
            const uint32_t word = read32(dma_.src);
            write32(dma_.dest, (dma_.config & 0x80) ? bswap32(word) : word);
            dma_.src += 4;
            dma_.dest += 4;
        }
        const uint64_t t = now();
        const uint64_t start = dma_.busy ? std::max(t, dma_.done_at) : t; // after one still going
        dma_.done_at = start + (dma_timed ? uint64_t{value} * ppc::CpuState::kCoreClockHz / kPciHz : 0);
        dma_.busy = true;
        run_dma(t);
        if (on_due_change) on_due_change(on_due_change_user);
        break;
    }
    case 0x10: // command
        if (value & 0x20000000) dma_.data = kMercuryPciId;                     // ID
        else if (value & 0x80000000) dma_.data = real3d_register(value & 0x3F); // read a status register
        break;
    case 0x14: dma_.data = 0xFFFFFFFF; break;
    default: break;
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

uint16_t Bus::read_slow16(uint32_t addr) {
    if (addr >= kWorkRamSize) {
        const Region* region = find_region(addr);
        if (region && region->device == Device::NetBoard) return net()->read16(addr);
    }
    return static_cast<uint16_t>(read_byte(addr) << 8 | read_byte(addr + 1));
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
    if (region->device == Device::NetBoard) return net()->read32(addr);
    if (region->device == Device::Real3dDma) return bswap32(dma_read32(addr & 0xFF));
    if (region->device == Device::SecurityCrypt) return security_read32(region->offset(addr));
    if (region->device == Device::Real3dStatus) return bswap32(real3d_register(addr & 0x3F));
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v = v << 8 | device_read(*region, addr + i);
    return v;
}

void Bus::write_slow(uint32_t addr, uint32_t value, uint32_t size) {
    if (on_watch_ && addr >= watch_lo_ && addr < watch_hi_) on_watch_(on_watch_user_, addr, value, size);
    if (addr < kWorkRamSize) { // straddles the end of RAM, or watched
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
    case Device::SoundUart: // byte registers (Supermodel ignores wider writes)
        if (size == 1) midi_write(region->offset(addr) & 0xF, static_cast<uint8_t>(value));
        else count(region, addr, true);
        return;
    case Device::Rtc: // only the top byte of each word reaches the chip
        for (uint32_t i = 0; i < size; ++i) {
            const uint32_t off = region->offset(addr) + i;
            if ((off & 3) == 0) rtc_.write(off >> 2, static_cast<uint8_t>(value >> (8 * (size - 1 - i))), now());
        }
        return;
    case Device::SecurityRam:
        for (uint32_t i = 0; i < size; ++i)
            security_ram_[(region->offset(addr) + i) % kSecurityRamSize] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        return;
    case Device::SecurityCrypt: // 32-bit registers
        if (size == 4) security_write32(region->offset(addr), value);
        else count(region, addr, true);
        return;
    case Device::TileRegs: { // 32-bit only; narrower writes don't reach the registers
        if (size != 4) {
            count(region, addr, true);
            return;
        }
        const uint32_t reg = region->offset(addr) & 0xFC, v = bswap32(value);
        tile_regs_[reg / 4] = v;
        if (reg == kTileIrqAck) ack_irq(v & 0xFF);
        if (reg == 0x0C) real3d_.tilegen_draw_frame(mercury_mode()); // the tile generator draws a frame (Supermodel)
        return;
    }
    case Device::System: // byte registers; wider writes reach several of them
        for (uint32_t i = 0; i < size; ++i)
            system_write(region->offset(addr) + i, static_cast<uint8_t>(value >> (8 * (size - 1 - i))));
        return;
    case Device::Io: // byte registers; wider writes reach several of them
        for (uint32_t i = 0; i < size; ++i)
            io_write(region->offset(addr) + i, static_cast<uint8_t>(value >> (8 * (size - 1 - i))));
        return;
    case Device::Real3dCommand:
        real3d_.write_command(mercury_mode());
        return;
    case Device::Real3dCullingLow:
    case Device::Real3dCullingHigh:
    case Device::Real3dTextureFifo:
    case Device::Real3dPolygonRam:
    case Device::Real3dConfig: { // 32-bit only
        if (size != 4) {
            count(region, addr, true);
            return;
        }
        const uint32_t off = region->offset(addr);
        switch (region->device) {
        case Device::Real3dCullingLow:  real3d_.write_culling_low(off, value); break;
        case Device::Real3dCullingHigh: real3d_.write_culling_high(off, value); break;
        case Device::Real3dTextureFifo: real3d_.write_texture_fifo(value); break;
        case Device::Real3dPolygonRam:  real3d_.write_polygon(off, value); break;
        default:                        real3d_.write_config(off, value); break;
        }
        return;
    }
    case Device::Real3dDma: // 8-bit status/config registers, 32-bit byte-swapped address/length/command
        if (size == 1) dma_write8(addr & 0xFF, static_cast<uint8_t>(value));
        else if (size == 4) dma_write32(addr & 0xFF, bswap32(value));
        else count(region, addr, true);
        return;
    case Device::NetBoard: // the board answers each width differently
        if (size == 1) net()->write8(addr, static_cast<uint8_t>(value));
        else if (size == 2) net()->write16(addr, static_cast<uint16_t>(value));
        else net()->write32(addr, value);
        return;
    default:
        count(region, addr, true);
        return;
    }
}

void Bus::state(State& s) {
    s.tag("bus");
    s.fixed(ram_);
    s.io(dma_.src), s.io(dma_.dest), s.io(dma_.data), s.io(dma_.status), s.io(dma_.config), s.io(dma_.busy);
    s.io(dma_.done_at);
    pci_.state(s);
    irq_.state(s);
    s.io(ping_pong_);
    jtag_.state(s);
    eeprom_.state(s);
    s.io(io_bank_), s.io(inputs);
    real3d_.state(s);
    rtc_.state(s);
    s.io(midi_ctrl_);
    midi_uart_.state(s);
    s.io(midi_out_), s.io(midi_bytes_);
    s.fixed(security_ram_);
    crypt_.state(s);
    s.io(security_first_read_);
    s.io(tile_regs_), s.fixed(tile_ram_), s.fixed(backup_ram_), s.io(crom_bank_), s.io(crom_bank_reg_);
    netboard_->state(s);
}

} // namespace m3
