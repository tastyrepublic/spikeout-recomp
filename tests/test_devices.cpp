#include "test.hpp"

#include "runtime/bus.hpp"
#include "runtime/jtag.hpp"
#include "runtime/netboard.hpp"
#include "runtime/rom.hpp"

#include <vector>

using namespace m3;

namespace {

// One full TCK cycle; returns TDO as seen after the falling edge.
bool clock(Jtag& j, bool tms, bool tdi = false) {
    j.write(true, tms, tdi, true);
    j.write(false, tms, tdi, true);
    return j.tdo();
}

uint32_t bits_to_word(const std::vector<bool>& bits, size_t from) {
    uint32_t v = 0;
    for (size_t i = 0; i < 32; ++i) v |= uint32_t{bits[from + i]} << i;
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
        for (size_t i = 0; i < in.size(); ++i) clock(j, i + 1 == in.size(), in[i]); // last bit: Exit1-IR
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
    for (size_t i = 0; i < in.size(); ++i) clock(j, i + 1 == in.size(), in[i]);
    clock(j, true); // Update-DR
    CHECK_EQ(j.modeword(Jtag::Asic::Mercury), word);
}

TEST_CASE("netboard: a lone cabinet's handshake and its segment going round the ring") {
    NetBoard nb;
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
    CHECK(nb.state() == NetBoard::State::Testing);
    CHECK_EQ(brx(nb.read32(io + 0x000)) & 1, 1u); // "IRQ 2 acknowledged"

    nb.write32(ram + 0x800, brx(0));      // master
    nb.write32(ram + 0x804, brx(0x3C00)); // total size
    nb.write32(ram + 0x808, brx(0x0C00)); // segment size
    nb.run_frame();                       // testing: the ring is this cabinet alone
    CHECK(nb.state() == NetBoard::State::Ready);
    CHECK_EQ(nb.status1(), 0x2021u);      // one machine, index 0

    nb.write32(0xC0000100, 0xCAFEF00D);  // this cabinet's segment
    nb.run_frame();                      // the board takes the CPU's bank...
    nb.run_frame();                      // ...sends it round, and hands it back
    CHECK_EQ(nb.read32(0xC0000100 + 0x0C00), 0xCAFEF00Du);
    CHECK(nb.state() == NetBoard::State::Ready);
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
