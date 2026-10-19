// Control-flow analysis: finds the functions, basic blocks and jump tables
// reachable from a set of entry points (reset and exception vectors).
//
// Code is followed by recursive descent. Along each straight-line path a
// small register tracker records constants (lis/addi/ori), loaded values and
// scaled table indices, which is enough to resolve:
//   - mtlr/mtctr/mtsrr0 of a constant followed by blr/bctr/rfi
//     (how the boot ROM enters the RAM copy of the program),
//   - jump and call tables: an index scaled by slwi/rlwinm, added to a table
//     address (lis+addi, or addis+addi on the index itself), loaded with
//     lwz/lwzx, then mtctr + bctr/bctrl. The table length comes from a
//     cmplwi + bgt/ble guard when there is one (exact), otherwise from the
//     index's bit width, and entries are read until one isn't code.
// What can't be resolved (function pointers loaded from memory, computed
// returns) is reported as an issue so it can be handled by hand or by the
// runtime's fallback interpreter.
#pragma once

#include "analysis/code_image.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace analysis {

// Why an address is a function entry.
enum class EntryKind : uint8_t {
    Seed,            // given by the caller: a vector or a hint
    Call,            // target of bl, bcl, or bctrl/blrl with a constant target
    TableCall,       // entry of a table called through bctrl/blrl
    ConstantJump,    // blr/bctr with a constant LR/CTR (leaves the function)
    ExceptionReturn, // rfi with a constant SRR0
    Pointer,         // found by the pointer scan (Options::scan_pointers)
};

std::string_view entry_kind_name(EntryKind kind);

struct Entry {
    uint32_t addr;
    EntryKind kind = EntryKind::Seed;
    std::string name;
};

// How a basic block ends.
enum class Exit : uint8_t {
    Fallthrough, // runs into the next block, which something else branches to
    Jump,        // b, or a bc that always branches
    Branch,      // conditional bc: target and fallthrough
    Return,      // blr (a conditional one also falls through)
    TailCall,    // jump to another function's entry
    JumpTable,   // bctr through a recognized table
    Indirect,    // bctr/blr whose target couldn't be worked out
    Rfi,
    Trap,        // unconditional trap (tw 31)
    Invalid,     // the last word doesn't decode
    OutOfImage,  // falls off the end of the image
};

std::string_view exit_name(Exit exit);

struct Block {
    uint32_t start = 0;
    uint32_t end = 0; // one past the last instruction
    Exit exit = Exit::Fallthrough;
    std::vector<uint32_t> successors; // inside the same function
    std::vector<uint32_t> calls;      // functions called from this block

    uint32_t instruction_count() const { return (end - start) / 4; }
};

struct Function {
    uint32_t entry = 0;
    EntryKind kind = EntryKind::Seed;
    std::string name;
    std::vector<uint32_t> blocks; // block start addresses, sorted
    uint32_t instructions = 0;
};

struct JumpTable {
    uint32_t site = 0;   // the bctr/bctrl
    uint32_t table = 0;  // address of entry 0
    uint32_t stride = 4; // bytes between entries
    bool exact = false;  // length known from a compare, not guessed
    bool call = false;   // bctrl: entries are functions
    std::vector<uint32_t> targets;
};

enum class IssueKind : uint8_t {
    InvalidInstruction, // ran into a word that doesn't decode
    InvalidAfterCall,   // ...right after a call (the callee probably never returns)
    TargetOutsideImage, // branch or call to an address that has no code loaded
    UnresolvedJump,     // bctr/blr to an unknown address
    UnresolvedCall,     // bctrl/blrl to an unknown address (function pointer)
    BadTableEntry,      // an exactly-bounded table holds a non-code address
};

std::string_view issue_kind_name(IssueKind kind);

struct Issue {
    IssueKind kind;
    uint32_t addr;       // instruction the issue is about
    uint32_t target = 0; // for TargetOutsideImage / BadTableEntry
};

struct Options {
    // Longest table read when the length isn't known from a compare.
    uint32_t max_table_entries = 1024;

    // Also treat code addresses found as values as function entries: words
    // in the segments listed in `pointer_segments` (by name) and constants
    // the code builds in registers (lis + addi), pointing into those same
    // segments. Most of a game is reached through callbacks stored at run
    // time, which the vectors alone don't reach. A value counts only if it
    // points at plausible code right after a function boundary (blr,
    // unconditional branch, rfi or padding).
    bool scan_pointers = false;
    std::vector<std::string> pointer_segments;
};

struct Analysis {
    std::map<uint32_t, Block> blocks;       // by start address
    std::map<uint32_t, Function> functions; // by entry address
    std::vector<JumpTable> jump_tables;
    std::vector<Issue> issues; // sorted by address

    uint32_t instruction_count() const;
    const Block* block_containing(uint32_t addr) const;
};

Analysis analyze(const CodeImage& image, std::span<const Entry> entries, const Options& options = {});

} // namespace analysis
