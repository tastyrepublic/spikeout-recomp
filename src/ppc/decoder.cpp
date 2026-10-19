#include "ppc/instruction.hpp"

#include <array>

namespace ppc {

namespace {

struct OpInfo {
    const char* mnemonic;
    Form form;
};

constexpr std::array<OpInfo, static_cast<size_t>(Op::Count)> kOpInfo = {{
#define PPC_X(name, mnem, frm) {mnem, Form::frm},
    PPC_OPCODES(PPC_X)
#undef PPC_X
}};

// Primary opcode 19: branch-to-register, CR logical, rfi, isync.
Op decode19(uint32_t raw) {
    switch ((raw >> 1) & 0x3FF) {
    case 0:   return Op::Mcrf;
    case 16:  return Op::Bclr;
    case 33:  return Op::Crnor;
    case 50:  return Op::Rfi;
    case 129: return Op::Crandc;
    case 150: return Op::Isync;
    case 193: return Op::Crxor;
    case 225: return Op::Crnand;
    case 257: return Op::Crand;
    case 289: return Op::Creqv;
    case 417: return Op::Crorc;
    case 449: return Op::Cror;
    case 528: return Op::Bcctr;
    default:  return Op::Invalid;
    }
}

// Primary opcode 31: XO-form arithmetic (9-bit XO + OE) and X-form everything else.
Op decode31(uint32_t raw) {
    const uint32_t xo10 = (raw >> 1) & 0x3FF;

    // XO-form: the OE bit is the top bit of the 10-bit field. None of these
    // 9-bit values collide with an X-form opcode, with or without OE set.
    switch (xo10 & 0x1FF) {
    case 8:   return Op::Subfc;
    case 10:  return Op::Addc;
    case 11:  return Op::Mulhwu;
    case 40:  return Op::Subf;
    case 75:  return Op::Mulhw;
    case 104: return Op::Neg;
    case 136: return Op::Subfe;
    case 138: return Op::Adde;
    case 200: return Op::Subfze;
    case 202: return Op::Addze;
    case 232: return Op::Subfme;
    case 234: return Op::Addme;
    case 235: return Op::Mullw;
    case 266: return Op::Add;
    case 459: return Op::Divwu;
    case 491: return Op::Divw;
    default:  break;
    }

    switch (xo10) {
    case 0:    return Op::Cmp;
    case 4:    return Op::Tw;
    case 19:   return Op::Mfcr;
    case 20:   return Op::Lwarx;
    case 23:   return Op::Lwzx;
    case 24:   return Op::Slw;
    case 26:   return Op::Cntlzw;
    case 28:   return Op::And;
    case 32:   return Op::Cmpl;
    case 54:   return Op::Dcbst;
    case 55:   return Op::Lwzux;
    case 60:   return Op::Andc;
    case 83:   return Op::Mfmsr;
    case 86:   return Op::Dcbf;
    case 87:   return Op::Lbzx;
    case 119:  return Op::Lbzux;
    case 124:  return Op::Nor;
    case 144:  return Op::Mtcrf;
    case 146:  return Op::Mtmsr;
    case 150:  return Op::StwcxRc;
    case 151:  return Op::Stwx;
    case 183:  return Op::Stwux;
    case 210:  return Op::Mtsr;
    case 215:  return Op::Stbx;
    case 242:  return Op::Mtsrin;
    case 246:  return Op::Dcbtst;
    case 247:  return Op::Stbux;
    case 278:  return Op::Dcbt;
    case 279:  return Op::Lhzx;
    case 284:  return Op::Eqv;
    case 306:  return Op::Tlbie;
    case 310:  return Op::Eciwx;
    case 311:  return Op::Lhzux;
    case 316:  return Op::Xor;
    case 339:  return Op::Mfspr;
    case 343:  return Op::Lhax;
    case 370:  return Op::Tlbia;
    case 371:  return Op::Mftb;
    case 375:  return Op::Lhaux;
    case 407:  return Op::Sthx;
    case 412:  return Op::Orc;
    case 438:  return Op::Ecowx;
    case 439:  return Op::Sthux;
    case 444:  return Op::Or;
    case 467:  return Op::Mtspr;
    case 470:  return Op::Dcbi;
    case 476:  return Op::Nand;
    case 512:  return Op::Mcrxr;
    case 533:  return Op::Lswx;
    case 534:  return Op::Lwbrx;
    case 535:  return Op::Lfsx;
    case 536:  return Op::Srw;
    case 566:  return Op::Tlbsync;
    case 567:  return Op::Lfsux;
    case 595:  return Op::Mfsr;
    case 597:  return Op::Lswi;
    case 598:  return Op::Sync;
    case 599:  return Op::Lfdx;
    case 631:  return Op::Lfdux;
    case 659:  return Op::Mfsrin;
    case 661:  return Op::Stswx;
    case 662:  return Op::Stwbrx;
    case 663:  return Op::Stfsx;
    case 695:  return Op::Stfsux;
    case 725:  return Op::Stswi;
    case 727:  return Op::Stfdx;
    case 759:  return Op::Stfdux;
    case 790:  return Op::Lhbrx;
    case 792:  return Op::Sraw;
    case 824:  return Op::Srawi;
    case 854:  return Op::Eieio;
    case 918:  return Op::Sthbrx;
    case 922:  return Op::Extsh;
    case 954:  return Op::Extsb;
    case 978:  return Op::Tlbld;
    case 982:  return Op::Icbi;
    case 983:  return Op::Stfiwx;
    case 1010: return Op::Tlbli;
    case 1014: return Op::Dcbz;
    default:   return Op::Invalid;
    }
}

// Primary opcode 59: single-precision A-form arithmetic.
Op decode59(uint32_t raw) {
    switch ((raw >> 1) & 31) {
    case 18: return Op::Fdivs;
    case 20: return Op::Fsubs;
    case 21: return Op::Fadds;
    case 22: return Op::Fsqrts;
    case 24: return Op::Fres;
    case 25: return Op::Fmuls;
    case 28: return Op::Fmsubs;
    case 29: return Op::Fmadds;
    case 30: return Op::Fnmsubs;
    case 31: return Op::Fnmadds;
    default: return Op::Invalid;
    }
}

// Primary opcode 63: double-precision arithmetic (A-form when XO bit 4 of
// the 10-bit field is set) and X-form moves, compares and FPSCR ops.
Op decode63(uint32_t raw) {
    const uint32_t xo10 = (raw >> 1) & 0x3FF;
    if (xo10 & 0x10) {
        switch (xo10 & 31) {
        case 18: return Op::Fdiv;
        case 20: return Op::Fsub;
        case 21: return Op::Fadd;
        case 22: return Op::Fsqrt;
        case 23: return Op::Fsel;
        case 25: return Op::Fmul;
        case 26: return Op::Frsqrte;
        case 28: return Op::Fmsub;
        case 29: return Op::Fmadd;
        case 30: return Op::Fnmsub;
        case 31: return Op::Fnmadd;
        default: return Op::Invalid;
        }
    }
    switch (xo10) {
    case 0:   return Op::Fcmpu;
    case 12:  return Op::Frsp;
    case 14:  return Op::Fctiw;
    case 15:  return Op::Fctiwz;
    case 32:  return Op::Fcmpo;
    case 38:  return Op::Mtfsb1;
    case 40:  return Op::Fneg;
    case 64:  return Op::Mcrfs;
    case 70:  return Op::Mtfsb0;
    case 72:  return Op::Fmr;
    case 134: return Op::Mtfsfi;
    case 136: return Op::Fnabs;
    case 264: return Op::Fabs;
    case 583: return Op::Mffs;
    case 711: return Op::Mtfsf;
    default:  return Op::Invalid;
    }
}

Op decode_op(uint32_t raw) {
    switch (raw >> 26) {
    case 3:  return Op::Twi;
    case 7:  return Op::Mulli;
    case 8:  return Op::Subfic;
    case 10: return Op::Cmpli;
    case 11: return Op::Cmpi;
    case 12: return Op::Addic;
    case 13: return Op::AddicRc;
    case 14: return Op::Addi;
    case 15: return Op::Addis;
    case 16: return Op::Bc;
    case 17: return (raw & 2) ? Op::Sc : Op::Invalid;
    case 18: return Op::B;
    case 19: return decode19(raw);
    case 20: return Op::Rlwimi;
    case 21: return Op::Rlwinm;
    case 23: return Op::Rlwnm;
    case 24: return Op::Ori;
    case 25: return Op::Oris;
    case 26: return Op::Xori;
    case 27: return Op::Xoris;
    case 28: return Op::AndiRc;
    case 29: return Op::AndisRc;
    case 31: return decode31(raw);
    case 32: return Op::Lwz;
    case 33: return Op::Lwzu;
    case 34: return Op::Lbz;
    case 35: return Op::Lbzu;
    case 36: return Op::Stw;
    case 37: return Op::Stwu;
    case 38: return Op::Stb;
    case 39: return Op::Stbu;
    case 40: return Op::Lhz;
    case 41: return Op::Lhzu;
    case 42: return Op::Lha;
    case 43: return Op::Lhau;
    case 44: return Op::Sth;
    case 45: return Op::Sthu;
    case 46: return Op::Lmw;
    case 47: return Op::Stmw;
    case 48: return Op::Lfs;
    case 49: return Op::Lfsu;
    case 50: return Op::Lfd;
    case 51: return Op::Lfdu;
    case 52: return Op::Stfs;
    case 53: return Op::Stfsu;
    case 54: return Op::Stfd;
    case 55: return Op::Stfdu;
    case 59: return decode59(raw);
    case 63: return decode63(raw);
    default: return Op::Invalid;
    }
}

} // namespace

const char* mnemonic(Op op) { return kOpInfo[static_cast<size_t>(op)].mnemonic; }
Form form(Op op) { return kOpInfo[static_cast<size_t>(op)].form; }

// Reserved-field checking is intentionally not done here: real code on the
// board may set reserved bits, and the hardware ignores them.
Instruction decode(uint32_t raw) { return Instruction{raw, decode_op(raw)}; }

std::optional<uint32_t> branch_target(const Instruction& insn, uint32_t pc) {
    switch (insn.op) {
    case Op::B:
        return insn.aa() ? static_cast<uint32_t>(insn.li()) : pc + static_cast<uint32_t>(insn.li());
    case Op::Bc:
        return insn.aa() ? static_cast<uint32_t>(insn.bd()) : pc + static_cast<uint32_t>(insn.bd());
    default:
        return std::nullopt;
    }
}

} // namespace ppc
