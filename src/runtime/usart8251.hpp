// The main board's sound link: a NEC uPD71051 serial chip (an 8251 USART;
// MAME's board notes), clocked from 500 kHz (16 x 31,250 baud), its
// transmitter wired to the sound board's MIDI input and to the music board.
// Only the transmit side is modelled; nothing comes back.
//
// Spikeout programs it as a datasheet 8251: the reset sequence (three 0x00
// writes, which a chip expecting a mode takes as a sync mode and its two
// sync characters, then 0x40, internal reset), mode 0x4E (asynchronous,
// x16 clock, 8 data bits, no parity, 1 stop bit), then commands that turn
// the transmitter on (0x27) while it has bytes to send and off (0x06) when
// it hasn't. IRQ 0x40 is the TxRDY pin: up while the transmitter is
// enabled and its buffer is empty (CTS is held low, as MAME does). Turning
// the transmitter off stops new data only: bytes already written still go
// out, which the game relies on (it writes its last byte and turns the
// transmitter off at once). A byte
// written while the shifter is idle moves into it at once, so the buffer
// takes the next byte straight away; after that one byte every character
// time (10 bits, 320 us).
//
// Times are in main-CPU cycles. Bytes are reported when their stop bit has
// gone out, which is when the far end has them.
#pragma once

#include <cstdint>
#include <vector>

namespace m3 {

class State;

struct SerialByte {
    uint8_t value;
    uint64_t cycle; // when the far end has it
};
void state_fields(State& s, SerialByte& b); // save states

class Usart8251 {
public:
    Usart8251(uint64_t cpu_hz, uint64_t txc_hz) : cpu_hz_(cpu_hz), txc_hz_(txc_hz) {}

    void reset();
    void write_control(uint8_t value, uint64_t now);
    void write_data(uint8_t value, uint64_t now);
    // Status register: TxRDY (buffer empty), TxEMPTY, DSR (as Supermodel's
    // 0x83 has it); nothing is received.
    uint8_t status(uint64_t now);

    // Moves the transmitter on to `now`.
    void run_to(uint64_t now);
    // The cycle at which the transmitter next changes state (UINT64_MAX:
    // not until something is written).
    uint64_t next_event() const;
    bool txrdy_pin() const { return tx_enabled() && !buffered_; }
    uint64_t character_cycles() const;

    // Bytes that have reached the far end, oldest first (the caller empties it).
    std::vector<SerialByte>& sent() { return sent_; }

    uint8_t mode() const { return mode_; }
    uint8_t command() const { return command_; }

    void state(State& s); // save states (runtime/state.hpp)

private:
    bool configured() const { return !expect_mode_ && sync_left_ == 0; } // past the mode (and sync characters)
    bool tx_enabled() const { return configured() && (command_ & 0x01); }

    uint64_t cpu_hz_, txc_hz_;
    bool expect_mode_ = true;
    int sync_left_ = 0; // sync characters still expected after a sync mode
    uint8_t mode_ = 0, command_ = 0;
    bool buffered_ = false; // a byte waits in the transmit buffer
    uint8_t buffer_ = 0;
    uint64_t buffer_from_ = 0; // when it could start (written, or the transmitter enabled)
    bool shifting_ = false; // a byte is going out
    uint8_t shift_ = 0;
    uint64_t shift_end_ = 0; // when it has gone out (or the shifter went idle)
    std::vector<SerialByte> sent_;
};

} // namespace m3
