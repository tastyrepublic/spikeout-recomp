#include "test.hpp"

#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/dsb2.hpp"
#include "runtime/jtag.hpp"
#include "runtime/netboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/real3d_render.hpp"
#include "runtime/scsp.hpp"
#include "runtime/soundboard.hpp"
#include "runtime/tilegen.hpp"

#include <algorithm>
#include <bit>
#include <vector>

using namespace m3;

namespace {

// One full TCK cycle; returns TDO as seen after the falling edge.
bool clock(Jtag& j, bool tms, bool tdi = false) {
    j.write(true, tms, tdi, true);
    j.write(false, tms, tdi, true);
    return j.tdo();
}

uint32_t bits_to_word(const std::vector<bool>& bits, std::size_t from) {
    uint32_t v = 0;
    for (std::size_t i = 0; i < 32; ++i) v |= uint32_t{bits[from + i]} << i;
    return v;
}

// What the CPU writes with stwbrx of a 16-bit value to a network board
// register or net RAM word (the bus value is byte-reversed).
uint32_t brx(uint32_t v) { return bswap32(v); }

} // namespace

TEST_CASE("jtag: the IDCODE scan reads the Step 2.1 Real3D chips in chain order") {
    Jtag j;
    clock(j, true);
    clock(j, true); // Test-Logic-Reset: every ASIC selects IDCODE
    clock(j, false); // Run-Test/Idle
    clock(j, true);  // Select-DR-Scan
    clock(j, false); // Capture-DR
    bool tdo = clock(j, false); // Shift-DR: the first bit is out
    std::vector<bool> bits;
    for (int i = 0; i < 296; ++i) { // 9 ASICs x 32 bits + 8 3D-RAMs x 1 bit
        bits.push_back(tdo);
        tdo = clock(j, false);
    }
    // TDO comes from the end of the chain: three 3D-RAMs, then Mars,
    // Mars, ..., and Jupiter (the first device) last.
    CHECK_EQ(bits_to_word(bits, 3), 0x316c6057u);   // Mars 1/1
    CHECK_EQ(bits_to_word(bits, 200), 0x316c4057u); // Venus
    CHECK_EQ(bits_to_word(bits, 232), 0x416c3057u); // Mercury
    CHECK_EQ(bits_to_word(bits, 264), 0x516c7057u); // Jupiter
}

TEST_CASE("jtag: a mode word written to Mercury is kept") {
    Jtag j;
    auto shift_ir = [&](uint32_t instruction) { // every device gets `instruction` (ASICs: 5 bits, 3D-RAMs: 4)
        clock(j, false);
        clock(j, true);
        clock(j, true);  // Select-IR-Scan
        clock(j, false); // Capture-IR
        clock(j, false); // Shift-IR
        // Shift the chain's 9 x 5 + 8 x 4 = 77 bits: last device's bits go first.
        std::vector<bool> in;
        for (int d = 16; d >= 0; --d) {
            const bool asic = !(d == 4 || d == 7 || d == 8 || d == 9 || d == 11 || d == 14 || d == 15 || d == 16);
            const int len = asic ? 5 : 4;
            for (int b = 0; b < len; ++b) in.push_back((asic ? instruction : 15u) >> b & 1);
        }
        for (std::size_t i = 0; i < in.size(); ++i) clock(j, i + 1 == in.size(), in[i]); // last bit: Exit1-IR
        clock(j, true);  // Update-IR
    };
    shift_ir(8); // MODEWORD on every ASIC
    clock(j, true);  // Select-DR-Scan
    clock(j, false); // Capture-DR
    clock(j, false); // Shift-DR
    // 296 bits: the first device (Jupiter) gets the last 32, Mercury the 32 before.
    std::vector<bool> in(296, false);
    const uint32_t word = 0x12345678;
    for (int b = 0; b < 32; ++b) in[232 + b] = (word >> b) & 1;
    for (std::size_t i = 0; i < in.size(); ++i) clock(j, i + 1 == in.size(), in[i]);
    clock(j, true); // Update-DR
    CHECK_EQ(j.modeword(Jtag::Asic::Mercury), word);
}

TEST_CASE("netboard: a lone cabinet's handshake and its segment going round the ring") {
    SimNetBoard nb;
    const uint32_t io = 0xC0010000, ram = 0xC0020000;
    CHECK_EQ(nb.read32(0xC0000000), nb.read32(0xC0000000)); // comm RAM answers
    nb.write32(0xC0000000, 0x55555555);
    CHECK_EQ(nb.read32(0xC0000000), 0x55555555u);           // the game's RAM test

    nb.write32(io + 0x180, brx(0xFFFF)); // run
    CHECK(nb.running());
    nb.run_frame();                      // start
    CHECK_EQ(brx(nb.read32(io + 0x114)), 0xE000u); // status 1, read with lwbrx
    nb.write32(io + 0x110, brx(0xF000)); // status 0: the main board's last init step
    nb.run_frame();                      // init
    CHECK(nb.state() == SimNetBoard::State::Testing);
    CHECK_EQ(brx(nb.read32(io + 0x000)) & 1, 1u); // "IRQ 2 acknowledged"

    nb.write32(ram + 0x800, brx(0));      // master
    nb.write32(ram + 0x804, brx(0x3C00)); // total size
    nb.write32(ram + 0x808, brx(0x0C00)); // segment size
    nb.run_frame();                       // testing: the ring is this cabinet alone
    CHECK(nb.state() == SimNetBoard::State::Ready);
    CHECK_EQ(nb.status1(), 0x2021u);      // one machine, index 0

    nb.write32(0xC0000100, 0xCAFEF00D);  // this cabinet's segment
    nb.run_frame();                      // the board takes the CPU's bank...
    nb.run_frame();                      // ...sends it round, and hands it back
    CHECK_EQ(nb.read32(0xC0000100 + 0x0C00), 0xCAFEF00Du);
    CHECK(nb.state() == SimNetBoard::State::Ready);
}

TEST_CASE("commboard: uploaded 68000 firmware answers a command through IRQ 2") {
    // A tiny firmware: SSP 0xFF00, start at 0x100 (enable interrupts, loop);
    // IRQ 2 (autovector, 0x68 -> 0x200) copies status 0 to status 1 and acks.
    std::vector<uint16_t> rom(0x110, 0x4E71); // nop
    auto put32 = [&](uint32_t addr, uint32_t v) {
        rom[addr / 2] = static_cast<uint16_t>(v >> 16);
        rom[addr / 2 + 1] = static_cast<uint16_t>(v);
    };
    put32(0x00, 0x0000FF00);
    put32(0x04, 0x00000100);
    put32(0x68, 0x00000200);
    const uint16_t main_loop[] = {0x46FC, 0x2000, 0x60FE}; // move.w #$2000,sr; bra.s *
    const uint16_t handler[] = {
        0x3039, 0x000C, 0x0088, // move.w $c0088,d0   status 0
        0x33C0, 0x000C, 0x008A, // move.w d0,$c008a   status 1
        0x33FC, 0xFFFF, 0x0004, 0x00A0, // move.w #$ffff,$400a0   IRQ 2 ack
        0x4E73,                 // rte
    };
    for (std::size_t i = 0; i < 3; ++i) rom[0x100 / 2 + i] = main_loop[i];
    for (std::size_t i = 0; i < 11; ++i) rom[0x200 / 2 + i] = handler[i];

    CommBoard board;
    // Upload as the game does: stwbrx of each 68000 word, one per 32-bit word.
    for (uint32_t i = 0; i < rom.size(); ++i) board.write32(0xC0020000 + 4 * i, brx(rom[i]));
    CHECK(board.in_reset());
    board.write32(0xC0010180, brx(1)); // run
    CHECK(!board.in_reset());
    board.sync(1'000'000);               // let it start (main board cycles)
    CHECK_EQ(board.pc() & ~0xFu, 0x100u); // in its main loop

    board.write32(0xC0010110, brx(0x1234)); // a command: status 0, raises IRQ 2
    board.sync(2'000'000);
    CHECK_EQ(board.irqs_taken[2], 1u);
    CHECK_EQ(brx(board.read32(0xC0010114)), 0x1234u); // the firmware's answer
    CHECK_EQ(brx(board.read32(0xC0010000)) & 1, 1u);   // "IRQ 2 acknowledged"
}

TEST_CASE("bus: Real3D on the PCI bus, DMA ID command and ping-pong bit") {
    RomSet roms;
    Bus bus(roms);
    // PCI config through the MPC106: bus 0, device 13, register 0.
    bus.write32(0xFEC00CF8, bswap32(0x80000000u | 13u << 11));
    CHECK_EQ(bswap32(bus.read32(0xFEE00CFC)), 0x178611DBu);
    // DMA device: ID command, then the result.
    bus.write32(0xC2000010, bswap32(0x20000000));
    CHECK_EQ(bswap32(bus.read32(0xC2000014)), 0x16C311DBu);
    CHECK_EQ(bus.read8(0xC200000C) & 0x80u, 0u); // not busy
    // Status register 0, directly and through the DMA command.
    CHECK_EQ(bswap32(bus.read32(0x84000000)), 0xFDFFFFFFu);
    bus.flip_ping_pong();
    bus.write32(0xC2000010, bswap32(0x80000000));
    CHECK_EQ(bswap32(bus.read32(0xC2000014)), 0xFFFFFFFFu);
}

namespace {

// Clocks one command into the EEPROM through the I/O board (bank 1 selected).
struct EepromPins {
    Bus& bus;
    void clock(bool di) {
        const uint8_t base = 0x41 | (di ? 0x20 : 0); // CS, bank 1, DI
        bus.write8(0xF0040000, base);
        bus.write8(0xF0040000, base | 0x80); // rising edge
    }
    void send(uint32_t bits, int count) {
        for (int i = count - 1; i >= 0; --i) clock((bits >> i) & 1);
    }
    bool dout() { return (bus.read8(0xF0040004) & 0x20) != 0; }
    void deselect() { bus.write8(0xF0040000, 0x01); }
};

} // namespace

TEST_CASE("eeprom: writes need EWEN; READ gives the dummy 0 then D15 first") {
    RomSet roms;
    Bus bus(roms);
    uint64_t now = 0;
    bus.set_clock([](const void* t) { return *static_cast<const uint64_t*>(t); }, &now);
    EepromPins pins{bus};

    pins.send(0b1'01'000101'1011111011101111, 25); // WRITE 5, 0xBEEF while writes are disabled
    pins.deselect();
    CHECK_EQ(bus.eeprom().words()[5], 0xFFFFu);

    pins.send(0b1'00'110000, 9); // EWEN
    pins.deselect();
    pins.send(0b1'01'000101'1011111011101111, 25);
    pins.deselect();
    CHECK_EQ(bus.eeprom().words()[5], 0xBEEFu);
    pins.send(0, 1); // CS up again: busy (DO low) for the 1.75 ms programming cycle, then ready
    now = Eeprom93C46::kWriteUs * 166 - 1;
    CHECK(!pins.dout());
    now = Eeprom93C46::kWriteUs * 166;
    CHECK(pins.dout());
    pins.deselect();

    pins.send(0b1'10'000101, 9); // READ 5
    CHECK(!pins.dout());         // the dummy bit
    uint32_t word = 0;
    for (int i = 0; i < 32; ++i) { // two words: 5, then 6 (blank)
        pins.clock(false);
        word = word << 1 | (pins.dout() ? 1u : 0u);
    }
    CHECK_EQ(word, 0xBEEFFFFFu);
    pins.deselect();
}

TEST_CASE("security: 315-5881 block cipher matches MAME's") {
    // Known answers from MAME's implementation (via Supermodel's Crypto.cpp).
    CHECK_EQ(Crypt315_5881::block_decrypt(0x29236FC8, 0x9EF6, 0x0000, 0x0000), 0xE6AAu);
    CHECK_EQ(Crypt315_5881::block_decrypt(0x29236FC8, 0x9EF6, 0x0000, 0xFFFF), 0xBE94u);
    CHECK_EQ(Crypt315_5881::block_decrypt(0x29236FC8, 0x9EF6, 0x0001, 0xBEEF), 0xEB34u);
    CHECK_EQ(Crypt315_5881::block_decrypt(0x29236FC8, 0x9EF6, 0x1234, 0xFFFF), 0x63BDu);
}

TEST_CASE("security: the game's view of the board, plain and compressed streams") {
    RomSet roms;
    Bus bus(roms);
    // Encrypted words, one in the top half of each 32-bit word of the board's RAM.
    for (uint32_t i = 0; i < 0x8000; ++i) bus.write32(0xFE180000 + 4 * i, (i * 0x9E37u + 0x1234u) << 16);

    auto stream = [&](uint16_t sequence_key) {
        bus.write32(0xFE1A0010, 0); // start again at address 0
        bus.write32(0xFE1A0018, uint32_t{static_cast<uint16_t>(sequence_key << 8 | sequence_key >> 8)} << 16);
        CHECK_EQ(bus.read32(0xFE1A0000), 0u);          // status: never busy
        CHECK_EQ(bus.read32(0xFE1A001C), 0xFFFF0000u); // the first read after a restart
        std::vector<uint32_t> words;
        for (int i = 0; i < 12; ++i) words.push_back(bus.read32(0xFE1A001C) >> 16);
        return words;
    };

    // No key (no security chip): the data reads as all ones.
    CHECK_EQ(stream(0x0000).at(0), 0xFFFFu);

    bus.set_security_key(0x29236FC8); // Spikeout's
    // Expected output from MAME's implementation for the same RAM and keys.
    const std::vector<uint32_t> plain = {0x1A81, 0xBB2E, 0x5E03, 0xDFFA, 0x8A95, 0x5ECA,
                                         0xACE1, 0x6288, 0x17F6, 0x6BA8, 0x9F5D, 0x716A};
    const std::vector<uint32_t> compressed = {0x0000, 0x4B4B, 0x0F4B, 0x0F0F, 0x000F, 0x0000,
                                              0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
    CHECK((stream(0x0000) == plain));      // header 0x1D4FA: uncompressed
    CHECK((stream(0x1234) == compressed)); // header 0x354EB: compressed (bit 17)
    CHECK((stream(0x0000) == plain));      // a restart starts the stream over
}

TEST_CASE("rtc: calendar helpers round-trip") {
    const Rtc72421::Date d = Rtc72421::to_date(Rtc72421::to_seconds({2024, 2, 29, 23, 59, 58}));
    CHECK_EQ(d.year, 2024);
    CHECK_EQ(d.month, 2);
    CHECK_EQ(d.day, 29);
    CHECK_EQ(d.hour, 23);
    CHECK_EQ(Rtc72421::to_seconds({1970, 1, 1, 0, 0, 0}), int64_t{0});
    CHECK_EQ(Rtc72421::to_seconds({2000, 3, 1, 0, 0, 0}), int64_t{951868800});
}

TEST_CASE("rtc: digits on the bus, emulated time, writes and 12-hour mode") {
    RomSet roms;
    Bus bus(roms); // no CPU attached: the time stays at cycle 0
    auto reg = [&](uint32_t n) { return bus.read8(0xFE140000 + 4 * n); };
    bus.rtc().set_time(Rtc72421::to_seconds({2026, 10, 19, 21, 7, 45}), 0);
    CHECK_EQ(bus.read8(0xFE140001), 0x03u); // battery OK
    const uint8_t expected[13] = {5, 4, 7, 0, 1, 2, 9, 1, 0, 1, 6, 2, 1}; // ... Monday
    for (uint32_t n = 0; n < 13; ++n) CHECK_EQ(reg(n), uint32_t{expected[n]});

    // One emulated second later.
    Rtc72421 rtc(1000);
    rtc.set_time(Rtc72421::to_seconds({1999, 12, 31, 23, 59, 59}), 0);
    CHECK_EQ(rtc.read(0, 999), 9u);
    CHECK_EQ(rtc.read(0, 1000), 0u);
    CHECK_EQ(rtc.read(11, 1000), 0u); // year 00
    CHECK_EQ(rtc.read(6, 1000), 1u);  // day 1

    // STOP holds it; writing a digit sets it.
    rtc.write(15, 0x4 | 0x2, 1000); // 24-hour, STOP
    rtc.write(5, 1, 1000);          // hour 10..
    rtc.write(4, 3, 1000);          // 13:00
    CHECK_EQ(rtc.read(5, 50000), 1u);
    CHECK_EQ(rtc.read(4, 50000), 3u);
    CHECK_EQ(rtc.read(0, 50000), 0u); // stopped
    rtc.write(15, 0x4, 50000);        // run again
    CHECK_EQ(rtc.read(0, 51000), 1u);

    // 12-hour mode: 13:xx reads 01 with the PM flag.
    rtc.write(15, 0x0, 51000);
    CHECK_EQ(rtc.read(4, 51000), 1u);
    CHECK_EQ(rtc.read(5, 51000), 4u);
}

namespace {

// Tilegen RAM in CPU byte order; the tile generator's word i is little-endian.
struct TileRam {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x120000, 0);
    void put(uint32_t byte_addr, uint32_t v) {
        for (uint32_t i = 0; i < 4; ++i) bytes[byte_addr + i] = static_cast<uint8_t>(v >> (8 * i));
    }
};

} // namespace

TEST_CASE("tilegen: 4- and 8-bit tiles, scroll, line mask, transparency, colour offset") {
    TileRam ram;
    std::array<uint32_t, 64> regs{};
    // Layer 0: 4-bit, above the 3D, on. Entry 0x0011: pattern 0x22, colours 0x10-0x1F.
    regs[0x20 / 4] = 1u << 12 | 1u << 8;
    regs[0x60 / 4] = 0x80000000u;
    ram.put(0xF8000, 0x0011u << 16);       // tiles 0 (left, high half) and 1 of row 0
    ram.put(0x22 * 32, 0x01234567u);        // its first row: pixels 0, 1, 2, ... 7
    ram.put(0x100000 + 0x10 * 4, 0x8000u);  // colour 0x10: transparent
    ram.put(0x100000 + 0x11 * 4, 0x001Fu);  // 0x11: red
    ram.put(0x100000 + 0x12 * 4, 0x03E0u);  // 0x12: green
    ram.put(0xF7000, 0xFFFF0000u);          // line 0: layers 0/1 show the primary (0)

    TilegenRenderer r;
    r.begin_frame();
    r.draw_line(ram.bytes, regs, 0);
    CHECK_EQ(r.top()[0], 0u);            // transparent
    CHECK_EQ(r.top()[1], 0xFFFF0000u);   // red
    CHECK_EQ(r.top()[2], 0xFF00FF00u);   // green
    CHECK_EQ(r.bottom()[1], 0u);

    regs[0x60 / 4] = 0x80000001u; // x scroll 1
    regs[0x40 / 4] = 0xF0u;       // red offset -16 * 2
    r.begin_frame();
    r.draw_line(ram.bytes, regs, 0);
    CHECK_EQ(r.top()[0], 0xFFDF0000u);

    ram.put(0xF7000, 0x00000000u); // the mask picks the alternate layer (1, off): nothing
    r.begin_frame();
    r.draw_line(ram.bytes, regs, 0);
    CHECK_EQ(r.top()[0], 0u);

    // Layer 2: 8-bit, below the 3D. Entry 0x0103: pattern 0x103, colours 0x100-0x1FF.
    regs[0x68 / 4] = 0x80000000u;
    ram.put(0xF7000, 0x0000FFFFu);
    ram.put(0xFC000, 0x0103u << 16);
    ram.put(0x103 * 64, 0x05060708u);
    ram.put(0x100000 + 0x105 * 4, 0x7C00u); // blue
    r.begin_frame();
    r.draw_line(ram.bytes, regs, 0);
    CHECK_EQ(r.bottom()[0], 0xFF0000FFu);
    std::vector<uint32_t> out(TilegenRenderer::kWidth * TilegenRenderer::kHeight);
    r.composite(out);
    CHECK_EQ(out[0], 0xFF0000FFu);
    CHECK_EQ(out[TilegenRenderer::kWidth], 0xFF000000u); // line 1 not drawn: black
}

TEST_CASE("sound: MIDI port bytes, control and IRQ 0x40 (Supermodel's timing)") {
    RomSet roms;
    Bus bus(roms);
    bus.midi_burst = true;
    CHECK_EQ(bus.read8(0xF0080004), 0x83u);
    bus.irq().assert_irq(0x40);
    bus.write8(0xF0080004, 0x07); // periodic IRQs off: clears it
    CHECK_EQ(bus.irq().state() & 0x40, 0u);
    bus.write8(0xF0080004, 0x27);
    CHECK_EQ(bus.midi_control(), 0x27u);
    bus.irq().assert_irq(0x40);
    bus.write8(0xF0080000, 0xAE);
    CHECK_EQ(bus.irq().state() & 0x40, 0u);
    CHECK_EQ(bus.midi_out().size(), size_t{1});
    CHECK_EQ(bus.midi_out()[0].value, 0xAEu);
    CHECK_EQ(bus.midi_bytes(), uint64_t{1});
}

TEST_CASE("usart8251: the datasheet reset sequence, mode, TxRDY and character times") {
    Usart8251 u(166'000'000, 500'000);
    // Three 0x00 (a sync mode and its two sync characters), then internal reset.
    for (uint8_t v : {uint8_t{0x00}, uint8_t{0x00}, uint8_t{0x00}, uint8_t{0x40}}) u.write_control(v, 0);
    u.write_control(0x4E, 0); // async, x16, 8N1
    CHECK_EQ(u.mode(), 0x4Eu);
    CHECK(!u.txrdy_pin()); // transmitter still off
    CHECK_EQ(u.status(0), 0x85u); // TxRDY, TxEMPTY, DSR
    u.write_control(0x27, 100);
    CHECK(u.txrdy_pin());
    u.write_data(0x12, 100);
    CHECK(u.txrdy_pin()); // into the shifter at once
    CHECK_EQ(u.status(100), 0x81u); // shifting: not empty
    u.write_data(0x34, 200);
    CHECK(!u.txrdy_pin());
    const uint64_t ch = u.character_cycles();
    CHECK_EQ(ch, uint64_t{53'120}); // 10 bits at 31,250 baud
    CHECK_EQ(u.next_event(), 100 + ch);
    u.run_to(100 + ch);
    CHECK(u.txrdy_pin());
    CHECK_EQ(u.sent().size(), size_t{1});
    CHECK_EQ(u.sent()[0].cycle, 100 + ch);
    u.run_to(100 + 2 * ch);
    CHECK_EQ(u.sent().size(), size_t{2});
    CHECK_EQ(u.sent()[1].value, 0x34u);
    CHECK_EQ(u.next_event(), UINT64_MAX);
    // 2 stop bits and parity: 12 bits; x64: a quarter of the rate.
    u.write_control(0x40, 0);
    u.write_control(0xDF, 0); // 2 stop, even parity, 8 bits, x64
    CHECK_EQ(u.character_cycles(), uint64_t{166'000'000} * 64 * 12 / 500'000);
    // The last byte written and the transmitter turned off at once (as
    // Spikeout does): TxRDY drops, the byte still goes out.
    u.write_control(0x27, 1'000'000);
    u.write_data(0x56, 1'000'000);
    u.write_data(0x78, 1'000'000);
    u.write_control(0x06, 1'000'000);
    CHECK(!u.txrdy_pin());
    u.run_to(5'000'000);
    CHECK_EQ(u.sent().size(), size_t{4});
    CHECK_EQ(u.sent()[3].value, 0x78u);
    CHECK_EQ(u.sent()[3].cycle, 1'000'000 + 2 * u.character_cycles());
}

TEST_CASE("sound: the MIDI port's IRQ 0x40 is the serial chip's TxRDY, a level") {
    RomSet roms;
    Bus bus(roms);
    bus.write8(0xF0080004, 0x4E);
    bus.write8(0xF0080004, 0x27);
    CHECK((bus.irq().state() & 0x40) != 0);
    bus.write8(0xF0100018, 0x40); // acknowledged (system register 0x18), but still ready
    CHECK((bus.irq().state() & 0x40) != 0);
    bus.write8(0xF0080004, 0x06);
    CHECK_EQ(bus.irq().state() & 0x40, 0u);
    CHECK_EQ(bus.read8(0xF0080004), 0x85u);
}

TEST_CASE("scsp: a looped 16-bit slot plays in the centre at full level, and fades out after key off") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    for (uint32_t i = 0; i < 16; ++i) ram[0x1000 + 2 * i] = 0x40; // 0x4000
    Scsp scsp(ram, true);
    scsp.write16(0x02, 0x1000); // SA
    scsp.write16(0x04, 0);      // LSA
    scsp.write16(0x06, 16);     // LEA
    scsp.write16(0x08, 0x001F); // AR 31: no attack time; D1R, D2R 0: hold
    scsp.write16(0x0A, 0x001F); // RR 31
    scsp.write16(0x16, 0xE000); // DISDL 7 (0 dB), DIPAN 0 (centre)
    scsp.write16(0x00, 0x1800 | 1 << 5); // KYONEX, KYONB, normal loop
    CHECK_EQ(scsp.active_slots(), 1);
    int32_t l = 0, r = 0;
    for (int i = 0; i < 100; ++i) scsp.sample(l, r);
    CHECK_EQ(l, 0x4000);
    CHECK_EQ(r, 0x4000);
    scsp.write16(0x400, 0x0100); // DAC18B: the 18-bit mix unscaled (Supermodel)
    scsp.sample(l, r);
    CHECK_EQ(l, 0x10000);
    scsp.write16(0x400, 0);
    scsp.write16(0x00, 0x1000 | 1 << 5); // key off: release
    for (int i = 0; i < 1000; ++i) scsp.sample(l, r);
    CHECK_EQ(scsp.active_slots(), 0);
    CHECK_EQ(l, 0);
}

TEST_CASE("scsp: external inputs mix at slot 16/17's levels, silent until set") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    Scsp scsp(ram, true);
    int32_t l = 0, r = 0;
    scsp.sample(l, r, 0x1000, 0x1000);
    CHECK_EQ(l, 0); // levels 0: nothing
    scsp.write16(16 * 0x20 + 0x16, 0xE000); // slot 16: DISDL 7 (0 dB), centre; EFSDL 0
    scsp.sample(l, r, 0x1000, 0);
    CHECK_EQ(l, 0x1000);
    CHECK_EQ(r, 0x1000);
    scsp.write16(16 * 0x20 + 0x16, 0xE0A0); // EFSDL 5 (-12 dB) takes over
    scsp.sample(l, r, 0x1000, 0);
    CHECK(l > 0x3E0 && l < 0x420);
}

TEST_CASE("scsp: timer A overflow interrupts at its level until acknowledged") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    Scsp scsp(ram, true);
    scsp.write16(0x424, 1 << 6); // SCILV0, SCILV1: timer A at level 3
    scsp.write16(0x426, 1 << 6);
    scsp.write16(0x41E, 0x40);   // SCIEB: timer A
    scsp.write16(0x418, 0x00FE); // prescale 1, count 0xFE
    scsp.tick_timers();
    CHECK_EQ(scsp.irq_level(), 0);
    CHECK_EQ(scsp.read16(0x418), 0x00FFu);
    scsp.tick_timers(); // overflows
    CHECK_EQ(scsp.irq_level(), 3);
    scsp.write16(0x418, 0x00FE);
    scsp.write16(0x422, 0x40); // SCIRE
    CHECK_EQ(scsp.irq_level(), 0);
}

TEST_CASE("scsp: MIDI input interrupts while bytes wait; only the data byte takes one") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    Scsp scsp(ram, true);
    scsp.write16(0x424, 1 << 3); // MIDI at level 1
    scsp.write16(0x41E, 0x08);
    CHECK_EQ(scsp.irq_level(), 0);
    scsp.midi_in(0x12);
    scsp.midi_in(0x34);
    CHECK_EQ(scsp.irq_level(), 1);
    CHECK_EQ(scsp.read8(0x404) & 1, 0); // MIEMPTY clear; the status byte takes nothing
    CHECK_EQ(scsp.read16(0x404) & 0xFF, 0x12);
    CHECK_EQ(scsp.read8(0x405), 0x34u);
    CHECK_EQ(scsp.read8(0x404) & 1, 1); // now empty
    CHECK_EQ(scsp.irq_level(), 0);
}

TEST_CASE("sound board: vectors from the program, sample ROM banks through 0x400001") {
    std::vector<uint8_t> program(0x80000, 0), samples(0x1000000, 0);
    const uint8_t code[] = {
        0x00, 0x00, 0x20, 0x00, 0x00, 0x60, 0x01, 0x00, // SSP 0x2000, PC 0x600100
    };
    std::copy(std::begin(code), std::end(code), program.begin());
    const uint8_t body[] = {
        0x33, 0xF9, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, // move.w $800000,$1000
        0x13, 0xFC, 0x00, 0x10, 0x00, 0x40, 0x00, 0x01,             // move.b #$10,$400001
        0x33, 0xF9, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x10, 0x02, // move.w $800000,$1002
        0x60, 0xFE,                                                 // bra.s *
    };
    std::copy(std::begin(body), std::end(body), program.begin() + 0x100);
    samples[0] = 0x12;
    samples[1] = 0x34;
    samples[0x800000] = 0x56;
    samples[0x800001] = 0x78;
    SoundBoard board(program, samples);
    std::vector<int16_t> out(2 * 4);
    board.render(out.data(), 4);
    CHECK_EQ(board.pc(), 0x60011Cu);
    CHECK_EQ(board.ram(0)[0x1000], 0x12u);
    CHECK_EQ(board.ram(0)[0x1001], 0x34u);
    CHECK_EQ(board.ram(0)[0x1002], 0x56u);
    CHECK_EQ(board.ram(0)[0x1003], 0x78u);
}

TEST_CASE("dsb2: bytes arrive at the serial line's rate, each through IRQ 1, and reach the decoder") {
    std::vector<uint8_t> program(0x20000, 0), mpeg(16, 0);
    const uint8_t vectors[] = {0x00, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00}; // SSP 0xF10000, PC 0x100
    std::copy(std::begin(vectors), std::end(vectors), program.begin());
    const uint8_t autovectors[] = {0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x10}; // autovectors 1: 0x200, 2 (timer): 0x210
    std::copy(std::begin(autovectors), std::end(autovectors), program.begin() + 0x64);
    const uint8_t idle[] = {0x46, 0xFC, 0x20, 0x00, 0x60, 0xFE}; // move.w #$2000,sr; bra.s *
    std::copy(std::begin(idle), std::end(idle), program.begin() + 0x100);
    const uint8_t handler[] = {0x13, 0xF9, 0x00, 0xC0, 0x00, 0x01, 0x00, 0xE0, 0x00, 0x03, // move.b $C00001,$E00003
                               0x4E, 0x73};                                                 // rte
    std::copy(std::begin(handler), std::end(handler), program.begin() + 0x200);
    program[0x210] = 0x4E; // rte
    program[0x211] = 0x73;
    Dsb2 dsb(program, mpeg);
    for (uint8_t b : std::initializer_list<uint8_t>{0x24, 0x00, 0x00, 0x10, 0x74}) dsb.midi_in(b); // end address 0x10, play
    std::vector<int16_t> out(2 * 200, 0);
    dsb.render(out.data(), 20); // 20 samples: 5,000 cycles, two bytes' time at 31,250 baud
    CHECK(dsb.commands >= 1 && dsb.commands <= 2);
    dsb.render(out.data(), 200);
    CHECK_EQ(dsb.commands, uint64_t{5});
    CHECK_EQ(dsb.irqs_taken[1], uint64_t{5});
    CHECK_EQ(dsb.tracks_started, uint64_t{1});
    CHECK(!dsb.playing()); // no MPEG frames in 16 zero bytes: it stops
    CHECK_EQ(out[399], int16_t{0});
}

TEST_CASE("real3d: textures through the FIFO land in 8x8 tiles, pairs swapped") {
    RomSet roms;
    Bus bus(roms);
    // A 32x32 16-bit texture without mipmaps at (64, 32): texel value = its
    // index in the upload order.
    const uint32_t header = 0x01000000u | 1u << 23 | (32 / 32) << 7 | (64 / 32);
    const uint32_t texels = 32 * 32;
    auto fifo = [&](uint32_t word) { bus.write32(0x94000000, bswap32(word)); }; // the Real3D sees it byte-swapped
    fifo((texels * 2 + 8) * 2 - 2); // length word: (2 + w / 2) / 4 words in all
    fifo(header);
    for (uint32_t i = 0; i < texels; i += 2) fifo(i | (i + 1) << 16);
    bus.write32(0x88000000, 0);
    const Real3d& r = bus.real3d();
    CHECK_EQ(r.textures_uploaded(), uint64_t{1});
    CHECK_EQ(r.texel(64, 32), 1u);  // the first tile's order: 1, 0, 5, 4, ...
    CHECK_EQ(r.texel(65, 32), 0u);
    CHECK_EQ(r.texel(66, 32), 5u);
    CHECK_EQ(r.texel(64, 33), 3u);  // second row: 3, 2, 7, 6, ...
    CHECK_EQ(r.texel(64, 34), 17u); // third: 17, 16, ...
    CHECK_EQ(r.texel(72, 32), 65u); // the next tile starts at 64
}

TEST_CASE("real3d: VROM words are little-endian, polygon RAM takes the CPU's bytes swapped") {
    const std::vector<uint8_t> vrom(0x400010, 0x00);
    std::vector<uint8_t> v = vrom;
    v[0x400000] = 0x11;
    v[0x400001] = 0x22;
    v[0x400002] = 0x33;
    v[0x400003] = 0x44;
    Real3d r(v);
    CHECK_EQ(r.model(0x100000), 0x44332211u);
    r.write_polygon(8, 0xAABBCCDD);
    CHECK_EQ(r.model(2), 0xDDCCBBAAu);
}

namespace {

uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }

// One viewport over the whole screen and a red quad 5 units in front.
void build_scene(Real3d& r) {
    auto cull = [&](uint32_t word_addr, uint32_t value) { // as the Real3D sees it
        if (word_addr >= 0x800000) r.write_culling_high((word_addr - 0x800000) * 4, bswap32(value));
        else r.write_culling_low(word_addr * 4, bswap32(value));
    };
    auto poly = [&](uint32_t word_addr, uint32_t value) { r.write_polygon(word_addr * 4, bswap32(value)); };
    cull(0x800000 + 0x00, 0);                  // enabled, priority 0
    cull(0x800000 + 0x01, 0x01000000);         // the last viewport
    cull(0x800000 + 0x02, 0x01000100);         // root: the model at polygon RAM 0x100
    for (uint32_t w : {0x8u, 0x9u, 0xAu, 0xBu}) cull(0x800000 + w, fbits(0.5f)); // frustum -1..1
    cull(0x800000 + 0x14, (384u * 4) << 16 | 496u * 4); // 496 x 384
    cull(0x800000 + 0x15, 0);                  // eye matrix 0
    cull(0x800000 + 0x16, 0x1000);             // matrix base in low culling RAM
    cull(0x800000 + 0x17, 0x2000);             // LOD table (unused)
    const float identity[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (uint32_t i = 0; i < 12; ++i) cull(0x1000 + i, fbits(identity[i]));
    // The quad: 7 header words, then 4 vertices of x, y, z, uv.
    const uint32_t header[7] = {0x40, 0x06, 0, 0, 0xFF000000u, 0, 0x810000};
    for (uint32_t i = 0; i < 7; ++i) poly(0x100 + i, header[i]);
    auto fixed = [](float f) { return static_cast<uint32_t>(static_cast<int32_t>(f * 2048.0f) << 8); };
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (uint32_t k = 0; k < 4; ++k) { // x = 5 is ahead of the eye (GL: -z)
        poly(0x107 + k * 4, fixed(5));
        poly(0x108 + k * 4, fixed(corners[k][0]));
        poly(0x109 + k * 4, fixed(corners[k][1]));
        poly(0x10A + k * 4, 0);
    }
}

} // namespace

TEST_CASE("real3d: a viewport and a red quad, drawn the same with one thread or four") {
    Real3d mem;
    build_scene(mem);
    Real3dRenderer one(1), four(4);
    one.render(mem);
    four.render(mem);
    CHECK_EQ(one.stats().viewports, 1u);
    CHECK_EQ(one.stats().models, 1u);
    CHECK_EQ(one.stats().polygons, 1u);
    const size_t centre = 192 * 496 + 248;
    CHECK_EQ(one.image()[centre], 0xFFFF0000u); // red, lighting off
    CHECK_EQ(one.image()[0], 0u);               // nothing at the corner
    // The quad spans y and z of +-1 at distance 5: a fifth of the half-width.
    CHECK_EQ(one.image()[192 * 496 + 248 + 40], 0xFFFF0000u);
    CHECK_EQ(one.image()[192 * 496 + 248 + 60], 0u);
    CHECK((std::vector<uint32_t>(one.image().begin(), one.image().end()) ==
           std::vector<uint32_t>(four.image().begin(), four.image().end())));
}

TEST_CASE("real3d: block culling is taken when the command port and the tile generator both signal a frame") {
    Real3d r;
    r.write_command(4); // one signal only
    CHECK(!r.block_culling());
    r.tilegen_draw_frame(4); // both, before the flip: Mercury bit 2 now
    CHECK(r.block_culling());
    r.tilegen_draw_frame(0);
    r.write_command(0);
    CHECK(!r.block_culling());
    // After the flip the signals count at VBlank.
    r.ping_pong_flipped();
    r.write_command(4);
    r.tilegen_draw_frame(4);
    CHECK(!r.block_culling());
    r.begin_vblank(4);
    CHECK(r.block_culling());
    // A frame with only the tile generator's signal keeps the last setting.
    r.ping_pong_flipped();
    r.tilegen_draw_frame(0);
    r.begin_vblank(0);
    CHECK(r.block_culling());
}

TEST_CASE("bus: a Real3D DMA copy moves its data at once and completes at PCI speed, then its IRQ") {
    RomSet roms;
    Bus bus(roms);
    uint64_t now = 1000;
    bus.set_clock([](const void* t) { return *static_cast<const uint64_t*>(t); }, &now);
    bus.write32(0x1000, 0x11223344);
    bus.write32(0x1004, 0x55667788);
    bus.write8(0xC200000E, 0x81);              // byte-swap, IRQ on completion
    bus.write32(0xC2000000, bswap32(0x1000));  // source
    bus.write32(0xC2000004, bswap32(0x2000));  // destination (RAM, to see it)
    bus.write32(0xC2000008, bswap32(330));     // 330 words: 1,660 CPU cycles at 33 MHz
    CHECK_EQ(bus.read32(0x2000), 0x44332211u); // the data is there
    CHECK_EQ(bus.read8(0xC200000C) & 0x81u, 0x80u); // busy, no IRQ yet
    CHECK_EQ(bus.dma_due(), uint64_t{1000 + 1660});
    bus.run_devices(1000 + 1659);
    CHECK_EQ(bus.irq().state() & 0x100u, 0u);
    bus.run_devices(1000 + 1660);
    CHECK_EQ(bus.read8(0xC200000C) & 0x81u, 0x01u); // done, IRQ pending
    CHECK((bus.irq().state() & 0x100u) != 0);
    bus.dma_timed = false; // Supermodel and MAME: done at once
    bus.write32(0xC2000008, bswap32(330));
    CHECK_EQ(bus.read8(0xC200000C) & 0x80u, 0u);
}

TEST_CASE("mpc106: reset values and byte order as the user's manual gives them") {
    RomSet roms;
    Bus bus(roms);
    // Big-endian mode, map B (MPC106UM 3.2.1.2): the address and the data go
    // in byte-swapped; register 0xA8 then holds 0xAABBCCDD (0xAB to 0xA8).
    bus.write32(0xFEC00000, bswap32(0x800000A8));
    CHECK_EQ(bus.read32(0xFEE00000), 0x100000FFu); // PICR1's reset value 0xFF000010, as lwz sees it
    bus.write32(0xFEE00000, 0xDDCCBBAA);
    CHECK_EQ(bus.read8(0xFEE00000), 0xDDu); // register byte 0xA8
    CHECK_EQ(bus.read8(0xFEE00003), 0xAAu); // register byte 0xAB
    // Class code 0x06 (bridge) in byte 0x0B; cache line size 8 in 0x0C; ErrEnR1 0x01.
    bus.write32(0xFEC00000, bswap32(0x80000008));
    CHECK_EQ(bus.read8(0xFEE00003), 0x06u);
    bus.write32(0xFEC00000, bswap32(0x8000000C));
    CHECK_EQ(bus.read8(0xFEE00000), 0x08u);
    bus.write32(0xFEC00000, bswap32(0x800000C0));
    CHECK_EQ(bus.read8(0xFEE00000), 0x01u);
}

TEST_CASE("scsp: SCIPD takes only bit 5 from a write; timer C and per-sample interrupts use bit 7's level") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    Scsp scsp(ram, true);
    scsp.write16(0x420, 0xFFFF); // only the CPU interrupt (bit 5) is set
    CHECK_EQ(scsp.common_reg(0x420) & 0x7FFu, 0x020u);
    scsp.write16(0x420, 0x0000); // writing 0 does nothing
    CHECK_EQ(scsp.common_reg(0x420) & 0x7FFu, 0x020u);
    scsp.write16(0x422, 0x0020); // SCIRE clears it
    CHECK_EQ(scsp.common_reg(0x420) & 0x020u, 0u);
    // Bit 7's level column (here 5) serves timers B and C, MIDI out and each sample.
    scsp.write16(0x424, 1 << 7);
    scsp.write16(0x426, 0);
    scsp.write16(0x428, 1 << 7);
    scsp.write16(0x41E, 0x400); // the per-sample interrupt
    CHECK_EQ(scsp.irq_level(), 0);
    scsp.tick_timers();
    CHECK_EQ(scsp.irq_level(), 5);
}

TEST_CASE("scsp: a rate of 0 holds the envelope whatever the key-rate scale; SSCTL noise plays") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    Scsp scsp(ram, true);
    scsp.write16(0x04, 0);
    scsp.write16(0x06, 16);
    scsp.write16(0x08, 0x001F);           // AR 31, D1R 0, D2R 0
    scsp.write16(0x0A, 0xE << 10 | 0x1F); // KRS 0xE (the most scaling), DL 0, RR 31
    scsp.write16(0x10, 7 << 11);          // octave 7: the most key-rate scale
    scsp.write16(0x16, 0xE000);           // DISDL 7
    scsp.write16(0x00, 0x1800 | 1 << 7 | 1 << 5); // SSCTL 1 (noise), key on, loop
    int32_t l = 0, r = 0, loud = 0;
    for (int i = 0; i < 4000; ++i) scsp.sample(l, r); // attack, then decay 1 at rate 0
    for (int i = 0; i < 2000; ++i) {
        scsp.sample(l, r);
        loud = std::max(loud, std::abs(l));
    }
    CHECK(loud > 4000); // noise, still at full level: D1R 0 doesn't decay it
    scsp.write16(0x00, 0x1000 | 1 << 7 | 1 << 5); // key off: it releases and ends
    for (int i = 0; i < 3000; ++i) scsp.sample(l, r);
    CHECK_EQ(scsp.active_slots(), 0);
}

TEST_CASE("commboard: IRQ 5 locked to the main board's frames") {
    // SSP 0xFF00, start at 0x100 (enable interrupts, loop); IRQ 5 (0x74 ->
    // 0x200) acknowledges and returns.
    std::vector<uint16_t> rom(0x110, 0x4E71);
    auto put32 = [&](uint32_t addr, uint32_t v) {
        rom[addr / 2] = static_cast<uint16_t>(v >> 16);
        rom[addr / 2 + 1] = static_cast<uint16_t>(v);
    };
    put32(0x00, 0x0000FF00);
    put32(0x04, 0x00000100);
    put32(0x74, 0x00000200);
    const uint16_t main_loop[] = {0x46FC, 0x2000, 0x60FE};               // move.w #$2000,sr; bra.s *
    const uint16_t handler[] = {0x33FC, 0xFFFF, 0x0004, 0x0040, 0x4E73}; // move.w #$ffff,$40040; rte
    for (std::size_t i = 0; i < 3; ++i) rom[0x100 / 2 + i] = main_loop[i];
    for (std::size_t i = 0; i < 5; ++i) rom[0x200 / 2 + i] = handler[i];

    CommBoardOptions opts;
    opts.irq5_per_frame = 2;
    CommBoard board(nullptr, opts);
    for (uint32_t i = 0; i < rom.size(); ++i) board.write32(0xC0020000 + 4 * i, brx(rom[i]));
    board.write32(0xC0010180, brx(1));
    const uint64_t frame = 2'885'790; // main board cycles in a frame
    board.sync(frame);
    CHECK_EQ(board.irqs_taken[5], 0u); // not on a timer
    board.end_frame();                 // the frame's first
    board.sync(frame + frame / 4);
    CHECK_EQ(board.irqs_taken[5], 1u);
    board.sync(frame + frame * 3 / 4); // the second, half a frame on
    CHECK_EQ(board.irqs_taken[5], 2u);
    board.sync(3 * frame);             // no more until the next frame ends
    CHECK_EQ(board.irqs_taken[5], 2u);
}

TEST_CASE("link ring: board i sends to i + 1, a packet arrives once the receiver reaches its time") {
    LinkRing ring(2);
    auto a = ring.port(0), b = ring.port(1);
    CHECK(!a->loopback());
    const uint8_t data[] = {1, 2, 3};
    a->set_time(1000);
    a->send(data, sizeof data);
    b->set_time(999);
    CHECK(b->receive().empty()); // sent later than the receiver's time
    CHECK(a->receive().empty()); // nothing sent to board 0
    b->set_time(1000);
    CHECK_EQ(b->receive().size(), 3u);
    CHECK_EQ(ring.delivered(), 1u);
    CHECK(LinkRing(1).port(0)->loopback());
}

namespace {

// A firmware for the link tests: SSP 0xFF00, start at 0x100; IRQ 4 (0x70
// -> 0x300) and IRQ 6 (0x78 -> 0x380) acknowledge and return.
std::vector<uint16_t> link_firmware(const std::vector<uint16_t>& main) {
    std::vector<uint16_t> rom(0x200, 0x4E71);
    auto put32 = [&](uint32_t addr, uint32_t v) {
        rom[addr / 2] = static_cast<uint16_t>(v >> 16);
        rom[addr / 2 + 1] = static_cast<uint16_t>(v);
    };
    put32(0x00, 0x0000FF00);
    put32(0x04, 0x00000100);
    put32(0x70, 0x00000300);
    put32(0x74, 0x00000340);
    put32(0x78, 0x00000380);
    for (std::size_t i = 0; i < main.size(); ++i) rom[0x100 / 2 + i] = main[i];
    const uint16_t irq4[] = {0x13FC, 0x0000, 0x000C, 0x001D, 0x4E73};               // move.b #0,$c001d; rte
    const uint16_t irq5[] = {0x33FC, 0xFFFF, 0x0004, 0x0040, 0x4E73};               // move.w #$ffff,$40040; rte
    const uint16_t irq6[] = {0x13FC, 0x0000, 0x000C, 0x0017, 0x13FC, 0x0000, 0x000C, 0x0015, 0x4E73}; // disarm; rte
    std::copy(std::begin(irq4), std::end(irq4), rom.begin() + 0x300 / 2);
    std::copy(std::begin(irq5), std::end(irq5), rom.begin() + 0x340 / 2);
    std::copy(std::begin(irq6), std::end(irq6), rom.begin() + 0x380 / 2);
    return rom;
}

void start(CommBoard& board, const std::vector<uint16_t>& rom) {
    for (uint32_t i = 0; i < rom.size(); ++i) board.write32(0xC0020000 + 4 * i, brx(rom[i]));
    board.write32(0xC0010180, brx(1));
}

} // namespace

TEST_CASE("commboard: a write to control register 0xE0 raises IRQ 5") {
    const std::vector<uint16_t> main = {
        0x46FC, 0x2000,                         // move.w #$2000,sr
        0x33FC, 0x0000, 0x0004, 0x00E0,         // move.w #0,$400e0
        0x60FE,                                 // bra.s *
    };
    CommBoardOptions opts;
    opts.irq5_per_frame = 0;
    opts.irq5_hz = 1; // no timer tick in this test
    CommBoard board(nullptr, opts);
    start(board, link_firmware(main));
    board.sync(1'000'000);
    CHECK_EQ(board.irqs_taken[5], 1u);

    opts.irq5_on_e0 = false;
    CommBoard old(nullptr, opts);
    start(old, link_firmware(main));
    old.sync(1'000'000);
    CHECK_EQ(old.irqs_taken[5], 0u);
}

TEST_CASE("commboard: transmit complete waits for the frame being received") {
    // Send with the receiver armed, as the link firmware does, then wait.
    const std::vector<uint16_t> main = {
        0x46FC, 0x2700,                         // move.w #$2700,sr
        0x13FC, 0x0080, 0x000C, 0x001B,         // move.b #$80,$c001b   transmit
        0x13FC, 0x0080, 0x000C, 0x0015,         // move.b #$80,$c0015   receive enable
        0x13FC, 0x008C, 0x000C, 0x0017,         // move.b #$8c,$c0017   IRQ 6 when in
        0x13FC, 0x008C, 0x000C, 0x001D,         // move.b #$8c,$c001d   IRQ 4 when sent
        0x46FC, 0x2000,                         // move.w #$2000,sr
        0x60FE,                                 // bra.s *
    };
    for (const bool waits : {true, false}) {
        LinkRing ring(2);
        CommBoardOptions opts;
        opts.send_waits_for_receive = waits;
        CommBoard board(ring.port(0), opts);
        auto other = ring.port(1);
        start(board, link_firmware(main));
        board.sync(1'000'000);
        CHECK_EQ(board.packets_sent, 1u);
        CHECK_EQ(board.irqs_taken[4], waits ? 0u : 1u);
        const uint8_t frame[] = {0x68, 0x00};
        other->set_time(1'000'000);
        other->send(frame, sizeof frame);
        board.sync(2'000'000);
        CHECK_EQ(board.irqs_taken[6], 1u);
        CHECK_EQ(board.irqs_taken[4], 1u);
    }
}
