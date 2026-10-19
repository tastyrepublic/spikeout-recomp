#include "lifter/program.hpp"

#include <stdexcept>

namespace lifter_test {

namespace {

// Encoders (field layout as in the PowerPC manuals).
constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, uint32_t imm) {
    return op << 26 | rt << 21 | ra << 16 | (imm & 0xFFFF);
}
constexpr uint32_t XO(uint32_t xo, uint32_t rt, uint32_t ra, uint32_t rb, uint32_t oe = 0, uint32_t rc = 0) {
    return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | oe << 10 | xo << 1 | rc;
}
constexpr uint32_t X(uint32_t xo, uint32_t rt, uint32_t ra, uint32_t rb, uint32_t rc = 0) {
    return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc;
}
constexpr uint32_t M(uint32_t op, uint32_t rs, uint32_t ra, uint32_t sh, uint32_t mb, uint32_t me, uint32_t rc = 0) {
    return op << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1 | rc;
}
constexpr uint32_t XL(uint32_t xo, uint32_t bt, uint32_t ba, uint32_t bb, uint32_t lk = 0) {
    return 19u << 26 | bt << 21 | ba << 16 | bb << 11 | xo << 1 | lk;
}
constexpr uint32_t spr_field(uint32_t spr) { return ((spr & 31) << 5 | spr >> 5) << 11; }
constexpr uint32_t mfspr(uint32_t rt, uint32_t spr) { return 31u << 26 | rt << 21 | spr_field(spr) | 339u << 1; }
constexpr uint32_t mtspr(uint32_t spr, uint32_t rs) { return 31u << 26 | rs << 21 | spr_field(spr) | 467u << 1; }
constexpr uint32_t mftb(uint32_t rt, uint32_t tbr) { return 31u << 26 | rt << 21 | spr_field(tbr) | 371u << 1; }

constexpr uint32_t li(uint32_t r, uint32_t imm) { return D(14, r, 0, imm); }
constexpr uint32_t lis(uint32_t r, uint32_t imm) { return D(15, r, 0, imm); }
constexpr uint32_t addi(uint32_t rt, uint32_t ra, uint32_t imm) { return D(14, rt, ra, imm); }
constexpr uint32_t mr(uint32_t ra, uint32_t rs) { return X(444, rs, ra, rs); }
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kBctr = 0x4E800420;
constexpr uint32_t kBctrl = 0x4E800421;
constexpr uint32_t mflr(uint32_t r) { return mfspr(r, 8); }
constexpr uint32_t mtlr(uint32_t r) { return mtspr(8, r); }
constexpr uint32_t mtctr(uint32_t r) { return mtspr(9, r); }

class Assembler {
public:
    uint32_t here() const { return kBase + 4 * static_cast<uint32_t>(p_.words.size()); }
    void emit(uint32_t w) { p_.words.push_back(w); }
    void org(uint32_t addr) {
        while (here() < addr) emit(0);
    }
    void label(const std::string& name) { p_.labels[name] = here(); }
    void entry(const std::string& name) {
        label(name);
        p_.entries.push_back(here());
    }

    // Branches and data that refer to labels, patched by finish().
    void b(const std::string& target) { fixup(target, Kind::B, 18u << 26); }
    void bl(const std::string& target) { fixup(target, Kind::B, 18u << 26 | 1); }
    void bc(uint32_t bo, uint32_t bi, const std::string& target, uint32_t lk = 0) {
        fixup(target, Kind::Bc, 16u << 26 | bo << 21 | bi << 16 | lk);
    }
    void word(const std::string& target) { fixup(target, Kind::Word, 0); }
    // lis + addi of a label's address ("la")
    void la(uint32_t r, const std::string& target) {
        fixup(target, Kind::Hi, lis(r, 0));
        fixup(target, Kind::Lo, addi(r, r, 0));
    }

    Program finish() {
        for (const Fixup& f : fixups_) {
            const auto it = p_.labels.find(f.target);
            if (it == p_.labels.end()) throw std::logic_error("undefined label " + f.target);
            const uint32_t target = it->second, at = kBase + 4 * f.index;
            uint32_t& w = p_.words[f.index];
            switch (f.kind) {
            case Kind::B:    w |= (target - at) & 0x03FFFFFC; break;
            case Kind::Bc:   w |= (target - at) & 0xFFFC; break;
            case Kind::Word: w = target; break;
            case Kind::Hi:   w |= ((target + 0x8000) >> 16) & 0xFFFF; break;
            case Kind::Lo:   w |= target & 0xFFFF; break;
            }
        }
        return p_;
    }

private:
    enum class Kind { B, Bc, Word, Hi, Lo };
    struct Fixup {
        uint32_t index;
        std::string target;
        Kind kind;
    };
    void fixup(const std::string& target, Kind kind, uint32_t w) {
        fixups_.push_back({static_cast<uint32_t>(p_.words.size()), target, kind});
        emit(w);
    }
    Program p_;
    std::vector<Fixup> fixups_;
};

Program build() {
    Assembler a;

    // Exception vectors (MSR[IP] = 0 in the tests that use them). Counters
    // live at kBuffer + 0x100 (external interrupts) and + 0x108 (decrementer).
    // 0x500 external interrupt: acknowledge everything pending, count it.
    a.entry("ext_vector");
    a.emit(lis(10, 0xFE10));
    a.emit(D(34, 11, 10, 0x18));     // lbz    r11, 0x18(r10)   pending
    a.emit(D(38, 11, 10, 0x18));     // stb    r11, 0x18(r10)   acknowledge
    a.emit(lis(12, kBuffer >> 16));
    a.emit(D(32, 13, 12, 0x100));    // lwz    r13, 0x100(r12)
    a.emit(addi(13, 13, 1));
    a.emit(D(36, 13, 12, 0x100));    // stw    r13, 0x100(r12)
    a.emit(D(38, 11, 12, 0x104));    // stb    r11, 0x104(r12)  what was pending
    a.emit(mfspr(13, 26));
    a.emit(D(36, 13, 12, 0x10C));    // stw    SRR0, 0x10C(r12) where it was taken
    a.emit(0x4C000064);              // rfi

    // 0x700 program exception: step SRR0 past the trap, note it, and return
    // through rfi from a called function (the rfi can be calls deep).
    a.org(0x700);
    a.entry("trap_vector");
    a.emit(mflr(9));
    a.emit(mfspr(4, 26));            // mfsrr0 r4
    a.emit(addi(4, 4, 4));
    a.emit(mtspr(26, 4));            // mtsrr0 r4
    a.emit(li(7, 0x77));
    a.emit(mfspr(8, 27));            // mfsrr1 r8
    a.bl("trap_exit");
    a.emit(kBlr);                    // never reached
    a.entry("trap_exit");
    a.emit(mtlr(9));
    a.emit(0x4C000064);              // rfi

    // 0x900 decrementer: count it, push the next one far away
    a.org(0x900);
    a.entry("dec_vector");
    a.emit(lis(12, kBuffer >> 16));
    a.emit(D(32, 13, 12, 0x108));    // lwz    r13, 0x108(r12)
    a.emit(addi(13, 13, 1));
    a.emit(D(36, 13, 12, 0x108));    // stw    r13, 0x108(r12)
    a.emit(lis(13, 0x7FFF));
    a.emit(mtspr(22, 13));           // mtdec  r13
    a.emit(0x4C000064);              // rfi

    // 0xC00 system call: r3 += 1000, report SRR0/SRR1/MSR, return
    a.org(0xC00);
    a.entry("sc_vector");
    a.emit(addi(3, 3, 1000));
    a.emit(mfspr(4, 26));            // mfsrr0 r4
    a.emit(mfspr(5, 27));            // mfsrr1 r5
    a.emit(X(83, 6, 0, 0));          // mfmsr  r6
    a.emit(0x4C000064);              // rfi

    a.org(0x1000);
    // arith(r3 = a, r4 = b)
    a.entry("arith");
    a.emit(XO(266, 5, 3, 4));       // add    r5, r3, r4
    a.emit(XO(40, 6, 4, 3));        // subf   r6, r4, r3   (r3 - r4)
    a.emit(XO(10, 7, 3, 4));        // addc   r7, r3, r4
    a.emit(XO(138, 8, 3, 4));       // adde   r8, r3, r4
    a.emit(XO(104, 9, 3, 0));       // neg    r9, r3
    a.emit(XO(235, 10, 3, 4));      // mullw  r10, r3, r4
    a.emit(XO(75, 11, 3, 4));       // mulhw  r11, r3, r4
    a.emit(XO(11, 12, 3, 4));       // mulhwu r12, r3, r4
    a.emit(XO(491, 13, 3, 4));      // divw   r13, r3, r4
    a.emit(XO(459, 14, 3, 4));      // divwu  r14, r3, r4
    a.emit(XO(266, 15, 3, 4, 1, 1)); // addo.  r15, r3, r4
    a.emit(mfspr(16, 1));           // mfxer  r16
    a.emit(X(19, 17, 0, 0));        // mfcr   r17
    a.emit(kBlr);

    // logic(r3 = x, r4 = n, r6 = preset)
    a.entry("logic");
    a.emit(M(21, 3, 5, 8, 24, 31));  // rlwinm r5, r3, 8, 24, 31
    a.emit(M(20, 3, 6, 16, 0, 15));  // rlwimi r6, r3, 16, 0, 15
    a.emit(X(24, 3, 7, 4));          // slw    r7, r3, r4
    a.emit(X(536, 3, 8, 4));         // srw    r8, r3, r4
    a.emit(X(792, 3, 9, 4));         // sraw   r9, r3, r4
    a.emit(mfspr(10, 1));            // mfxer  r10
    a.emit(X(824, 3, 11, 4));        // srawi  r11, r3, 4
    a.emit(X(26, 3, 12, 0));         // cntlzw r12, r3
    a.emit(X(954, 3, 13, 0));        // extsb  r13, r3
    a.emit(X(922, 3, 14, 0));        // extsh  r14, r3
    a.emit(D(28, 3, 15, 0xFF00));    // andi.  r15, r3, 0xff00
    a.emit(X(19, 16, 0, 0));         // mfcr   r16
    a.emit(kBlr);

    // memory(r3 = buffer, r29-r31 preset)
    a.entry("memory");
    a.emit(li(5, 0xFFFE));           // li     r5, -2
    a.emit(D(36, 5, 3, 0));          // stw    r5, 0(r3)
    a.emit(D(42, 6, 3, 0));          // lha    r6, 0(r3)
    a.emit(D(40, 7, 3, 2));          // lhz    r7, 2(r3)
    a.emit(D(34, 8, 3, 3));          // lbz    r8, 3(r3)
    a.emit(li(9, 0x1234));           // li     r9, 0x1234
    a.emit(D(44, 9, 3, 4));          // sth    r9, 4(r3)
    a.emit(D(38, 9, 3, 6));          // stb    r9, 6(r3)
    a.emit(D(37, 5, 3, 8));          // stwu   r5, 8(r3)
    a.emit(X(534, 10, 0, 3));        // lwbrx  r10, 0, r3
    a.emit(mr(11, 3));               // mr     r11, r3
    a.emit(X(662, 9, 0, 3));         // stwbrx r9, 0, r3
    a.emit(D(32, 12, 3, 0));         // lwz    r12, 0(r3)
    a.emit(D(47, 29, 3, 16));        // stmw   r29, 16(r3)
    a.emit(D(46, 28, 3, 16));        // lmw    r28, 16(r3)
    a.emit(addi(20, 3, 62));         // addi   r20, r3, 62
    a.emit(D(36, 5, 3, 56));         // stw    r5, 56(r3)
    a.emit(X(1014, 0, 0, 20));       // dcbz   0, r20
    a.emit(D(32, 21, 3, 56));        // lwz    r21, 56(r3)
    a.emit(kBlr);

    // sumloop(r3 = n): r4 = n + (n-1) + ... + 1
    a.entry("sumloop");
    a.emit(mtctr(3));
    a.emit(li(4, 0));
    a.label("sumloop.top");
    a.emit(mfspr(5, 9));             // mfctr  r5
    a.emit(XO(266, 4, 4, 5));        // add    r4, r4, r5
    a.bc(16, 0, "sumloop.top");      // bdnz
    a.emit(kBlr);

    // double(r3): r3 += r3
    a.entry("double");
    a.emit(XO(266, 3, 3, 3));
    a.emit(kBlr);

    // inc: r4 += 1
    a.entry("inc");
    a.emit(addi(4, 4, 1));
    a.emit(kBlr);

    // caller: r3 = double(7) + 1
    a.entry("caller");
    a.emit(mflr(31));
    a.emit(li(3, 7));
    a.bl("double");
    a.label("caller.after"); // a return site
    a.emit(addi(3, 3, 1));
    a.emit(mtlr(31));
    a.emit(kBlr);

    // tailcall: r3 = double(5), via b
    a.entry("tailcall");
    a.emit(li(3, 5));
    a.b("double");

    // condcall(r3): r4 = (r3 == 0) ? 1 : 0, with beql
    a.entry("condcall");
    a.emit(mflr(31));
    a.emit(li(4, 0));
    a.emit(D(11, 0, 3, 0));          // cmpwi  r3, 0
    a.bc(12, 2, "inc", 1);           // beql   inc
    a.emit(mtlr(31));
    a.emit(kBlr);

    // switch(r3): r6 = 100 + r3 for 0..2, else -1 (jump table)
    a.entry("switch");
    a.emit(D(10, 0, 3, 2));          // cmplwi r3, 2
    a.bc(12, 1, "switch.default");   // bgt
    a.la(4, "switch.table");
    a.emit(M(21, 3, 3, 2, 0, 29));   // slwi   r3, r3, 2
    a.emit(X(23, 5, 4, 3));          // lwzx   r5, r4, r3
    a.emit(mtctr(5));
    a.emit(kBctr);
    a.label("switch.case0");
    a.emit(li(6, 100));
    a.emit(kBlr);
    a.label("switch.case1");
    a.emit(li(6, 101));
    a.emit(kBlr);
    a.label("switch.case2");
    a.emit(li(6, 102));
    a.emit(kBlr);
    a.label("switch.default");
    a.emit(li(6, 0xFFFF));
    a.emit(kBlr);
    a.label("switch.table");
    a.word("switch.case0");
    a.word("switch.case1");
    a.word("switch.case2");

    // indirect: r3 = double(21) through a function pointer loaded from memory
    a.entry("indirect");
    a.emit(mflr(31));
    a.la(12, "fnptr");
    a.emit(D(32, 12, 12, 0));        // lwz    r12, 0(r12)
    a.emit(mtctr(12));
    a.emit(li(3, 21));
    a.emit(kBctrl);
    a.emit(mtlr(31));
    a.emit(kBlr);

    // crtest(r3, r4)
    a.entry("crtest");
    a.emit(X(0, 1 << 2, 3, 4));      // cmpw   cr1, r3, r4
    a.emit(X(32, 2 << 2, 3, 4));     // cmplw  cr2, r3, r4
    a.emit(XL(449, 0, 4, 9));        // cror   0, 4, 9
    a.emit(XL(0, 3 << 2, 1 << 2, 0)); // mcrf  cr3, cr1
    a.emit(X(19, 5, 0, 0));          // mfcr   r5
    a.emit(lis(6, 0x2000));
    a.emit(31u << 26 | 6u << 21 | 0x80u << 12 | 144u << 1); // mtcrf 0x80, r6
    a.emit(X(19, 7, 0, 0));          // mfcr   r7
    a.emit(kBlr);

    // timebase: r5 = second TBL read - first
    a.entry("timebase");
    a.emit(mftb(3, 268));
    a.emit(mftb(4, 268));
    a.emit(XO(40, 5, 3, 4));         // subf   r5, r3, r4
    a.emit(kBlr);

    // atomic(r3 = address): increment with lwarx/stwcx.
    a.entry("atomic");
    a.emit(X(20, 5, 0, 3));          // lwarx  r5, 0, r3
    a.emit(addi(5, 5, 1));
    a.emit(X(150, 5, 0, 3, 1));      // stwcx. r5, 0, r3
    a.emit(X(19, 6, 0, 0));          // mfcr   r6
    a.emit(kBlr);

    // Ways to stop.
    a.entry("spin");
    a.b("spin");

    a.entry("missing");              // jumps to 0x20000, where there is no code
    a.emit(lis(12, 2));
    a.emit(mtctr(12));
    a.emit(kBctr);

    a.entry("badret");               // "returns" to the address in fnptr
    a.la(12, "fnptr");
    a.emit(D(32, 0, 12, 0));         // lwz    r0, 0(r12)
    a.emit(mtlr(0));
    a.emit(kBlr);

    a.entry("syscall");              // r3 = sc(5) + 1
    a.emit(li(3, 5));
    a.emit(0x44000002);              // sc
    a.label("syscall.after");
    a.emit(addi(3, 3, 1));
    a.emit(kBlr);

    a.entry("trapeq");
    a.label("trapeq.tw");
    a.emit(X(4, 4, 3, 3));           // tw     4, r3, r3 (trap if equal)
    a.emit(kBlr);

    // fp(r3 = buffer of inputs, see lifter_tests.cpp; results stored after them)
    auto A = [](uint32_t op, uint32_t xo, uint32_t d, uint32_t fa, uint32_t fb, uint32_t fc) {
        return op << 26 | d << 21 | fa << 16 | fb << 11 | fc << 6 | xo << 1;
    };
    // unwind: inner returns straight to the instruction after outer's call (r5
    // holds it), as a task does when it yields to its scheduler: middle must
    // not carry on. r4 counts what ran: outer adds 1, middle 100.
    a.entry("unwind_outer");
    a.emit(mflr(31));
    a.emit(li(4, 0));
    a.bl("unwind_middle");
    a.label("unwind_outer.after");
    a.emit(addi(4, 4, 1));
    a.emit(mtlr(31));
    a.emit(kBlr);
    a.entry("unwind_middle");
    a.emit(mflr(30));
    a.bl("unwind_inner");
    a.emit(addi(4, 4, 100));
    a.emit(mtlr(30));
    a.emit(kBlr);
    a.entry("unwind_inner");
    a.emit(mtlr(5));
    a.emit(kBlr);

    // fprf: CR0 from FPSCR[FPCC] after a zero result, CR1 after a negative one (mfcr r5).
    a.entry("fprf");
    a.emit(D(48, 1, 3, 0));                               // lfs    f1, 0(r3)  1.5
    a.emit(A(59, 20, 2, 1, 1, 0));                        // fsubs  f2, f1, f1 -> +0
    a.emit(63u << 26 | 0u << 23 | 4u << 18 | 64u << 1);    // mcrfs  cr0, cr4
    a.emit(A(59, 20, 3, 2, 1, 0));                        // fsubs  f3, f2, f1 -> -1.5
    a.emit(63u << 26 | 1u << 23 | 4u << 18 | 64u << 1);    // mcrfs  cr1, cr4
    a.emit(X(19, 5, 0, 0));                               // mfcr   r5
    a.emit(kBlr);

    a.entry("fp");
    a.emit(D(48, 1, 3, 0));               // lfs    f1, 0(r3)     1.5
    a.emit(D(48, 2, 3, 4));               // lfs    f2, 4(r3)     2.25
    a.emit(A(59, 21, 3, 1, 2, 0));        // fadds  f3, f1, f2
    a.emit(A(59, 25, 4, 1, 0, 2));        // fmuls  f4, f1, f2
    a.emit(A(59, 18, 5, 2, 1, 0));        // fdivs  f5, f2, f1
    a.emit(A(59, 20, 6, 1, 2, 0));        // fsubs  f6, f1, f2
    a.emit(A(59, 29, 7, 1, 1, 2));        // fmadds f7, f1, f2, f1   (f1 * f2 + f1)
    a.emit(A(63, 40, 8, 0, 1, 0));        // fneg   f8, f1
    a.emit(A(63, 264, 9, 0, 8, 0));       // fabs   f9, f8
    a.emit(A(63, 23, 10, 6, 2, 1));       // fsel   f10, f6, f1, f2  (f6 >= 0 ? f1 : f2)
    a.emit(63u << 26 | 1u << 23 | 1u << 16 | 2u << 11); // fcmpu cr1, f1, f2
    a.emit(D(50, 11, 3, 8));              // lfd    f11, 8(r3)    1e300
    a.emit(A(63, 25, 12, 11, 0, 11));     // fmul   f12, f11, f11 -> inf
    a.emit(A(63, 12, 13, 0, 11, 0));      // frsp   f13, f11      -> inf
    a.emit(D(48, 15, 3, 16));             // lfs    f15, 16(r3)   -2.7
    a.emit(A(63, 15, 14, 0, 15, 0));      // fctiwz f14, f15      -> -2
    a.emit(X(983, 14, 0, 21));            // stfiwx f14, 0, r21   (r21 = result address)
    a.emit(A(63, 14, 16, 0, 15, 0));      // fctiw  f16, f15      -> -3 (nearest)
    a.emit(63u << 26 | 7u << 23 | 1u << 12 | 134u << 1); // mtfsfi 7, 1 (RN: toward zero)
    a.emit(A(63, 14, 17, 0, 15, 0));      // fctiw  f17, f15      -> -2
    a.emit(63u << 26 | 7u << 23 | 0u << 12 | 134u << 1); // mtfsfi 7, 0 (back to nearest)
    a.emit(D(50, 18, 3, 24));             // lfd    f18, 24(r3)   1 + 0.75 * 2^-23
    a.emit(D(52, 18, 3, 32));             // stfs   f18, 32(r3)   truncates
    a.emit(A(59, 21, 19, 18, 18, 0) );    // fadds  f19, f18, f18 rounds to nearest single
    a.emit(D(52, 3, 3, 40));              // stfs   f3, 40(r3)
    a.emit(D(54, 12, 3, 48));             // stfd   f12, 48(r3)
    a.emit(D(48, 20, 3, 56));             // lfs    f20, 56(r3)   a signaling NaN
    a.emit(D(52, 20, 3, 60));             // stfs   f20, 60(r3)   stays signaling
    a.emit(X(19, 5, 0, 0));               // mfcr   r5
    a.emit(kBlr);

    // Interrupt tests: each waits until a counter written by a handler moves.
    const uint32_t mtmsr3 = X(146, 3, 0, 0);
    const uint32_t ee_on[] = {X(83, 3, 0, 0), D(24, 3, 3, 0x8000), mtmsr3}; // mfmsr; ori 0x8000; mtmsr

    a.entry("irqwait");              // until one external interrupt was taken
    a.emit(lis(12, kBuffer >> 16));
    a.label("irqwait.loop");
    a.emit(D(32, 5, 12, 0x100));
    a.emit(D(11, 0, 5, 0));          // cmpwi  r5, 0
    a.bc(12, 2, "irqwait.loop");     // beq
    a.emit(kBlr);

    a.entry("irqlater");             // EE turned on here, with an IRQ already pending
    a.emit(lis(12, kBuffer >> 16));
    for (uint32_t w : ee_on) a.emit(w);
    a.label("irqlater.loop");
    a.emit(D(32, 5, 12, 0x100));
    a.emit(D(11, 0, 5, 0));
    a.bc(12, 2, "irqlater.loop");
    a.emit(kBlr);

    a.entry("decwait");              // DEC = 100, EE on, wait for the decrementer
    a.emit(li(3, 100));
    a.emit(mtspr(22, 3));            // mtdec  r3
    for (uint32_t w : ee_on) a.emit(w);
    a.emit(lis(12, kBuffer >> 16));
    a.label("decwait.loop");
    a.emit(D(32, 5, 12, 0x108));
    a.emit(D(11, 0, 5, 0));
    a.bc(12, 2, "decwait.loop");
    a.emit(kBlr);

    a.entry("frames");               // enable IRQ 0x02, wait for three of them
    a.emit(lis(10, 0xFE10));
    a.emit(li(3, 2));
    a.emit(D(38, 3, 10, 0x14));      // stb    r3, 0x14(r10)   IRQ enable
    for (uint32_t w : ee_on) a.emit(w);
    a.emit(lis(12, kBuffer >> 16));
    a.label("frames.loop");
    a.emit(D(32, 5, 12, 0x100));
    a.emit(D(11, 0, 5, 3));          // cmpwi  r5, 3
    a.bc(12, 0, "frames.loop");      // blt
    a.emit(kBlr);

    // LR set to a shared epilogue, then a branch (not a call) to a handler
    // that is also an ordinary function: its blr lands in the epilogue
    // (Spikeout's printf at 0x6FF4 does this).
    a.entry("lrhelper");             // r4 += 100
    a.emit(mflr(30));
    a.emit(addi(4, 4, 100));
    a.emit(mtlr(30));
    a.emit(kBlr);
    a.entry("lrtrick");              // r3 == 0: through lrhelper; else r4 = 1
    a.emit(mflr(31));
    a.la(2, "lrtrick.epilogue");
    a.emit(mtlr(2));
    a.emit(D(11, 0, 3, 0));          // cmpwi  r3, 0
    a.bc(12, 2, "lrhelper");         // beq    lrhelper
    a.emit(li(4, 1));
    a.emit(kBlr);                    // to the epilogue (LR is a constant here)
    a.label("lrtrick.epilogue");
    a.emit(mtlr(31));
    a.emit(kBlr);
    a.entry("lrouter");              // r4 = 0; lrtrick(r3)
    a.emit(mflr(29));
    a.emit(li(4, 0));
    a.bl("lrtrick");
    a.emit(mtlr(29));
    a.emit(kBlr);

    a.label("fnptr");
    a.word("double");
    return a.finish();
}

} // namespace

const Program& program() {
    static const Program p = build();
    return p;
}

} // namespace lifter_test
