#include "runtime/eeprom.hpp"

namespace m3 {

void Eeprom93C46::write_pins(bool cs, bool clk, bool di) {
    const bool rising = !clk_ && clk;
    clk_ = clk;
    if (!cs) { // deselected: end of command; programming may still be going on
        cs_ = false;
        sending_ = false;
        in_ = 0;
        in_bits_ = 0;
        done_ = false;
        return;
    }
    cs_ = true;
    if (!rising) return;

    if (sending_) { // each clock brings the next data bit; words follow on
        do_ = (out_ >> 15) & 1;
        out_ = static_cast<uint16_t>(out_ << 1);
        if (++out_bits_ == 16) {
            addr_ = static_cast<uint8_t>((addr_ + 1) & 0x3F);
            out_ = words_[addr_];
            out_bits_ = 0;
        }
        return;
    }

    if (done_) return; // a complete command: nothing more until CS drops
    if (in_bits_ == 0 && !di) return; // leading zeros before the start bit don't count
    in_ = in_ << 1 | (di ? 1u : 0u);
    ++in_bits_;
    if (in_bits_ < 9) return; // start bit + opcode + address

    const uint32_t op = (in_ >> (in_bits_ - 3)) & 3;  // the 2 bits after the start bit
    const uint8_t address = static_cast<uint8_t>((in_ >> (in_bits_ - 9)) & 0x3F);
    if (in_bits_ == 9) {
        switch (op) {
        case 2: // READ: dummy 0 now, D15 on the next clock
            addr_ = address;
            out_ = words_[addr_];
            out_bits_ = 0;
            do_ = false;
            sending_ = true;
            return;
        case 3: // ERASE
            done_ = true;
            if (writable_) { // a refused write starts no programming cycle
                words_[address] = 0xFFFF;
                busy_ = kBusyReads;
            }
            return;
        case 0:
            switch (address >> 4) {
            case 3: writable_ = true; done_ = true; return;  // EWEN
            case 0: writable_ = false; done_ = true; return; // EWDS
            case 2:                            // ERAL
                done_ = true;
                if (writable_) {
                    erase_all();
                    busy_ = kBusyReads;
                }
                return;
            default: return;                   // WRAL: wait for the data
            }
        default: return; // WRITE: wait for the data
        }
    }
    if (in_bits_ == 25) { // 16 data bits in
        const uint16_t data = static_cast<uint16_t>(in_);
        const uint8_t a = static_cast<uint8_t>((in_ >> 16) & 0x3F);
        if (op == 1 && writable_) words_[a] = data;          // WRITE
        if (op == 0 && (a >> 4) == 1 && writable_) words_.fill(data); // WRAL
        done_ = true;
        if (writable_) busy_ = kBusyReads;
    }
}

bool Eeprom93C46::read_do() {
    if (sending_) return do_;
    if (busy_ > 0) { // still programming
        --busy_;
        return false;
    }
    return true; // ready
}

} // namespace m3
