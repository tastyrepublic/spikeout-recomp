#include "ppc/disasm.hpp"

#include <format>

namespace ppc {

namespace {

std::string r(uint32_t n) { return std::format("r{}", n); }
std::string f(uint32_t n) { return std::format("f{}", n); }
std::string cr(uint32_t n) { return std::format("cr{}", n); }
std::string hex(uint32_t v) { return std::format("0x{:x}", v); }
std::string addr(uint32_t v) { return std::format("0x{:08x}", v); }
std::string mem(int32_t d, uint32_t ra) { return std::format("{}({})", d, r(ra)); }

std::string spr_text(uint32_t spr) {
    const char* name = spr_name(spr);
    return name ? std::string(name) : std::to_string(spr);
}

// Mnemonic plus the "o" (OE) and "." (Rc) suffixes the form allows.
std::string base_mnemonic(const Instruction& i) {
    std::string m = mnemonic(i.op);
    switch (form(i.op)) {
    case Form::DAB:
    case Form::DA:
        if (i.oe()) m += 'o';
        [[fallthrough]];
    case Form::DABNoOE:
    case Form::ASB:
    case Form::AS:
    case Form::ASSh:
    case Form::Rlwi:
    case Form::Rlwnm:
    case Form::FDAB:
    case Form::FDAC:
    case Form::FDACB:
    case Form::FDB:
    case Form::Mffs:
    case Form::Mtfsf:
    case Form::Mtfsfi:
    case Form::Mtfsb:
        if (i.record()) m += '.';
        break;
    default:
        break;
    }
    return m;
}

std::string op(const std::string& m, const std::string& operands = {}) {
    if (operands.empty()) return m;
    std::string s = m;
    s.resize(s.size() < 8 ? 8 : s.size() + 1, ' ');
    return s + operands;
}

// Simplified mnemonic for a conditional branch's condition, e.g. "eq", "ne",
// "dnz". Returns empty when BO has no common simplified form.
struct Cond {
    bool ok = false;
    bool always = false;
    std::string text;
};

Cond branch_cond(uint32_t bo, uint32_t bi) {
    static const char* kTrue[] = {"lt", "gt", "eq", "so"};
    static const char* kFalse[] = {"ge", "le", "ne", "ns"};
    if ((bo & 0x14) == 0x14) return {true, true, ""};
    switch (bo & 0x1E) {
    case 0x0C: return {true, false, kTrue[bi & 3]};
    case 0x04: return {true, false, kFalse[bi & 3]};
    case 0x10: return {true, false, "dnz"};
    case 0x12: return {true, false, "dz"};
    default:   return {};
    }
}

bool cond_uses_cr(uint32_t bo) { return (bo & 0x10) == 0; }

std::string branch_suffix(const Instruction& i, bool with_aa) {
    std::string s;
    if (i.lk()) s += 'l';
    if (with_aa && i.aa()) s += 'a';
    return s;
}

std::string disasm_branch_cond(const Instruction& i, uint32_t pc) {
    const Cond c = branch_cond(i.bo(), i.bi());
    const std::string crf = (cond_uses_cr(i.bo()) && (i.bi() >> 2) != 0) ? cr(i.bi() >> 2) + ", " : "";

    if (i.op == Op::Bc) {
        const std::string target = addr(*branch_target(i, pc));
        if (!c.ok)
            return op("bc" + branch_suffix(i, true), std::format("{}, {}, {}", i.bo(), i.bi(), target));
        return op("b" + c.text + branch_suffix(i, true), crf + target);
    }

    const char* reg = i.op == Op::Bclr ? "lr" : "ctr";
    const char* raw_m = i.op == Op::Bclr ? "bclr" : "bcctr";
    if (!c.ok)
        return op(raw_m + branch_suffix(i, false), std::format("{}, {}", i.bo(), i.bi()));
    std::string operands = crf;
    if (!operands.empty()) operands.resize(operands.size() - 2); // drop ", "
    return op("b" + c.text + reg + branch_suffix(i, false), operands);
}

std::string disasm_spr(const Instruction& i) {
    const uint32_t spr = i.spr();
    const bool from = i.op == Op::Mfspr;
    const char* simple = nullptr;
    switch (spr) {
    case 1: simple = from ? "mfxer" : "mtxer"; break;
    case 8: simple = from ? "mflr" : "mtlr"; break;
    case 9: simple = from ? "mfctr" : "mtctr"; break;
    default: break;
    }
    if (simple) return op(simple, r(i.rd()));
    if (from) return op("mfspr", r(i.rd()) + ", " + spr_text(spr));
    return op("mtspr", spr_text(spr) + ", " + r(i.rs()));
}

std::string disasm_rlwi(const Instruction& i) {
    const uint32_t sh = i.sh(), mb = i.mb(), me = i.me();
    const std::string dot = i.record() ? "." : "";
    const std::string as = r(i.ra()) + ", " + r(i.rs());
    if (i.op == Op::Rlwinm) {
        if (sh != 0 && mb == 0 && me == 31 - sh) return op("slwi" + dot, std::format("{}, {}", as, sh));
        if (me == 31 && sh != 0 && mb == 32 - sh) return op("srwi" + dot, std::format("{}, {}", as, mb));
        if (sh == 0 && me == 31) return op("clrlwi" + dot, std::format("{}, {}", as, mb));
        if (mb == 0 && me == 31) return op("rotlwi" + dot, std::format("{}, {}", as, sh));
    }
    return op(base_mnemonic(i), std::format("{}, {}, {}, {}", as, sh, mb, me));
}

} // namespace

const char* spr_name(uint32_t spr) {
    switch (spr) {
    case 1:    return "xer";
    case 8:    return "lr";
    case 9:    return "ctr";
    case 18:   return "dsisr";
    case 19:   return "dar";
    case 22:   return "dec";
    case 25:   return "sdr1";
    case 26:   return "srr0";
    case 27:   return "srr1";
    case 272:  return "sprg0";
    case 273:  return "sprg1";
    case 274:  return "sprg2";
    case 275:  return "sprg3";
    case 282:  return "ear";
    case 284:  return "tbl";
    case 285:  return "tbu";
    case 287:  return "pvr";
    case 528:  return "ibat0u";
    case 529:  return "ibat0l";
    case 530:  return "ibat1u";
    case 531:  return "ibat1l";
    case 532:  return "ibat2u";
    case 533:  return "ibat2l";
    case 534:  return "ibat3u";
    case 535:  return "ibat3l";
    case 536:  return "dbat0u";
    case 537:  return "dbat0l";
    case 538:  return "dbat1u";
    case 539:  return "dbat1l";
    case 540:  return "dbat2u";
    case 541:  return "dbat2l";
    case 542:  return "dbat3u";
    case 543:  return "dbat3l";
    case 976:  return "dmiss";
    case 977:  return "dcmp";
    case 978:  return "hash1";
    case 979:  return "hash2";
    case 980:  return "imiss";
    case 981:  return "icmp";
    case 982:  return "rpa";
    case 1008: return "hid0";
    case 1009: return "hid1";
    case 1010: return "iabr";
    default:   return nullptr;
    }
}

std::string disassemble(const Instruction& i, uint32_t pc) {
    const std::string m = base_mnemonic(i);

    switch (form(i.op)) {
    case Form::None:
        if (i.op == Op::Invalid) return op(".long", addr(i.raw));
        return m;

    case Form::DAB:
    case Form::DABNoOE:
        return op(m, std::format("{}, {}, {}", r(i.rd()), r(i.ra()), r(i.rb())));
    case Form::DA:
        return op(m, std::format("{}, {}", r(i.rd()), r(i.ra())));
    case Form::DASimm:
        if (i.op == Op::Addi && i.ra() == 0) return op("li", std::format("{}, {}", r(i.rd()), i.simm()));
        if (i.op == Op::Addis && i.ra() == 0) return op("lis", std::format("{}, {}", r(i.rd()), hex(i.uimm())));
        return op(m, std::format("{}, {}, {}", r(i.rd()), r(i.ra()), i.simm()));

    case Form::ASUimm:
        if (i.op == Op::Ori && i.raw == 0x60000000) return "nop";
        return op(m, std::format("{}, {}, {}", r(i.ra()), r(i.rs()), hex(i.uimm())));
    case Form::ASB:
        if (i.op == Op::Or && i.rs() == i.rb())
            return op(i.record() ? "mr." : "mr", std::format("{}, {}", r(i.ra()), r(i.rs())));
        if (i.op == Op::Nor && i.rs() == i.rb())
            return op(i.record() ? "not." : "not", std::format("{}, {}", r(i.ra()), r(i.rs())));
        return op(m, std::format("{}, {}, {}", r(i.ra()), r(i.rs()), r(i.rb())));
    case Form::AS:
        return op(m, std::format("{}, {}", r(i.ra()), r(i.rs())));
    case Form::ASSh:
        return op(m, std::format("{}, {}, {}", r(i.ra()), r(i.rs()), i.sh()));
    case Form::Rlwi:
        return disasm_rlwi(i);
    case Form::Rlwnm:
        if (i.mb() == 0 && i.me() == 31)
            return op(i.record() ? "rotlw." : "rotlw", std::format("{}, {}, {}", r(i.ra()), r(i.rs()), r(i.rb())));
        return op(m, std::format("{}, {}, {}, {}, {}", r(i.ra()), r(i.rs()), r(i.rb()), i.mb(), i.me()));

    case Form::Cmp:
    case Form::CmpSimm:
    case Form::CmpUimm: {
        // L=0 is the only valid setting on a 32-bit CPU; use the "w" forms.
        const bool logical = i.op == Op::Cmpl || i.op == Op::Cmpli;
        const bool imm = form(i.op) != Form::Cmp;
        const std::string name = std::string(logical ? "cmplw" : "cmpw") + (imm ? "i" : "");
        const std::string crf = i.crfd() ? cr(i.crfd()) + ", " : "";
        std::string rhs = !imm ? r(i.rb()) : logical ? hex(i.uimm()) : std::to_string(i.simm());
        return op(name, std::format("{}{}, {}", crf, r(i.ra()), rhs));
    }

    case Form::LoadStoreD:
        return op(m, std::format("{}, {}", r(i.rd()), mem(i.simm(), i.ra())));
    case Form::FLoadStoreD:
        return op(m, std::format("{}, {}", f(i.rd()), mem(i.simm(), i.ra())));
    case Form::LoadStoreX:
        return op(m, std::format("{}, {}, {}", r(i.rd()), r(i.ra()), r(i.rb())));
    case Form::FLoadStoreX:
        return op(m, std::format("{}, {}, {}", f(i.rd()), r(i.ra()), r(i.rb())));
    case Form::LoadStoreNB:
        return op(m, std::format("{}, {}, {}", r(i.rd()), r(i.ra()), i.nb()));
    case Form::Cache:
        return op(m, std::format("{}, {}", r(i.ra()), r(i.rb())));

    case Form::Branch:
        return op("b" + branch_suffix(i, true), addr(*branch_target(i, pc)));
    case Form::BranchCond:
    case Form::BranchLR:
    case Form::BranchCTR:
        return disasm_branch_cond(i, pc);

    case Form::Trap:
        if (i.to() == 31 && i.ra() == 0 && i.rb() == 0) return "trap";
        return op(m, std::format("{}, {}, {}", i.to(), r(i.ra()), r(i.rb())));
    case Form::TrapImm:
        return op(m, std::format("{}, {}, {}", i.to(), r(i.ra()), i.simm()));

    case Form::MfSpr:
    case Form::MtSpr:
        return disasm_spr(i);
    case Form::MfTb:
        if (i.spr() == 268) return op("mftb", r(i.rd()));
        if (i.spr() == 269) return op("mftbu", r(i.rd()));
        return op(m, std::format("{}, {}", r(i.rd()), i.spr()));
    case Form::MfReg:
        return op(m, r(i.rd()));
    case Form::MtReg:
        return op(m, r(i.rs()));
    case Form::MtCrf:
        if (i.crm() == 0xFF) return op("mtcr", r(i.rs()));
        return op(m, std::format("{}, {}", hex(i.crm()), r(i.rs())));
    case Form::MfSr:
        return op(m, std::format("{}, {}", r(i.rd()), i.sr()));
    case Form::MtSr:
        return op(m, std::format("{}, {}", i.sr(), r(i.rs())));
    case Form::MfSrin:
    case Form::MtSrin:
        return op(m, std::format("{}, {}", r(i.rd()), r(i.rb())));
    case Form::RB:
        return op(m, r(i.rb()));

    case Form::CrBit:
        return op(m, std::format("{}, {}, {}", i.rd(), i.ra(), i.rb()));
    case Form::Mcrf:
        return op(m, std::format("{}, {}", cr(i.crfd()), cr(i.crfs())));
    case Form::Mcrxr:
        return op(m, cr(i.crfd()));

    case Form::FDAB:
        return op(m, std::format("{}, {}, {}", f(i.rd()), f(i.ra()), f(i.rb())));
    case Form::FDAC:
        return op(m, std::format("{}, {}, {}", f(i.rd()), f(i.ra()), f(i.rc())));
    case Form::FDACB:
        return op(m, std::format("{}, {}, {}, {}", f(i.rd()), f(i.ra()), f(i.rc()), f(i.rb())));
    case Form::FDB:
        return op(m, std::format("{}, {}", f(i.rd()), f(i.rb())));
    case Form::FCmp:
        return op(m, std::format("{}, {}, {}", cr(i.crfd()), f(i.ra()), f(i.rb())));
    case Form::Mffs:
        return op(m, f(i.rd()));
    case Form::Mtfsf:
        return op(m, std::format("{}, {}", hex(i.fm()), f(i.rb())));
    case Form::Mtfsfi:
        return op(m, std::format("{}, {}", cr(i.crfd()), i.fpimm()));
    case Form::Mtfsb:
        return op(m, std::to_string(i.rd()));
    }
    return op(".long", addr(i.raw));
}

} // namespace ppc
