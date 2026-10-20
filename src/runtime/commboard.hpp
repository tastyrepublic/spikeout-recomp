// The Model 3 communication board (837-11861) emulated as hardware: its own
// 68000 runs the firmware the game uploads, against the board's memory and
// registers. The firmware is real code; the board around it is as known
// from MAME's sega/m3comm.cpp (hardware facts, BSD-3-Clause) and
// Supermodel's Src/Network/NetBoard.cpp, which agree on:
//
//   68000 0x00000  RAM, 64 KB: the main board's net RAM (0xC0020000), one
//                  byte-swapped 68000 word per 32-bit PowerPC word. The game
//                  uploads the firmware here.
//   68000 0x40000  control: 0x00 comm RAM bank (bit 0), 0x40 IRQ 5 ack,
//                  0xA0 IRQ 2 ack, 0x80 LEDs, 0xC0 node ID?, 0xE0 ?
//   68000 0x80000  comm RAM, 64 KB of 128 KB (two banks): shared with the
//                  main board at 0xC0000000
//   68000 0xC0000  I/O registers (16-bit), shared with the main board at
//                  0xC0010000, one register per 32-bit PowerPC word:
//                  0x88/0x8A status 0/1 (a mailbox both sides read and
//                  write), 0xC0 run (main board: 0 holds the 68000 in reset),
//                  and the link chips: 0x10 receive status, 0x14/0x16 receive
//                  enable / complete (IRQ 6), 0x18 transmit status, 0x1A/0x1C
//                  transmit enable / complete (IRQ 4), 0x40-0x46 receive and
//                  transmit offset and size (shifted in a byte at a time)
//
// Not known from either source, so these are choices (see Options and
// docs/fidelity.md): the 68000 clock, what raises IRQ 5 and how often, the
// link status values, the PowerPC's byte order of comm RAM (the two sources
// disagree), whether both CPUs see the same comm RAM bank, and how I/O
// register 0 (which the main board waits on after a command) gets set.
#pragma once

#include "runtime/netboard.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace m3 {

struct CommBoardOptions {
    // PowerPC comm RAM byte n is 68000 comm RAM byte n ^ comm_ram_xor:
    // 3 in Supermodel (a 32-bit byte reversal), 2 in MAME.
    uint32_t comm_ram_xor = 3;
    // false: both CPUs see the bank the 68000 selects (MAME);
    // true: the main board sees the other one (double buffering).
    bool host_sees_other_bank = false;
    uint64_t cpu_hz = 10'000'000;   // 40 MHz crystal / 4: unverified (MAME: 10 MHz "random")
    uint64_t irq5_hz = 100;         // MAME's timer, "where is this timing from?"
    // IRQ 5 locked to the main board's frame: this many a frame, evenly
    // spaced from the frame's end (0: irq5_hz instead). See the .cpp.
    uint32_t irq5_per_frame = 1;
    double main_frame_hz = 57.524160;
    uint64_t main_hz = 166'000'000; // the main board's CPU clock
    uint16_t link_status = 5;       // receive/transmit status reads: both sources return 5
};

class CommBoard : public NetBoardDevice {
public:
    using Options = CommBoardOptions;

    explicit CommBoard(std::unique_ptr<NetLink> link = nullptr, const Options& options = Options());
    ~CommBoard() override;

    uint8_t read8(uint32_t addr) override;
    uint16_t read16(uint32_t addr) override;
    uint32_t read32(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t data) override;
    void write16(uint32_t addr, uint16_t data) override;
    void write32(uint32_t addr, uint32_t data) override;
    void sync(uint64_t main_cycles) override;
    void end_frame() override;
    std::string describe() const override;

    // ---- Inspection (reports, tests, tracing the firmware) ----
    bool in_reset() const { return in_reset_; }
    uint32_t pc() const;
    uint64_t cpu_cycles() const;
    std::span<const uint8_t> ram() const { return ram_; }
    uint16_t io(uint32_t reg) const { return io_[(reg & 0xFF) / 2]; }
    std::string disassemble(uint32_t addr, uint32_t* length = nullptr) const;
    std::array<uint64_t, 8> irqs_taken{}; // by level
    uint64_t packets_sent = 0, packets_received = 0;

    // The 68000's accesses to its control and I/O registers, the first
    // `trace_limit` of them: what the firmware does with the hardware.
    struct Access {
        uint64_t cycle;
        uint32_t pc, addr;
        uint16_t value;
        bool write;
        uint8_t size;
    };
    std::vector<Access> trace;
    std::size_t trace_limit = 0;

    // Debug aid, off by default: when the game writes the link role (68000
    // word 0x400: 0 master, otherwise slave), store 0 instead. A lone
    // cabinet set up as a slave correctly reports NO CARRIER (it waits for
    // a master); this tries the master path while the game's settings come
    // from an EEPROM that isn't emulated yet.
    bool force_master = false;

private:
    class Cpu;
    friend class Cpu;

    // 68000 side
    uint8_t cpu_read8(uint32_t a);
    uint16_t cpu_read16(uint32_t a);
    void cpu_write8(uint32_t a, uint8_t v);
    void cpu_write16(uint32_t a, uint16_t v);
    uint16_t ctrl_read(uint32_t reg) const;
    void ctrl_write(uint32_t reg, uint16_t v);
    uint16_t io_read(uint32_t index) const;
    void io_write(uint32_t index, uint16_t v, uint16_t mask, bool from_host);
    void note(uint32_t addr, uint16_t value, bool write, uint8_t size);

    void set_irq(int level, bool on);
    void update_ipl();
    void run_until(uint64_t cpu_cycle);
    void deliver_receive();
    void release_reset();

    uint8_t* bank(bool host);
    uint8_t& host_comm(uint32_t addr) { return bank(true)[(addr ^ options_.comm_ram_xor) & 0xFFFF]; }
    uint8_t& host_ram(uint32_t addr, uint32_t byte); // byte 0/1 of the PowerPC word

    Options options_;
    std::unique_ptr<NetLink> link_;
    std::unique_ptr<Cpu> cpu_;
    std::vector<uint8_t> ram_;  // 64 KB, 68000 byte order (big-endian)
    std::vector<uint8_t> comm_; // 128 KB, two banks, 68000 byte order
    std::array<uint16_t, 128> io_{};
    std::array<uint16_t, 128> ctrl_{};
    uint16_t commbank_ = 0;

    bool in_reset_ = true;
    uint8_t irq_lines_ = 0; // bit n: level n asserted
    uint64_t next_irq5_ = 0;
    uint64_t frame_irq5_next_ = UINT64_MAX, frame_irq5_period_ = 0; // the rest of this frame's IRQ 5s
    uint32_t frame_irq5_left_ = 0;

    uint16_t recv_offset_ = 0, recv_size_ = 0, send_offset_ = 0, send_size_ = 0;
    bool receive_enabled_ = false; // 0x14 = 0x80
    bool receive_armed_ = false;   // 0x16 = 0x8C: IRQ 6 when data is in
};

} // namespace m3
