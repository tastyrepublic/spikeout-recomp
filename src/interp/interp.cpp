#include "interp/interp.hpp"

#include "analysis/boot_image.hpp"
#include "analysis/control_flow.hpp"
#include "ppc/instruction.hpp"
#include "ppc/timing.hpp"
#include "recomp/guest.hpp"
#include "runtime/memory_map.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

using recomp::Context;
using recomp::FunctionEntry;
using ppc::Instruction;
using ppc::Op;
namespace ops = recomp::ops;

// ---- The steps ----
// An instruction's step is its ppc::Op; the rest (block starts and the ways out of a block) come after them.
enum Kind : uint16_t {
    kTick = 0x200, // a block's start: imm its cycles, pc its address, flags & kHook: c.hook(pc)
    kEdge,         // go on to edges[imm]
    kBc,           // bc: d BO, a BI; edges[imm] taken, edges[imm + 1] not
    kCall,         // bl/bcl: d BO, a BI; edges[imm] the function called (kTail: run here, kJump: c.call)
    kLink,         // bl $+4 (reads the PC): LR set, the condition evaluated
    kCallReg,      // bclrl/bcctrl: d BO, a BI, b 0 LR / 1 CTR
    kRet,          // blr: d BO, a BI; edges[imm] when not taken
    kRegJump,      // bclr/bcctr to the register: as kRet
    kRegEdge,      // bclr/bcctr whose target the analysis knows: edges[imm], edges[imm + 1] when not taken
    kRegTable,     // bctr through a jump table: tables[imm]
    kRfi,
    kMissing,      // no code after this: c.missing(imm)
    kLi,           // addi rD, 0, SIMM
    kLis,          // addis rD, 0, SIMM
    kNop,
    kMfTbl, kMfTbu, kMfDec, kMtTbl, kMtTbu, kMtDec,
    kMfSpr, kMtSpr, // imm: the register's offset in CpuState
    kUnknownSpr,    // imm: the SPR
    kTrapAlways,
    kUnimplemented,
};

enum Flags : uint8_t { kRc = 1, kOe = 2, kHook = 4 };

struct Step {
    uint16_t kind;
    uint8_t d, a, b, x; // registers and small fields (x: frC, SH, SR, NB, a shift)
    uint8_t flags;
    uint32_t imm;
    uint32_t pc;
};
static_assert(sizeof(Step) == 16);

struct Fn;
enum class EdgeKind : uint8_t {
    Goto, // a block of this function: code[index]
    Tail, // a recompiled function: run it (a tail call, or a call)
    Jump, // anything else: c.jump(addr) (or c.call(addr) for a call)
};
struct Edge {
    EdgeKind kind;
    uint32_t index = 0;
    const Fn* fn = nullptr;
    uint32_t addr = 0;
};
struct Table {
    std::vector<std::pair<uint32_t, uint32_t>> cases; // (target, edge), sorted by target
    uint32_t next = 0;                                // edge when not taken
};

struct Fn {
    uint32_t entry = 0;
    uint32_t entry_index = 0;
    std::vector<Step> code;
    std::vector<Edge> edges;
    std::vector<Table> tables;
    std::vector<std::pair<uint32_t, uint32_t>> resume; // (return site or block start, index), sorted
};

// ---- State ----
std::vector<std::unique_ptr<Fn>> g_fns;
std::unordered_map<uint32_t, const Fn*> g_row_fn; // a table row's address: the function it runs
std::vector<FunctionEntry> g_functions, g_blocks;

// PowerPC MASK(mb, me), as the lifter.
uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    const uint32_t from = 0xFFFFFFFFu >> mb;
    const uint32_t to = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (from & to) : (from | to);
}

// The CpuState field an SPR is (the lifter's spr_field), as its offset; -1: none.
int spr_offset(uint32_t spr) {
    static const ppc::CpuState probe{};
    auto at = [](const void* p) {
        return static_cast<int>(static_cast<const char*>(p) - reinterpret_cast<const char*>(&probe));
    };
    switch (spr) {
    case 1:    return at(&probe.xer);
    case 8:    return at(&probe.lr);
    case 9:    return at(&probe.ctr);
    case 18:   return at(&probe.dsisr);
    case 19:   return at(&probe.dar);
    case 22:   return at(&probe.dec);
    case 25:   return at(&probe.sdr1);
    case 26:   return at(&probe.srr0);
    case 27:   return at(&probe.srr1);
    case 282:  return at(&probe.ear);
    case 287:  return at(&probe.pvr);
    case 976:  return at(&probe.dmiss);
    case 977:  return at(&probe.dcmp);
    case 978:  return at(&probe.hash1);
    case 979:  return at(&probe.hash2);
    case 980:  return at(&probe.imiss);
    case 981:  return at(&probe.icmp);
    case 982:  return at(&probe.rpa);
    case 1008: return at(&probe.hid0);
    case 1009: return at(&probe.hid1);
    case 1010: return at(&probe.iabr);
    default:   break;
    }
    if (spr >= 272 && spr <= 275) return at(&probe.sprg[spr - 272]);
    if (spr >= 528 && spr <= 535) return at(&probe.ibat[spr - 528]);
    if (spr >= 536 && spr <= 543) return at(&probe.dbat[spr - 536]);
    return -1;
}

uint32_t& spr_field(ppc::CpuState& s, uint32_t offset) {
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(&s) + offset);
}

// A bc-type condition (the lifter's condition()): CTR decremented first, then the CR bit; always true (and nothing
// changed) when BO says neither.
inline bool condition(ppc::CpuState& s, uint32_t bo, uint32_t bi) {
    bool ok = true;
    if (!(bo & 0x04)) ok = (bo & 0x02) ? --s.ctr == 0 : --s.ctr != 0;
    if (ok && !(bo & 0x10)) ok = s.crbit(bi) == ((bo & 0x08) != 0);
    return ok;
}
constexpr bool always(uint32_t bo) { return (bo & 0x14) == 0x14; }

// ---- Decoding: one function, as the lifter's Emitter writes it ----
class Decoder {
public:
    Decoder(const analysis::CodeImage& image, const analysis::Analysis& an, const std::set<uint32_t>& compiled,
            const std::set<uint32_t>& hooks, const std::unordered_map<uint32_t, Fn*>& fns, interp::Report& report)
        : image_(image), an_(an), compiled_(compiled), hooks_(hooks), fns_(fns), report_(report) {}

    void function(const analysis::Function& af, Fn& fn) {
        fn_ = &fn;
        blocks_.clear();
        blocks_.insert(af.blocks.begin(), af.blocks.end());
        block_index_.clear();
        gotos_.clear();
        std::map<uint32_t, uint32_t> sites; // return site -> index (or block start, resolved below)
        std::vector<uint32_t> block_sites;  // return sites at the next block
        for (uint32_t start : af.blocks) block(an_.blocks.at(start), sites, block_sites);
        for (const auto& [e, target] : gotos_) fn.edges[e].index = block_index_.at(target);
        for (uint32_t site : block_sites) sites.emplace(site, block_index_.at(site));
        // Resuming: return sites first, then every block start (the lifter's switch).
        std::map<uint32_t, uint32_t> resume = sites;
        for (uint32_t start : af.blocks) resume.emplace(start, block_index_.at(start));
        fn.resume.assign(resume.begin(), resume.end());
        fn.entry_index = block_index_.at(af.entry);
        sites_found.clear();
        for (const auto& [site, index] : sites) sites_found.push_back(site);
    }

    std::vector<uint32_t> sites_found; // the last function's return sites

private:
    Step& emit(uint16_t kind, uint32_t pc) {
        fn_->code.push_back(Step{kind, 0, 0, 0, 0, 0, 0, pc});
        return fn_->code.back();
    }

    // Transfer to `target` (the lifter's edge()).
    uint32_t edge(uint32_t target) {
        Edge e{};
        if (blocks_.contains(target)) {
            e.kind = EdgeKind::Goto;
            gotos_.emplace_back(static_cast<uint32_t>(fn_->edges.size()), target);
        } else if (compiled_.contains(target)) {
            e.kind = EdgeKind::Tail;
            e.fn = fns_.at(target);
        } else {
            e.kind = EdgeKind::Jump;
        }
        e.addr = target;
        fn_->edges.push_back(e);
        return static_cast<uint32_t>(fn_->edges.size() - 1);
    }
    // The function a call goes to (the lifter's call()): run here when recompiled, else c.call.
    uint32_t callee(uint32_t target) {
        Edge e{};
        e.kind = compiled_.contains(target) ? EdgeKind::Tail : EdgeKind::Jump;
        if (e.kind == EdgeKind::Tail) e.fn = fns_.at(target);
        e.addr = target;
        fn_->edges.push_back(e);
        return static_cast<uint32_t>(fn_->edges.size() - 1);
    }

    void block(const analysis::Block& b, std::map<uint32_t, uint32_t>& sites, std::vector<uint32_t>& block_sites) {
        ++report_.blocks;
        block_index_[b.start] = static_cast<uint32_t>(fn_->code.size());
        std::vector<Instruction> code;
        for (uint32_t pc = b.start; pc < b.end; pc += 4) code.push_back(ppc::decode(image_.read32(pc).value_or(0)));
        Step& tick = emit(kTick, b.start);
        tick.imm = ppc::block_cycles(code);
        if (hooks_.contains(b.start)) tick.flags = kHook;
        for (uint32_t pc = b.start; pc < b.end; pc += 4) {
            const Instruction in = code[(pc - b.start) / 4];
            ++report_.instructions;
            switch (in.op) {
            case Op::B: case Op::Bc: case Op::Bclr: case Op::Bcctr: case Op::Rfi:
                branch(in, pc, b);
                if (in.lk() && in.op != Op::Rfi) {
                    const uint32_t site = pc + 4;
                    if (site < b.end) sites.emplace(site, static_cast<uint32_t>(fn_->code.size()));
                    else if (blocks_.contains(site)) block_sites.push_back(site);
                }
                break;
            default:
                instruction(in, pc);
                break;
            }
        }
        switch (b.exit) {
        case analysis::Exit::Fallthrough: emit(kEdge, b.end).imm = edge(b.end); break;
        case analysis::Exit::OutOfImage:
        case analysis::Exit::Trap:        emit(kMissing, b.end).imm = b.end; break;
        default: break;
        }
    }

    void branch(const Instruction& in, uint32_t pc, const analysis::Block& b) {
        const uint32_t next = pc + 4;
        if (in.op == Op::Rfi) {
            emit(kRfi, pc);
            return;
        }
        // (b has no BO: always)
        const uint8_t bo = in.op == Op::B ? 0x14 : static_cast<uint8_t>(in.bo());
        const uint8_t bi = in.op == Op::B ? 0 : static_cast<uint8_t>(in.bi());
        auto with_cond = [&](uint16_t kind) -> Step& {
            Step& st = emit(kind, pc);
            st.d = bo, st.a = bi;
            return st;
        };
        if (in.op == Op::B || in.op == Op::Bc) {
            const uint32_t target = *ppc::branch_target(in, pc);
            if (in.lk()) {
                if (target == next) {
                    with_cond(kLink);
                    return;
                }
                const uint32_t e = callee(target);
                with_cond(kCall).imm = e;
                return;
            }
            if (always(bo)) {
                emit(kEdge, pc).imm = edge(target);
                return;
            }
            const uint32_t taken = edge(target);
            edge(next);
            with_cond(kBc).imm = taken;
            return;
        }
        // bclr / bcctr
        const uint8_t reg = in.op == Op::Bclr ? 0 : 1;
        if (in.lk()) {
            with_cond(kCallReg).b = reg;
            return;
        }
        switch (b.exit) {
        case analysis::Exit::Return: {
            const uint32_t n = edge(next);
            with_cond(kRet).imm = n;
            break;
        }
        case analysis::Exit::Jump:
        case analysis::Exit::TailCall:
            if (b.successors.empty()) {
                const uint32_t n = edge(next);
                Step& st = with_cond(kRegJump);
                st.b = reg, st.imm = n;
            } else {
                const uint32_t e = edge(b.successors[0]);
                edge(next);
                with_cond(kRegEdge).imm = e;
            }
            break;
        case analysis::Exit::JumpTable: {
            Table t;
            const size_t targets = b.successors.size() - (always(bo) ? 0 : 1);
            for (size_t i = 0; i < targets; ++i) {
                const uint32_t e = edge(b.successors[i]);
                t.cases.emplace_back(b.successors[i], e);
            }
            std::sort(t.cases.begin(), t.cases.end());
            t.next = edge(next);
            fn_->tables.push_back(std::move(t));
            Step& st = with_cond(kRegTable);
            st.b = reg, st.imm = static_cast<uint32_t>(fn_->tables.size() - 1);
            break;
        }
        default: {
            const uint32_t n = edge(next);
            Step& st = with_cond(kRegJump);
            st.b = reg, st.imm = n;
            break;
        }
        }
    }

    void instruction(const Instruction& in, uint32_t pc) {
        Step& st = emit(static_cast<uint16_t>(in.op), pc);
        st.d = static_cast<uint8_t>(in.rd()), st.a = static_cast<uint8_t>(in.ra()), st.b = static_cast<uint8_t>(in.rb());
        st.x = static_cast<uint8_t>(in.rc());
        st.flags = static_cast<uint8_t>((in.record() ? kRc : 0) | (in.oe() ? kOe : 0));
        st.imm = static_cast<uint32_t>(in.simm());
        switch (in.op) {
        case Op::Addi:
            if (in.ra() == 0) st.kind = kLi;
            break;
        case Op::Addis:
            st.imm = static_cast<uint32_t>(in.simm()) << 16;
            if (in.ra() == 0) st.kind = kLis;
            break;
        case Op::Cmp: case Op::Cmpl: case Op::Cmpi: st.d = static_cast<uint8_t>(in.crfd()); break;
        case Op::Cmpli: st.d = static_cast<uint8_t>(in.crfd()), st.imm = in.uimm(); break;
        case Op::AndiRc: case Op::Ori: case Op::Xori: st.imm = in.uimm(); break;
        case Op::AndisRc: case Op::Oris: case Op::Xoris: st.imm = in.uimm() << 16; break;
        case Op::Rlwinm: case Op::Rlwimi: case Op::Rlwnm:
            st.imm = ppc_mask(in.mb(), in.me());
            st.x = static_cast<uint8_t>(in.sh());
            break;
        case Op::Srawi: st.x = static_cast<uint8_t>(in.sh()); break;
        case Op::Lswi: case Op::Stswi: st.imm = in.nb() ? in.nb() : 32; break;
        case Op::Mcrf: case Op::Mcrfs: st.d = static_cast<uint8_t>(in.crfd()), st.a = static_cast<uint8_t>(in.crfs()); break;
        case Op::Mcrxr: case Op::Fcmpu: case Op::Fcmpo: st.d = static_cast<uint8_t>(in.crfd()); break;
        case Op::Mtcrf: {
            uint32_t mask = 0;
            for (uint32_t f = 0; f < 8; ++f)
                if (in.crm() & (0x80u >> f)) mask |= 0xF0000000u >> (4 * f);
            st.imm = mask;
            break;
        }
        case Op::Mfsr: case Op::Mtsr: st.x = static_cast<uint8_t>(in.sr()); break;
        case Op::Mftb:
        case Op::Mfspr: {
            const uint32_t spr = in.spr();
            if (spr == 268) st.kind = kMfTbl;
            else if (spr == 22 && in.op == Op::Mfspr) st.kind = kMfDec;
            else if (spr == 269) st.kind = kMfTbu;
            else if (const int at = in.op == Op::Mfspr ? spr_offset(spr) : -1; at >= 0) st.kind = kMfSpr, st.imm = static_cast<uint32_t>(at);
            else st.kind = kUnknownSpr, st.imm = spr;
            break;
        }
        case Op::Mtspr: {
            const uint32_t spr = in.spr();
            if (spr == 268 || spr == 284) st.kind = kMtTbl;
            else if (spr == 22) st.kind = kMtDec;
            else if (spr == 269 || spr == 285) st.kind = kMtTbu;
            else if (spr == 287) st.kind = kNop; // PVR is read-only
            else if (const int at = spr_offset(spr); at >= 0) st.kind = kMtSpr, st.imm = static_cast<uint32_t>(at);
            else st.kind = kUnknownSpr, st.imm = spr;
            break;
        }
        case Op::Tw: case Op::Twi:
            if (in.to() == 31) st.kind = kTrapAlways;
            break;
        case Op::Dcbf: case Op::Dcbi: case Op::Dcbst: case Op::Dcbt: case Op::Dcbtst: case Op::Icbi:
        case Op::Eieio: case Op::Isync: case Op::Sync:
        case Op::Tlbia: case Op::Tlbie: case Op::Tlbld: case Op::Tlbli: case Op::Tlbsync:
            st.kind = kNop;
            break;
        case Op::Mtfsf: {
            uint32_t mask = 0;
            for (uint32_t f = 0; f < 8; ++f)
                if (in.fm() & (0x80u >> f)) mask |= 0xF0000000u >> (4 * f);
            st.imm = mask;
            break;
        }
        case Op::Mtfsfi: st.x = static_cast<uint8_t>(28 - 4 * in.crfd()), st.imm = in.fpimm(); break;
        case Op::Mtfsb0: case Op::Mtfsb1: st.imm = 0x80000000u >> in.rd(); break;
        case Op::Eciwx: case Op::Ecowx: st.kind = kUnimplemented, st.imm = in.raw; break;
        case Op::Invalid: st.imm = in.raw; break;
        default: break;
        }
    }

    const analysis::CodeImage& image_;
    const analysis::Analysis& an_;
    const std::set<uint32_t>& compiled_;
    const std::set<uint32_t>& hooks_;
    const std::unordered_map<uint32_t, Fn*>& fns_;
    interp::Report& report_;
    Fn* fn_ = nullptr;
    std::set<uint32_t> blocks_;
    std::unordered_map<uint32_t, uint32_t> block_index_;
    std::vector<std::pair<uint32_t, uint32_t>> gotos_; // (edge, block start)
};

// ---- Running ----
void run(Context& c, const Fn* f);

// The table's function for every row (Context::call and resume_block note the row they run).
void thunk(Context& c) { run(c, g_row_fn.at(c.entered->addr)); }

// Where a function starts: at a return site or block when Context::call or resume_block asked (resume_at), else at
// its entry (the lifter's prologue).
uint32_t start_of(Context& c, const Fn* f) {
    if (c.resume_at) [[unlikely]] {
        const uint32_t at = c.resume_at;
        c.resume_at = 0;
        auto it = std::lower_bound(f->resume.begin(), f->resume.end(), std::make_pair(at, 0u));
        if (it != f->resume.end() && it->first == at) return it->second;
    }
    return f->entry_index;
}

void run(Context& c, const Fn* f) {
    auto& s = c.cpu;
    const Step* code = f->code.data();
    uint32_t i = start_of(c, f);
    // An edge taken: true to go on (a block here, or a function's in this loop: a tail call), false when it went
    // through c.jump (return then).
    auto take = [&](uint32_t edge) {
        const Edge& e = f->edges[edge];
        if (e.kind == EdgeKind::Goto) {
            i = e.index;
            return true;
        }
        if (e.kind == EdgeKind::Tail) {
            f = e.fn;
            code = f->code.data();
            i = start_of(c, f);
            return true;
        }
        c.jump(e.addr);
        return false;
    };
    // After a call: the callee returned elsewhere: follow it there, or return when an outer call waits for it.
    auto returned = [&](uint32_t next) { return c.returned_to != next && c.follow_return(next); };

#define GD s.gpr[o.d]
#define GA s.gpr[o.a]
#define GB s.gpr[o.b]
#define GA0 (o.a ? s.gpr[o.a] : 0u)
#define FD s.fpr[o.d]
#define FA s.fpr[o.a]
#define FB s.fpr[o.b]
#define FC s.fpr[o.x]
#define RC (o.flags & kRc)
#define OE ((o.flags & kOe) != 0)
#define CR0(v) do { if (RC) ops::set_cr0(s, v); } while (0)
#define CR1 do { if (RC) s.set_crf(1, s.fpscr >> 28); } while (0)
#define OPC(name) case static_cast<uint16_t>(Op::name)
    for (;;) {
        const Step& o = code[i++];
        switch (o.kind) {
        // ---- Blocks and branches ----
        case kTick:
            c.tick(o.pc, o.imm);
            if (o.flags & kHook) c.hook(o.pc);
            break;
        case kEdge:
            if (!take(o.imm)) return;
            break;
        case kBc:
            if (!take(condition(s, o.d, o.a) ? o.imm : o.imm + 1)) return;
            break;
        case kCall: {
            const uint32_t next = o.pc + 4;
            s.lr = next;
            if (condition(s, o.d, o.a)) {
                const Edge& e = f->edges[o.imm];
                c.push_return(next);
                if (e.kind == EdgeKind::Tail) run(c, e.fn);
                else c.call(e.addr);
                c.pop_return();
                if (returned(next)) return;
            }
            break;
        }
        case kLink:
            s.lr = o.pc + 4;
            (void)condition(s, o.d, o.a);
            break;
        case kCallReg: {
            const uint32_t next = o.pc + 4;
            const uint32_t t = (o.b ? s.ctr : s.lr) & ~3u;
            s.lr = next;
            if (condition(s, o.d, o.a)) {
                c.push_return(next);
                c.call(t);
                c.pop_return();
                if (returned(next)) return;
            }
            break;
        }
        case kRet:
            if (condition(s, o.d, o.a)) {
                c.returned_to = s.lr;
                return;
            }
            if (!take(o.imm)) return;
            break;
        case kRegJump:
            if (condition(s, o.d, o.a)) {
                c.jump((o.b ? s.ctr : s.lr) & ~3u);
                return;
            }
            if (!take(o.imm)) return;
            break;
        case kRegEdge:
            if (!take(condition(s, o.d, o.a) ? o.imm : o.imm + 1)) return;
            break;
        case kRegTable: {
            const Table& t = f->tables[o.imm];
            if (condition(s, o.d, o.a)) {
                const uint32_t target = (o.b ? s.ctr : s.lr) & ~3u;
                auto it = std::lower_bound(t.cases.begin(), t.cases.end(), std::make_pair(target, 0u));
                if (it != t.cases.end() && it->first == target) {
                    if (!take(it->second)) return;
                    break;
                }
                c.jump(target);
                return;
            }
            if (!take(t.next)) return;
            break;
        }
        case kRfi:
            c.rfi();
            return;
        case kMissing:
            c.missing(o.imm);

        // ---- Integer arithmetic ----
        case kLi: GD = o.imm; break;
        case kLis: GD = o.imm; break;
        OPC(Addi): GD = GA0 + o.imm; break;
        OPC(Addis): GD = GA0 + o.imm; break;
        OPC(Addic): GD = ops::addx(s, GA, o.imm, 0, true, false); break;
        OPC(AddicRc): GD = ops::addx(s, GA, o.imm, 0, true, false); ops::set_cr0(s, GD); break;
        OPC(Subfic): GD = ops::addx(s, ~GA, o.imm, 1, true, false); break;
        OPC(Mulli): GD = GA * o.imm; break;
        OPC(Add): GD = ops::addx(s, GA, GB, 0, false, OE); CR0(GD); break;
        OPC(Addc): GD = ops::addx(s, GA, GB, 0, true, OE); CR0(GD); break;
        OPC(Adde): GD = ops::addx(s, GA, GB, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Addme): GD = ops::addx(s, GA, 0xFFFFFFFFu, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Addze): GD = ops::addx(s, GA, 0u, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Subf): GD = ops::addx(s, ~GA, GB, 1, false, OE); CR0(GD); break;
        OPC(Subfc): GD = ops::addx(s, ~GA, GB, 1, true, OE); CR0(GD); break;
        OPC(Subfe): GD = ops::addx(s, ~GA, GB, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Subfme): GD = ops::addx(s, ~GA, 0xFFFFFFFFu, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Subfze): GD = ops::addx(s, ~GA, 0u, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Neg): GD = ops::addx(s, ~GA, 0u, 1, false, OE); CR0(GD); break;
        OPC(Mullw): GD = ops::mullw(s, GA, GB, OE); CR0(GD); break;
        OPC(Mulhw): GD = ops::mulhw(GA, GB); CR0(GD); break;
        OPC(Mulhwu): GD = ops::mulhwu(GA, GB); CR0(GD); break;
        OPC(Divw): GD = ops::divw(s, GA, GB, OE); CR0(GD); break;
        OPC(Divwu): GD = ops::divwu(s, GA, GB, OE); CR0(GD); break;

        // ---- Compare ----
        OPC(Cmp): ops::cmps(s, o.d, static_cast<int32_t>(GA), static_cast<int32_t>(GB)); break;
        OPC(Cmpi): ops::cmps(s, o.d, static_cast<int32_t>(GA), static_cast<int32_t>(o.imm)); break;
        OPC(Cmpl): ops::cmpu(s, o.d, GA, GB); break;
        OPC(Cmpli): ops::cmpu(s, o.d, GA, o.imm); break;

        // ---- Logical (rA from rS) ----
        OPC(And): GA = GD & GB; CR0(GA); break;
        OPC(Andc): GA = GD & ~GB; CR0(GA); break;
        OPC(Or): GA = GD | GB; CR0(GA); break;
        OPC(Orc): GA = GD | ~GB; CR0(GA); break;
        OPC(Xor): GA = GD ^ GB; CR0(GA); break;
        OPC(Nand): GA = ~(GD & GB); CR0(GA); break;
        OPC(Nor): GA = ~(GD | GB); CR0(GA); break;
        OPC(Eqv): GA = ~(GD ^ GB); CR0(GA); break;
        OPC(AndiRc): GA = GD & o.imm; ops::set_cr0(s, GA); break;
        OPC(AndisRc): GA = GD & o.imm; ops::set_cr0(s, GA); break;
        OPC(Ori): GA = GD | o.imm; break;
        OPC(Oris): GA = GD | o.imm; break;
        OPC(Xori): GA = GD ^ o.imm; break;
        OPC(Xoris): GA = GD ^ o.imm; break;
        OPC(Cntlzw): GA = ops::cntlzw(GD); CR0(GA); break;
        OPC(Extsb): GA = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(GD))); CR0(GA); break;
        OPC(Extsh): GA = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(GD))); CR0(GA); break;

        // ---- Rotate and shift ----
        OPC(Rlwinm): GA = ops::rotl(GD, o.x) & o.imm; CR0(GA); break;
        OPC(Rlwimi): GA = (ops::rotl(GD, o.x) & o.imm) | (GA & ~o.imm); CR0(GA); break;
        OPC(Rlwnm): GA = ops::rotl(GD, GB) & o.imm; CR0(GA); break;
        OPC(Slw): GA = ops::slw(GD, GB); CR0(GA); break;
        OPC(Srw): GA = ops::srw(GD, GB); CR0(GA); break;
        OPC(Sraw): GA = ops::sraw(s, GD, GB & 0x3F); CR0(GA); break;
        OPC(Srawi): GA = ops::sraw(s, GD, o.x); CR0(GA); break;

        // ---- Loads (rD = value, then rA = ea for the updating forms) ----
#define LOAD_D(value) { const uint32_t ea = GA0 + o.imm; GD = value; }
#define LOAD_DU(value) { const uint32_t ea = GA0 + o.imm; GD = value; GA = ea; }
#define LOAD_X(value) { const uint32_t ea = GA0 + GB; GD = value; }
#define LOAD_XU(value) { const uint32_t ea = GA0 + GB; GD = value; GA = ea; }
        OPC(Lbz): LOAD_D(c.read8(ea)) break;
        OPC(Lbzu): LOAD_DU(c.read8(ea)) break;
        OPC(Lbzx): LOAD_X(c.read8(ea)) break;
        OPC(Lbzux): LOAD_XU(c.read8(ea)) break;
        OPC(Lhz): LOAD_D(c.read16(ea)) break;
        OPC(Lhzu): LOAD_DU(c.read16(ea)) break;
        OPC(Lhzx): LOAD_X(c.read16(ea)) break;
        OPC(Lhzux): LOAD_XU(c.read16(ea)) break;
        OPC(Lha): LOAD_D(static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))) break;
        OPC(Lhau): LOAD_DU(static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))) break;
        OPC(Lhax): LOAD_X(static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))) break;
        OPC(Lhaux): LOAD_XU(static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))) break;
        OPC(Lwz): LOAD_D(c.read32(ea)) break;
        OPC(Lwzu): LOAD_DU(c.read32(ea)) break;
        OPC(Lwzx): LOAD_X(c.read32(ea)) break;
        OPC(Lwzux): LOAD_XU(c.read32(ea)) break;
        OPC(Lhbrx): LOAD_X(ops::bswap16(c.read16(ea))) break;
        OPC(Lwbrx): LOAD_X(m3::bswap32(c.read32(ea))) break;
        OPC(Lwarx): {
            const uint32_t ea = GA0 + GB;
            s.reserve = true;
            s.reserve_addr = ea;
            GD = c.read32(ea);
            break;
        }
        OPC(Lmw): {
            uint32_t ea = GA0 + o.imm;
            for (uint32_t r = o.d; r < 32; ++r, ea += 4) s.gpr[r] = c.read32(ea);
            break;
        }
        OPC(Lswi): c.load_string(o.d, GA0, o.imm); break;
        OPC(Lswx): c.load_string(o.d, GA0 + GB, s.xer & 0x7F); break;

        // ---- Stores (rS in d) ----
#define STORE_D(action) { const uint32_t ea = GA0 + o.imm; action; }
#define STORE_DU(action) { const uint32_t ea = GA0 + o.imm; action; GA = ea; }
#define STORE_X(action) { const uint32_t ea = GA0 + GB; action; }
#define STORE_XU(action) { const uint32_t ea = GA0 + GB; action; GA = ea; }
        OPC(Stb): STORE_D(c.write8(ea, static_cast<uint8_t>(GD))) break;
        OPC(Stbu): STORE_DU(c.write8(ea, static_cast<uint8_t>(GD))) break;
        OPC(Stbx): STORE_X(c.write8(ea, static_cast<uint8_t>(GD))) break;
        OPC(Stbux): STORE_XU(c.write8(ea, static_cast<uint8_t>(GD))) break;
        OPC(Sth): STORE_D(c.write16(ea, static_cast<uint16_t>(GD))) break;
        OPC(Sthu): STORE_DU(c.write16(ea, static_cast<uint16_t>(GD))) break;
        OPC(Sthx): STORE_X(c.write16(ea, static_cast<uint16_t>(GD))) break;
        OPC(Sthux): STORE_XU(c.write16(ea, static_cast<uint16_t>(GD))) break;
        OPC(Stw): STORE_D(c.write32(ea, GD)) break;
        OPC(Stwu): STORE_DU(c.write32(ea, GD)) break;
        OPC(Stwx): STORE_X(c.write32(ea, GD)) break;
        OPC(Stwux): STORE_XU(c.write32(ea, GD)) break;
        OPC(Sthbrx): STORE_X(c.write16(ea, static_cast<uint16_t>(ops::bswap16(GD)))) break;
        OPC(Stwbrx): STORE_X(c.write32(ea, m3::bswap32(GD))) break;
        OPC(StwcxRc): {
            const uint32_t ea = GA0 + GB;
            if (s.reserve) {
                c.write32(ea, GD);
                s.set_crf(0, 2u | ops::so(s));
            } else {
                s.set_crf(0, ops::so(s));
            }
            s.reserve = false;
            break;
        }
        OPC(Stmw): {
            uint32_t ea = GA0 + o.imm;
            for (uint32_t r = o.d; r < 32; ++r, ea += 4) c.write32(ea, s.gpr[r]);
            break;
        }
        OPC(Stswi): c.store_string(o.d, GA0, o.imm); break;
        OPC(Stswx): c.store_string(o.d, GA0 + GB, s.xer & 0x7F); break;

        // ---- Caches, condition register ----
        OPC(Dcbz): c.dcbz(GA0 + GB); break;
        case kNop: break;
        OPC(Crand): s.set_crbit(o.d, s.crbit(o.a) && s.crbit(o.b)); break;
        OPC(Crandc): s.set_crbit(o.d, s.crbit(o.a) && !s.crbit(o.b)); break;
        OPC(Creqv): s.set_crbit(o.d, s.crbit(o.a) == s.crbit(o.b)); break;
        OPC(Crnand): s.set_crbit(o.d, !(s.crbit(o.a) && s.crbit(o.b))); break;
        OPC(Crnor): s.set_crbit(o.d, !(s.crbit(o.a) || s.crbit(o.b))); break;
        OPC(Cror): s.set_crbit(o.d, s.crbit(o.a) || s.crbit(o.b)); break;
        OPC(Crorc): s.set_crbit(o.d, s.crbit(o.a) || !s.crbit(o.b)); break;
        OPC(Crxor): s.set_crbit(o.d, s.crbit(o.a) != s.crbit(o.b)); break;
        OPC(Mcrf): s.set_crf(o.d, s.crf(o.a)); break;
        OPC(Mcrxr): s.set_crf(o.d, s.xer >> 28); s.xer &= 0x0FFFFFFFu; break;
        OPC(Mfcr): GD = s.cr; break;
        OPC(Mtcrf): s.cr = (s.cr & ~o.imm) | (GD & o.imm); break;

        // ---- Processor control ----
        OPC(Mfmsr): GD = s.msr; break;
        OPC(Mtmsr): s.msr = GD; c.msr_changed(); break;
        OPC(Mfsr): GD = s.sr[o.x]; break;
        OPC(Mtsr): s.sr[o.x] = GD; break;
        OPC(Mfsrin): GD = s.sr[GB >> 28]; break;
        OPC(Mtsrin): s.sr[GB >> 28] = GD; break;
        case kMfTbl: GD = c.read_tbl(); break;
        case kMfTbu: GD = c.read_tbu(); break;
        case kMfDec: GD = c.read_dec(); break;
        case kMtTbl: c.write_tbl(GD); break;
        case kMtTbu: c.write_tbu(GD); break;
        case kMtDec: c.write_dec(GD); break;
        case kMfSpr: GD = spr_field(s, o.imm); break;
        case kMtSpr: spr_field(s, o.imm) = GD; break;
        case kUnknownSpr: c.unknown_spr(o.pc, o.imm);

        // ---- Traps and system calls (TO in d) ----
        case kTrapAlways: c.trap(o.pc); break;
        OPC(Tw): if (ops::trap_if(o.d, GA, GB)) c.trap(o.pc); break;
        OPC(Twi): if (ops::trap_if(o.d, GA, o.imm)) c.trap(o.pc); break;
        OPC(Sc): c.system_call(o.pc); break;
        OPC(Invalid): c.invalid(o.pc, o.imm);
        case kUnimplemented: c.unimplemented(o.pc, o.imm);

        // ---- Floating-point loads and stores (frD / frS in d) ----
        OPC(Lfs): { const uint32_t ea = GA0 + o.imm; FD = ops::single_to_double(c.read32(ea)); break; }
        OPC(Lfsu): { const uint32_t ea = GA0 + o.imm; FD = ops::single_to_double(c.read32(ea)); GA = ea; break; }
        OPC(Lfsx): { const uint32_t ea = GA0 + GB; FD = ops::single_to_double(c.read32(ea)); break; }
        OPC(Lfsux): { const uint32_t ea = GA0 + GB; FD = ops::single_to_double(c.read32(ea)); GA = ea; break; }
        OPC(Lfd): { const uint32_t ea = GA0 + o.imm; FD = c.read64(ea); break; }
        OPC(Lfdu): { const uint32_t ea = GA0 + o.imm; FD = c.read64(ea); GA = ea; break; }
        OPC(Lfdx): { const uint32_t ea = GA0 + GB; FD = c.read64(ea); break; }
        OPC(Lfdux): { const uint32_t ea = GA0 + GB; FD = c.read64(ea); GA = ea; break; }
        OPC(Stfs): { const uint32_t ea = GA0 + o.imm; c.write32(ea, ops::double_to_single(FD)); break; }
        OPC(Stfsu): { const uint32_t ea = GA0 + o.imm; c.write32(ea, ops::double_to_single(FD)); GA = ea; break; }
        OPC(Stfsx): { const uint32_t ea = GA0 + GB; c.write32(ea, ops::double_to_single(FD)); break; }
        OPC(Stfsux): { const uint32_t ea = GA0 + GB; c.write32(ea, ops::double_to_single(FD)); GA = ea; break; }
        OPC(Stfd): { const uint32_t ea = GA0 + o.imm; c.write64(ea, FD); break; }
        OPC(Stfdu): { const uint32_t ea = GA0 + o.imm; c.write64(ea, FD); GA = ea; break; }
        OPC(Stfdx): { const uint32_t ea = GA0 + GB; c.write64(ea, FD); break; }
        OPC(Stfdux): { const uint32_t ea = GA0 + GB; c.write64(ea, FD); GA = ea; break; }
        OPC(Stfiwx): { const uint32_t ea = GA0 + GB; c.write32(ea, static_cast<uint32_t>(FD)); break; }

        // ---- Floating-point arithmetic (fmul and the multiply-adds: frC the multiplier) ----
#define DBL(expr) { FD = ops::bits(expr); ops::set_fprf(s, FD, false); CR1; break; }
#define SGL(expr) { FD = ops::round_single(expr); ops::set_fprf(s, FD, true); CR1; break; }
#define RAW(expr) { FD = (expr); CR1; break; }
#define fa ops::f64(FA)
#define fb ops::f64(FB)
#define fc ops::f64(FC)
        OPC(Fadd): DBL(fa + fb)
        OPC(Fadds): SGL(fa + fb)
        OPC(Fsub): DBL(fa - fb)
        OPC(Fsubs): SGL(fa - fb)
        OPC(Fmul): DBL(fa * fc)
        OPC(Fmuls): SGL(fa * fc)
        OPC(Fdiv): DBL(fa / fb)
        OPC(Fdivs): SGL(fa / fb)
        OPC(Fmadd): DBL(std::fma(fa, fc, fb))
        OPC(Fmadds): SGL(std::fma(fa, fc, fb))
        OPC(Fmsub): DBL(std::fma(fa, fc, -fb))
        OPC(Fmsubs): SGL(std::fma(fa, fc, -fb))
        OPC(Fnmadd): DBL(-std::fma(fa, fc, fb))
        OPC(Fnmadds): SGL(-std::fma(fa, fc, fb))
        OPC(Fnmsub): DBL(-std::fma(fa, fc, -fb))
        OPC(Fnmsubs): SGL(-std::fma(fa, fc, -fb))
        OPC(Fsqrt): DBL(std::sqrt(fb))
        OPC(Fsqrts): SGL(std::sqrt(fb))
        OPC(Fres): SGL(1.0 / fb)
        OPC(Frsqrte): DBL(1.0 / std::sqrt(fb))
        OPC(Fsel): RAW(fa >= 0.0 ? FC : FB)
        OPC(Fmr): RAW(FB)
        OPC(Fneg): RAW(FB ^ 0x8000000000000000u)
        OPC(Fabs): RAW(FB & 0x7FFFFFFFFFFFFFFFu)
        OPC(Fnabs): RAW(FB | 0x8000000000000000u)
        OPC(Frsp): SGL(fb)
        OPC(Fctiw): RAW(ops::fctiw(s, FB, false))
        OPC(Fctiwz): RAW(ops::fctiw(s, FB, true))
        OPC(Fcmpu): ops::fcmp(s, o.d, FA, FB); break;
        OPC(Fcmpo): ops::fcmp(s, o.d, FA, FB); break;
        OPC(Mffs): RAW(0xFFF8000000000000u | s.fpscr)
        OPC(Mcrfs): ops::mcrfs(s, o.d, o.a); break;
        OPC(Mtfsf):
            s.fpscr = (s.fpscr & ~o.imm) | (static_cast<uint32_t>(FB) & o.imm);
            ops::fpscr_written(s);
            CR1;
            break;
        OPC(Mtfsfi):
            s.fpscr = (s.fpscr & ~(0xFu << o.x)) | (o.imm << o.x);
            ops::fpscr_written(s);
            CR1;
            break;
        OPC(Mtfsb0): s.fpscr &= ~o.imm; ops::fpscr_written(s); CR1; break;
        OPC(Mtfsb1): s.fpscr |= o.imm; ops::fpscr_written(s); CR1; break;

        default:
            // (an instruction the lifter writes as unimplemented: the decoder keeps its raw word nowhere, so look
            // it up again)
            c.unimplemented(o.pc, 0);
        }
    }
#undef GD
#undef GA
#undef GB
#undef GA0
#undef FD
#undef FA
#undef FB
#undef FC
#undef RC
#undef OE
#undef CR0
#undef CR1
#undef OPC
#undef LOAD_D
#undef LOAD_DU
#undef LOAD_X
#undef LOAD_XU
#undef STORE_D
#undef STORE_DU
#undef STORE_X
#undef STORE_XU
#undef DBL
#undef SGL
#undef RAW
#undef fa
#undef fb
#undef fc
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

namespace interp {

std::span<const recomp::FunctionEntry> functions() { return g_functions; }
std::span<const recomp::FunctionEntry> blocks() { return g_blocks; }

Report prepare(const m3::GameSpec& game, const m3::RomSet& roms, bool verbose) {
    const auto start = std::chrono::steady_clock::now();
    Report report;
    g_fns.clear(), g_row_fn.clear(), g_functions.clear(), g_blocks.clear();

    // The analysis, as ppcrecomp and the JIT: the boot ROM's functions from the vectors, the game's with pointers
    // scanned too.
    const analysis::BootImage boot = analysis::make_boot_image(game, roms);
    analysis::Options base;
    for (const analysis::Segment& seg : boot.image.segments())
        if (seg.base < m3::kWorkRamSize) base.pointer_segments.push_back(seg.name);
    analysis::Options ram_options = base;
    ram_options.scan_pointers = true;
    const analysis::Analysis crom_an = analysis::analyze(boot.image, boot.entries, base);
    const analysis::Analysis ram_an = analysis::analyze(boot.image, boot.entries, ram_options);
    const analysis::Analysis* analyses[] = {&crom_an, &ram_an};
    const char* segment_of[] = {"crom", "ram"};
    std::set<uint32_t> compiled[2];
    for (size_t l = 0; l < 2; ++l)
        for (const auto& [entry, fn] : analyses[l]->functions) {
            const analysis::Segment* seg = boot.image.segment_at(entry);
            if (seg && seg->name == segment_of[l] && !fn.blocks.empty()) compiled[l].insert(entry);
        }
    const std::set<uint32_t> hooks(game.hooks.begin(), game.hooks.end());
    report.analysis_s = seconds_since(start);

    // The functions (made first: calls and tail calls point at them).
    const auto t = std::chrono::steady_clock::now();
    std::unordered_map<uint32_t, Fn*> fns;
    for (size_t l = 0; l < 2; ++l)
        for (uint32_t entry : compiled[l]) {
            g_fns.push_back(std::make_unique<Fn>());
            g_fns.back()->entry = entry;
            fns[entry] = g_fns.back().get();
        }
    for (size_t l = 0; l < 2; ++l) {
        // The table's rows, as the lifter's (each lift its own table, merged as game_setup.hpp merges them):
        // functions, then return sites and block starts with the first function that has them.
        std::map<uint32_t, uint32_t> resume_sites, block_sites;
        Decoder decoder(boot.image, *analyses[l], compiled[l], hooks, fns, report);
        for (uint32_t entry : compiled[l]) {
            const analysis::Function& af = analyses[l]->functions.at(entry);
            decoder.function(af, *fns.at(entry));
            ++report.functions;
            for (uint32_t site : decoder.sites_found) resume_sites.emplace(site, entry);
            for (uint32_t b : af.blocks)
                if (!std::binary_search(decoder.sites_found.begin(), decoder.sites_found.end(), b))
                    block_sites.emplace(b, entry);
        }
        std::map<uint32_t, FunctionEntry> rows;
        for (uint32_t entry : compiled[l]) {
            rows.emplace(entry, FunctionEntry{entry, thunk, 0});
            g_row_fn[entry] = fns.at(entry);
        }
        for (const auto& [site, entry] : resume_sites)
            if (rows.emplace(site, FunctionEntry{site, thunk, site}).second) g_row_fn[site] = fns.at(entry);
        for (const auto& [addr, row] : rows) g_functions.push_back(row);
        for (const auto& [b, entry] : block_sites)
            if (!rows.contains(b)) {
                g_blocks.push_back(FunctionEntry{b, thunk, b});
                g_row_fn[b] = fns.at(entry);
            }
    }
    auto by_addr = [](const FunctionEntry& a, const FunctionEntry& b) { return a.addr < b.addr; };
    std::sort(g_functions.begin(), g_functions.end(), by_addr);
    std::sort(g_blocks.begin(), g_blocks.end(), by_addr);
    for (const auto& fn : g_fns) {
        report.steps += fn->code.size();
        report.bytes += fn->code.size() * sizeof(Step) + fn->edges.size() * sizeof(Edge) +
                        fn->resume.size() * sizeof(fn->resume[0]);
        for (const Table& tb : fn->tables) report.bytes += tb.cases.size() * sizeof(tb.cases[0]);
    }
    report.decode_s = seconds_since(t);
    report.total_s = seconds_since(start);
    if (verbose)
        std::printf("interp: prepared in %.2f s (analysis %.2f, decode %.2f): %zu functions, %zu blocks, "
                    "%zu instructions, %zu steps (%.1f MB)\n",
                    report.total_s, report.analysis_s, report.decode_s, report.functions, report.blocks,
                    report.instructions, report.steps, static_cast<double>(report.bytes) / 1e6);
    return report;
}

} // namespace interp

// The tables game_setup.hpp reads (the boot ROM's are in the game's).
namespace generated {
std::span<const recomp::FunctionEntry> game_ram_functions() { return interp::functions(); }
std::span<const recomp::FunctionEntry> game_ram_blocks() { return interp::blocks(); }
std::span<const recomp::FunctionEntry> boot_rom_functions() { return {}; }
std::span<const recomp::FunctionEntry> boot_rom_blocks() { return {}; }
} // namespace generated
