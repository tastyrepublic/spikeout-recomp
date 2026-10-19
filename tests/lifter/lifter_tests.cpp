// Runs the lifted test program (generated at build time from program.cpp)
// and checks the results against values worked out by hand.
#include "test.hpp"

#include "lifter/program.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/rom.hpp"

#include <bit>
#include <cmath>
#include <span>

namespace generated {
std::span<const recomp::FunctionEntry> lifter_test_functions();
}

namespace {

using lifter_test::kBuffer;

constexpr uint32_t kReturnAddr = 0x00ABCDE0; // LR given to the function under test

struct Machine {
    m3::RomSet roms; // none: the program runs from RAM
    m3::Bus bus{roms};
    recomp::Context c{bus, generated::lifter_test_functions()};
    ppc::CpuState& s = c.cpu;

    Machine() {
        // The code is in RAM, as on the board: jump tables and pointers are read from it.
        uint32_t addr = lifter_test::kBase;
        for (uint32_t w : lifter_test::program().words) {
            bus.write32(addr, w);
            addr += 4;
        }
    }

    // Call a function of the test program; returns where its blr went.
    uint32_t run(const char* name) {
        s.lr = kReturnAddr;
        c.returned_to = 0;
        c.call(lifter_test::program().labels.at(name));
        return c.returned_to;
    }
};

uint32_t addr_of(const char* name) { return lifter_test::program().labels.at(name); }

} // namespace

TEST_CASE("lifter: every entry was recompiled") {
    Machine m;
    for (uint32_t entry : lifter_test::program().entries) CHECK(m.c.find(entry) != nullptr);
}

TEST_CASE("lifter: integer arithmetic, overflow into SO, CR0") {
    Machine m;
    m.s.gpr[3] = 0x7FFFFFF0;
    m.s.gpr[4] = 0x00000020;
    CHECK_EQ(m.run("arith"), kReturnAddr);
    CHECK_EQ(m.s.gpr[5], 0x80000010u);  // add
    CHECK_EQ(m.s.gpr[6], 0x7FFFFFD0u);  // subf: r3 - r4
    CHECK_EQ(m.s.gpr[7], 0x80000010u);  // addc, no carry
    CHECK_EQ(m.s.gpr[8], 0x80000010u);  // adde
    CHECK_EQ(m.s.gpr[9], 0x80000010u);  // neg
    CHECK_EQ(m.s.gpr[10], 0xFFFFFE00u); // mullw
    CHECK_EQ(m.s.gpr[11], 0x0000000Fu); // mulhw
    CHECK_EQ(m.s.gpr[12], 0x0000000Fu); // mulhwu
    CHECK_EQ(m.s.gpr[13], 0x03FFFFFFu); // divw
    CHECK_EQ(m.s.gpr[14], 0x03FFFFFFu); // divwu
    CHECK_EQ(m.s.gpr[15], 0x80000010u); // addo.
    CHECK_EQ(m.s.gpr[16], 0xC0000000u); // XER: SO, OV
    CHECK_EQ(m.s.gpr[17], 0x90000000u); // CR0: LT, SO
}

TEST_CASE("lifter: carries and signed multiply/divide") {
    Machine m;
    m.s.gpr[3] = 0xFFFFFFFF;
    m.s.gpr[4] = 0x00000002;
    m.run("arith");
    CHECK_EQ(m.s.gpr[7], 0x00000001u);  // addc: carries out
    CHECK_EQ(m.s.gpr[8], 0x00000002u);  // adde: + CA
    CHECK_EQ(m.s.gpr[9], 0x00000001u);  // neg -1
    CHECK_EQ(m.s.gpr[10], 0xFFFFFFFEu); // mullw: -2
    CHECK_EQ(m.s.gpr[11], 0xFFFFFFFFu); // mulhw: high word of -2
    CHECK_EQ(m.s.gpr[12], 0x00000001u); // mulhwu
    CHECK_EQ(m.s.gpr[13], 0x00000000u); // divw: -1 / 2 rounds toward zero
    CHECK_EQ(m.s.gpr[14], 0x7FFFFFFFu); // divwu
    CHECK_EQ(m.s.gpr[16], 0x20000000u); // XER: CA from adde, no overflow
    CHECK_EQ(m.s.gpr[17], 0x40000000u); // CR0: GT
}

TEST_CASE("lifter: rotates, shifts and logical") {
    Machine m;
    m.s.gpr[3] = 0x80F01234;
    m.s.gpr[4] = 4;
    m.s.gpr[6] = 0x11112222;
    m.run("logic");
    CHECK_EQ(m.s.gpr[5], 0x00000080u);  // rlwinm: top byte
    CHECK_EQ(m.s.gpr[6], 0x12342222u);  // rlwimi: insert low half into high half
    CHECK_EQ(m.s.gpr[7], 0x0F012340u);  // slw
    CHECK_EQ(m.s.gpr[8], 0x080F0123u);  // srw
    CHECK_EQ(m.s.gpr[9], 0xF80F0123u);  // sraw
    CHECK_EQ(m.s.gpr[10], 0x20000000u); // CA: negative, 1 bits shifted out
    CHECK_EQ(m.s.gpr[11], 0xF80F0123u); // srawi
    CHECK_EQ(m.s.gpr[12], 0u);          // cntlzw
    CHECK_EQ(m.s.gpr[13], 0x00000034u); // extsb
    CHECK_EQ(m.s.gpr[14], 0x00001234u); // extsh
    CHECK_EQ(m.s.gpr[15], 0x00001200u); // andi.
    CHECK_EQ(m.s.gpr[16], 0x40000000u); // CR0: GT

    Machine big;
    big.s.gpr[3] = 0x80F01234;
    big.s.gpr[4] = 40; // shift amounts of 32-63
    big.run("logic");
    CHECK_EQ(big.s.gpr[7], 0u);
    CHECK_EQ(big.s.gpr[8], 0u);
    CHECK_EQ(big.s.gpr[9], 0xFFFFFFFFu);
    CHECK_EQ(big.s.gpr[10], 0x20000000u);
}

TEST_CASE("lifter: loads, stores, update forms, byte reversal, lmw/stmw, dcbz") {
    Machine m;
    m.s.gpr[3] = kBuffer;
    m.s.gpr[29] = 0xA;
    m.s.gpr[30] = 0xB;
    m.s.gpr[31] = 0xC;
    m.run("memory");
    CHECK_EQ(m.s.gpr[6], 0xFFFFFFFFu);  // lha sign-extends
    CHECK_EQ(m.s.gpr[7], 0x0000FFFEu);  // lhz
    CHECK_EQ(m.s.gpr[8], 0x000000FEu);  // lbz
    CHECK_EQ(m.bus.read16(kBuffer + 4), 0x1234u);
    CHECK_EQ(m.bus.read8(kBuffer + 6), 0x34u);
    CHECK_EQ(m.s.gpr[3], kBuffer + 8);  // stwu updated r3
    CHECK_EQ(m.s.gpr[10], 0xFEFFFFFFu); // lwbrx
    CHECK_EQ(m.s.gpr[11], kBuffer + 8);
    CHECK_EQ(m.s.gpr[12], 0x34120000u); // stwbrx then lwz
    CHECK_EQ(m.s.gpr[28], 0xAu);        // stmw r29 / lmw r28
    CHECK_EQ(m.s.gpr[29], 0xBu);
    CHECK_EQ(m.s.gpr[30], 0xCu);
    CHECK_EQ(m.s.gpr[31], 0u);
    CHECK_EQ(m.s.gpr[21], 0u);          // dcbz cleared the whole 32-byte line
}

TEST_CASE("lifter: bdnz loop") {
    Machine m;
    m.s.gpr[3] = 5;
    m.run("sumloop");
    CHECK_EQ(m.s.gpr[4], 15u);
    CHECK_EQ(m.s.ctr, 0u);
}

TEST_CASE("lifter: calls, tail calls and conditional calls") {
    Machine m;
    CHECK_EQ(m.run("caller"), kReturnAddr);
    CHECK_EQ(m.s.gpr[3], 15u);

    CHECK_EQ(m.run("tailcall"), kReturnAddr); // double's blr returns to our caller
    CHECK_EQ(m.s.gpr[3], 10u);

    m.s.gpr[3] = 0;
    CHECK_EQ(m.run("condcall"), kReturnAddr);
    CHECK_EQ(m.s.gpr[4], 1u);
    m.s.gpr[3] = 1;
    m.run("condcall");
    CHECK_EQ(m.s.gpr[4], 0u);
}

TEST_CASE("lifter: jump table") {
    for (uint32_t i = 0; i < 4; ++i) {
        Machine m;
        m.s.gpr[3] = i;
        CHECK_EQ(m.run("switch"), kReturnAddr);
        CHECK_EQ(m.s.gpr[6], i < 3 ? 100 + i : 0xFFFFFFFFu);
    }
}

TEST_CASE("lifter: call through a function pointer in memory") {
    Machine m;
    CHECK_EQ(m.run("indirect"), kReturnAddr);
    CHECK_EQ(m.s.gpr[3], 42u);
}

TEST_CASE("lifter: compares, CR logic, mcrf, mtcrf") {
    Machine m;
    m.s.gpr[3] = 0xFFFFFFFF;
    m.s.gpr[4] = 1;
    m.run("crtest");
    CHECK_EQ(m.s.gpr[5], 0x88480000u); // cr0 = lt from cror, cr1 lt (signed), cr2 gt (unsigned), cr3 = cr1
    CHECK_EQ(m.s.gpr[7], 0x28480000u); // mtcrf replaced cr0 only
}

TEST_CASE("lifter: time base moves on every read") {
    Machine m;
    m.run("timebase");
    CHECK_EQ(m.s.gpr[5], m.c.tb_step);
}

TEST_CASE("lifter: lwarx / stwcx.") {
    Machine m;
    m.bus.write32(kBuffer, 41);
    m.s.gpr[3] = kBuffer;
    m.run("atomic");
    CHECK_EQ(m.bus.read32(kBuffer), 42u);
    CHECK_EQ(m.s.gpr[6], 0x20000000u); // CR0 EQ: the store went through
}

TEST_CASE("lifter: floating point") {
    Machine m;
    auto put64 = [&](uint32_t addr, uint64_t v) { m.bus.write64(addr, v); };
    m.bus.write32(kBuffer + 0, std::bit_cast<uint32_t>(1.5f));
    m.bus.write32(kBuffer + 4, std::bit_cast<uint32_t>(2.25f));
    put64(kBuffer + 8, std::bit_cast<uint64_t>(1e300));
    m.bus.write32(kBuffer + 16, std::bit_cast<uint32_t>(-2.7f));
    put64(kBuffer + 24, 0x3FF0000018000000u); // 1 + 0.75 * 2^-23: not a single
    m.bus.write32(kBuffer + 56, 0x7F800001u);  // signaling NaN
    m.s.gpr[3] = kBuffer;
    m.s.gpr[21] = kBuffer + 64;
    CHECK_EQ(m.run("fp"), kReturnAddr);

    auto f = [&](int n) { return std::bit_cast<double>(m.s.fpr[n]); };
    CHECK_EQ(f(3), 3.75);   // fadds
    CHECK_EQ(f(4), 3.375);  // fmuls
    CHECK_EQ(f(5), 1.5);    // fdivs
    CHECK_EQ(f(6), -0.75);  // fsubs
    CHECK_EQ(f(7), 4.875);  // fmadds
    CHECK_EQ(f(8), -1.5);   // fneg
    CHECK_EQ(f(9), 1.5);    // fabs
    CHECK_EQ(f(10), 2.25);  // fsel: f6 < 0 picks frB
    CHECK(std::isinf(f(12))); // fmul overflow
    CHECK(std::isinf(f(13))); // frsp: too large for a single
    CHECK_EQ(static_cast<uint32_t>(m.s.fpr[14]), 0xFFFFFFFEu); // fctiwz -2.7 = -2
    CHECK_EQ(m.bus.read32(kBuffer + 64), 0xFFFFFFFEu);        // stfiwx
    CHECK_EQ(static_cast<uint32_t>(m.s.fpr[16]), 0xFFFFFFFDu); // fctiw, nearest: -3
    CHECK_EQ(static_cast<uint32_t>(m.s.fpr[17]), 0xFFFFFFFEu); // fctiw, RN toward zero: -2
    CHECK_EQ(m.s.fpscr & 3, 0u);                              // RN restored
    CHECK_EQ(m.bus.read32(kBuffer + 32), 0x3F800000u);        // stfs truncates, doesn't round
    CHECK_EQ(m.s.fpr[19], std::bit_cast<uint64_t>(static_cast<double>(std::bit_cast<float>(0x40000001u)))); // fadds rounds
    CHECK_EQ(m.bus.read32(kBuffer + 40), std::bit_cast<uint32_t>(3.75f));
    CHECK_EQ(m.bus.read64(kBuffer + 48), 0x7FF0000000000000u); // stfd inf
    CHECK_EQ(m.s.fpr[20], 0x7FF0000020000000u);               // lfs keeps the NaN signaling
    CHECK_EQ(m.bus.read32(kBuffer + 60), 0x7F800001u);        // and stfs gives it back unchanged
    CHECK_EQ(m.s.gpr[5] & 0x0F000000u, 0x08000000u);          // fcmpu cr1: 1.5 < 2.25
    CHECK_EQ(m.s.fpscr & 0xF000u, 0x8000u);                   // FPCC: FL
}

TEST_CASE("lifter: sc enters the 0xC00 handler and its rfi returns after the sc") {
    Machine m;
    m.s.msr = 0x9032; // EE, ME, IR, DR, RI; IP clear: vectors at 0
    CHECK_EQ(m.run("syscall"), kReturnAddr);
    CHECK_EQ(m.s.gpr[3], 1006u);                      // handler added 1000, then +1 after the sc
    CHECK_EQ(m.s.gpr[4], addr_of("syscall.after"));   // SRR0
    CHECK_EQ(m.s.gpr[5], 0x9032u);                    // SRR1 = old MSR
    CHECK_EQ(m.s.gpr[6], 0x1000u);                    // MSR in the handler: only ME left
    CHECK_EQ(m.s.msr, 0x9032u);                       // restored by rfi
    CHECK_EQ(m.c.exception_depth(), size_t{0});
}

TEST_CASE("lifter: trap enters 0x700, handler steps past it, rfi from a called function") {
    Machine m;
    m.s.msr = 0x1000;
    m.s.gpr[3] = 7;
    CHECK_EQ(m.run("trapeq"), kReturnAddr);
    CHECK_EQ(m.s.gpr[7], 0x77u);                           // the handler ran
    CHECK_EQ(m.s.gpr[4], addr_of("trapeq.tw") + 4);        // SRR0 was the trap
    CHECK_EQ(m.s.gpr[8] & recomp::Context::kSrr1Trap, recomp::Context::kSrr1Trap);
    CHECK_EQ(m.c.exception_depth(), size_t{0});
}

TEST_CASE("interrupts: a pending IRQ is taken at the first block, then acknowledged") {
    Machine m;
    m.s.msr = 0x9000; // EE, ME
    m.bus.irq().set_enable(0x02);
    m.bus.irq().assert_irq(0x02);
    m.c.limit_blocks(1000);
    CHECK_EQ(m.run("irqwait"), kReturnAddr);
    CHECK_EQ(m.bus.read32(kBuffer + 0x100), 1u);
    CHECK_EQ(m.bus.read8(kBuffer + 0x104), 0x02u); // the handler saw IRQ 0x02 pending
    CHECK_EQ(m.bus.irq().state(), 0u);             // and acknowledged it
    CHECK_EQ(m.bus.read32(kBuffer + 0x10C), addr_of("irqwait")); // taken before the first block ran
    CHECK_EQ(m.c.external_interrupts, 1u);
}

TEST_CASE("interrupts: a pending IRQ waits for MSR[EE], then is taken at the next block") {
    Machine m;
    m.s.msr = 0x1000; // EE off
    m.bus.irq().set_enable(0x0D);
    m.bus.irq().assert_irq(0x0D);
    m.c.limit_blocks(1000);
    CHECK_EQ(m.run("irqlater"), kReturnAddr);
    CHECK_EQ(m.bus.read32(kBuffer + 0x100), 1u);
    CHECK_EQ(m.bus.read32(kBuffer + 0x10C), addr_of("irqlater.loop")); // the first block after mtmsr
}

TEST_CASE("interrupts: decrementer") {
    Machine m;
    m.s.msr = 0x1000;
    m.c.limit_blocks(100000);
    CHECK_EQ(m.run("decwait"), kReturnAddr);
    CHECK_EQ(m.bus.read32(kBuffer + 0x108), 1u);
    CHECK_EQ(m.c.decrementer_interrupts, 1u);
    // DEC was 100: it fires after 101 time base ticks, about 1000 CPU cycles.
    CHECK(m.c.cycles() * 33 / 332 >= 101);
}

TEST_CASE("interrupts: frame timing raises IRQ 0x02 once a frame") {
    Machine m;
    recomp::FrameTiming timing;
    m.c.set_scheduler(&timing);
    m.s.msr = 0x1000;
    m.c.limit_blocks(10'000'000);
    CHECK_EQ(m.run("frames"), kReturnAddr);
    CHECK_EQ(m.bus.read32(kBuffer + 0x100), 3u);
    CHECK_EQ(timing.frames(), 2u); // the third IRQ 0x02 comes near the end of the third frame
    CHECK_EQ(m.bus.irq().enable(), 0x02u);
    const uint64_t irq_cycle = 2 * timing.frame_cycles() + (40 + 383) * timing.line_cycles();
    CHECK(m.c.cycles() >= irq_cycle && m.c.cycles() < irq_cycle + timing.line_cycles());
}

TEST_CASE("lifter: a blr that lands somewhere else is followed back") {
    Machine m;
    m.s.gpr[3] = 0; // through the handler, whose blr goes to lrtrick's epilogue
    CHECK_EQ(m.run("lrouter"), kReturnAddr);
    CHECK_EQ(m.s.gpr[4], 100u);
    m.s.gpr[3] = 1; // straight to the epilogue
    CHECK_EQ(m.run("lrouter"), kReturnAddr);
    CHECK_EQ(m.s.gpr[4], 1u);
}

TEST_CASE("lifter: runs stop on loops, missing code and bad returns") {
    {
        Machine m;
        m.c.limit_blocks(100);
        bool stopped = false;
        try {
            m.run("spin");
        } catch (const recomp::OutOfBudget& e) {
            stopped = e.pc == addr_of("spin");
        }
        CHECK(stopped);
    }
    {
        Machine m;
        uint32_t missing = 0;
        try {
            m.run("missing");
        } catch (const recomp::MissingCode& e) {
            missing = e.addr;
        }
        CHECK_EQ(missing, 0x20000u);
    }
    {
        Machine m;
        CHECK_EQ(m.run("badret"), addr_of("double")); // what a caller's check would catch
    }
}
