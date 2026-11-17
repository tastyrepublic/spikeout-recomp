#include "interp/interp.hpp"

#include "analysis/boot_image.hpp"
#include "analysis/control_flow.hpp"
#include "ppc/instruction.hpp"
#include "ppc/timing.hpp"
#include "recomp/guest.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/test_move.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using recomp::Context;
using recomp::FunctionEntry;
using ppc::Instruction;
using ppc::Op;
namespace ops = recomp::ops;

// ---- Test aid: how the program's own words are read (docs/reasm.md) ----
// SPIKEOUT_INTERP_ACCESS=FILE writes at exit a byte for every word of the first MB of RAM (the program the boot
// code copies there): how the game read it while it still held the copy's value (1 bytes, 2 halfwords, 4 words,
// 8 floats), 16 once written. SPIKEOUT_INTERP_ACCESS_IMAGE=FILE (the program as copied, e.g. for a run from a
// save state): at the first block run, words that no longer hold FILE's value count as written. Tells data read as whole words (maybe addresses) from bytes and halfwords. Off: one
// flag tested per load and store.
uint32_t block_pc();
struct AccessMap {
    static constexpr uint32_t kBytes = 0x100000;
    std::vector<uint8_t> words;
    std::string path;
    std::vector<uint8_t> image; // SPIKEOUT_INTERP_ACCESS_IMAGE's bytes
    AccessMap() {
        if (const char* p = std::getenv("SPIKEOUT_INTERP_ACCESS"); p && *p) {
            path = p;
            words.assign(kBytes / 4, 0);
            if (const char* i = std::getenv("SPIKEOUT_INTERP_ACCESS_IMAGE"); i && *i)
                if (std::FILE* f = std::fopen(i, "rb")) {
                    image.resize(kBytes);
                    image.resize(std::fread(image.data(), 1, kBytes, f));
                    std::fclose(f);
                }
        }
    }
    std::set<uint32_t> odd; // words read at an address not a multiple of 4 and used as addresses (FILE.odd, a line each)
    ~AccessMap() {
        if (path.empty()) return;
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(words.data(), 1, words.size(), f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen((path + ".odd").c_str(), "w")) {
            for (uint32_t a : odd) std::fprintf(f, "%08x\n", a);
            std::fclose(f);
        }
    }
    // SPIKEOUT_INTERP_ACCESS_WHY=ADDR: each new mark on that word printed, with the block that made it
    const uint32_t why = [] {
        const char* p = std::getenv("SPIKEOUT_INTERP_ACCESS_WHY");
        return p && *p ? static_cast<uint32_t>(std::strtoul(p, nullptr, 0)) : ~0u;
    }();
    void mark(uint32_t from, uint8_t flag) { // where a value came from: its word, or (not on a word) the odd list
        if (from == why && (words[from / 4] & flag) != flag) [[unlikely]]
            std::fprintf(stderr, "access why: %08x marked %u by block %08x\n", from, flag, block_pc());
        if (!(from & 3)) words[from / 4] |= flag;
        else if (flag & 96) odd.insert(from);
    }
    bool unwritten(uint32_t ea) const { return !(words[ea / 4] & 16) && !(words[(ea + 3) / 4 % words.size()] & 16); }
    void read(uint32_t ea, uint32_t n, uint8_t kind) {
        for (uint32_t a = ea & ~3u; a < ea + n && a < kBytes; a += 4)
            if (!(words[a / 4] & 16)) words[a / 4] |= kind;
    }
    bool started = false; // from the first block run in RAM (the boot code's copy before it doesn't count)
    void enter(Context& c, uint32_t pc) {
        if (!started && pc < kBytes) {
            started = true;
            std::fill(words.begin(), words.end(), uint8_t{0});
            for (uint32_t a = 0; a + 4 <= image.size(); a += 4) {
                const uint32_t v = static_cast<uint32_t>(image[a]) << 24 | static_cast<uint32_t>(image[a + 1]) << 16 |
                                   static_cast<uint32_t>(image[a + 2]) << 8 | image[a + 3];
                if (c.read32(a) != v) words[a / 4] = 16;
            }
        }
    }
    void write(uint32_t ea, uint32_t n) {
        for (uint32_t a = ea & ~3u; a < ea + n && a < kBytes; a += 4) words[a / 4] |= 16;
    }
};
AccessMap g_access;

// SPIKEOUT_INTERP_TRACE=FILE:COUNT (test aid): the first COUNT blocks run, each its address (as the original program
// has it: SPIKEOUT_TEST_PROGRAM_MOVE) the time base's low word, LR (as the original has it) and r3, 16 bytes, to compare two runs and find where they part.
struct BlockTrace {
    std::FILE* file = nullptr;
    uint64_t left = 0, skip = 0; // SPIKEOUT_INTERP_TRACE_SKIP=N: from the Nth block on
    // SPIKEOUT_INTERP_TRACE_CYCLES=1: the exact cycle count in place of r3 and MSR in place of LR (where two runs'
    // interrupts part: the time base is too coarse to show it)
    const bool cycles = std::getenv("SPIKEOUT_INTERP_TRACE_CYCLES") != nullptr;
    BlockTrace() {
        if (const char* k = std::getenv("SPIKEOUT_INTERP_TRACE_SKIP")) skip = std::strtoull(k, nullptr, 0);
        if (const char* p = std::getenv("SPIKEOUT_INTERP_TRACE"); p && *p) {
            const std::string s(p);
            const size_t colon = s.rfind(':');
            if (colon != std::string::npos) {
                left = std::strtoull(s.c_str() + colon + 1, nullptr, 0);
                file = std::fopen(s.substr(0, colon).c_str(), "wb");
            }
        }
    }
    ~BlockTrace() { if (file) std::fclose(file); }
    void block(uint32_t pc, uint32_t tb, uint32_t lr, uint32_t r3) {
        if (!left) return;
        if (skip) { --skip; return; }
        const uint32_t a[4] = {m3::test_move::original(pc), tb, cycles ? lr : m3::test_move::original(lr), r3};
        std::fwrite(a, 4, 4, file);
        if (--left == 0) std::fflush(file);
    }
};
BlockTrace g_trace;
const bool g_trace_on = g_trace.file != nullptr;
// SPIKEOUT_INTERP_WATCH=ADDR (test aid): each write to that word printed, with the block that made it (as the
// original program has it) and the value.
const uint32_t g_watch = [] {
    const char* p = std::getenv("SPIKEOUT_INTERP_WATCH");
    return p && *p ? static_cast<uint32_t>(std::strtoul(p, nullptr, 0)) & ~3u : ~0u;
}();
uint32_t g_block_pc = 0;
uint32_t block_pc() { return m3::test_move::original(g_block_pc); }

// SPIKEOUT_INTERP_MODES=FILE@ADDR (test aid): for each block of the program's MB, the values (0-127) the byte at ADDR
// (a game's mode, say) had when the block ran: 16 bytes a word (a bit a value), written at exit. Which code runs in
// which part of the game.
struct ModeProfile {
    std::string path;
    uint32_t addr = 0;
    std::vector<uint64_t> bits; // two a word
    ModeProfile() {
        if (const char* p = std::getenv("SPIKEOUT_INTERP_MODES"); p && *p) {
            const std::string s(p);
            const size_t at = s.rfind('@');
            if (at == std::string::npos) return;
            path = s.substr(0, at);
            addr = static_cast<uint32_t>(std::strtoul(s.c_str() + at + 1, nullptr, 0));
            bits.assign(AccessMap::kBytes / 4 * 2, 0);
        }
    }
    ~ModeProfile() {
        if (path.empty()) return;
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(bits.data(), 8, bits.size(), f);
            std::fclose(f);
        }
    }
    void block(Context& c, uint32_t pc) {
        if (pc >= AccessMap::kBytes) return;
        const uint32_t m = c.read8(addr) & 127u;
        bits[pc / 4 * 2 + m / 64] |= uint64_t{1} << (m % 64);
    }
};
ModeProfile g_modes;
const bool g_modes_on = !g_modes.path.empty();

// SPIKEOUT_INTERP_CALLS=FILE (test aid, docs/reasm.md): every call or jump through a register (bctrl, blrl, bctr:
// function pointers, task tables) as "site target count", the original program's addresses, written at exit. The
// calls a listing can't follow.
struct CallLog {
    std::string path;
    std::unordered_map<uint64_t, uint64_t> seen;
    CallLog() {
        if (const char* p = std::getenv("SPIKEOUT_INTERP_CALLS"); p && *p) path = p;
    }
    ~CallLog() {
        if (path.empty()) return;
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            for (const auto& [k, n] : seen)
                std::fprintf(f, "%08x %08x %llu\n", static_cast<uint32_t>(k >> 32), static_cast<uint32_t>(k),
                             static_cast<unsigned long long>(n));
            std::fclose(f);
        }
    }
    void add(uint32_t site, uint32_t target) {
        ++seen[uint64_t{m3::test_move::original(site)} << 32 | m3::test_move::original(target)];
    }
};
CallLog g_calls;
const bool g_calls_on = !g_calls.path.empty();
const bool g_access_on = !g_access.path.empty();
uint32_t g_word_ea = ~0u; // the last rd32's address (each load's start resets it)
inline void note_read(uint32_t ea, uint32_t n, uint8_t kind) {
    if (g_access_on) [[unlikely]] if (ea < AccessMap::kBytes) g_access.read(ea, n, kind);
}
inline void note_write(uint32_t ea, uint32_t n) {
    if (g_access_on) [[unlikely]] if (ea < AccessMap::kBytes) g_access.write(ea, n);
}
inline uint8_t rd8(Context& c, uint32_t ea) { note_read(ea, 1, 1); return c.read8(ea); }
inline uint16_t rd16(Context& c, uint32_t ea) { note_read(ea, 2, 2); return c.read16(ea); }
inline uint32_t rd32(Context& c, uint32_t ea) { note_read(ea, 4, 4); if (g_access_on) [[unlikely]] g_word_ea = ea; return c.read32(ea); }
inline uint32_t rdf32(Context& c, uint32_t ea) { note_read(ea, 4, 8); return c.read32(ea); }
inline uint64_t rdf64(Context& c, uint32_t ea) { note_read(ea, 8, 8); return c.read64(ea); }
// And whether a word read is then used as an address (32: a load or store through it, at most 64 KB on; 64: moved
// to LR or CTR, a jump): each register remembers the program word it was loaded from (its value checked at the use,
// so a register written since doesn't count). Followed through memory too: a register stored whole (stw, stmw)
// leaves its mark at that address, picked up by the next word load from it (a pointer kept in a structure or
// on the stack and used later).
struct Loaded { uint32_t from = ~0u, value = 0; };
Loaded g_loaded[32];
Loaded g_ctr; // what was moved into CTR (mtctr): a jump target once bctr/bctrl uses it, a number once bdnz counts it
std::unordered_map<uint32_t, Loaded> g_kept; // memory word -> the program word its value came from
inline void forget(uint32_t ea, uint32_t n) {
    if (g_watch != ~0u) [[unlikely]]
        if ((ea & ~3u) == g_watch)
            std::fprintf(stderr, "watch: block %08x writes %u bytes at %08x\n", m3::test_move::original(g_block_pc), n, ea);
    if (g_access_on) [[unlikely]] {
        note_write(ea, n);
        if (!g_kept.empty())
            for (uint32_t a = ea & ~3u; a < ea + n; a += 4) g_kept.erase(a);
    }
}
inline void wr8(Context& c, uint32_t ea, uint8_t v) { forget(ea, 1); c.write8(ea, v); }
inline void wr16(Context& c, uint32_t ea, uint16_t v) { forget(ea, 2); c.write16(ea, v); }
inline void wr32(Context& c, uint32_t ea, uint32_t v) { forget(ea, 4); c.write32(ea, v); }
inline void wr64(Context& c, uint32_t ea, uint64_t v) { forget(ea, 8); c.write64(ea, v); }
inline void kept(uint32_t ea, uint32_t r, uint32_t v) { // register r (value v) stored whole at ea
    if (g_access_on) [[unlikely]] {
        const Loaded& l = g_loaded[r];
        if (l.from == ~0u || v - l.value + 0x10000u >= 0x20000u) return; // (a lis's value is its high half)
        // given to the Real3D's DMA (its source register): used as an address. Not any device store: uploads copy
        // data words (numbers among them) to the 3D board's memories.
        if ((ea & 0xFFFFFF00u) == 0xC2000000u) g_access.mark(l.from, 32);
        else if (ea < 0x800000 && !(ea & 3)) g_kept[ea] = l;
    }
}
// And by value, for a register whose own mark was lost (an interrupt handler borrowing it, say): each exact value
// a program word or lis pair made, and where it came from.
std::unordered_map<uint32_t, uint32_t> g_made;
inline void made(uint32_t v, uint32_t from) {
    if (v >= 0x1000 && v < AccessMap::kBytes) g_made[v] = from;
}
inline void nb(uint32_t r, uint32_t v, uint8_t flag = 32) { // register r (value v) used as an address
    if (g_access_on) [[unlikely]] {
        if (!r) return;
        const Loaded& l = g_loaded[r];
        if (l.from != ~0u && v - l.value + 0x10000u < 0x20000u) g_access.mark(l.from, flag); // within 64 KB
        else if (auto it = g_made.find(v); it != g_made.end()) g_access.mark(it->second, flag);
    }
}
inline void nl0() { if (g_access_on) [[unlikely]] g_word_ea = ~0u; }
inline void nd(uint32_t d, uint32_t v) { // a load into register d
    if (g_access_on) [[unlikely]] {
        const uint32_t ea = g_word_ea;
        if (ea < AccessMap::kBytes - 3 && g_access.unwritten(ea) && v >= 0x1000 && v < AccessMap::kBytes)
            g_loaded[d] = {ea, v}, made(v, ea);
        else if (auto it = ea != ~0u ? g_kept.find(ea) : g_kept.end(); it != g_kept.end() && v - it->second.value + 0x10000u < 0x20000u)
            g_loaded[d] = it->second;
        else
            g_loaded[d].from = ~0u;
    }
}
// A lis (addis rD,0,hi) run: 128 on its word, and rD remembers it, so a value it builds that is used as an address
// marks the lis 32 too (a lis pair run but never so used built a number: a mask, a delay).
inline void nlis(uint32_t d, uint32_t pc, uint32_t v) {
    if (g_access_on) [[unlikely]] {
        if (pc < AccessMap::kBytes) {
            g_access.words[pc / 4] |= 128;
            g_loaded[d] = {pc, v};
        } else
            g_loaded[d].from = ~0u;
    }
}
// Two registers compared or subtracted (a pointer against a table's end, an end minus a start): where both hold
// values within 64 KB of each other, each one that came from the program counts as used as an address. Both from
// 0x10000 on: lengths are compared too (0x4000 against 0x10000, in the memory map's upload loop at 0x7937C).
inline void npair(uint32_t a, uint32_t va, uint32_t b, uint32_t vb) {
    if (g_access_on) [[unlikely]] {
        if (va - vb + 0x10000u >= 0x20000u || va < 0x10000 || vb < 0x10000 || va >= AccessMap::kBytes) return;
        nb(a, va);
        nb(b, vb);
    }
}
// An indexed access (lwzx rD,rA,rB ...): with one register a small offset (under 0x10000) and the other a pointer,
// only the pointer was used as an address (a sine table's angle step 0x6000, a copy's offset reaching 0x4000: equal
// to the task table's work sizes, which the by-value lookup would otherwise mark as addresses).
inline void nbx(uint32_t a, uint32_t va, uint32_t b, uint32_t vb) {
    if ((va < 0x10000) != (vb < 0x10000)) {
        const uint32_t s = va < 0x10000 ? a : b, vs = va < 0x10000 ? va : vb;
        if (g_access_on && s && g_loaded[s].from != ~0u && vs - g_loaded[s].value + 0x10000u < 0x20000u)
            g_access.mark(g_loaded[s].from, 32);            // (its own mark only, no lookup by value)
        return nb(va < 0x10000 ? b : a, va < 0x10000 ? vb : va);
    }
    nb(a, va);
    nb(b, vb);
}
// A jump or call through CTR (bctr, bctrl): what was moved into it was a jump target (64)
inline void nctr_jump(uint32_t target) {
    if (g_access_on) [[unlikely]] {
        if (g_ctr.from != ~0u) g_access.mark(g_ctr.from, 64);
        else if (auto it = g_made.find(target); it != g_made.end()) g_access.mark(it->second, 64);
    }
}
inline void nclear(uint32_t d) { if (g_access_on) [[unlikely]] g_loaded[d] = Loaded{}; } // d holds a new value (mflr ...)
// d = a, slightly changed (rlwinm aligning a pointer): its mark kept only while the value stays within 64 KB
inline void nmoved(uint32_t d, uint32_t a, uint32_t va, uint32_t v) {
    if (g_access_on) [[unlikely]] g_loaded[d] = v - va + 0x10000u < 0x20000u ? g_loaded[a] : Loaded{};
}
inline void ncopy(uint32_t d, uint32_t a) { if (g_access_on) [[unlikely]] g_loaded[d] = g_loaded[a]; }
inline void naddi(uint32_t d, uint32_t a, uint32_t v) { // d = a + an immediate (a lis pair's low half, an offset)
    if (g_access_on) [[unlikely]] {
        g_loaded[d] = a ? g_loaded[a] : Loaded{}; // (li rD,imm: rA 0 is zero, not r0)
        if (g_loaded[d].from != ~0u && v - g_loaded[d].value + 0x10000u < 0x20000u) made(v, g_loaded[d].from);
    }
}
inline bool fresh(uint32_t r, uint32_t v) { // r's mark still its value's (not left from before it was written)
    const Loaded& l = g_loaded[r];
    return l.from != ~0u && v - l.value + 0x10000u < 0x20000u;
}
// A value used as a number (added with carry, as to the time base for a delay; multiplied, divided, shifted, and/or/
// xor-ed: a mask): 8 on
// the word or lis it came from (with no use as an address, the lis pair built a number). Only from the register's own
// mark, never looked up by value: a task's next step (lis + addi, stored into its record) would be marked a number.
inline void nnum(uint32_t r, uint32_t v) {
    if (g_access_on && fresh(r, v)) [[unlikely]] g_access.mark(g_loaded[r].from, 8);
}
inline void ncopy2(uint32_t d, uint32_t a, uint32_t va, uint32_t b, uint32_t vb) { // d = a + b (or b - a)
    if (g_access_on) [[unlikely]] g_loaded[d] = fresh(a, va) ? g_loaded[a] : fresh(b, vb) ? g_loaded[b] : Loaded{};
}

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
    if (!(bo & 0x04) && g_access_on && g_ctr.from != ~0u) [[unlikely]] g_access.mark(g_ctr.from, 8), g_ctr = {}; // bdnz: a count
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
    auto returned = [&](uint32_t next) {
        if (c.returned_to == next) return false;
        // (SPIKEOUT_INTERP_CALLS: a blr to somewhere else, a resume point or a jump through LR, logged as the call
        // at next - 4 going on there)
        if (g_calls_on) [[unlikely]]
            if (!c.outer_waits_for(c.returned_to)) g_calls.add(next - 4, c.returned_to & ~3u);
        return c.follow_return(next);
    };

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
            if (g_access_on) [[unlikely]] g_access.enter(c, o.pc);
            g_block_pc = o.pc;
            if (g_modes_on) [[unlikely]] g_modes.block(c, m3::test_move::original(o.pc));
            if (g_trace_on) [[unlikely]] g_trace.block(o.pc, c.read_tbl(), g_trace.cycles ? s.msr : s.lr, g_trace.cycles ? static_cast<uint32_t>(c.cycles()) : s.gpr[3]);
            if (o.flags & kHook) c.hook(m3::test_move::original(o.pc));
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
                if (o.b) nctr_jump(t);
                if (g_calls_on) [[unlikely]] g_calls.add(o.pc, t);
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
                if (g_calls_on && o.b) [[unlikely]] g_calls.add(o.pc, s.ctr & ~3u); // (bctr; blr is a return)
                if (o.b) nctr_jump(s.ctr & ~3u);
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
                if (o.b) nctr_jump(target);
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
        case kLi: GD = o.imm; nclear(o.d); break;
        case kLis: GD = o.imm; nlis(o.d, o.pc, GD); break;
        OPC(Addi): GD = GA0 + o.imm; naddi(o.d, o.a, GD); break;
        OPC(Addis): GD = GA0 + o.imm; nlis(o.d, o.pc, GD); break; // (onto an index: a table's high half, as a lis)
        OPC(Addic): GD = ops::addx(s, GA, o.imm, 0, true, false); break;
        OPC(AddicRc): if (o.d == o.a && o.imm == 0xFFFFFFFFu) nnum(o.a, GA); // addic. rX,rX,-1: a loop count
                      GD = ops::addx(s, GA, o.imm, 0, true, false); ops::set_cr0(s, GD); break;
        OPC(Subfic): GD = ops::addx(s, ~GA, o.imm, 1, true, false); nclear(o.d); break;
        OPC(Mulli): nnum(o.a, GA); GD = GA * o.imm; nclear(o.d); break;
        OPC(Add): ncopy2(o.d, o.a, GA, o.b, GB); GD = ops::addx(s, GA, GB, 0, false, OE); CR0(GD); break;
        OPC(Addc): nnum(o.a, GA); nnum(o.b, GB); GD = ops::addx(s, GA, GB, 0, true, OE); CR0(GD); break;
        OPC(Adde): nnum(o.a, GA); nnum(o.b, GB); GD = ops::addx(s, GA, GB, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Addme): GD = ops::addx(s, GA, 0xFFFFFFFFu, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Addze): GD = ops::addx(s, GA, 0u, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Subf): npair(o.a, GA, o.b, GB); ncopy2(o.d, o.b, GB, o.b, GB); GD = ops::addx(s, ~GA, GB, 1, false, OE); CR0(GD); break;
        OPC(Subfc): GD = ops::addx(s, ~GA, GB, 1, true, OE); CR0(GD); break;
        OPC(Subfe): GD = ops::addx(s, ~GA, GB, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Subfme): GD = ops::addx(s, ~GA, 0xFFFFFFFFu, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Subfze): GD = ops::addx(s, ~GA, 0u, ops::ca(s), true, OE); CR0(GD); break;
        OPC(Neg): GD = ops::addx(s, ~GA, 0u, 1, false, OE); CR0(GD); nclear(o.d); break;
        OPC(Mullw): nnum(o.a, GA); nnum(o.b, GB); GD = ops::mullw(s, GA, GB, OE); CR0(GD); break;
        OPC(Mulhw): nnum(o.a, GA); nnum(o.b, GB); GD = ops::mulhw(GA, GB); CR0(GD); break;
        OPC(Mulhwu): nnum(o.a, GA); nnum(o.b, GB); GD = ops::mulhwu(GA, GB); CR0(GD); break;
        OPC(Divw): nnum(o.a, GA); nnum(o.b, GB); GD = ops::divw(s, GA, GB, OE); CR0(GD); break;
        OPC(Divwu): nnum(o.a, GA); nnum(o.b, GB); GD = ops::divwu(s, GA, GB, OE); CR0(GD); break;

        // ---- Compare ----
        OPC(Cmp): npair(o.a, GA, o.b, GB); ops::cmps(s, o.d, static_cast<int32_t>(GA), static_cast<int32_t>(GB)); break;
        OPC(Cmpi): ops::cmps(s, o.d, static_cast<int32_t>(GA), static_cast<int32_t>(o.imm)); break;
        OPC(Cmpl): npair(o.a, GA, o.b, GB); ops::cmpu(s, o.d, GA, GB); break;
        OPC(Cmpli): ops::cmpu(s, o.d, GA, o.imm); break;

        // ---- Logical (rA from rS) ----
        OPC(And): nnum(o.d, GD); nnum(o.b, GB); GA = GD & GB; CR0(GA); break;
        OPC(Andc): nnum(o.d, GD); nnum(o.b, GB); GA = GD & ~GB; CR0(GA); break;
        OPC(Or): if (o.d != o.b) nnum(o.d, GD), nnum(o.b, GB); GA = GD | GB; CR0(GA); if (o.d == o.b) ncopy(o.a, o.d); break;
        OPC(Orc): GA = GD | ~GB; CR0(GA); break;
        OPC(Xor): nnum(o.d, GD); nnum(o.b, GB); GA = GD ^ GB; CR0(GA); break;
        OPC(Nand): GA = ~(GD & GB); CR0(GA); break;
        OPC(Nor): GA = ~(GD | GB); CR0(GA); break;
        OPC(Eqv): GA = ~(GD ^ GB); CR0(GA); break;
        OPC(AndiRc): GA = GD & o.imm; ops::set_cr0(s, GA); nclear(o.a); break;
        OPC(AndisRc): GA = GD & o.imm; ops::set_cr0(s, GA); nclear(o.a); break;
        OPC(Ori): GA = GD | o.imm; break;
        OPC(Oris): GA = GD | o.imm; break;
        OPC(Xori): GA = GD ^ o.imm; nclear(o.a); break;
        OPC(Xoris): GA = GD ^ o.imm; nclear(o.a); break;
        OPC(Cntlzw): GA = ops::cntlzw(GD); CR0(GA); nclear(o.a); break;
        OPC(Extsb): GA = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(GD))); CR0(GA); nclear(o.a); break;
        OPC(Extsh): GA = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(GD))); CR0(GA); nclear(o.a); break;

        // ---- Rotate and shift ----
        OPC(Rlwinm): { const uint32_t v = ops::rotl(GD, o.x) & o.imm; nmoved(o.a, o.d, GD, v); GA = v; CR0(GA); break; }
        OPC(Rlwimi): GA = (ops::rotl(GD, o.x) & o.imm) | (GA & ~o.imm); CR0(GA); nclear(o.a); break;
        OPC(Rlwnm): GA = ops::rotl(GD, GB) & o.imm; CR0(GA); nclear(o.a); break;
        OPC(Slw): nnum(o.d, GD); GA = ops::slw(GD, GB); CR0(GA); nclear(o.a); break;
        OPC(Srw): nnum(o.d, GD); GA = ops::srw(GD, GB); CR0(GA); nclear(o.a); break;
        OPC(Sraw): nnum(o.d, GD); GA = ops::sraw(s, GD, GB & 0x3F); CR0(GA); nclear(o.a); break;
        OPC(Srawi): GA = ops::sraw(s, GD, o.x); CR0(GA); nclear(o.a); break;

        // ---- Loads (rD = value, then rA = ea for the updating forms) ----
#define LOAD_D(value) { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); nl0(); GD = value; nd(o.d, GD); }
#define LOAD_DU(value) { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); nl0(); GD = value; nd(o.d, GD); GA = ea; }
#define LOAD_X(value) { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); nl0(); GD = value; nd(o.d, GD); }
#define LOAD_XU(value) { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); nl0(); GD = value; nd(o.d, GD); GA = ea; }
        OPC(Lbz): LOAD_D(rd8(c, ea)) break;
        OPC(Lbzu): LOAD_DU(rd8(c, ea)) break;
        OPC(Lbzx): LOAD_X(rd8(c, ea)) break;
        OPC(Lbzux): LOAD_XU(rd8(c, ea)) break;
        OPC(Lhz): LOAD_D(rd16(c, ea)) break;
        OPC(Lhzu): LOAD_DU(rd16(c, ea)) break;
        OPC(Lhzx): LOAD_X(rd16(c, ea)) break;
        OPC(Lhzux): LOAD_XU(rd16(c, ea)) break;
        OPC(Lha): LOAD_D(static_cast<uint32_t>(static_cast<int16_t>(rd16(c, ea)))) break;
        OPC(Lhau): LOAD_DU(static_cast<uint32_t>(static_cast<int16_t>(rd16(c, ea)))) break;
        OPC(Lhax): LOAD_X(static_cast<uint32_t>(static_cast<int16_t>(rd16(c, ea)))) break;
        OPC(Lhaux): LOAD_XU(static_cast<uint32_t>(static_cast<int16_t>(rd16(c, ea)))) break;
        OPC(Lwz): LOAD_D(rd32(c, ea)) break;
        OPC(Lwzu): LOAD_DU(rd32(c, ea)) break;
        OPC(Lwzx): LOAD_X(rd32(c, ea)) break;
        OPC(Lwzux): LOAD_XU(rd32(c, ea)) break;
        OPC(Lhbrx): LOAD_X(ops::bswap16(rd16(c, ea))) break;
        OPC(Lwbrx): LOAD_X(m3::bswap32(rd32(c, ea))) break;
        OPC(Lwarx): {
            const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB);
            s.reserve = true;
            s.reserve_addr = ea;
            GD = rd32(c, ea);
            break;
        }
        OPC(Lmw): {
            uint32_t ea = GA0 + o.imm; nb(o.a, GA0);
            for (uint32_t r = o.d; r < 32; ++r, ea += 4) { nl0(); s.gpr[r] = rd32(c, ea); nd(r, s.gpr[r]); }
            break;
        }
        OPC(Lswi): c.load_string(o.d, GA0, o.imm); break;
        OPC(Lswx): c.load_string(o.d, GA0 + GB, s.xer & 0x7F); break;

        // ---- Stores (rS in d) ----
#define STORE_D(action) { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); action; }
#define STORE_DU(action) { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); action; GA = ea; }
#define STORE_X(action) { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); action; }
#define STORE_XU(action) { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); action; GA = ea; }
        OPC(Stb): STORE_D(wr8(c, ea, static_cast<uint8_t>(GD))) break;
        OPC(Stbu): STORE_DU(wr8(c, ea, static_cast<uint8_t>(GD))) break;
        OPC(Stbx): STORE_X(wr8(c, ea, static_cast<uint8_t>(GD))) break;
        OPC(Stbux): STORE_XU(wr8(c, ea, static_cast<uint8_t>(GD))) break;
        OPC(Sth): STORE_D(wr16(c, ea, static_cast<uint16_t>(GD))) break;
        OPC(Sthu): STORE_DU(wr16(c, ea, static_cast<uint16_t>(GD))) break;
        OPC(Sthx): STORE_X(wr16(c, ea, static_cast<uint16_t>(GD))) break;
        OPC(Sthux): STORE_XU(wr16(c, ea, static_cast<uint16_t>(GD))) break;
        OPC(Stw): STORE_D(wr32(c, ea, GD); kept(ea, o.d, GD)) break;
        OPC(Stwu): STORE_DU(wr32(c, ea, GD); kept(ea, o.d, GD)) break;
        OPC(Stwx): STORE_X(wr32(c, ea, GD); kept(ea, o.d, GD)) break;
        OPC(Stwux): STORE_XU(wr32(c, ea, GD); kept(ea, o.d, GD)) break;
        OPC(Sthbrx): STORE_X(wr16(c, ea, static_cast<uint16_t>(ops::bswap16(GD)))) break;
        OPC(Stwbrx): STORE_X(wr32(c, ea, m3::bswap32(GD)); kept(ea, o.d, GD)) break;
        OPC(StwcxRc): {
            const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB);
            if (s.reserve) {
                wr32(c, ea, GD);
                s.set_crf(0, 2u | ops::so(s));
            } else {
                s.set_crf(0, ops::so(s));
            }
            s.reserve = false;
            break;
        }
        OPC(Stmw): {
            uint32_t ea = GA0 + o.imm; nb(o.a, GA0);
            for (uint32_t r = o.d; r < 32; ++r, ea += 4) { wr32(c, ea, s.gpr[r]); kept(ea, r, s.gpr[r]); }
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
        OPC(Mfcr): nclear(o.d); GD = s.cr; break;
        OPC(Mtcrf): s.cr = (s.cr & ~o.imm) | (GD & o.imm); break;

        // ---- Processor control ----
        OPC(Mfmsr): nclear(o.d); GD = s.msr; break;
        OPC(Mtmsr): s.msr = GD; c.msr_changed(); break;
        OPC(Mfsr): GD = s.sr[o.x]; break;
        OPC(Mtsr): s.sr[o.x] = GD; break;
        OPC(Mfsrin): GD = s.sr[GB >> 28]; break;
        OPC(Mtsrin): s.sr[GB >> 28] = GD; break;
        case kMfTbl: nclear(o.d); GD = c.read_tbl(); break;
        case kMfTbu: GD = c.read_tbu(); break;
        case kMfDec: nclear(o.d); GD = c.read_dec(); break;
        case kMtTbl: c.write_tbl(GD); break;
        case kMtTbu: c.write_tbu(GD); break;
        case kMtDec: c.write_dec(GD); break;
        case kMfSpr: nclear(o.d); GD = spr_field(s, o.imm); break; // (a return address in LR: not the lis tag the register had)
        case kMtSpr:
            if (g_access_on) [[unlikely]] {
                if (o.imm == static_cast<uint32_t>(spr_offset(8))) nb(o.d, GD, 64); // LR: a return address or a jump
                else if (o.imm == static_cast<uint32_t>(spr_offset(9))) g_ctr = fresh(o.d, GD) ? g_loaded[o.d] : Loaded{};
            }
            spr_field(s, o.imm) = GD;
            break;
        case kUnknownSpr: c.unknown_spr(o.pc, o.imm);

        // ---- Traps and system calls (TO in d) ----
        case kTrapAlways: c.trap(o.pc); break;
        OPC(Tw): if (ops::trap_if(o.d, GA, GB)) c.trap(o.pc); break;
        OPC(Twi): if (ops::trap_if(o.d, GA, o.imm)) c.trap(o.pc); break;
        OPC(Sc): c.system_call(o.pc); break;
        OPC(Invalid): c.invalid(o.pc, o.imm);
        case kUnimplemented: c.unimplemented(o.pc, o.imm);

        // ---- Floating-point loads and stores (frD / frS in d) ----
        OPC(Lfs): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); FD = ops::single_to_double(rdf32(c, ea)); break; }
        OPC(Lfsu): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); FD = ops::single_to_double(rdf32(c, ea)); GA = ea; break; }
        OPC(Lfsx): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); FD = ops::single_to_double(rdf32(c, ea)); break; }
        OPC(Lfsux): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); FD = ops::single_to_double(rdf32(c, ea)); GA = ea; break; }
        OPC(Lfd): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); FD = rdf64(c, ea); break; }
        OPC(Lfdu): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); FD = rdf64(c, ea); GA = ea; break; }
        OPC(Lfdx): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); FD = rdf64(c, ea); break; }
        OPC(Lfdux): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); FD = rdf64(c, ea); GA = ea; break; }
        OPC(Stfs): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); wr32(c, ea, ops::double_to_single(FD)); break; }
        OPC(Stfsu): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); wr32(c, ea, ops::double_to_single(FD)); GA = ea; break; }
        OPC(Stfsx): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); wr32(c, ea, ops::double_to_single(FD)); break; }
        OPC(Stfsux): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); wr32(c, ea, ops::double_to_single(FD)); GA = ea; break; }
        OPC(Stfd): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); wr64(c, ea, FD); break; }
        OPC(Stfdu): { const uint32_t ea = GA0 + o.imm; nb(o.a, GA0); wr64(c, ea, FD); GA = ea; break; }
        OPC(Stfdx): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); wr64(c, ea, FD); break; }
        OPC(Stfdux): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); wr64(c, ea, FD); GA = ea; break; }
        OPC(Stfiwx): { const uint32_t ea = GA0 + GB; nbx(o.a, GA0, o.b, GB); wr32(c, ea, static_cast<uint32_t>(FD)); break; }

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
    std::set<uint32_t> hooks; // (where the program has them: SPIKEOUT_TEST_PROGRAM_MOVE)
    for (uint32_t h : game.hooks) hooks.insert(m3::test_move::moved(h));
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
