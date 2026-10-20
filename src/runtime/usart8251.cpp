#include "runtime/usart8251.hpp"

#include <algorithm>

namespace m3 {

void Usart8251::reset() {
    expect_mode_ = true;
    sync_left_ = 0;
    mode_ = command_ = 0;
    buffered_ = false;
}

// Start bit, data bits, parity, stop bits, from the mode byte, in clock
// periods of the x1/x16/x64 baud factor (synchronous: no start or stop bits).
uint64_t Usart8251::character_cycles() const {
    const uint64_t data_bits = 5u + ((mode_ >> 2) & 3u);
    const uint64_t parity = (mode_ & 0x10) ? 1 : 0;
    static constexpr uint64_t kFactor[4] = {1, 1, 16, 64};
    const uint64_t factor = kFactor[mode_ & 3];
    uint64_t half_bits; // in half bits: 1.5 stop bits exist
    if ((mode_ & 3) == 0) {
        half_bits = 2 * (data_bits + parity);
    } else {
        static constexpr uint64_t kStopHalves[4] = {2, 2, 3, 4}; // 0 is invalid; taken as 1
        half_bits = 2 * (1 + data_bits + parity) + kStopHalves[mode_ >> 6];
    }
    return cpu_hz_ * factor * half_bits / (2 * txc_hz_);
}

void Usart8251::run_to(uint64_t now) {
    for (;;) {
        if (shifting_ && shift_end_ <= now) {
            sent_.push_back({shift_, shift_end_});
            shifting_ = false;
        }
        // TxEN gates new data only (TxRDY): a byte already in the buffer
        // still goes out. Spikeout writes its last byte and turns the
        // transmitter off in the same breath.
        if (shifting_ || !buffered_ || !configured()) return;
        const uint64_t start = std::max(buffer_from_, shift_end_);
        if (start > now) return;
        shift_ = buffer_;
        buffered_ = false;
        shifting_ = true;
        shift_end_ = start + character_cycles();
    }
}

uint64_t Usart8251::next_event() const {
    if (shifting_) return shift_end_;
    if (buffered_ && configured()) return std::max(buffer_from_, shift_end_);
    return UINT64_MAX;
}

void Usart8251::write_control(uint8_t value, uint64_t now) {
    run_to(now);
    if (expect_mode_) {
        mode_ = value;
        expect_mode_ = false;
        // Synchronous: one or two sync characters follow (bit 7: single).
        sync_left_ = (value & 3) == 0 ? ((value & 0x80) ? 1 : 2) : 0;
    } else if (sync_left_ > 0) {
        --sync_left_;
    } else if (value & 0x40) { // internal reset: a mode comes next
        reset();
    } else {
        command_ = value;
    }
    if (configured()) buffer_from_ = std::max(buffer_from_, now); // a byte written before the mode can't have started
    run_to(now);
}

void Usart8251::write_data(uint8_t value, uint64_t now) {
    run_to(now);
    buffer_ = value; // a byte still waiting is overwritten
    buffered_ = true;
    buffer_from_ = now;
    run_to(now);
}

uint8_t Usart8251::status(uint64_t now) {
    run_to(now);
    uint8_t s = 0x80; // DSR
    if (!buffered_) s |= 0x01;
    if (!buffered_ && !shifting_) s |= 0x04;
    return s;
}

} // namespace m3
