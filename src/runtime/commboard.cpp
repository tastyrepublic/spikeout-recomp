// Register behavior from MAME's sega/m3comm.cpp (BSD-3-Clause, MetalliC)
// and Supermodel's Src/Network/NetBoard.cpp (GPL-3.0-or-later, Copyright
// 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis, Harry Tuttle, and
// Spindizzi), see commboard.hpp.
#include "runtime/commboard.hpp"

#include "Moira.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace m3 {

namespace {

constexpr uint16_t swap16(uint16_t v) { return static_cast<uint16_t>(v << 8 | v >> 8); }

// Main board view: 0xC0000000 comm RAM, 0xC0010000 I/O, 0xC0020000 68000 RAM.
enum HostRegion : uint32_t { kHostComm = 0, kHostIo = 1 };
uint32_t host_region(uint32_t addr) { return (addr & 0x3FFFF) >> 16; }
uint32_t host_io_index(uint32_t addr) { return (addr & 0x1FF) / 4; }

// I/O register word indices (byte offset / 2).
constexpr uint32_t kIoAck = 0x00 / 2;
constexpr uint32_t kIoRxStatus = 0x10 / 2, kIoRxEnable = 0x14 / 2, kIoRxIrq = 0x16 / 2;
constexpr uint32_t kIoTxStatus = 0x18 / 2, kIoTxEnable = 0x1A / 2, kIoTxIrq = 0x1C / 2;
constexpr uint32_t kIoRxOffset = 0x40 / 2, kIoRxSize = 0x42 / 2, kIoTxOffset = 0x44 / 2, kIoTxSize = 0x46 / 2;
constexpr uint32_t kIoIrqStatus = 0x82 / 2, kIoStatus0 = 0x88 / 2, kIoRun = 0xC0 / 2;

} // namespace

class CommBoard::Cpu : public moira::Moira {
public:
    explicit Cpu(CommBoard& board) : board_(board) {}
    moira::u8 read8(moira::u32 a) const override { return board_.cpu_read8(a); }
    moira::u16 read16(moira::u32 a) const override { return board_.cpu_read16(a); }
    void write8(moira::u32 a, moira::u8 v) const override { board_.cpu_write8(a, v); }
    void write16(moira::u32 a, moira::u16 v) const override { board_.cpu_write16(a, v); }
    void willInterrupt(moira::u8 level) override { ++board_.irqs_taken[level & 7]; }

private:
    CommBoard& board_;
};

CommBoard::CommBoard(std::unique_ptr<NetLink> link, const Options& options)
    : options_(options), link_(link ? std::move(link) : std::make_unique<LoopbackLink>()),
      cpu_(std::make_unique<Cpu>(*this)), ram_(0x10000, 0), comm_(0x20000, 0) {}

CommBoard::~CommBoard() = default;

uint32_t CommBoard::pc() const { return cpu_->getPC(); }
uint64_t CommBoard::cpu_cycles() const { return static_cast<uint64_t>(cpu_->getClock()); }

std::string CommBoard::disassemble(uint32_t addr, uint32_t* length) const {
    char buf[128] = {};
    const int n = cpu_->disassemble(buf, addr);
    if (length) *length = static_cast<uint32_t>(n);
    return buf;
}

std::string CommBoard::describe() const {
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "68000 board, %s, PC %06x, %llu cycles; status0 %04x, status1 %04x; IRQs 2:%llu 4:%llu 5:%llu "
                  "6:%llu; packets %llu out, %llu in; set up as %s",
                  in_reset_ ? "held in reset" : "running", pc(), static_cast<unsigned long long>(cpu_cycles()),
                  io_[kIoStatus0], io_[kIoStatus0 + 1], static_cast<unsigned long long>(irqs_taken[2]),
                  static_cast<unsigned long long>(irqs_taken[4]), static_cast<unsigned long long>(irqs_taken[5]),
                  static_cast<unsigned long long>(irqs_taken[6]), static_cast<unsigned long long>(packets_sent),
                  static_cast<unsigned long long>(packets_received), ram_[0x400] == 0 && ram_[0x401] == 0 ? "master" : "slave");
    return buf;
}

uint8_t* CommBoard::bank(bool host) {
    const bool second = (commbank_ & 1) != (host && options_.host_sees_other_bank);
    return comm_.data() + (second ? 0x10000 : 0);
}

uint8_t& CommBoard::host_ram(uint32_t addr, uint32_t byte) {
    const uint32_t word = ((addr & 0x1FFFF) / 4) * 2; // 68000 byte address of the word
    return ram_[(word + (byte == 0 ? 1 : 0)) & 0xFFFF]; // PowerPC byte 0 is the 68000's low byte
}

// ---- Time ----

void CommBoard::sync(uint64_t main_cycles) {
    const uint64_t target = main_cycles * options_.cpu_hz / options_.main_hz;
    if (in_reset_) {
        cpu_->setClock(static_cast<moira::i64>(target));
        return;
    }
    run_until(target);
}

void CommBoard::run_until(uint64_t target) {
    const uint64_t irq5_period = options_.cpu_hz / options_.irq5_hz;
    const bool timed = options_.irq5_per_frame == 0;
    while (!in_reset_ && cpu_cycles() < target) {
        const uint64_t stop = std::min({target, timed ? next_irq5_ : UINT64_MAX, frame_irq5_next_});
        cpu_->executeUntil(static_cast<moira::i64>(stop));
        if (timed && cpu_cycles() >= next_irq5_) {
            set_irq(5, true);
            next_irq5_ += irq5_period;
        }
        if (cpu_cycles() >= frame_irq5_next_) {
            set_irq(5, true);
            frame_irq5_next_ = --frame_irq5_left_ ? frame_irq5_next_ + frame_irq5_period_ : UINT64_MAX;
        }
        deliver_receive();
    }
}

// IRQ 5 locked to the main board's frames: the first at the end of each
// frame (the board has caught up to it), the others spread over the next.
// Spikeout's link checks lock-step every frame (each cabinet's data must be
// the one from exactly one frame before, "Sync_sys_broken" otherwise): with
// MAME's free-running 100 Hz a lone master fails it within seconds, with
// IRQ 5 once a frame it doesn't. What drives it on the board isn't known.
void CommBoard::end_frame() {
    if (in_reset_ || options_.irq5_per_frame == 0) return;
    set_irq(5, true);
    frame_irq5_left_ = options_.irq5_per_frame - 1;
    frame_irq5_period_ =
        static_cast<uint64_t>(static_cast<double>(options_.cpu_hz) / options_.main_frame_hz / options_.irq5_per_frame);
    frame_irq5_next_ = frame_irq5_left_ ? cpu_cycles() + frame_irq5_period_ : UINT64_MAX;
}

void CommBoard::release_reset() {
    in_reset_ = false;
    const moira::i64 now = cpu_->getClock();
    cpu_->reset(); // fetches SSP and PC from 68000 address 0: the uploaded firmware
    cpu_->setClock(now);
    irq_lines_ = 0;
    update_ipl();
    next_irq5_ = cpu_cycles() + options_.cpu_hz / options_.irq5_hz;
}

void CommBoard::set_irq(int level, bool on) {
    const uint8_t bit = static_cast<uint8_t>(1u << level);
    irq_lines_ = on ? (irq_lines_ | bit) : (irq_lines_ & ~bit);
    update_ipl();
}

void CommBoard::update_ipl() {
    int level = 0;
    for (int l = 7; l > 0; --l)
        if (irq_lines_ & (1 << l)) {
            level = l;
            break;
        }
    cpu_->setIPL(static_cast<moira::u8>(level));
}

// ---- Link ----

void CommBoard::deliver_receive() {
    if (!receive_enabled_ || !receive_armed_) return;
    std::vector<uint8_t> packet = link_->receive();
    if (packet.empty()) return; // nothing yet (or the link is down)
    ++packets_received;
    uint8_t* comm = bank(false);
    const std::size_t size = std::min<std::size_t>({packet.size(), recv_size_ & 0x7FFFu, 0x10000u - recv_offset_});
    std::memcpy(comm + recv_offset_, packet.data(), size);
    receive_armed_ = false;
    set_irq(6, true); // receive complete
}

// ---- 68000 side ----

void CommBoard::note(uint32_t addr, uint16_t value, bool write, uint8_t size) {
    if (trace.size() < trace_limit) trace.push_back({cpu_cycles(), cpu_->getPC0(), addr, value, write, size});
}

uint16_t CommBoard::ctrl_read(uint32_t reg) const { return reg == 0 ? commbank_ : 0; }

void CommBoard::ctrl_write(uint32_t reg, uint16_t v) {
    ctrl_[(reg & 0xFF) / 2] = v;
    switch (reg & 0xFE) {
    case 0x00: commbank_ = v; break; // flipped by the firmware's IRQ 2 and IRQ 5 handlers
    case 0x40: set_irq(5, false); break;
    case 0xA0: // IRQ 2 ack; the main board then sees bit 0 of I/O register 0 (Supermodel's guess)
        set_irq(2, false);
        io_[kIoAck] |= 1;
        break;
    default: break; // 0x80 LEDs, 0xC0 node ID?, 0xE0 ?
    }
}

uint16_t CommBoard::io_read(uint32_t index) const {
    switch (index) {
    case kIoRxStatus:
    case kIoTxStatus: return options_.link_status;
    case kIoIrqStatus: return 0xA0; // MAME
    default: return io_[index & 0x7F];
    }
}

void CommBoard::io_write(uint32_t index, uint16_t v, uint16_t mask, bool from_host) {
    index &= 0x7F;
    const uint16_t old = io_[index];
    io_[index] = static_cast<uint16_t>((old & ~mask) | (v & mask));
    const uint16_t low = io_[index] & 0xFF;
    if (from_host) {
        if (index == kIoStatus0) set_irq(2, true); // MAME: a command from the main board raises IRQ 2
        if (index == kIoRun) {
            if (io_[index] == 0) {
                in_reset_ = true;
            } else if (in_reset_) {
                release_reset();
            }
        }
        return;
    }
    switch (index) {
    case kIoRxEnable:
        receive_enabled_ = low == 0x80;
        if (low != 0x80) set_irq(6, false); // also how the IRQ 6 handler acknowledges
        deliver_receive();
        break;
    case kIoRxIrq:
        receive_armed_ = low == 0x8C;
        deliver_receive();
        break;
    case kIoTxEnable:
        if (low == 0x80) { // data transfer enable: send now
            const std::size_t size = std::min<std::size_t>(send_size_ & 0x7FFFu, 0x10000u - send_offset_);
            link_->send(bank(false) + send_offset_, size);
            ++packets_sent;
            deliver_receive();
        }
        break;
    case kIoTxIrq: set_irq(4, low == 0x8C); break; // transmit complete while 0x8C (MAME)
    case kIoRxOffset: recv_offset_ = static_cast<uint16_t>(recv_offset_ >> 8 | low << 8); break;
    case kIoRxSize:   recv_size_ = static_cast<uint16_t>(recv_size_ >> 8 | low << 8); break;
    case kIoTxOffset: send_offset_ = static_cast<uint16_t>(send_offset_ >> 8 | low << 8); break;
    case kIoTxSize:   send_size_ = static_cast<uint16_t>(send_size_ >> 8 | low << 8); break;
    default: break;
    }
}

uint8_t CommBoard::cpu_read8(uint32_t a) {
    switch ((a >> 16) & 0xF) {
    case 0x0: return ram_[a & 0xFFFF];
    case 0x4: {
        const uint16_t v = ctrl_read(a & 0xFE);
        note(a, v, false, 1);
        return static_cast<uint8_t>((a & 1) ? v : v >> 8);
    }
    case 0x8: return bank(false)[a & 0xFFFF];
    case 0xC: {
        const uint16_t v = io_read((a & 0xFF) / 2);
        note(a, v, false, 1);
        return static_cast<uint8_t>((a & 1) ? v : v >> 8);
    }
    default: return 0;
    }
}

uint16_t CommBoard::cpu_read16(uint32_t a) {
    a &= ~1u;
    switch ((a >> 16) & 0xF) {
    case 0x0: return static_cast<uint16_t>(ram_[a & 0xFFFF] << 8 | ram_[(a + 1) & 0xFFFF]);
    case 0x4: {
        const uint16_t v = ctrl_read(a & 0xFE);
        note(a, v, false, 2);
        return v;
    }
    case 0x8: {
        const uint8_t* comm = bank(false);
        return static_cast<uint16_t>(comm[a & 0xFFFF] << 8 | comm[(a + 1) & 0xFFFF]);
    }
    case 0xC: {
        const uint16_t v = io_read((a & 0xFF) / 2);
        note(a, v, false, 2);
        return v;
    }
    default: return 0;
    }
}

void CommBoard::cpu_write8(uint32_t a, uint8_t v) {
    switch ((a >> 16) & 0xF) {
    case 0x0: ram_[a & 0xFFFF] = v; break;
    case 0x4:
        note(a, v, true, 1);
        ctrl_write(a & 0xFE, (a & 1) ? v : static_cast<uint16_t>(v << 8)); // byte lane of a 16-bit register
        break;
    case 0x8: bank(false)[a & 0xFFFF] = v; break;
    case 0xC:
        note(a, v, true, 1);
        io_write((a & 0xFF) / 2, (a & 1) ? v : static_cast<uint16_t>(v << 8), (a & 1) ? 0x00FF : 0xFF00, false);
        break;
    default: break;
    }
}

void CommBoard::cpu_write16(uint32_t a, uint16_t v) {
    a &= ~1u;
    switch ((a >> 16) & 0xF) {
    case 0x0:
        ram_[a & 0xFFFF] = static_cast<uint8_t>(v >> 8);
        ram_[(a + 1) & 0xFFFF] = static_cast<uint8_t>(v);
        break;
    case 0x4:
        note(a, v, true, 2);
        ctrl_write(a & 0xFE, v);
        break;
    case 0x8: {
        uint8_t* comm = bank(false);
        comm[a & 0xFFFF] = static_cast<uint8_t>(v >> 8);
        comm[(a + 1) & 0xFFFF] = static_cast<uint8_t>(v);
        break;
    }
    case 0xC:
        note(a, v, true, 2);
        io_write((a & 0xFF) / 2, v, 0xFFFF, false);
        break;
    default: break;
    }
}

// ---- Main board side ----

uint8_t CommBoard::read8(uint32_t addr) {
    switch (host_region(addr)) {
    case kHostComm: return host_comm(addr);
    case kHostIo: {
        const uint16_t v = io_read(host_io_index(addr));
        const uint32_t k = addr & 3; // the register is the upper half of the word, byte-swapped
        return k == 0 ? static_cast<uint8_t>(v) : k == 1 ? static_cast<uint8_t>(v >> 8) : 0;
    }
    default: return (addr & 2) ? 0 : host_ram(addr, addr & 1);
    }
}

uint16_t CommBoard::read16(uint32_t addr) {
    return static_cast<uint16_t>(read8(addr) << 8 | read8(addr + 1));
}

uint32_t CommBoard::read32(uint32_t addr) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v = v << 8 | read8(addr + i);
    return v;
}

void CommBoard::write8(uint32_t addr, uint8_t data) {
    switch (host_region(addr)) {
    case kHostComm: host_comm(addr) = data; break;
    case kHostIo: {
        const uint32_t k = addr & 3;
        if (k == 0) io_write(host_io_index(addr), data, 0x00FF, true);
        else if (k == 1) io_write(host_io_index(addr), static_cast<uint16_t>(data << 8), 0xFF00, true);
        break;
    }
    default:
        if (!(addr & 2)) {
            uint8_t& cell = host_ram(addr, addr & 1);
            const bool role = &cell == &ram_[0x400] || &cell == &ram_[0x401];
            cell = force_master && role ? 0 : data;
        }
        break;
    }
}

void CommBoard::write16(uint32_t addr, uint16_t data) {
    if (host_region(addr) == kHostIo && (addr & 3) == 0) { // one access to the whole register
        io_write(host_io_index(addr), swap16(data), 0xFFFF, true);
        return;
    }
    write8(addr, static_cast<uint8_t>(data >> 8));
    write8(addr + 1, static_cast<uint8_t>(data));
}

void CommBoard::write32(uint32_t addr, uint32_t data) {
    if (host_region(addr) == kHostIo) { // the register is the upper half
        io_write(host_io_index(addr), swap16(static_cast<uint16_t>(data >> 16)), 0xFFFF, true);
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) write8(addr + i, static_cast<uint8_t>(data >> (24 - 8 * i)));
}

} // namespace m3
