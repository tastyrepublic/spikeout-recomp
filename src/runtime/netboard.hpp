// Network board (the cabinet link), simulated at a high level.
//
// Adapted from Supermodel's Src/Network/SimNetBoard.cpp and the network
// board cases of Src/Model3/Model3.cpp:
//   Copyright 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis,
//   Harry Tuttle, and Spindizzi. GPL-3.0-or-later.
//
// Every Spikeout cabinet has one, even when it plays alone: a lone cabinet
// is a ring of one, its link output fed back to its input. The board's own
// 68000 isn't emulated; instead, once a frame, run_frame() does what its
// firmware does for "type one" games (Spikeout, Daytona 2, Scud Race, ...):
// a start-up handshake through two status registers, then each frame it
// sends this cabinet's segment of comm RAM around the ring and swaps the
// comm RAM banks.
//
// Memory layout follows Supermodel's exactly, as stored on its
// little-endian host, so the byte-order quirks of the 16-bit board on the
// 32-bit bus come out the same:
//   0xC0000000  comm RAM, 64 KB, two banks (the CPU sees one, the board
//               fills the other)
//   0xC0010000  I/O registers, 16-bit, index (addr & 0x1FF) / 2:
//               0x00 IRQ 2 acknowledge flags, 0x88/0x8A status 0/1,
//               0xC0 run control (0 resets and stops, anything else runs)
//   0xC0020000  net RAM, 64 KB of 16-bit words, one per 32-bit CPU word
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace m3 {

// What the board sends its segments over: the next cabinet in the ring,
// receiving from the previous one.
class NetLink {
public:
    virtual ~NetLink() = default;
    virtual void send(const uint8_t* data, size_t size) = 0;
    // The next packet from the previous cabinet; empty if the link broke.
    virtual std::vector<uint8_t> receive() = 0;
    // True when this cabinet is linked to itself (a ring of one).
    virtual bool loopback() const = 0;
};

// A lone cabinet: everything sent comes straight back.
class LoopbackLink : public NetLink {
public:
    void send(const uint8_t* data, size_t size) override { queue_.emplace_back(data, data + size); }
    std::vector<uint8_t> receive() override;
    bool loopback() const override { return true; }

private:
    std::deque<std::vector<uint8_t>> queue_;
};

class NetBoard {
public:
    // Without a link the board is a lone cabinet (loopback).
    explicit NetBoard(std::unique_ptr<NetLink> link = nullptr);

    // CPU accesses, by width (Model3.cpp's Read8/16/32 and Write8/16/32
    // for 0xC0000000-0xC003FFFF; 64-bit ones are two 32-bit ones).
    uint8_t read8(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;
    void write8(uint32_t addr, uint8_t data);
    void write16(uint32_t addr, uint16_t data);
    void write32(uint32_t addr, uint32_t data);

    // Once a frame, after the main board's frame.
    void run_frame();
    void reset();

    enum class State : uint8_t { Start, Init, Testing, Ready, Error };
    State state() const { return state_; }
    bool running() const { return running_; }
    uint16_t status0() const { return status0_; }
    uint16_t status1() const { return status1_; }
    // What the game wrote to net RAM byte `byte` (0x400: 0 master / 1 slave,
    // 0x402: total size, 0x404: segment size).
    uint16_t net_ram16(uint32_t byte) const { return le16(&net_ram_[byte & 0xFFFE]); }

private:
    uint16_t read_io(uint32_t reg) const;
    void write_io(uint32_t reg, uint16_t data);

    // Little-endian 16/32-bit loads and stores, as on Supermodel's host.
    static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
    static uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
    static void put16(uint8_t* p, uint16_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }
    static void put32(uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    uint8_t* comm() { return buffer_.data() + (commbank_ ? 0x10000 : 0); }          // the board's bank
    const uint8_t* external() const { return buffer_.data() + (commbank_ ? 0 : 0x10000); } // the CPU's bank
    uint8_t* external() { return buffer_.data() + (commbank_ ? 0 : 0x10000); }

    std::unique_ptr<NetLink> link_;
    std::vector<uint8_t> net_ram_;  // 64 KB
    std::vector<uint8_t> buffer_;   // 128 KB: two comm RAM banks
    bool commbank_ = false;

    State state_ = State::Start;
    bool running_ = false;
    uint16_t irq2ack_ = 0;
    uint16_t status0_ = 0;
    uint16_t status1_ = 0;
    uint8_t machines_ = 0;
    uint16_t counter_ = 0;
    uint16_t segment_size_ = 0;
};

} // namespace m3
