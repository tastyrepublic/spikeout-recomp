#include "test.hpp"

#include "ppc/cpu_state.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"

#include <string>

using ppc::decode;
using ppc::Op;

namespace {

std::string dis(uint32_t raw, uint32_t pc = 0x1000) { return ppc::disassemble(decode(raw), pc); }

} // namespace

TEST_CASE("decode: primary opcodes") {
    CHECK(decode(0x38600000).op == Op::Addi);   // li r3, 0
    CHECK(decode(0x3C60FFF0).op == Op::Addis);  // lis r3, 0xfff0
    CHECK(decode(0x9421FFF0).op == Op::Stwu);   // stwu r1, -16(r1)
    CHECK(decode(0x80010014).op == Op::Lwz);    // lwz r0, 20(r1)
    CHECK(decode(0x2C030000).op == Op::Cmpi);   // cmpwi r3, 0
    CHECK(decode(0x60000000).op == Op::Ori);    // nop
    CHECK(decode(0x5483103A).op == Op::Rlwinm); // slwi r3, r4, 2
    CHECK(decode(0x48000101).op == Op::B);      // bl
    CHECK(decode(0x4182000C).op == Op::Bc);     // beq
    CHECK(decode(0x44000002).op == Op::Sc);
}

TEST_CASE("decode: extended opcodes") {
    CHECK(decode(0x7C0802A6).op == Op::Mfspr);  // mflr r0
    CHECK(decode(0x7C0803A6).op == Op::Mtspr);  // mtlr r0
    CHECK(decode(0x4E800020).op == Op::Bclr);   // blr
    CHECK(decode(0x4E800420).op == Op::Bcctr);  // bctr
    CHECK(decode(0x4C000064).op == Op::Rfi);
    CHECK(decode(0x4C00012C).op == Op::Isync);
    CHECK(decode(0x7C000124).op == Op::Mtmsr);
    CHECK(decode(0x7C7F1B78).op == Op::Or);     // mr r31, r3
    CHECK(decode(0x7C642A14).op == Op::Add);
    CHECK(decode(0x7C642E15).op == Op::Add);    // addo. (OE=1, Rc=1)
    CHECK(decode(0x7C001FE4).op == Op::Tlbli);  // 603-specific
    CHECK(decode(0x7C0004AC).op == Op::Sync);
    CHECK(decode(0x7C001FEC).op == Op::Dcbz);
    CHECK(decode(0xFC22182A).op == Op::Fadd);
    CHECK(decode(0xEC2220FA).op == Op::Fmadds);
    CHECK(decode(0xFC200890).op == Op::Fmr);    // fmr f1, f1
    CHECK(decode(0xFC00048E).op == Op::Mffs);
}

TEST_CASE("decode: invalid encodings") {
    CHECK(!decode(0x00000000).valid());
    CHECK(!decode(0x04000000).valid()); // primary opcode 1 is unused
    CHECK(decode(0xFFFFFFFF).op == Op::Fnmadd); // all-ones is a real FP instruction
    CHECK(!decode(0x44000000).valid()); // sc requires bit 30 set
    CHECK(!decode(0x4C000002).valid()); // opcode 19, unused XO
}

TEST_CASE("decode: fields") {
    const auto stwu = decode(0x9421FFF0);
    CHECK_EQ(stwu.rs(), 1u);
    CHECK_EQ(stwu.ra(), 1u);
    CHECK_EQ(stwu.simm(), -16);

    CHECK_EQ(decode(0x7C0802A6).spr(), 8u);   // lr
    CHECK_EQ(decode(0x7C1A02A6).spr(), 26u);  // srr0
    CHECK_EQ(decode(0x7C10FBA6).spr(), 1008u); // hid0

    const auto addo = decode(0x7C642E15);
    CHECK(addo.oe());
    CHECK(addo.record());
}

TEST_CASE("branch targets") {
    CHECK_EQ(*ppc::branch_target(decode(0x48000101), 0x1000), 0x1100u);       // bl +0x100
    CHECK_EQ(*ppc::branch_target(decode(0x4BFFFFFC), 0x1000), 0x0FFCu);       // b -4
    CHECK_EQ(*ppc::branch_target(decode(0x4BFFFF02), 0x1000), 0xFFFFFF00u);   // ba -0x100 (absolute)
    CHECK_EQ(*ppc::branch_target(decode(0x4182000C), 0x2000), 0x200Cu);       // beq +12
    CHECK_EQ(*ppc::branch_target(decode(0x4082FFF8), 0x2000), 0x1FF8u);       // bne -8
    CHECK(!ppc::branch_target(decode(0x4E800020), 0x2000).has_value());       // blr
}

TEST_CASE("disassembly: simplified mnemonics") {
    CHECK_EQ(dis(0x38600000), std::string("li      r3, 0"));
    CHECK_EQ(dis(0x3C60FFF0), std::string("lis     r3, 0xfff0"));
    CHECK_EQ(dis(0x60000000), std::string("nop"));
    CHECK_EQ(dis(0x7C7F1B78), std::string("mr      r31, r3"));
    CHECK_EQ(dis(0x7C0802A6), std::string("mflr    r0"));
    CHECK_EQ(dis(0x7C0803A6), std::string("mtlr    r0"));
    CHECK_EQ(dis(0x4E800020), std::string("blr"));
    CHECK_EQ(dis(0x4E800420), std::string("bctr"));
    CHECK_EQ(dis(0x4E800421), std::string("bctrl"));
    CHECK_EQ(dis(0x4D820020), std::string("beqlr"));
    CHECK_EQ(dis(0x2C030000), std::string("cmpwi   r3, 0"));
    CHECK_EQ(dis(0x2B83000A), std::string("cmplwi  cr7, r3, 0xa"));
    CHECK_EQ(dis(0x5483103A), std::string("slwi    r3, r4, 2"));
    CHECK_EQ(dis(0x5483F0BE), std::string("srwi    r3, r4, 2"));
    CHECK_EQ(dis(0x7C6318F8), std::string("not     r3, r3"));
}

TEST_CASE("disassembly: branches") {
    CHECK_EQ(dis(0x48000101, 0x1000), std::string("bl      0x00001100"));
    CHECK_EQ(dis(0x4182000C, 0x2000), std::string("beq     0x0000200c"));
    CHECK_EQ(dis(0x4186000C, 0x2000), std::string("beq     cr1, 0x0000200c"));
    CHECK_EQ(dis(0x4082FFF8, 0x2000), std::string("bne     0x00001ff8"));
    CHECK_EQ(dis(0x4200FFF0, 0x2000), std::string("bdnz    0x00001ff0"));
    CHECK_EQ(dis(0x4BFFFF02, 0x1000), std::string("ba      0xffffff00"));
}

TEST_CASE("disassembly: general forms") {
    CHECK_EQ(dis(0x9421FFF0), std::string("stwu    r1, -16(r1)"));
    CHECK_EQ(dis(0x80010014), std::string("lwz     r0, 20(r1)"));
    CHECK_EQ(dis(0x7C642A14), std::string("add     r3, r4, r5"));
    CHECK_EQ(dis(0x7C642E15), std::string("addo.   r3, r4, r5"));
    CHECK_EQ(dis(0x7C1A02A6), std::string("mfspr   r0, srr0"));
    CHECK_EQ(dis(0x7C10FBA6), std::string("mtspr   hid0, r0"));
    CHECK_EQ(dis(0xFC22182A), std::string("fadd    f1, f2, f3"));
    CHECK_EQ(dis(0xEC2220FA), std::string("fmadds  f1, f2, f3, f4"));
    CHECK_EQ(dis(0xC0230008), std::string("lfs     f1, 8(r3)"));
    CHECK_EQ(dis(0x7C001FE4), std::string("tlbli   r3"));
    CHECK_EQ(dis(0x4C000064), std::string("rfi"));
    CHECK_EQ(dis(0x00000000), std::string(".long   0x00000000"));
}

TEST_CASE("decode: every opcode has a mnemonic and disassembles") {
    // Walk every primary opcode with every 10-bit extended opcode and both
    // Rc/OE settings; nothing may crash and every valid decode must have a name.
    for (uint32_t opcd = 0; opcd < 64; ++opcd) {
        for (uint32_t xo = 0; xo < 1024; ++xo) {
            const uint32_t raw = (opcd << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (xo << 1) | (xo & 1);
            const auto insn = decode(raw);
            CHECK(ppc::mnemonic(insn.op) != nullptr);
            CHECK(!ppc::disassemble(insn, 0x1000).empty());
        }
    }
}

TEST_CASE("cpu state: CR helpers and reset") {
    ppc::CpuState s;
    s.set_crf(0, 0x8);
    s.set_crf(7, 0x2);
    CHECK_EQ(s.cr, 0x80000002u);
    CHECK_EQ(s.crf(0), 0x8u);
    CHECK(s.crbit(0));
    CHECK(s.crbit(30));
    s.set_crbit(0, false);
    CHECK_EQ(s.cr, 0x00000002u);

    s.reset();
    CHECK_EQ(s.pc, 0xFFF00100u);
    CHECK(s.msr & ppc::CpuState::kMsrIP);
}
