#include "analysis/control_flow.hpp"

#include "ppc/instruction.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace analysis {

using ppc::Form;
using ppc::Instruction;
using ppc::Op;

std::string_view entry_kind_name(EntryKind kind) {
    switch (kind) {
    case EntryKind::Seed:            return "seed";
    case EntryKind::Call:            return "call";
    case EntryKind::TableCall:       return "table call";
    case EntryKind::ConstantJump:    return "constant jump";
    case EntryKind::ExceptionReturn: return "rfi";
    case EntryKind::Pointer:         return "pointer";
    }
    return "?";
}

std::string_view exit_name(Exit exit) {
    switch (exit) {
    case Exit::Fallthrough: return "fallthrough";
    case Exit::Jump:        return "jump";
    case Exit::Branch:      return "branch";
    case Exit::Return:      return "return";
    case Exit::TailCall:    return "tail call";
    case Exit::JumpTable:   return "jump table";
    case Exit::Indirect:    return "indirect";
    case Exit::Rfi:         return "rfi";
    case Exit::Trap:        return "trap";
    case Exit::Invalid:     return "invalid";
    case Exit::OutOfImage:  return "out of image";
    }
    return "?";
}

std::string_view issue_kind_name(IssueKind kind) {
    switch (kind) {
    case IssueKind::InvalidInstruction: return "invalid instruction";
    case IssueKind::InvalidAfterCall:   return "invalid instruction after a call";
    case IssueKind::TargetOutsideImage: return "target outside the image";
    case IssueKind::UnresolvedJump:     return "unresolved jump";
    case IssueKind::UnresolvedCall:     return "unresolved call";
    case IssueKind::BadTableEntry:      return "bad table entry";
    }
    return "?";
}

uint32_t Analysis::instruction_count() const {
    uint32_t n = 0;
    for (const auto& [start, block] : blocks) n += block.instruction_count();
    return n;
}

const Block* Analysis::block_containing(uint32_t addr) const {
    auto it = blocks.upper_bound(addr);
    if (it == blocks.begin()) return nullptr;
    --it;
    return addr < it->second.end ? &it->second : nullptr;
}

namespace {

constexpr uint32_t kSprLr = 8;
constexpr uint32_t kSprCtr = 9;
constexpr uint32_t kSprSrr0 = 26;

// What the tracker knows about a register's value on the current path.
struct Val {
    enum Kind : uint8_t {
        Unknown,  // set before this path started (argument, saved register, the caller's LR)
        Computed, // produced on this path by an instruction that isn't modelled
        Loaded,   // loaded from memory (a saved LR, a function pointer, ...)
        Const,    // exactly `value`
        Index,    // i * stride, for 0 <= i < count
        Table,    // value + i * stride
        Entry,    // the word loaded from value + i * stride
    };
    Kind kind = Unknown;
    uint32_t value = 0;
    uint32_t stride = 1;
    uint32_t count = 0; // 0: no bound known
    bool exact = false; // count comes from a compare-and-branch, not just the bit width

    static Val of(Kind k) {
        Val v;
        v.kind = k;
        return v;
    }
    static Val constant(uint32_t x) {
        Val v;
        v.kind = Const;
        v.value = x;
        return v;
    }
    static Val index(uint32_t step, uint32_t n, bool is_exact) {
        Val v;
        v.kind = Index;
        v.stride = step;
        v.count = n;
        v.exact = is_exact;
        return v;
    }
    Val with_kind(Kind k, uint32_t base) const {
        Val v = *this;
        v.kind = k;
        v.value = base;
        return v;
    }
};

struct Compare {
    bool valid = false;
    uint32_t reg = 0;
    uint32_t imm = 0;
};

struct State {
    std::array<Val, 32> gpr{};
    Val lr, ctr, srr0;
    std::array<Compare, 8> cmp{}; // last unsigned compare-with-immediate per CR field
};

uint32_t bits_count(uint32_t width) { return width >= 32 ? 0 : uint32_t{1} << width; }

uint32_t rotl32(uint32_t x, uint32_t n) { return n == 0 ? x : (x << n) | (x >> (32 - n)); }

// PowerPC MASK(mb, me): ones from bit mb through bit me (bit 0 = MSB), wrapping.
uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    const uint32_t from = 0xFFFFFFFFu >> mb;
    const uint32_t to = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (from & to) : (from | to);
}

void set(State& s, uint32_t reg, Val v) {
    s.gpr[reg] = v;
    for (Compare& c : s.cmp)
        if (c.valid && c.reg == reg) c.valid = false;
}

Val add_imm(const Val& v, uint32_t imm) {
    switch (v.kind) {
    case Val::Const: return Val::constant(v.value + imm);
    case Val::Index: return v.with_kind(Val::Table, imm);
    case Val::Table: return v.with_kind(Val::Table, v.value + imm);
    default:         return Val::of(Val::Computed);
    }
}

Val add(const Val& a, const Val& b) {
    if (a.kind == Val::Const && b.kind == Val::Const) return Val::constant(a.value + b.value);
    if (a.kind == Val::Const) return add_imm(b, a.value);
    if (b.kind == Val::Const) return add_imm(a, b.value);
    return Val::of(Val::Computed);
}

Val rlwinm(const Val& src, uint32_t sh, uint32_t mb, uint32_t me) {
    if (src.kind == Val::Const) return Val::constant(rotl32(src.value, sh) & ppc_mask(mb, me));
    if (mb > me) return Val::of(Val::Computed);
    // slwi of an index keeps its bound.
    if (mb == 0 && me == 31 - sh && src.kind == Val::Index)
        return Val::index(src.stride << sh, src.count, src.exact);
    // Otherwise the result is a (me - mb + 1)-bit field shifted left by (31 - me).
    Val out = Val::index(uint32_t{1} << (31 - me), bits_count(me - mb + 1), false);
    if (sh == 0 && me == 31 && src.kind == Val::Index && src.stride == 1 && src.count != 0 &&
        (out.count == 0 || src.count <= out.count)) {
        out.count = src.count; // clrlwi of an already-bounded index
        out.exact = src.exact;
    }
    return out;
}

bool is_load(Op op) {
    switch (op) {
    case Op::Lbz: case Op::Lbzu: case Op::Lbzux: case Op::Lbzx:
    case Op::Lha: case Op::Lhau: case Op::Lhaux: case Op::Lhax: case Op::Lhbrx:
    case Op::Lhz: case Op::Lhzu: case Op::Lhzux: case Op::Lhzx:
    case Op::Lwarx: case Op::Lwbrx: case Op::Lwz: case Op::Lwzu: case Op::Lwzux: case Op::Lwzx:
    case Op::Eciwx:
        return true;
    default:
        return false;
    }
}

bool is_update(Op op) {
    switch (op) {
    case Op::Lbzu: case Op::Lbzux: case Op::Lhau: case Op::Lhaux: case Op::Lhzu: case Op::Lhzux:
    case Op::Lwzu: case Op::Lwzux: case Op::Stbu: case Op::Stbux: case Op::Sthu: case Op::Sthux:
    case Op::Stwu: case Op::Stwux: case Op::Lfdu: case Op::Lfdux: case Op::Lfsu: case Op::Lfsux:
    case Op::Stfdu: case Op::Stfdux: case Op::Stfsu: case Op::Stfsux:
        return true;
    default:
        return false;
    }
}

// Forget whatever the instruction overwrites. Used for everything that the
// tracker doesn't model more precisely.
void clobber(State& s, const Instruction& in) {
    switch (ppc::form(in.op)) {
    case Form::DAB: case Form::DABNoOE: case Form::DA: case Form::DASimm:
    case Form::MfSpr: case Form::MfTb: case Form::MfReg: case Form::MfSr: case Form::MfSrin:
        set(s, in.rd(), Val::of(Val::Computed));
        return;
    case Form::ASUimm: case Form::ASB: case Form::AS: case Form::ASSh: case Form::Rlwi: case Form::Rlwnm:
        set(s, in.ra(), Val::of(Val::Computed));
        return;
    case Form::LoadStoreD: case Form::LoadStoreX: case Form::LoadStoreNB:
        if (in.op == Op::Lmw) {
            for (uint32_t r = in.rd(); r < 32; ++r) set(s, r, Val::of(Val::Loaded));
        } else if (in.op == Op::Lswi || in.op == Op::Lswx) {
            for (uint32_t r = 0; r < 32; ++r) set(s, r, Val::of(Val::Loaded));
        } else if (is_load(in.op)) {
            set(s, in.rd(), Val::of(Val::Loaded));
        }
        if (is_update(in.op)) set(s, in.ra(), Val::of(Val::Computed));
        return;
    case Form::FLoadStoreD: case Form::FLoadStoreX:
        if (is_update(in.op)) set(s, in.ra(), Val::of(Val::Computed));
        return;
    default:
        return;
    }
}

// Keep the compare records in line with what the instruction does to the CR.
void track_cr(State& s, const Instruction& in) {
    switch (ppc::form(in.op)) {
    case Form::CmpUimm:
        // cmplwi only (L = 0: 32-bit compare)
        s.cmp[in.crfd()] = ((in.raw >> 21) & 1) == 0 ? Compare{true, in.ra(), in.uimm()} : Compare{};
        return;
    case Form::Cmp: case Form::CmpSimm: case Form::FCmp:
        s.cmp[in.crfd()] = {};
        return;
    case Form::CrBit: case Form::Mcrf: case Form::Mcrxr: case Form::MtCrf:
        s.cmp = {};
        return;
    case Form::DAB: case Form::DABNoOE: case Form::DA: case Form::ASB: case Form::AS: case Form::ASSh:
    case Form::Rlwi: case Form::Rlwnm:
        if (in.record()) s.cmp[0] = {};
        return;
    case Form::FDAB: case Form::FDAC: case Form::FDACB: case Form::FDB:
    case Form::Mffs: case Form::Mtfsf: case Form::Mtfsfi: case Form::Mtfsb:
        if (in.record()) s.cmp[1] = {};
        return;
    default:
        break;
    }
    if (in.op == Op::AndiRc || in.op == Op::AndisRc || in.op == Op::AddicRc || in.op == Op::StwcxRc) s.cmp[0] = {};
}

// Model one non-branch instruction.
void step(State& s, const Instruction& in) {
    track_cr(s, in);
    const uint32_t rd = in.rd(), ra = in.ra(), rb = in.rb();
    const auto& g = s.gpr;
    const uint32_t simm = static_cast<uint32_t>(in.simm());

    switch (in.op) {
    case Op::Addi:
        set(s, rd, ra == 0 ? Val::constant(simm) : add_imm(g[ra], simm));
        return;
    case Op::Addis:
        set(s, rd, ra == 0 ? Val::constant(simm << 16) : add_imm(g[ra], simm << 16));
        return;
    case Op::Addic: // the same sum (rA is never r0's zero here); compilers also build addresses with it
    case Op::AddicRc:
        set(s, rd, add_imm(g[ra], simm));
        return;
    case Op::Add:
        set(s, rd, add(g[ra], g[rb]));
        return;
    case Op::Ori: case Op::Oris: case Op::Xori: case Op::Xoris: {
        const bool shifted = in.op == Op::Oris || in.op == Op::Xoris;
        const bool is_or = in.op == Op::Ori || in.op == Op::Oris;
        const uint32_t imm = shifted ? in.uimm() << 16 : in.uimm();
        const Val& src = g[in.rs()];
        Val out = Val::of(Val::Computed);
        if (src.kind == Val::Const) out = Val::constant(is_or ? src.value | imm : src.value ^ imm);
        else if (imm == 0) out = src;
        set(s, ra, out);
        return;
    }
    case Op::Or:
        if (in.rs() == rb) {
            set(s, ra, g[rb]); // mr
        } else if (g[in.rs()].kind == Val::Const && g[rb].kind == Val::Const) {
            set(s, ra, Val::constant(g[in.rs()].value | g[rb].value));
        } else {
            set(s, ra, Val::of(Val::Computed));
        }
        return;
    case Op::Rlwinm:
        set(s, ra, rlwinm(g[in.rs()], in.sh(), in.mb(), in.me()));
        return;
    case Op::AndiRc: {
        const Val& src = g[in.rs()];
        const uint32_t imm = in.uimm();
        if (src.kind == Val::Const) set(s, ra, Val::constant(src.value & imm));
        else if ((imm & (imm + 1)) == 0) set(s, ra, Val::index(1, imm + 1, false));
        else set(s, ra, Val::of(Val::Computed));
        return;
    }
    case Op::Lbz: case Op::Lbzx: case Op::Lbzu: case Op::Lbzux:
        set(s, rd, Val::index(1, 0x100, false));
        if (is_update(in.op)) set(s, ra, Val::of(Val::Computed));
        return;
    case Op::Lhz: case Op::Lhzx: case Op::Lhzu: case Op::Lhzux:
        set(s, rd, Val::index(1, 0x10000, false));
        if (is_update(in.op)) set(s, ra, Val::of(Val::Computed));
        return;
    case Op::Lwz: {
        const Val& base = g[ra];
        set(s, rd, ra != 0 && base.kind == Val::Table ? base.with_kind(Val::Entry, base.value + simm)
                                                      : Val::of(Val::Loaded));
        return;
    }
    case Op::Lwzx: {
        const Val a = ra == 0 ? Val::constant(0) : g[ra];
        const Val& b = g[rb];
        Val out = Val::of(Val::Loaded);
        if (a.kind == Val::Const && b.kind == Val::Index) out = b.with_kind(Val::Entry, a.value);
        else if (b.kind == Val::Const && a.kind == Val::Index) out = a.with_kind(Val::Entry, b.value);
        else if (a.kind == Val::Table && b.kind == Val::Const) out = a.with_kind(Val::Entry, a.value + b.value);
        else if (b.kind == Val::Table && a.kind == Val::Const) out = b.with_kind(Val::Entry, b.value + a.value);
        set(s, rd, out);
        return;
    }
    case Op::Mfspr:
        if (in.spr() == kSprLr) set(s, rd, s.lr);
        else if (in.spr() == kSprCtr) set(s, rd, s.ctr);
        else set(s, rd, Val::of(Val::Computed));
        return;
    case Op::Mtspr:
        if (in.spr() == kSprLr) s.lr = g[in.rs()];
        else if (in.spr() == kSprCtr) s.ctr = g[in.rs()];
        else if (in.spr() == kSprSrr0) s.srr0 = g[in.rs()];
        return;
    default:
        clobber(s, in);
        return;
    }
}

// A called function may change any register: Spikeout's code doesn't keep
// to the SVR4 ABI (r2 is a scratch register, and its register-restore helper
// at 0x372D4 hands the saved LR back in r2 for `mtlr r2; blr`). So after a
// call everything but the stack pointer is as unknown as at function entry.
void clobber_call(State& s) {
    for (uint32_t r = 0; r < 32; ++r)
        if (r != 1) set(s, r, Val::of(Val::Unknown));
    s.lr = Val::of(Val::Unknown);
    s.ctr = Val::of(Val::Computed);
    s.cmp = {};
}

// Narrow the compared register for the path on which CR bit `bi` is `bit`.
void refine(State& s, uint32_t bi, bool bit) {
    const Compare c = s.cmp[bi >> 2];
    if (!c.valid) return;
    uint32_t count = 0;
    switch (bi & 3) {
    case 0: if (bit) count = c.imm; break;      // lt: reg < imm
    case 1: if (!bit) count = c.imm + 1; break; // not gt: reg <= imm
    default: return;
    }
    if (count == 0) return;
    Val& v = s.gpr[c.reg];
    if (v.kind == Val::Index && v.stride == 1) {
        if (v.count == 0 || count < v.count) v.count = count;
        v.exact = true;
    } else if (v.kind == Val::Unknown || v.kind == Val::Computed || v.kind == Val::Loaded) {
        v = Val::index(1, count, true);
    }
}

// Unconditional transfer that never falls through to the next word.
bool ends_flow(const Instruction& in) {
    const bool always = (in.bo() & 0x14) == 0x14;
    switch (in.op) {
    case Op::B:     return !in.lk();
    case Op::Bc:
    case Op::Bclr:
    case Op::Bcctr: return always && !in.lk();
    case Op::Rfi:   return true;
    default:        return false;
    }
}

// Whether an address looks like the start of code: it decodes up to its
// first unconditional branch (or for 64 instructions) and its direct
// branches stay in the image. Text and other data often decode as an
// instruction or two, rarely as a whole run.
//
// `strict` is for addresses that are only guesses (the pointer scan): floats
// and table data decode as branches a lot, so also reject a start with a
// conditional branch or a branch to itself, and absolute branches, which
// compiled code running from RAM doesn't use.
bool plausible_code(const CodeImage& image, uint32_t addr, bool strict = false) {
    for (uint32_t n = 0; n < 64; ++n, addr += 4) {
        const auto word = image.read32(addr);
        if (!word) return false;
        const Instruction in = ppc::decode(*word);
        if (!in.valid()) return false;
        const auto target = ppc::branch_target(in, addr);
        if (target && !image.read32(*target)) return false;
        if (strict && target && (in.aa() || (n == 0 && (in.op == Op::Bc || *target == addr)))) return false;
        if (ends_flow(in)) return true;
    }
    return true;
}

// Whether `addr` follows a function boundary: the start of a segment, or a
// word after which execution can't fall through (blr, b, rfi, padding).
bool after_boundary(const CodeImage& image, uint32_t addr) {
    const Segment* seg = image.segment_at(addr);
    if (!seg) return false;
    if (addr == seg->base) return true;
    const auto prev = image.read32(addr - 4);
    if (!prev || *prev == 0 || *prev == 0x60000000) return true; // padding (zeros, nop)
    return ends_flow(ppc::decode(*prev));
}

// A value that the pointer scan takes as a function address.
bool looks_like_function(const CodeImage& image, uint32_t addr) {
    return (addr & 3) == 0 && after_boundary(image, addr) && plausible_code(image, addr, true);
}

// Per-instruction record kept while exploring.
struct Insn {
    bool ends = false; // last instruction of its block
    Exit exit = Exit::Fallthrough;
    std::vector<uint32_t> succ;
    std::vector<uint32_t> calls;
};

class Analyzer {
public:
    Analyzer(const CodeImage& image, const Options& options, const std::set<uint32_t>& table_starts)
        : image_(image), options_(options), table_starts_(table_starts) {}

    Analysis run(std::span<const Entry> entries) {
        for (const Entry& e : entries) {
            if (!image_.read32(e.addr)) {
                issue(IssueKind::TargetOutsideImage, e.addr, e.addr);
                continue;
            }
            add_function(e.addr, e.kind, e.name);
        }
        while (!work_.empty()) {
            Run r = std::move(work_.back());
            work_.pop_back();
            explore(r.pc, r.state);
        }
        Analysis out;
        build_blocks(out);
        build_functions(out);
        out.jump_tables = std::move(tables_);
        std::sort(out.jump_tables.begin(), out.jump_tables.end(),
                  [](const JumpTable& a, const JumpTable& b) { return a.site < b.site; });
        out.issues = std::move(issues_);
        auto key = [](const Issue& i) { return std::tuple(i.addr, i.kind, i.target); };
        std::sort(out.issues.begin(), out.issues.end(), [&](const Issue& a, const Issue& b) { return key(a) < key(b); });
        out.issues.erase(std::unique(out.issues.begin(), out.issues.end(),
                                     [&](const Issue& a, const Issue& b) { return key(a) == key(b); }),
                         out.issues.end());
        return out;
    }

    const std::set<uint32_t>& constants() const { return constants_; }

private:
    struct Run {
        uint32_t pc;
        State state;
    };

    void issue(IssueKind kind, uint32_t addr, uint32_t target = 0) { issues_.push_back({kind, addr, target}); }

    bool plausible_code(uint32_t addr) const { return analysis::plausible_code(image_, addr); }

    bool check_target(uint32_t from, uint32_t target) {
        if (image_.read32(target)) return true;
        issue(IssueKind::TargetOutsideImage, from, target);
        return false;
    }

    void add_function(uint32_t addr, EntryKind kind, const std::string& name = {}) {
        auto [it, added] = functions_.try_emplace(addr);
        if (added) {
            it->second.entry = addr;
            it->second.kind = kind;
            leaders_.insert(addr);
            work_.push_back({addr, State{}});
        }
        if (it->second.name.empty()) it->second.name = name;
    }

    void branch_to(uint32_t from, uint32_t target, const State& s, Insn& rec) {
        if (!check_target(from, target)) return;
        rec.succ.push_back(target);
        leaders_.insert(target);
        work_.push_back({target, s});
    }

    void call(uint32_t from, uint32_t target, EntryKind kind, Insn& rec) {
        if (!check_target(from, target)) return;
        rec.calls.push_back(target);
        add_function(target, kind);
    }

    // blr/bctr to a constant: the target is treated as a function of its own.
    void constant_jump(uint32_t from, uint32_t target, Insn& rec) {
        rec.exit = Exit::Jump;
        if (!check_target(from, target)) return;
        rec.succ.push_back(target);
        add_function(target, EntryKind::ConstantJump);
    }

    std::optional<JumpTable> read_table(uint32_t site, const Val& v, bool is_call) {
        if (v.stride == 0 || v.stride % 4 != 0) return std::nullopt;
        JumpTable t;
        t.site = site;
        t.table = v.value;
        t.stride = v.stride;
        t.exact = v.exact;
        t.call = is_call;
        const uint32_t limit = v.exact ? v.count
                             : v.count == 0 ? options_.max_table_entries
                                            : std::min(v.count, options_.max_table_entries);
        for (uint32_t i = 0; i < limit; ++i) {
            const uint64_t addr = uint64_t{t.table} + uint64_t{i} * t.stride;
            if (addr > 0xFFFFFFFCu) break;
            if (i > 0 && !v.exact && table_starts_.contains(static_cast<uint32_t>(addr))) break; // next table
            const auto word = image_.read32(static_cast<uint32_t>(addr));
            if (!word || !plausible_code(*word)) {
                if (!v.exact) break; // guessed length: the table ends here
                issue(IssueKind::BadTableEntry, site, word ? *word : static_cast<uint32_t>(addr));
                continue;
            }
            t.targets.push_back(*word);
        }
        if (t.targets.empty()) return std::nullopt;
        return t;
    }

    void indirect_call(uint32_t pc, const Val& target, Insn& rec) {
        if (target.kind == Val::Const) {
            call(pc, target.value, EntryKind::Call, rec);
        } else if (target.kind == Val::Entry) {
            if (auto t = read_table(pc, target, true)) {
                for (uint32_t f : t->targets) {
                    rec.calls.push_back(f);
                    add_function(f, EntryKind::TableCall);
                }
                tables_.push_back(std::move(*t));
                return;
            }
            issue(IssueKind::UnresolvedCall, pc);
        } else {
            issue(IssueKind::UnresolvedCall, pc);
        }
    }

    void table_jump(uint32_t pc, const Val& target, Insn& rec) {
        auto t = read_table(pc, target, false);
        if (!t) {
            rec.exit = Exit::Indirect;
            issue(IssueKind::UnresolvedJump, pc);
            return;
        }
        rec.exit = Exit::JumpTable;
        for (uint32_t dest : t->targets) {
            rec.succ.push_back(dest);
            leaders_.insert(dest);
            work_.push_back({dest, State{}});
        }
        tables_.push_back(std::move(*t));
    }

    void explore(uint32_t pc, State& s) {
        bool after_call = false;
        while (!insns_.contains(pc)) {
            const auto word = image_.read32(pc);
            if (!word) { // fell off the end of a segment
                auto prev = insns_.find(pc - 4);
                if (prev != insns_.end() && !prev->second.ends) {
                    prev->second.ends = true;
                    prev->second.exit = Exit::OutOfImage;
                }
                issue(IssueKind::TargetOutsideImage, pc - 4, pc);
                return;
            }
            const Instruction in = ppc::decode(*word);
            Insn& rec = insns_[pc];
            const uint32_t next = pc + 4;
            if (!in.valid()) {
                rec.ends = true;
                rec.exit = Exit::Invalid;
                issue(after_call ? IssueKind::InvalidAfterCall : IssueKind::InvalidInstruction, pc);
                return;
            }
            after_call = false;
            const uint32_t bo = in.bo();
            const bool always = (bo & 0x14) == 0x14;

            switch (in.op) {
            case Op::B:
            case Op::Bc: {
                const uint32_t target = *ppc::branch_target(in, pc);
                if (in.lk()) {
                    if (target == next) { // bl/bcl $+4 reads the PC, it isn't a call
                        s.lr = Val::constant(next);
                        break;
                    }
                    call(pc, target, EntryKind::Call, rec);
                    clobber_call(s);
                    after_call = true;
                    break;
                }
                rec.ends = true;
                if (in.op == Op::B || always) {
                    rec.exit = Exit::Jump;
                    branch_to(pc, target, s, rec);
                    return;
                }
                if ((bo & 0x04) == 0) s.ctr = Val::of(Val::Computed); // bdnz and friends
                rec.exit = Exit::Branch;
                State taken = s;
                if ((bo & 0x10) == 0) {
                    const bool if_true = (bo & 0x08) != 0;
                    refine(taken, in.bi(), if_true);
                    refine(s, in.bi(), !if_true);
                }
                branch_to(pc, target, taken, rec);
                rec.succ.push_back(next);
                leaders_.insert(next);
                pc = next;
                continue;
            }
            case Op::Bclr:
            case Op::Bcctr: {
                const Val target = in.op == Op::Bclr ? s.lr : s.ctr;
                if (in.lk()) {
                    indirect_call(pc, target, rec);
                    clobber_call(s);
                    after_call = true;
                    break;
                }
                rec.ends = true;
                if (target.kind == Val::Const) {
                    constant_jump(pc, target.value, rec);
                } else if (target.kind == Val::Entry) {
                    table_jump(pc, target, rec);
                } else if (in.op == Op::Bclr && (target.kind == Val::Unknown || target.kind == Val::Loaded)) {
                    rec.exit = Exit::Return; // the caller's LR, possibly restored from the stack
                } else {
                    rec.exit = Exit::Indirect;
                    issue(IssueKind::UnresolvedJump, pc);
                }
                if (always) return;
                rec.succ.push_back(next);
                leaders_.insert(next);
                pc = next;
                continue;
            }
            case Op::Rfi:
                rec.ends = true;
                rec.exit = Exit::Rfi;
                if (s.srr0.kind == Val::Const && check_target(pc, s.srr0.value)) {
                    rec.calls.push_back(s.srr0.value);
                    add_function(s.srr0.value, EntryKind::ExceptionReturn);
                }
                return;
            case Op::Tw:
            case Op::Twi:
                if (in.to() == 31) {
                    rec.ends = true;
                    rec.exit = Exit::Trap;
                    return;
                }
                break;
            case Op::Sc:
                break;
            default:
                step(s, in);
                if (options_.scan_pointers) {
                    // Constants that might be code addresses (lis + addi of a callback).
                    for (uint32_t r : {in.rd(), in.ra()})
                        if (s.gpr[r].kind == Val::Const && image_.contains(s.gpr[r].value))
                            constants_.insert(s.gpr[r].value);
                }
                break;
            }
            pc = next;
        }
    }

    static void append_unique(std::vector<uint32_t>& to, const std::vector<uint32_t>& from) {
        for (uint32_t a : from)
            if (std::find(to.begin(), to.end(), a) == to.end()) to.push_back(a);
    }

    void build_blocks(Analysis& out) const {
        Block* cur = nullptr;
        uint32_t expected = 0;
        for (const auto& [addr, rec] : insns_) {
            if (cur && (addr != expected || leaders_.contains(addr))) {
                if (addr == expected) cur->successors.push_back(addr); // Exit::Fallthrough
                cur = nullptr;
            }
            if (!cur) {
                cur = &out.blocks[addr];
                cur->start = addr;
            }
            cur->end = addr + 4;
            append_unique(cur->calls, rec.calls);
            if (rec.ends) {
                cur->exit = rec.exit;
                append_unique(cur->successors, rec.succ);
                cur = nullptr;
            }
            expected = addr + 4;
        }
    }

    void build_functions(Analysis& out) {
        for (auto& [entry, fn] : functions_) {
            std::set<uint32_t> seen;
            std::vector<uint32_t> stack{entry};
            while (!stack.empty()) {
                const uint32_t b = stack.back();
                stack.pop_back();
                auto it = out.blocks.find(b);
                if (it == out.blocks.end() || !seen.insert(b).second) continue;
                Block& block = it->second;
                fn.instructions += block.instruction_count();
                for (uint32_t succ : block.successors) {
                    if (succ != entry && functions_.contains(succ)) continue; // into another function
                    stack.push_back(succ);
                }
                if (block.exit == Exit::Jump && block.successors.size() == 1 && block.successors[0] != entry &&
                    functions_.contains(block.successors[0]))
                    block.exit = Exit::TailCall;
            }
            fn.blocks.assign(seen.begin(), seen.end());
        }
        out.functions = std::move(functions_);
    }

    const CodeImage& image_;
    const Options& options_;
    const std::set<uint32_t>& table_starts_; // tables found so far, to end guessed ones
    std::set<uint32_t> constants_;           // in-image constants built by the code
    std::map<uint32_t, Insn> insns_;
    std::set<uint32_t> leaders_;
    std::map<uint32_t, Function> functions_;
    std::vector<Run> work_;
    std::vector<JumpTable> tables_;
    std::vector<Issue> issues_;
};

} // namespace

Analysis analyze(const CodeImage& image, std::span<const Entry> entries, const Options& options) {
    // Repeat until nothing new turns up: a guessed table length ends at the
    // next table, but tables are only found while exploring; and pointers to
    // new functions turn up in code found by the previous pass.
    std::vector<Entry> all(entries.begin(), entries.end());
    std::set<uint32_t> known;
    for (const Entry& e : all) known.insert(e.addr);

    // Pointers are looked for in, and may only point into, these segments.
    auto scanned = [&](uint32_t addr) {
        const Segment* seg = image.segment_at(addr);
        return seg && std::find(options.pointer_segments.begin(), options.pointer_segments.end(), seg->name) !=
                          options.pointer_segments.end();
    };
    std::vector<uint32_t> data_words;
    if (options.scan_pointers) {
        for (const Segment& seg : image.segments()) {
            if (!scanned(seg.base)) continue;
            for (uint64_t addr = seg.base; addr + 4 <= seg.end(); addr += 4) {
                const uint32_t value = *image.read32(static_cast<uint32_t>(addr));
                if (scanned(value)) data_words.push_back(value);
            }
        }
    }

    std::set<uint32_t> table_starts;
    Analysis a;
    for (int pass = 0; pass < 32; ++pass) {
        Analyzer analyzer(image, options, table_starts);
        a = analyzer.run(all);
        bool changed = false;
        for (const JumpTable& t : a.jump_tables) changed |= table_starts.insert(t.table).second;
        if (options.scan_pointers) {
            auto consider = [&](uint32_t value, bool built_by_code) {
                if (known.contains(value) || a.functions.contains(value) || !scanned(value)) return;
                // A constant the code builds that lands on an instruction it
                // already runs is taken even inside a function: tasks that
                // yield store the address to resume at (lis + addi, then
                // stw into the task) and their scheduler calls it later.
                const bool resume_point = built_by_code && (value & 3) == 0 && a.block_containing(value);
                if (!resume_point && !looks_like_function(image, value)) return;
                known.insert(value);
                all.push_back({value, EntryKind::Pointer, ""});
                changed = true;
            };
            for (uint32_t value : data_words) consider(value, false);
            for (uint32_t value : analyzer.constants()) consider(value, true);
        }
        if (!changed) break;
    }
    return a;
}

} // namespace analysis
