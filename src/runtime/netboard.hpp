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
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace m3 {

// What the board sends its segments over: the next cabinet in the ring,
// receiving from the previous one.
class NetLink {
public:
    virtual ~NetLink() = default;
    virtual void send(const uint8_t* data, std::size_t size) = 0;
    // The next packet from the previous cabinet; empty if the link broke.
    virtual std::vector<uint8_t> receive() = 0;
    // True when this cabinet is linked to itself (a ring of one).
    virtual bool loopback() const = 0;
    // The board's time in main board cycles, set before each send() and
    // receive(): a link between boards in one process (LinkRing) delivers
    // a packet once the receiver has reached the time it was sent.
    virtual void set_time(uint64_t /*main_cycles*/) {}
};

// A lone cabinet: everything sent comes straight back.
class LoopbackLink : public NetLink {
public:
    void send(const uint8_t* data, std::size_t size) override { queue_.emplace_back(data, data + size); }
    std::vector<uint8_t> receive() override;
    bool loopback() const override { return true; }

private:
    std::deque<std::vector<uint8_t>> queue_;
};

// Cabinets linked in one process: board i sends to board i + 1, the last
// to board 0. Each packet carries the time it was sent and arrives once
// the receiver's time reaches it (plus `latency`). Not thread-safe: the
// boards take turns (gamerun --boards hands the turn round every few
// lines), so a board can be up to a turn late to see a packet sent by a
// board that ran after it, never early.
//
// Deferred (boards running at once): what a board sends waits in its own
// outbox until exchange(), called with every board stopped (between two
// slices), moves it on, in board order. With `latency` at least a slice,
// nothing sent in a slice could have arrived in it anyway: the same every
// run, whichever thread ran first. Each port then touches only its own
// outbox and inbox while the boards run.
class LinkRing {
public:
    explicit LinkRing(std::size_t boards);
    std::size_t boards() const { return state_->inbox.size(); }
    // Board i's end of the ring (each taken once).
    std::unique_ptr<NetLink> port(std::size_t i);
    void set_latency(uint64_t main_cycles) { state_->latency = main_cycles; }
    void set_deferred(bool on) { state_->deferred = on; }
    void exchange();
    uint64_t delivered() const;
    // Debug aid: called for each packet sent (board, its time, the data).
    void on_send(std::function<void(std::size_t, uint64_t, const std::vector<uint8_t>&)> fn) {
        state_->on_send = std::move(fn);
    }

private:
    struct Packet {
        uint64_t at; // main board cycle it can arrive
        std::vector<uint8_t> data;
    };
    struct State {
        std::vector<std::deque<Packet>> inbox;  // by receiver
        std::vector<std::deque<Packet>> outbox; // by sender (deferred)
        std::vector<uint64_t> delivered;        // by receiver
        uint64_t latency = 0;
        bool deferred = false;
        std::function<void(std::size_t, uint64_t, const std::vector<uint8_t>&)> on_send;
    };
    class Port;
    std::shared_ptr<State> state_;
};

// The network board as the main board's bus sees it, whichever way it is
// emulated (SimNetBoard: a behavioral simulation; CommBoard: the board's own
// 68000 running the firmware the game uploads).
class NetBoardDevice {
public:
    virtual ~NetBoardDevice() = default;

    // CPU accesses to 0xC0000000-0xC003FFFF, by width (64-bit ones are two
    // 32-bit ones). Not const: a board with its own CPU catches up first.
    virtual uint8_t read8(uint32_t addr) = 0;
    virtual uint16_t read16(uint32_t addr) = 0;
    virtual uint32_t read32(uint32_t addr) = 0;
    virtual void write8(uint32_t addr, uint8_t data) = 0;
    virtual void write16(uint32_t addr, uint16_t data) = 0;
    virtual void write32(uint32_t addr, uint32_t data) = 0;

    // The main board's time, in its CPU cycles: a board with its own CPU
    // runs up to it. Called before every access and at the end of a frame.
    virtual void sync(uint64_t /*main_cycles*/) {}
    // End of a main board frame (after sync).
    virtual void end_frame() {}

    // One line for run reports.
    virtual std::string describe() const = 0;
};

class SimNetBoard : public NetBoardDevice {
public:
    // Without a link the board is a lone cabinet (loopback).
    explicit SimNetBoard(std::unique_ptr<NetLink> link = nullptr);

    // CPU accesses (Model3.cpp's Read8/16/32 and Write8/16/32).
    uint8_t read8(uint32_t addr) override;
    uint16_t read16(uint32_t addr) override;
    uint32_t read32(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t data) override;
    void write16(uint32_t addr, uint16_t data) override;
    void write32(uint32_t addr, uint32_t data) override;

    void end_frame() override { run_frame(); }
    std::string describe() const override;

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
