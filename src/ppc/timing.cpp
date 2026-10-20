#include "ppc/timing.hpp"

#include <algorithm>
#include <array>

namespace ppc {
namespace {

// Registers as scoreboard entries.
constexpr uint8_t kGpr = 0, kFpr = 32, kCr = 64, kLr = 72, kCtr = 73, kXer = 74, kFpscr = 75, kCount = 76;

struct Regs {
    std::array<uint8_t, 40> src{}, dst{};
    uint8_t ns = 0, nd = 0;
    void read(uint32_t r) {
        if (ns < src.size()) src[ns++] = static_cast<uint8_t>(r);
    }
    void write(uint32_t r) {
        if (nd < dst.size()) dst[nd++] = static_cast<uint8_t>(r);
    }
};

bool is_load(Op op) {
    switch (op) {
    case Op::Lbz: case Op::Lbzu: case Op::Lbzux: case Op::Lbzx: case Op::Lha: case Op::Lhau: case Op::Lhaux:
    case Op::Lhax: case Op::Lhbrx: case Op::Lhz: case Op::Lhzu: case Op::Lhzux: case Op::Lhzx: case Op::Lmw:
    case Op::Lswi: case Op::Lswx: case Op::Lwarx: case Op::Lwbrx: case Op::Lwz: case Op::Lwzu: case Op::Lwzux:
    case Op::Lwzx: case Op::Eciwx: case Op::Lfd: case Op::Lfdu: case Op::Lfdux: case Op::Lfdx: case Op::Lfs:
    case Op::Lfsu: case Op::Lfsux: case Op::Lfsx:
        return true;
    default:
        return false;
    }
}

bool is_update(Op op) {
    switch (op) {
    case Op::Lbzu: case Op::Lbzux: case Op::Lhau: case Op::Lhaux: case Op::Lhzu: case Op::Lhzux: case Op::Lwzu:
    case Op::Lwzux: case Op::Stbu: case Op::Stbux: case Op::Sthu: case Op::Sthux: case Op::Stwu: case Op::Stwux:
    case Op::Lfdu: case Op::Lfdux: case Op::Lfsu: case Op::Lfsux: case Op::Stfdu: case Op::Stfdux: case Op::Stfsu:
    case Op::Stfsux:
        return true;
    default:
        return false;
    }
}

bool writes_ca(Op op) {
    switch (op) {
    case Op::Addc: case Op::Adde: case Op::Addic: case Op::AddicRc: case Op::Addme: case Op::Addze: case Op::Subfc:
    case Op::Subfe: case Op::Subfic: case Op::Subfme: case Op::Subfze: case Op::Sraw: case Op::Srawi:
        return true;
    default:
        return false;
    }
}

bool reads_ca(Op op) {
    switch (op) {
    case Op::Adde: case Op::Addme: case Op::Addze: case Op::Subfe: case Op::Subfme: case Op::Subfze:
        return true;
    default:
        return false;
    }
}

uint8_t spr_reg(uint32_t spr) {
    switch (spr) {
    case 1: return kXer;
    case 8: return kLr;
    case 9: return kCtr;
    default: return kCount; // not tracked
    }
}

Regs registers(const Instruction& in) {
    Regs r;
    const Op op = in.op;
    auto ra_or_zero = [&] {
        if (in.ra() != 0) r.read(kGpr + in.ra());
    };
    switch (form(op)) {
    case Form::DAB:
    case Form::DABNoOE:
        r.read(kGpr + in.ra());
        r.read(kGpr + in.rb());
        r.write(kGpr + in.rd());
        break;
    case Form::DA:
        r.read(kGpr + in.ra());
        r.write(kGpr + in.rd());
        break;
    case Form::DASimm:
        if (op == Op::Addi || op == Op::Addis) ra_or_zero();
        else r.read(kGpr + in.ra());
        r.write(kGpr + in.rd());
        break;
    case Form::ASUimm:
    case Form::AS:
    case Form::ASSh:
        r.read(kGpr + in.rs());
        r.write(kGpr + in.ra());
        break;
    case Form::ASB:
    case Form::Rlwnm:
        r.read(kGpr + in.rs());
        r.read(kGpr + in.rb());
        r.write(kGpr + in.ra());
        break;
    case Form::Rlwi:
        r.read(kGpr + in.rs());
        if (op == Op::Rlwimi) r.read(kGpr + in.ra());
        r.write(kGpr + in.ra());
        break;
    case Form::Cmp:
        r.read(kGpr + in.ra());
        r.read(kGpr + in.rb());
        r.write(kCr + in.crfd());
        break;
    case Form::CmpSimm:
    case Form::CmpUimm:
        r.read(kGpr + in.ra());
        r.write(kCr + in.crfd());
        break;
    case Form::LoadStoreD:
    case Form::LoadStoreX:
    case Form::LoadStoreNB:
    case Form::FLoadStoreD:
    case Form::FLoadStoreX: {
        const bool fp = form(op) == Form::FLoadStoreD || form(op) == Form::FLoadStoreX;
        const uint8_t base = fp ? kFpr : kGpr;
        ra_or_zero();
        if (form(op) == Form::LoadStoreX || form(op) == Form::FLoadStoreX) r.read(kGpr + in.rb());
        if (op == Op::Lmw) {
            for (uint32_t i = in.rd(); i < 32; ++i) r.write(kGpr + i);
        } else if (op == Op::Stmw) {
            for (uint32_t i = in.rs(); i < 32; ++i) r.read(kGpr + i);
        } else if (is_load(op)) {
            r.write(base + in.rd());
        } else {
            r.read(base + in.rs());
        }
        if (is_update(op)) r.write(kGpr + in.ra());
        if (op == Op::StwcxRc) r.write(kCr + 0);
        break;
    }
    case Form::Cache:
        ra_or_zero();
        r.read(kGpr + in.rb());
        break;
    case Form::BranchCond:
    case Form::BranchLR:
    case Form::BranchCTR:
        if (!(in.bo() & 0x10)) r.read(kCr + (in.bi() >> 2)); // a condition
        if (!(in.bo() & 0x04)) {                              // CTR counts down
            r.read(kCtr);
            r.write(kCtr);
        }
        if (form(op) == Form::BranchLR) r.read(kLr);
        if (form(op) == Form::BranchCTR) r.read(kCtr);
        if (in.raw & 1) r.write(kLr);
        break;
    case Form::Branch:
        if (in.raw & 1) r.write(kLr);
        break;
    case Form::Trap:
        r.read(kGpr + in.ra());
        r.read(kGpr + in.rb());
        break;
    case Form::TrapImm:
        r.read(kGpr + in.ra());
        break;
    case Form::MfSpr:
        if (spr_reg(in.spr()) != kCount) r.read(spr_reg(in.spr()));
        r.write(kGpr + in.rd());
        break;
    case Form::MtSpr:
        r.read(kGpr + in.rs());
        if (spr_reg(in.spr()) != kCount) r.write(spr_reg(in.spr()));
        break;
    case Form::MfTb:
        r.write(kGpr + in.rd());
        break;
    case Form::MfReg:
        if (op == Op::Mfcr)
            for (uint8_t f = 0; f < 8; ++f) r.read(kCr + f);
        r.write(kGpr + in.rd());
        break;
    case Form::MtReg:
    case Form::MtSr:
        r.read(kGpr + in.rs());
        break;
    case Form::MtCrf:
        r.read(kGpr + in.rs());
        for (uint8_t f = 0; f < 8; ++f)
            if (in.crm() & (0x80 >> f)) r.write(kCr + f);
        break;
    case Form::MfSr:
        r.write(kGpr + in.rd());
        break;
    case Form::MfSrin:
        r.read(kGpr + in.rb());
        r.write(kGpr + in.rd());
        break;
    case Form::MtSrin:
        r.read(kGpr + in.rs());
        r.read(kGpr + in.rb());
        break;
    case Form::RB:
        r.read(kGpr + in.rb());
        break;
    case Form::CrBit:
        r.read(kCr + (in.ra() >> 2));
        r.read(kCr + (in.rb() >> 2));
        r.write(kCr + (in.rd() >> 2));
        break;
    case Form::Mcrf:
        r.read(op == Op::Mcrfs ? kFpscr : kCr + in.crfs());
        r.write(kCr + in.crfd());
        break;
    case Form::Mcrxr:
        r.read(kXer);
        r.write(kCr + in.crfd());
        r.write(kXer);
        break;
    case Form::FDAB:
        r.read(kFpr + in.ra());
        r.read(kFpr + in.rb());
        r.write(kFpr + in.rd());
        break;
    case Form::FDAC:
        r.read(kFpr + in.ra());
        r.read(kFpr + in.rc());
        r.write(kFpr + in.rd());
        break;
    case Form::FDACB:
        r.read(kFpr + in.ra());
        r.read(kFpr + in.rc());
        r.read(kFpr + in.rb());
        r.write(kFpr + in.rd());
        break;
    case Form::FDB:
        r.read(kFpr + in.rb());
        r.write(kFpr + in.rd());
        break;
    case Form::FCmp:
        r.read(kFpr + in.ra());
        r.read(kFpr + in.rb());
        r.write(kCr + in.crfd());
        break;
    case Form::Mffs:
        r.read(kFpscr);
        r.write(kFpr + in.rd());
        break;
    case Form::Mtfsf:
        r.read(kFpr + in.rb());
        r.write(kFpscr);
        break;
    case Form::Mtfsfi:
    case Form::Mtfsb:
        r.write(kFpscr);
        break;
    case Form::None:
        break;
    }
    // Record forms set CR0 (integer) or CR1 (floating point); carries and XER.
    const Form f = form(op);
    const bool rc_form = f == Form::DAB || f == Form::DABNoOE || f == Form::DA || f == Form::ASB || f == Form::AS ||
                         f == Form::ASSh || f == Form::Rlwi || f == Form::Rlwnm;
    const bool frc_form = f == Form::FDAB || f == Form::FDAC || f == Form::FDACB || f == Form::FDB;
    if ((rc_form && in.record()) || op == Op::AddicRc || op == Op::AndiRc || op == Op::AndisRc) r.write(kCr + 0);
    if (frc_form && in.record()) r.write(kCr + 1);
    if (reads_ca(op)) r.read(kXer);
    if (writes_ca(op)) r.write(kXer); // (FPSCR status bits don't hold up the FP pipeline)
    return r;
}

} // namespace

OpTiming timing(const Instruction& in) {
    const Op op = in.op;
    switch (op) {
    // Integer unit: one cycle, except multiply and divide.
    case Op::Mulli: return {Unit::IU, 3, 2};
    case Op::Mullw: case Op::Mulhw: case Op::Mulhwu: return {Unit::IU, 4, 4};
    case Op::Divw: case Op::Divwu: return {Unit::IU, 37, 37};
    // Loads and stores (cache hits).
    case Op::Lmw: return {Unit::LSU, static_cast<uint8_t>(2 + (32 - in.rd())), static_cast<uint8_t>(1 + (32 - in.rd()))};
    case Op::Stmw: return {Unit::LSU, 1, static_cast<uint8_t>(1 + (32 - in.rs()))};
    case Op::Lswi: case Op::Stswi: {
        const uint32_t words = ((in.nb() ? in.nb() : 32) + 3) / 4;
        return {Unit::LSU, static_cast<uint8_t>(2 + words), static_cast<uint8_t>(1 + words)};
    }
    case Op::Lswx: case Op::Stswx: return {Unit::LSU, 10, 9};
    case Op::Lwarx: return {Unit::LSU, 3, 1};
    case Op::StwcxRc: return {Unit::LSU, 3, 2};
    case Op::Dcbz: return {Unit::LSU, 10, 10};
    case Op::Dcbf: case Op::Dcbi: case Op::Dcbst: case Op::Icbi: return {Unit::LSU, 3, 3};
    case Op::Dcbt: case Op::Dcbtst: return {Unit::LSU, 1, 1};
    case Op::Eciwx: case Op::Ecowx: return {Unit::LSU, 2, 1};
    case Op::Tlbie: case Op::Tlbia: case Op::Tlbsync: case Op::Tlbld: case Op::Tlbli: return {Unit::LSU, 3, 3, true};
    case Op::Eieio: return {Unit::LSU, 1, 1};
    // Floating point: single precision and adds pipelined; double multiplies
    // take the multiplier twice; divides and reciprocals are not pipelined.
    case Op::Fmul: case Op::Fmadd: case Op::Fmsub: case Op::Fnmadd: case Op::Fnmsub: return {Unit::FPU, 4, 2};
    case Op::Fdivs: return {Unit::FPU, 18, 18};
    case Op::Fdiv: return {Unit::FPU, 33, 33};
    case Op::Fres: return {Unit::FPU, 18, 18};
    case Op::Mtfsf: case Op::Mtfsfi: case Op::Mtfsb0: case Op::Mtfsb1: return {Unit::FPU, 3, 3};
    case Op::Mffs: return {Unit::FPU, 3, 1};
    // System register unit.
    case Op::Mfspr:
        return in.spr() == 8 || in.spr() == 9 ? OpTiming{Unit::SRU, 1, 1} : OpTiming{Unit::SRU, 3, 3};
    case Op::Mtspr:
        return in.spr() == 8 || in.spr() == 9 ? OpTiming{Unit::SRU, 2, 1} : OpTiming{Unit::SRU, 2, 2};
    case Op::Mftb: case Op::Mfmsr: case Op::Mfcr: case Op::Mtcrf: case Op::Mcrf: case Op::Mcrxr: case Op::Mcrfs:
    case Op::Crand: case Op::Crandc: case Op::Creqv: case Op::Crnand: case Op::Crnor: case Op::Cror: case Op::Crorc:
    case Op::Crxor:
        return {Unit::SRU, 1, 1};
    case Op::Mfsr: case Op::Mfsrin: return {Unit::SRU, 3, 3};
    case Op::Mtmsr: case Op::Mtsr: case Op::Mtsrin: return {Unit::SRU, 2, 2, true};
    case Op::Sync: return {Unit::SRU, 3, 3, true};
    case Op::Isync: return {Unit::SRU, 2, 2, true};
    case Op::Rfi: case Op::Sc: return {Unit::SRU, 3, 3, true};
    // Branches: the branch unit, no result.
    case Op::B: case Op::Bc: case Op::Bclr: case Op::Bcctr: return {Unit::BPU, 1, 1};
    case Op::Tw: case Op::Twi: return {Unit::IU, 1, 1};
    default: break;
    }
    if (form(op) == Form::FLoadStoreD || form(op) == Form::FLoadStoreX) return {Unit::LSU, 2, 1};
    if (form(op) == Form::LoadStoreD || form(op) == Form::LoadStoreX) return {Unit::LSU, 2, 1};
    switch (form(op)) {
    case Form::FDAB: case Form::FDAC: case Form::FDACB: case Form::FDB: case Form::FCmp:
        return {Unit::FPU, 3, 1};
    default:
        return {Unit::IU, 1, 1};
    }
}

uint32_t block_cycles(std::span<const Instruction> block) {
    std::array<uint32_t, kCount> ready{}; // cycle each register's value is ready
    std::array<uint32_t, 5> unit_free{};
    uint32_t cycle = 0, slots = 0, end = 1, done = 0; // done: all results so far are in
    for (const Instruction& in : block) {
        const OpTiming t = timing(in);
        const Regs regs = registers(in);
        uint32_t at = std::max(cycle, unit_free[static_cast<size_t>(t.unit)]);
        for (uint8_t i = 0; i < regs.ns; ++i)
            if (regs.src[i] < kCount) at = std::max(at, ready[regs.src[i]]);
        if (t.serial) at = std::max(at, done);
        if (at == cycle && slots == 2) ++at; // two dispatches a cycle
        if (at > cycle) {
            cycle = at;
            slots = 0;
        }
        ++slots;
        unit_free[static_cast<size_t>(t.unit)] = at + t.busy;
        for (uint8_t i = 0; i < regs.nd; ++i)
            if (regs.dst[i] < kCount) ready[regs.dst[i]] = at + t.latency;
        done = std::max(done, at + t.latency);
        if (t.serial && at + t.latency > cycle) { // nothing dispatches past it until it completes
            cycle = at + t.latency;
            slots = 0;
        }
        end = std::max({end, at + 1, at + t.busy});
    }
    return end;
}

} // namespace ppc
