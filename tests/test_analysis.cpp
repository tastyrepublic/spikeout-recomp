#include "test.hpp"

#include "analysis/boot_image.hpp"
#include "analysis/code_image.hpp"
#include "analysis/control_flow.hpp"
#include "runtime/rom.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace analysis;

namespace {

// Tiny assembler for the instructions the tests need.
constexpr uint32_t D(uint32_t op, uint32_t rd, uint32_t ra, uint32_t imm) {
    return op << 26 | rd << 21 | ra << 16 | (imm & 0xFFFF);
}
constexpr uint32_t X(uint32_t xo, uint32_t rd, uint32_t ra, uint32_t rb) {
    return 31u << 26 | rd << 21 | ra << 16 | rb << 11 | xo << 1;
}
constexpr uint32_t li(uint32_t r, uint32_t imm) { return D(14, r, 0, imm); }
constexpr uint32_t lis(uint32_t r, uint32_t imm) { return D(15, r, 0, imm); }
constexpr uint32_t addi(uint32_t rd, uint32_t ra, uint32_t imm) { return D(14, rd, ra, imm); }
constexpr uint32_t addis(uint32_t rd, uint32_t ra, uint32_t imm) { return D(15, rd, ra, imm); }
constexpr uint32_t ori(uint32_t ra, uint32_t rs, uint32_t imm) { return D(24, rs, ra, imm); }
constexpr uint32_t mulli(uint32_t rd, uint32_t ra, uint32_t imm) { return D(7, rd, ra, imm); }
constexpr uint32_t cmplwi(uint32_t ra, uint32_t imm) { return D(10, 0, ra, imm); }
constexpr uint32_t lwz(uint32_t rd, uint32_t d, uint32_t ra) { return D(32, rd, ra, d); }
constexpr uint32_t lbz(uint32_t rd, uint32_t d, uint32_t ra) { return D(34, rd, ra, d); }
constexpr uint32_t slwi(uint32_t ra, uint32_t rs, uint32_t n) {
    return 21u << 26 | rs << 21 | ra << 16 | n << 11 | 0u << 6 | (31 - n) << 1;
}
constexpr uint32_t lwzx(uint32_t rd, uint32_t ra, uint32_t rb) { return X(23, rd, ra, rb); }
constexpr uint32_t add(uint32_t rd, uint32_t ra, uint32_t rb) { return X(266, rd, ra, rb); }
constexpr uint32_t mtspr(uint32_t spr, uint32_t rs) {
    return 31u << 26 | rs << 21 | (spr & 31) << 16 | (spr >> 5) << 11 | 467u << 1;
}
constexpr uint32_t mtlr(uint32_t rs) { return mtspr(8, rs); }
constexpr uint32_t mtctr(uint32_t rs) { return mtspr(9, rs); }
constexpr uint32_t mtsrr0(uint32_t rs) { return mtspr(26, rs); }
constexpr uint32_t b(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC); }
constexpr uint32_t bl(uint32_t from, uint32_t to) { return b(from, to) | 1; }
constexpr uint32_t bgt(uint32_t from, uint32_t to) { return 16u << 26 | 12u << 21 | 1u << 16 | ((to - from) & 0xFFFC); }
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kBctr = 0x4E800420;
constexpr uint32_t kRfi = 0x4C000064;
constexpr uint32_t kNop = 0x60000000;

constexpr uint32_t kBase = 0x1000;
constexpr uint32_t at(uint32_t i) { return kBase + 4 * i; }

std::vector<uint8_t> to_bytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    for (uint32_t w : words)
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(w >> shift));
    return bytes;
}

CodeImage image_of(const std::vector<uint32_t>& words) {
    CodeImage image;
    image.add("test", kBase, to_bytes(words));
    return image;
}

Analysis run(const std::vector<uint32_t>& words, std::vector<uint32_t> seeds = {kBase}) {
    std::vector<Entry> entries;
    for (uint32_t s : seeds) entries.push_back({s, EntryKind::Seed, ""});
    return analyze(image_of(words), entries);
}

bool has_issue(const Analysis& a, IssueKind kind, uint32_t addr) {
    return std::any_of(a.issues.begin(), a.issues.end(),
                       [&](const Issue& i) { return i.kind == kind && i.addr == addr; });
}

} // namespace

TEST_CASE("code image: reads and bounds") {
    CodeImage image;
    image.add("a", 0x1000, to_bytes({0x11223344, 0x55667788}));
    image.add("top", 0xFFFFFFF8, to_bytes({0xAABBCCDD, 0xEEFF0011}));
    CHECK_EQ(*image.read32(0x1004), 0x55667788u);
    CHECK_EQ(*image.read32(0xFFFFFFFC), 0xEEFF0011u); // segment ending at the top of memory
    CHECK(!image.read32(0x1008));
    CHECK(!image.read32(0x1002)); // misaligned
    CHECK(!image.read32(0x0FFC));
    bool threw = false;
    try {
        image.add("overlap", 0x1004, to_bytes({0}));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("control flow: call and return") {
    const auto a = run({
        bl(at(0), at(3)),
        kNop,
        kBlr,
        li(3, 1), // callee
        kBlr,
    });
    CHECK_EQ(a.functions.size(), size_t{2});
    CHECK_EQ(a.functions.at(at(3)).kind, EntryKind::Call);
    const Block& caller = a.blocks.at(at(0));
    CHECK_EQ(caller.end, at(3));
    CHECK_EQ(caller.exit, Exit::Return);
    CHECK_EQ(caller.calls.size(), size_t{1});
    CHECK_EQ(a.instruction_count(), 5u);
    CHECK(a.issues.empty());
}

TEST_CASE("control flow: LR restored from the stack is a return") {
    const auto a = run({lwz(0, 20, 1), mtlr(0), kBlr});
    CHECK_EQ(a.blocks.at(kBase).exit, Exit::Return);
    CHECK(a.issues.empty());
}

TEST_CASE("control flow: if/else blocks") {
    const auto a = run({
        cmplwi(3, 0),
        bgt(at(1), at(4)),
        li(4, 1),
        b(at(3), at(5)),
        li(4, 2),
        kBlr,
    });
    CHECK_EQ(a.blocks.size(), size_t{4});
    CHECK_EQ(a.blocks.at(at(0)).exit, Exit::Branch);
    CHECK_EQ(a.blocks.at(at(0)).successors.size(), size_t{2});
    CHECK_EQ(a.blocks.at(at(2)).exit, Exit::Jump);
    CHECK_EQ(a.blocks.at(at(4)).exit, Exit::Fallthrough);
    CHECK_EQ(a.blocks.at(at(4)).successors.at(0), at(5));
    CHECK_EQ(a.blocks.at(at(5)).exit, Exit::Return);
    CHECK_EQ(a.functions.at(kBase).blocks.size(), size_t{4});
}

TEST_CASE("control flow: jump table guarded by cmplwi") {
    const auto a = run({
        cmplwi(3, 2),         // 0
        bgt(at(1), at(10)),   // 1: default
        lis(4, 0),            // 2
        slwi(3, 3, 2),        // 3
        addi(4, 4, at(16)),   // 4: table
        lwzx(5, 4, 3),        // 5
        mtctr(5),             // 6
        kBctr,                // 7
        li(6, 1), kBlr,       // 8: case 0
        li(6, 9), kBlr,       // 10: default, case 2
        li(6, 2), kBlr,       // 12: case 1
        kNop, kNop,           // 14: never reached
        at(8), at(12), at(10), at(14), // 16: table; the 4th entry is past the bound
    });
    CHECK_EQ(a.jump_tables.size(), size_t{1});
    const JumpTable& t = a.jump_tables.at(0);
    CHECK_EQ(t.site, at(7));
    CHECK_EQ(t.table, at(16));
    CHECK(t.exact);
    CHECK(!t.call);
    CHECK((t.targets == std::vector<uint32_t>{at(8), at(12), at(10)}));
    CHECK_EQ(a.blocks.at(at(2)).exit, Exit::JumpTable);
    CHECK(!a.block_containing(at(14)));
    CHECK(a.issues.empty());
}

TEST_CASE("control flow: unguarded table on a byte index (addis on the index)") {
    const auto a = run({
        lbz(3, 0, 4),          // 0
        slwi(3, 3, 2),         // 1
        addis(3, 3, 0),        // 2
        addi(3, 3, at(12)),    // 3
        lwz(3, 0, 3),          // 4
        mtctr(3),              // 5
        kBctr,                 // 6
        li(5, 1), kBlr,        // 7
        li(5, 2), kBlr,        // 9
        kNop,                  // 11
        at(7), at(9), 0,       // 12: the table ends at the first non-code entry
    });
    CHECK_EQ(a.jump_tables.size(), size_t{1});
    CHECK(!a.jump_tables.at(0).exact);
    CHECK_EQ(a.jump_tables.at(0).targets.size(), size_t{2});
    CHECK_EQ(a.functions.at(kBase).blocks.size(), size_t{3});
}

TEST_CASE("control flow: a guessed table ends where the next one starts") {
    const auto a = run(
        {
            lbz(3, 0, 4), slwi(3, 3, 2), lis(5, 0), addi(5, 5, at(20)), lwzx(6, 5, 3), mtctr(6), kBctr, // 0
            lbz(3, 0, 4), slwi(3, 3, 2), lis(5, 0), addi(5, 5, at(22)), lwzx(6, 5, 3), mtctr(6), kBctr, // 7
            li(7, 1), kBlr,  // 14
            li(7, 2), kBlr,  // 16
            li(7, 3), kBlr,  // 18
            at(14), at(16),  // 20: first table
            at(18), at(14),  // 22: second table, right after it
            0,
        },
        {at(0), at(7)});
    CHECK_EQ(a.jump_tables.size(), size_t{2});
    CHECK_EQ(a.jump_tables.at(0).targets.size(), size_t{2});
    CHECK_EQ(a.jump_tables.at(1).targets.size(), size_t{2});
}

TEST_CASE("control flow: a table entry pointing at text isn't code") {
    const auto a = run({
        lbz(3, 0, 4), slwi(3, 3, 2), lis(5, 0), addi(5, 5, at(10)), lwzx(6, 5, 3), mtlr(6),
        0x4E800021, // blrl
        kBlr,
        li(7, 1), kBlr,         // 8
        at(8), at(12),          // 10: call table
        0x20202020, 0x45584954, // 12: "    EXIT": decodes as subfic, then nothing
    });
    CHECK_EQ(a.jump_tables.size(), size_t{1});
    CHECK(a.jump_tables.at(0).call);
    CHECK((a.jump_tables.at(0).targets == std::vector<uint32_t>{at(8)}));
    CHECK(!a.functions.contains(at(12)));
    CHECK(a.issues.empty());
}

TEST_CASE("control flow: pointer scan finds callbacks in data and constants") {
    const std::vector<uint32_t> code = {
        lis(3, 0), addi(3, 3, at(6)), D(36, 3, 4, 0), kBlr, // 0: stores a callback (stw r3, 0(r4))
        li(5, 0), kBlr,                                      // 4: only pointed to from data
        li(5, 1), kBlr,                                      // 6: the stored callback
        li(6, 1), li(6, 2), kBlr,                            // 8: data points into the middle (9)
        0,
        at(4), at(9), at(16),                                // 12: data
        kBlr,
        0x42EB0000,                                          // 16: 117.5f, decodes as a branch to itself
    };
    const CodeImage image = image_of(code);
    const std::array entries = {Entry{kBase, EntryKind::Seed, ""}};

    CHECK_EQ(analyze(image, entries).functions.size(), size_t{1});

    Options options;
    options.scan_pointers = true;
    options.pointer_segments = {"test"};
    const Analysis a = analyze(image, entries, options);
    CHECK_EQ(a.functions.size(), size_t{3});
    CHECK_EQ(a.functions.at(at(4)).kind, EntryKind::Pointer);
    CHECK_EQ(a.functions.at(at(6)).kind, EntryKind::Pointer);
    CHECK(!a.functions.contains(at(9))); // not after a function boundary
    CHECK(!a.functions.contains(at(8)));  // nothing points here
    CHECK(!a.functions.contains(at(16))); // a float, not code
}

TEST_CASE("control flow: constant blr and rfi start new functions") {
    const auto a = run({
        li(3, static_cast<uint32_t>(at(4))), // 0
        mtlr(3),
        kBlr,
        kNop,
        lis(3, 0),                           // 4
        ori(3, 3, at(8)),
        mtsrr0(3),
        kRfi,
        b(at(8), at(8)),                     // 8: loops forever
    });
    CHECK_EQ(a.functions.size(), size_t{3});
    CHECK_EQ(a.functions.at(at(4)).kind, EntryKind::ConstantJump);
    CHECK_EQ(a.functions.at(at(8)).kind, EntryKind::ExceptionReturn);
    CHECK_EQ(a.blocks.at(at(0)).exit, Exit::TailCall);
    CHECK_EQ(a.blocks.at(at(4)).exit, Exit::Rfi);
    CHECK_EQ(a.blocks.at(at(8)).exit, Exit::Jump); // a jump to its own entry is a loop
    CHECK(a.issues.empty());
}

TEST_CASE("control flow: computed blr is reported") {
    const auto a = run({
        mulli(2, 8, 12),
        lis(10, 0),
        addi(10, 10, 0x1100),
        add(10, 10, 2),
        mtlr(10),
        kBlr,
    });
    CHECK_EQ(a.blocks.at(kBase).exit, Exit::Indirect);
    CHECK(has_issue(a, IssueKind::UnresolvedJump, at(5)));
}

TEST_CASE("control flow: data after a call that never returns") {
    const auto a = run({bl(at(0), at(3)), 0, 0, b(at(3), at(3))});
    CHECK(has_issue(a, IssueKind::InvalidAfterCall, at(1)));
    CHECK(!has_issue(a, IssueKind::InvalidInstruction, at(1)));
}

TEST_CASE("control flow: tail call stays out of the caller") {
    const auto a = run(
        {
            bl(at(0), at(4)),
            kBlr,
            li(3, 0),         // 2: second seed
            b(at(3), at(4)),  // tail call
            kBlr,             // 4
        },
        {at(0), at(2)});
    CHECK_EQ(a.blocks.at(at(2)).exit, Exit::TailCall);
    CHECK((a.functions.at(at(2)).blocks == std::vector<uint32_t>{at(2)}));
    CHECK_EQ(a.functions.at(at(4)).instructions, 1u);
}

TEST_CASE("control flow: targets outside the image") {
    const auto a = run({bl(at(0), 0x200000), kBlr});
    CHECK_EQ(a.issues.size(), size_t{1});
    CHECK_EQ(a.issues.at(0).kind, IssueKind::TargetOutsideImage);
    CHECK_EQ(a.issues.at(0).target, 0x200000u);
    CHECK_EQ(a.functions.size(), size_t{1});
}

TEST_CASE("boot image: high vectors, RAM copy and the jump into it") {
    m3::RomSet set;
    auto& crom = set.region(m3::RomRegionId::Crom);
    crom.assign(0x800000, 0);
    auto put = [&](uint32_t addr, uint32_t word) {
        const uint32_t off = addr - 0xFF800000;
        for (int k = 0; k < 4; ++k) crom[off + k] = static_cast<uint8_t>(word >> (24 - 8 * k));
    };
    put(0xFFF00100, li(3, 0x100)); // reset: jump to the RAM copy
    put(0xFFF00104, mtlr(3));
    put(0xFFF00108, kBlr);
    put(0xFFE00100, 0x48000000);   // RAM 0x100: b .

    const std::array copies = {m3::CodeCopy{0, 0xFFE00000, 0x2000}};
    const m3::GameSpec game{"test", "Test", 0, {}, copies};
    const BootImage boot = make_boot_image(game, set);

    CHECK_EQ(boot.image.segments().size(), size_t{2});
    CHECK_EQ(boot.entries.size(), size_t{2}); // the only non-zero vectors
    CHECK_EQ(boot.entries.at(0).addr, 0xFFF00100u);
    CHECK_EQ(boot.entries.at(0).name, std::string("system_reset"));
    CHECK_EQ(boot.entries.at(1).addr, 0x100u);
    CHECK_EQ(boot.entries.at(1).name, std::string("ram_system_reset"));

    const Analysis a = analyze(boot.image, boot.entries);
    CHECK_EQ(a.functions.size(), size_t{2});
    CHECK_EQ(a.blocks.at(0xFFF00100).exit, Exit::TailCall);
    CHECK(a.issues.empty());
}
