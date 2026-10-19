#include "test.hpp"

#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/jtag.hpp"
#include "runtime/netboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/tilegen.hpp"

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
    EepromPins pins{bus};

    pins.send(0b1'01'000101'1011111011101111, 25); // WRITE 5, 0xBEEF while writes are disabled
    pins.deselect();
    CHECK_EQ(bus.eeprom().words()[5], 0xFFFFu);

    pins.send(0b1'00'110000, 9); // EWEN
    pins.deselect();
    pins.send(0b1'01'000101'1011111011101111, 25);
    pins.deselect();
    CHECK_EQ(bus.eeprom().words()[5], 0xBEEFu);
    pins.send(0, 1); // CS up again: busy (DO low), then ready
    for (int i = 0; i < Eeprom93C46::kBusyReads; ++i) CHECK(!pins.dout());
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

TEST_CASE("sound: MIDI port bytes, control and IRQ 0x40") {
    RomSet roms;
    Bus bus(roms);
    CHECK_EQ(bus.read8(0xF0080004), 0x83u);
    bus.irq().assert_irq(0x40);
    bus.write8(0xF0080004, 0x07); // periodic IRQs off: clears it
    CHECK_EQ(bus.irq().state() & 0x40, 0u);
    bus.write8(0xF0080004, 0x27);
    CHECK_EQ(bus.midi_control(), 0x27u);
    bus.irq().assert_irq(0x40);
    bus.write8(0xF0080000, 0xAE);
    CHECK_EQ(bus.irq().state() & 0x40, 0u);
    CHECK((bus.midi_out() == std::vector<uint8_t>{0xAE}));
    CHECK_EQ(bus.midi_bytes(), uint64_t{1});
}
