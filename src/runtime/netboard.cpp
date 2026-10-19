// Adapted from Supermodel's Src/Network/SimNetBoard.cpp and Src/Model3/Model3.cpp:
//   Copyright 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis,
//   Harry Tuttle, and Spindizzi. GPL-3.0-or-later.
#include "runtime/netboard.hpp"

#include <algorithm>
#include <cstring>

namespace m3 {

namespace {

constexpr uint64_t kNetGuid = 0x5bf177da34872; // Supermodel's: identifies a compatible build on the link

constexpr uint16_t flip16(uint16_t v) { return static_cast<uint16_t>(v << 8 | v >> 8); }
constexpr uint32_t flip32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

enum Region : uint32_t { kComm = 0, kIo = 1 };

uint32_t region_of(uint32_t addr) { return (addr & 0x3FFFF) >> 16; } // 2 and 3: net RAM

} // namespace

std::vector<uint8_t> LoopbackLink::receive() {
    if (queue_.empty()) return {};
    std::vector<uint8_t> packet = std::move(queue_.front());
    queue_.pop_front();
    return packet;
}

NetBoard::NetBoard(std::unique_ptr<NetLink> link)
    : link_(link ? std::move(link) : std::make_unique<LoopbackLink>()), net_ram_(0x10000, 0), buffer_(0x20000, 0) {}

// ---- CPU side (Model3.cpp) ----

uint8_t NetBoard::read8(uint32_t addr) const {
    switch (region_of(addr)) {
    case kComm: return external()[(addr & 0xFFFF) ^ 2];
    case kIo:   return static_cast<uint8_t>(read_io((addr & 0x1FF) / 2));
    default:    return net_ram_[(addr & 0x1FFFF) / 2];
    }
}

uint16_t NetBoard::read16(uint32_t addr) const {
    if (region_of(addr) != kComm) return 0xFFFF; // only comm RAM answers 16-bit reads
    return flip16(le16(&external()[(addr & 0xFFFF) ^ 2]));
}

uint32_t NetBoard::read32(uint32_t addr) const {
    switch (region_of(addr)) {
    case kComm: {
        const uint32_t v = flip32(le32(&external()[addr & 0xFFFC]));
        return v << 16 | v >> 16;
    }
    case kIo: return flip32(read_io((addr & 0x1FF) / 2));
    default:  return flip32(le16(&net_ram_[((addr & 0x1FFFF) / 2) & 0xFFFE]));
    }
}

void NetBoard::write8(uint32_t addr, uint8_t data) {
    switch (region_of(addr)) {
    case kComm: external()[(addr & 0xFFFF) ^ 2] = data; break;
    case kIo:   write_io((addr & 0x1FF) / 2, data); break;
    default:    net_ram_[(addr & 0x1FFFF) / 2] = data; break;
    }
}

void NetBoard::write16(uint32_t addr, uint16_t data) {
    if (region_of(addr) == kComm) put16(&external()[(addr & 0xFFFF) ^ 2], flip16(data));
}

void NetBoard::write32(uint32_t addr, uint32_t data) {
    switch (region_of(addr)) {
    case kComm: {
        const uint32_t v = flip32(data);
        put32(&external()[addr & 0xFFFC], v << 16 | v >> 16);
        break;
    }
    case kIo: write_io((addr & 0x1FF) / 2, flip16(static_cast<uint16_t>(data >> 16))); break;
    default:  put16(&net_ram_[((addr & 0x1FFFF) / 2) & 0xFFFE], flip16(static_cast<uint16_t>(data >> 16))); break;
    }
}

uint16_t NetBoard::read_io(uint32_t reg) const {
    if (!running_) return 0;
    switch (reg) {
    case 0x00: return irq2ack_;
    case 0x88: return status0_;
    case 0x8A: return status1_;
    default:   return 0;
    }
}

void NetBoard::write_io(uint32_t reg, uint16_t data) {
    switch (reg) {
    case 0x00: irq2ack_ = data; break;
    case 0x88: status0_ = data; break;
    case 0x8A: status1_ = data; break;
    case 0xC0:
        if (data == 0) reset();
        running_ = data != 0;
        break;
    default: break;
    }
}

// ---- The board's side (SimNetBoard::RunFrame, type one games) ----

void NetBoard::reset() {
    if (state_ == State::Ready) { // let the other cabinets know
        link_->send(nullptr, 0);
        link_->receive();
    }
    running_ = false;
    state_ = State::Start;
}

void NetBoard::run_frame() {
    if (!running_) return;
    uint8_t* comm_ram = comm();
    auto ram16 = [&](uint32_t byte) { return le16(&net_ram_[byte]); };
    auto comm16 = [&](uint32_t byte, uint16_t v) { put16(&comm_ram[byte], v); };

    switch (state_) {
    case State::Start:
        status0_ = 0;
        status1_ = 0xE000;
        state_ = State::Init;
        break;

    case State::Init:
        std::fill(buffer_.begin(), buffer_.end(), 0); // both banks
        if (status0_ & 0x8000) { // the main board has written its init step
            irq2ack_ |= 0x01;    // as if IRQ 2 had been acknowledged
            if (status0_ == 0xF000) {
                status1_ = 0;
                comm16(0x72, flip16(1));
                state_ = State::Testing;
            }
            status0_ = 0;
        }
        break;

    case State::Testing: {
        status0_ = static_cast<uint16_t>(status0_ + 1); // type one games need this to move every frame
        uint8_t machines = 0, index = 0;
        if (ram16(0x400) == 0) { // master: check the ring and count the other cabinets
            const uint64_t guid = kNetGuid;
            link_->send(reinterpret_cast<const uint8_t*>(&guid), sizeof guid);
            std::vector<uint8_t> back = link_->receive();
            if (back.size() != sizeof guid) break;
            uint64_t test;
            std::memcpy(&test, back.data(), sizeof test);
            if (test != kNetGuid) test = 0;
            link_->send(reinterpret_cast<const uint8_t*>(&test), sizeof test);
            link_->receive();
            if (test != kNetGuid) {
                state_ = State::Error;
                break;
            }
            link_->send(&index, 1);
            back = link_->receive();
            if (back.empty()) break;
            machines = back[0];
            link_->send(&machines, 1);
            link_->receive();
        } else { // slave: pass the handshake on
            std::vector<uint8_t> in = link_->receive();
            if (in.size() != sizeof(uint64_t)) break;
            link_->send(in.data(), in.size());
            in = link_->receive();
            if (in.size() != sizeof(uint64_t)) break;
            link_->send(in.data(), in.size());
            uint64_t test;
            std::memcpy(&test, in.data(), sizeof test);
            if (test != kNetGuid) {
                state_ = State::Error;
                break;
            }
            in = link_->receive();
            if (in.empty()) break;
            index = static_cast<uint8_t>(in[0] + 1);
            link_->send(&index, 1);
            in = link_->receive();
            if (in.empty()) break;
            machines = in[0];
            link_->send(&machines, 1);
        }
        if (machines == 0 && !link_->loopback()) { // nobody else on a real link
            state_ = State::Error;
            break;
        }
        machines_ = static_cast<uint8_t>(machines + 1);
        status0_ = 0;
        status1_ = static_cast<uint16_t>(0x2021 + machines * 0x20 + index);
        comm16(0x0, ram16(0x400)); // 0 master, 1 slave
        comm16(0x2, machines);
        comm16(0x4, index);
        counter_ = 0;
        comm16(0x6, 0);
        segment_size_ = ram16(0x404);
        comm16(0x8, flip16(static_cast<uint16_t>(0x100 + segment_size_)));
        comm16(0xA, flip16(static_cast<uint16_t>(ram16(0x402) - segment_size_ - 1)));
        comm16(0xC, flip16(0x100));
        comm16(0xE, flip16(static_cast<uint16_t>(ram16(0x402) - segment_size_ + 0x200)));
        state_ = State::Ready;
        break;
    }

    case State::Ready:
        ++counter_;
        comm16(0x6, flip16(counter_));
        // Each cabinet's segment goes round the ring; every cabinet gets
        // all of them back, its own included.
        for (uint32_t i = 0; i < machines_; ++i) {
            const uint32_t from = 0x100 + i * segment_size_, to = 0x100 + (i + 1) * segment_size_;
            if (to + segment_size_ > 0x10000) break;
            link_->send(comm_ram + from, segment_size_);
            const std::vector<uint8_t> in = link_->receive();
            if (in.empty()) {
                link_->send(nullptr, 0); // tell the others the link broke
                state_ = State::Error;
                status1_ = 0x40;          // "link broken" for the main board
                break;
            }
            std::memcpy(comm_ram + to, in.data(), std::min<size_t>(in.size(), 0x10000 - to));
        }
        commbank_ = !commbank_;
        break;

    case State::Error:
        break;
    }
}

} // namespace m3
