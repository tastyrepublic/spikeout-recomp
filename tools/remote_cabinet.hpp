// One board a PC (docs/netplay.md, "One board a PC"): the other cabinets of
// a linked game made here, from this board's own game and their players'
// buttons, with no main board of theirs running. Each other cabinet keeps
// its network board (on the ring as before), fed each frame a block made the
// way its main board would have written it: this board's records of the same
// logic counts (the shared game is the same on every board) with that
// cabinet's own fields: its buttons, its camera's heading and view of every
// character (its camera run here on the side: its update, its view tests),
// its status (its own flow, the mode dispatcher, run here on the side with
// its own variables), its player's fields as this board has them when it
// writes its own record.
//
// A side run is game code run once more, as the other cabinet: its camera
// in the camera's place, its own variables in theirs, its buttons, its local
// player; the bus's journal puts RAM back after, device writes are held
// (its sounds aren't this board's), the CPU and its clock put back, no
// interrupts. So this board's own game is untouched: the same, hash for
// hash, as with the other boards running.
//
// Relations: a slave runs a frame behind the master. A record of logic count
// L carries its camera's heading after update L-1, its view of each
// character from the view tests of L-2, its player's first word's view bits
// from L-1. A slave's game reads the master's record from two counts back,
// which a slave has made itself by then; the master's newest (the count a
// slave hasn't reached) is made from the slave's own newest for now.
#pragma once

#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/netboard.hpp"
#include "runtime/watched_memory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tools::remote {

// The camera (r12 its update's, at 0x11D480; +0x2A its heading as the link
// sends it, +0x4A the player it follows, +0x640 its view matrix), its update,
// its view of a character (r14: flags bit 0x08 on its screen, 0x10 near it),
// what makes its view after the update; the update's end (0xC670 called from
// 0x32514). The logic counter.
constexpr uint32_t kCamera = 0x11D480, kCameraBytes = 0xC00, kCameraHeading = 0x2A, kCameraFollows = 0x4A, kViewMatrix = 0x640;
constexpr uint32_t kCameraUpdate = 0x2C4DC, kViewTest = 0x33A6C, kCameraView = 0x32578;
constexpr uint32_t kAfterUpdate = 0xC670, kAfterUpdateReturn = 0x32514;
constexpr uint32_t kLogicCounter = 0x1000A8;
// Where a cabinet writes its own record (+0x04 its buttons, +0x10 its
// player's first word, +0xDC the logic count).
constexpr uint32_t kRecordWrite = 0x6DA90;
// Camera shakes (0x345E4, r3 the kind: the table at 0x90114, 8 bytes a kind,
// its byte into the camera's +0xD2 and +0xD3, its float into +0xD4) decided
// by a cabinet's own player: 0x21C88 (a character's animation command, r14
// the character, r28+4 the kind: in mode 3 with a flag every cabinet's, else
// the local player's or one holding it) and 0x22750 (kind 7 for the player
// the camera follows), and 0x26070 (a hit, r14 the one hitting, r15 the one
// hit: the kind its +7784, when the local player is one of them, or in mode 3
// with a flag, unless the one hit is down). This board's own decisions are
// written at 0x345EC.
constexpr uint32_t kShakeByAnimation = 0x21C88, kShakeFollowed = 0x22750, kShakeByHit = 0x26070, kShakeWrite = 0x345EC,
                   kShakeTable = 0x90114;
// A cabinet's own flow: the mode dispatcher, once a frame from the main loop.
constexpr uint32_t kModeDispatch = 0x492AC;
// This cabinet's own continue and name entry keep their working values in
// its camera (+0xB04-0xB1E): not the other's.
constexpr uint32_t kOwnCameraWriters[] = {0x7682C, 0x76F78, 0x76868, 0x76944};
// The block: header 0x100, four records of 0x200 (a record of logic count L
// in slot L & 3), at 0x141000 in RAM's order; written to the network board
// at 0xC0000100.
constexpr uint32_t kBlock = 0x141000, kBlockWords = 0x240, kNetBlock = 0xC0000100;
// The cabinet's working record (0x140200, filled by its game as a count runs, stamped and copied into the
// block at its next link by 0x6DA48).
constexpr uint32_t kWorkingRecord = 0x140200;
// The link (0x6D758, once a frame from the main loop after it wakes): what it received copied in from the
// network board (0xC0000100 + 0xC00 a node, to 0x142000 + 0x900 a node), whether its count goes on (0x6DBE0:
// the others' counts too old, held back; 0x6DCB0: who is in the game, the header's node mask +0x28), its
// header (0x6D9A8), its record stamped and copied into its block (0x6DA48), the block to its network board
// (0x6DB98). A frame it holds back ends at 0x36E94 and the main loop's top, its flow and drawing skipped.
// Its memory: 0x140000-0x14FFFF (control, header 0x140100, working record, the others' records, its block
// 0x141000, what it received), and its counters (0x1000A4 its frames, 0x1000A8 its logic count) and
// variables (-7544..-7531(r13)).
constexpr uint32_t kLink = 0x6D758, kLinkSkipped = 0x36E94;
// The main loop's tasks (0x11DF4: the list at 0x110000, the world's step among them, then the drawing, which
// waits for the vblank at 0x36E04).
constexpr uint32_t kTasks = 0x11DF4, kVblankWait = 0x36E04;
// The task list (each entry: +0 its flags, bit 0 run; +8 the next's offset; +12 its routine), and its 2D task
// (0x135C0: the HUD's tile maps, through 0x92EC).
constexpr uint32_t kTaskList = 0x110000, k2dTask = 0x135C0, kTaskCount = 0x8CF64; // (how many tasks the walker runs: the ROM's word)
// Where the main loop goes on after its call of the tasks; its wake flag (-32709(r13): its idle wait goes on
// when set, a frame missed counted when 2 or more).
constexpr uint32_t kTasksReturn = 0x11AE0, kWakeFlag = 0x10004B;
// In 0x36E94, its frames pushed and its time taken, its wait while the decrementer is negative (0x36F24; just
// after, it reads the wake flag for its idle wait): the decrementer's interrupt (0x900) bumps the flag, the
// board's wake-up.
constexpr uint32_t kBeforeIdle = 0x36F24;
// The vblank wait's loop (0x36E08, until -32712(r13), the vblank count, changes) and a transfer's (0x4DD8, the OS
// waiting for 0x7100B4 to clear when the transfer is done): in a side run that goes through the drawing, no
// vblank comes and no transfer is made (device writes held), so each is let through there.
constexpr uint32_t kVblankLoop = 0x36E08, kVblankCount = 0x100038, kTransferLoop = 0x4DD8, kTransferBusy = 0x7100B4;
// The drawing's copy of the world's words into the link's header (0x624F4, r12 0x11CF80: +0x50 its word, +0x5C the
// +0x288 of the object at -16068(r13)), before the next link sends them; the drawing changes the word first.
constexpr uint32_t kHeaderWorld = 0x11CF80, kHeaderObject = 0x10413C;
// A character's animation step (0x62AF4, r14 the character, from its update): its pose into its buffers
// (through its +0x2400, in 0x210000-0x22FFFF), which differ between cabinets (the pose of a character a
// cabinet's camera doesn't see isn't worked out the same) and which its game reads back.
constexpr uint32_t kAnimationStep = 0x62AF4;
// Each cabinet's own areas, kept for it whole: its 2D (0x180000-0x19FFFF: its layers, its tasks' 2D state, which
// its 2D tasks read back: run on another's, they unpack from nonsense), the objects of players 3 and 4 (0x1A8000-
// 0x1AFFFF: unused with two players, each cabinet's leftovers there, which its tasks copy into its records; a
// seat with a player in it (the link's players, the header's +0x28: 0x80000000 >> its node) the game's, not
// a cabinet's) and its pose buffers (0x210000-0x22FFFF).
constexpr std::pair<uint32_t, uint32_t> kOwnRegions[] = {{0x180000, 0x1A0000}, {0x1A8000, 0x1AC000}, {0x1AC000, 0x1B0000}, {0x210000, 0x230000}};
constexpr uint32_t kRegionBytes = 0x48000;
constexpr uint32_t kLinkPlayers = 0x140128;
// The players in play (the word at +0x50 of the object -16108(r13) points to, 0x111100 in play: a player's bit cleared
// as they time out, set as they play on) and a cabinet's mode (-16376(r13), its own), which its player's update
// (0x55050) tests before writing its record's +0x08 to +0x0D. r13 is 0x108000.
constexpr uint32_t kPlayersObject = 0x104114, kOwnMode = 0x104008;
constexpr uint32_t kLinkError = 0x106288; // (the link's error bits, -7544(r13): its checks of the entries received)
// Whether own area `i` is a cabinet's own with the link's players `mask` (players 3 and 4: while their seats are empty).
inline bool region_own(std::size_t i, uint32_t mask) {
    return !(i == 1 && (mask & (0x80000000u >> 2))) && !(i == 2 && (mask & (0x80000000u >> 3)));
}
inline std::vector<uint8_t> gather_regions(std::span<const uint8_t> ram) {
    std::vector<uint8_t> out;
    out.reserve(kRegionBytes);
    for (const auto& [lo, hi] : kOwnRegions) out.insert(out.end(), ram.begin() + lo, ram.begin() + hi);
    return out;
}
// (its own areas into `ram`, those of the link's players there as `ram` has them)
inline void scatter_regions(std::span<uint8_t> ram, const std::vector<uint8_t>& regions) {
    const uint32_t mask = uint32_t(ram[kLinkPlayers]) << 24 | uint32_t(ram[kLinkPlayers + 1]) << 16 | uint32_t(ram[kLinkPlayers + 2]) << 8 | ram[kLinkPlayers + 3];
    std::size_t n = 0, i = 0;
    for (const auto& [lo, hi] : kOwnRegions) {
        if (region_own(i++, mask))
            for (uint32_t a = lo; a < hi; ++a) ram[a] = regions[n + a - lo];
        n += hi - lo;
    }
}
constexpr uint32_t kLinkMemory = 0x140000, kLinkBytes = 0x10000;
constexpr std::pair<uint32_t, uint32_t> kLinkVars[] = {{0x100004, 0x100008}, {0x1000A4, 0x1000AC}, {0x106288, 0x106295}};
// In the link memory, what the game (not the link) writes, the same on every cabinet (the world's): +0x44 (0x5A9A4,
// 0x5A94C), the header's +0x06 (0x4B4C0), +0x18 (0x6F7B0 from 0x59B8C), +0x3F (0x59C7C), +0x50-0x57 and +0x5C-0x5F
// (0x624F4 from 0x5EA24, each frame: the master's header sends +0x50 to the others). A made board's link memory
// takes them from this board's, not its last link's (2026-11-07: +0x53 went stale, tools/onepc-diff.ps1).
constexpr std::pair<uint32_t, uint32_t> kLinkGameBytes[] = {
    {0x140044, 0x140046}, {0x140118, 0x14011C}, {0x14013F, 0x140140}, {0x140150, 0x140154}, {0x14015C, 0x140160}};
// The main loop's own count (-32764(r13), one up at its top every time round: what a link resync puts in the
// header's +0x20).
constexpr uint32_t kLoopCount = 0x100004;
// Twelve countdowns (-16188(r13) on), each nonzero one a step down at every vblank (0x496C, at the end of its
// interrupt): 0x1040C7, one of them, a cabinet's own (set to 10 at 0x36838).
constexpr uint32_t kVblankCountdowns = 0x1040C4;
using Block = std::array<uint32_t, kBlockWords>;

// A cabinet's own variables (its flow's, its camera's): the bytes of r13's
// areas that differ between two linked boards through a time over, a
// continue and a game over, less the main loop's timings; its buttons'
// words (0x5094's) and copies; its header status; each character's view of
// it (+0x34A0-0x34BF, which its view tests write and its camera's update
// reads) and view bits (the first word's low byte).
constexpr std::pair<uint32_t, uint32_t> kCabinetOwn[] = {
    {0x104000, 0x104014}, {0x10401A, 0x10401C}, {0x104024, 0x10402C}, {0x104051, 0x104052}, {0x10407B, 0x10407C},
    {0x104083, 0x104084}, {0x104090, 0x104094}, {0x1040B0, 0x1040BC}, {0x1040C7, 0x1040C8}, {0x1040F0, 0x1040FC},
    {0x104103, 0x104104}, {0x10065B, 0x10065C}, {0x106004, 0x10600C}, {0x106180, 0x106181}, {0x1061B7, 0x1061BB},
    {0x1061CD, 0x1061E6}, {0x106242, 0x10624D}, {0x106268, 0x10626C}, {0x106295, 0x1062A4}, {0x1062AD, 0x1062AE},
    {0x1062C7, 0x1062C8}, {0x1062DC, 0x1062DD}, {0x100100, 0x100120}, {0x140104, 0x140108}, {0x140154, 0x140157}, {0x140300, 0x140304},
    {0x1A34A0, 0x1A34C0}, {0x1A74A0, 0x1A74C0}, {0x1AB4A0, 0x1AB4C0}, {0x1AF4A0, 0x1AF4C0}, {0x1B34A0, 0x1B34C0},
    {0x1B74A0, 0x1B74C0}, {0x1BB4A0, 0x1BB4C0}, {0x1BF4A0, 0x1BF4C0}, {0x1C34A0, 0x1C34C0}, {0x1C74A0, 0x1C74C0},
    {0x1CB4A0, 0x1CB4C0}, {0x1CF4A0, 0x1CF4C0},
    {0x1A0003, 0x1A0004}, {0x1A4003, 0x1A4004}, {0x1A8003, 0x1A8004}, {0x1AC003, 0x1AC004}, {0x1B0003, 0x1B0004},
    {0x1B4003, 0x1B4004}, {0x1B8003, 0x1B8004}, {0x1BC003, 0x1BC004}, {0x1C0003, 0x1C0004}, {0x1C4003, 0x1C4004},
    {0x1C8003, 0x1C8004}, {0x1CC003, 0x1CC004},
    // (the header task kHeaderWorld's fields: its HUD object, each cabinet's own (its flags word changes with its own timing:
    // a third cabinet's 9 frames after the others', 2026-11-07, tools/onepc-diff.ps1); bit 0 of its word 0 on the master, 1 on a slave)
    {0x11CF80, 0x11D000}};

inline std::vector<uint32_t> own_addresses() {
    std::vector<uint32_t> out;
    for (const auto& [lo, hi] : kCabinetOwn)
        for (uint32_t a = lo; a < hi; ++a) out.push_back(a);
    return out;
}
// A player's object (0..3), an enemy's (0..7).
constexpr uint32_t player_object(std::size_t seat) { return 0x1A0000 + 0x4000 * static_cast<uint32_t>(seat); }
constexpr uint32_t character_object(uint32_t k) { return k < 4 ? 0x1A0000 + 0x4000 * k : 0x1B0000 + 0x4000 * (k - 4); }
// A record's character entry's +4..+7 (0x74808, 0x74884): the low bytes of these fields of the character's object
// (offset, size). Each cabinet keeps its own: +0x34FC is its logic count when the character was set up (0x1E938),
// which differs between cabinets by as many counts as one had missed before (two in a bot game: 0x24F against
// 0x251, an enemy set up long before one board a PC began).
constexpr std::pair<uint32_t, uint32_t> kEntryFields[4] = {{0x3F02, 2}, {0x3F22, 2}, {0x34A0, 4}, {0x34FC, 4}};
// A record's character entry's +8 (0x74808, 0x74884): the sum of these fields of the character's object (offset,
// size). An object a character has left keeps them as its cabinet last wrote them, which differ between cabinets
// (2026-11-09: enemy 1 gone, the master's +0x74 63ac against 3245 and 3107 on the slaves): each cabinet's own sum,
// kept as its fields are (Remote::kept_sum); a one-board game went out of step 5 frames after a hand-back with
// every made cabinet sending the staying board's (tools/window-autoplay-test.ps1, frame 4440).
constexpr std::pair<uint32_t, uint32_t> kSumFields[7] = {{0x74, 2}, {0x76, 2}, {0x3C, 2}, {0x3E, 2}, {0x380, 4}, {0x384, 4}, {0x388, 4}};

// A seat's buttons word (0x5094's held word: the system board's 0x04 under
// select 0 in bits 0-7, 0x08 in 8-15, 0x0C in 16-23, 0x04 under select 1 in
// 24-31, inverted; the EEPROM's data line idle) from its input bytes.
constexpr uint32_t held_word(uint8_t bank0, uint8_t controls, uint8_t controls2 = 0xFF) {
    return ~(uint32_t{bank0} | uint32_t{controls} << 8 | uint32_t{controls2} << 16 | 0xFFu << 24);
}

// What a side cabinet starts from: its camera (kCameraBytes) and its own
// variables (own_addresses' order), the latter after its flow of count
// `own_logic`.
struct Seed {
    std::vector<uint8_t> camera, own, regions; // (regions: kRegionBytes, or empty: this board's)
    uint32_t own_logic = 0;
};

struct LinkSkipped {};  // (a link run on the side holding its frame back: done)
struct TasksDrawing {}; // (a frame's tasks run on the side reaching the drawing: done)
struct DrawingDone {};  // (the master's capture through its drawing reaching its main loop's 0x36E94)

class Cabinets {
public:
    // `held(seat, frame)`: that seat's held buttons word for its frame (each
    // board's frames count alike). `write_at`: when in its frame another's link
    // runs here, writing its block (cycles after the frame starts: after this
    // board's view tests of the count, about 2.4M in, which its newest record
    // carries when it is a count ahead; before the frame's end, when its
    // network board sends).
    using Held = std::function<uint32_t(std::size_t seat, uint64_t frame)>;
    // A side cabinet's start, asked at this board's flow of count `logic` (its camera as its update of
    // `logic` starts, its own variables after its flow of Seed::own_logic).
    using Seeder = std::function<bool(std::size_t seat, uint32_t logic, Seed& out)>;
    Cabinets(recomp::Context& c, m3::Bus& bus, recomp::FrameTiming& timing, std::size_t local_seat, Held held,
             uint64_t write_at = 2800000)
        : c_(c), bus_(bus), timing_(timing), local_(local_seat), held_(std::move(held)), write_at_(write_at), own_(own_addresses()),
          own_mask_(own_.size(), 0xFF) {
        // (a character's first word's low byte: only its view bits, 0x08 on screen and 0x10 near, are a cabinet's own; the rest every board's)
        for (std::size_t k = 0; k < own_.size(); ++k) {
            for (uint32_t ch = 0; ch < 12; ++ch)
                if (own_[k] == character_object(ch) + 3) own_mask_[k] = 0x18;
        }
    }

    ~Cabinets() { stop_workers(); }
    Cabinets(const Cabinets&) = delete;
    Cabinets& operator=(const Cabinets&) = delete;

    // Another cabinet: its seat, its logic count less this board's at the
    // same time (+1 the master seen from a slave, -1 a slave from the
    // master, 0 one slave from another), and how its side runs start (asked
    // at this board's flows until it can; false: not yet).
    void add(std::size_t seat, int ahead, Seeder seeder) {
        settle();
        Remote r;
        r.seat = seat, r.ahead = ahead, r.seeder = std::move(seeder);
        remotes_.push_back(std::move(r));
    }
    // From now its blocks are made here, into its network board (its main
    // board gone): `ram` its main board's work RAM as it stopped (its link
    // memory, link variables and own variables taken from it), after its
    // frame `last` (its link run there already).
    void start_making(std::size_t seat, m3::NetBoardDevice& netboard, std::span<const uint8_t> ram, uint64_t last) {
        settle();
        Remote* r = find(seat);
        if (!r) return;
        auto word = [&](uint32_t a) { return uint32_t(ram[a]) << 24 | uint32_t(ram[a + 1]) << 16 | uint32_t(ram[a + 2]) << 8 | ram[a + 3]; };
        r->netboard = &netboard, r->making = true, r->working_at.clear(), r->game_at.clear();
        for (uint32_t i = 0; i < kBlockWords; ++i) r->block[i] = word(kBlock + 4 * i);
        r->link.assign(ram.begin() + kLinkMemory, ram.begin() + kLinkMemory + kLinkBytes);
        r->link_vars.clear();
        for (const auto& [lo, hi] : kLinkVars)
            for (uint32_t a = lo; a < hi; ++a) r->link_vars.push_back(ram[a]);
        r->state_own.resize(own_.size());
        for (std::size_t k = 0; k < own_.size(); ++k) r->state_own[k] = ram[own_[k]];
        r->logic = word(kLogicCounter), r->logic_frame = last, r->frame_at[r->logic] = last, r->flow_applied = r->logic;
        r->written_frame = last;
        for (uint32_t k = 0; k < 12; ++k)
            for (uint32_t j = 0; j < 4; ++j) {
                const auto [off, size] = kEntryFields[j];
                const uint32_t a = character_object(k) + off;
                auto& kept = r->kept[k][j];
                kept.theirs = size == 2 ? uint32_t{ram[a]} << 8 | ram[a + 1] : word(a);
                kept.mine = field(a, size);
                kept.on = (kept.theirs & 0xFF) != (kept.mine & 0xFF);
            }
        for (uint32_t k = 0; k < 12; ++k) { // (each character's entry +8: kSumFields' sum, its and this board's)
            auto& kept = r->kept_sum[k];
            kept.mine = kept.theirs = 0;
            for (uint32_t j = 0; j < 7; ++j) {
                const auto [off, size] = kSumFields[j];
                const uint32_t a = character_object(k) + off;
                kept.their_fields[j] = size == 2 ? uint32_t{ram[a]} << 8 | ram[a + 1] : word(a);
                kept.mine_fields[j] = field(a, size);
                kept.theirs += kept.their_fields[j], kept.mine += kept.mine_fields[j];
            }
            kept.on = kept.theirs != kept.mine;
        }
    }
    // Its main board's frame `frame` lost (its work ran past the next
    // wake-up): no block written that frame, so its network board sends
    // the older bank again.
    void miss(std::size_t seat, uint64_t frame) {
        settle();
        if (Remote* r = find(seat)) r->missed.push_back(frame);
    }
    // What a shadow of it (a whole board run as it, from this board's state
    // where it has finished count `count`, at 0x36E94) needs of it: its own
    // variables and camera after that count, its link memory and variables
    // after its link of it, its working record (its next record, made).
    struct Shadow {
        std::vector<uint8_t> own, camera, link, link_vars, regions;
        std::array<uint32_t, 0x80> working{};
        struct Field {
            uint32_t addr, size, value;
        };
        std::vector<Field> fields; // (its characters' fields it keeps for itself (kEntryFields), where this board's differ)
    };
    bool shadow_state(std::size_t seat, uint32_t count, Shadow& out);
    // Whether shadow_state of `count` has what it needs now (nothing printed); and, if not, its link run of this frame
    // wanted at the first meeting from `from` on (its shadow starting soon after, before write_at: the staying board
    // finished the count late in its frame, so its link of it comes in the frame its shadow starts in).
    bool shadow_ready(std::size_t seat, uint32_t count) const {
        for (const Remote& r : remotes_)
            if (r.seat == seat) {
                const auto own = r.own_snap.find(count + 1);
                return own != r.own_snap.end() && !own->second.camera.empty() && r.link_snap.count(count) != 0;
            }
        return false;
    }
    void link_early(std::size_t seat, uint64_t from) {
        if (Remote* r = find(seat)) r->link_from = from;
    }
    // The master's shadow, at this board's dispatcher of count L (the master then just past its link of L+1, a
    // count ahead): its count L run here as it, flow and tasks, up to its drawing's vblank wait; `ram` and `cpu`
    // its RAM and CPU there, for a board to go on from as it (its drawing of L, its link of L+1 at once, its count
    // L+1, all before its next wake-up).
    bool shadow_master(std::size_t seat, std::vector<uint8_t>& ram, ppc::CpuState& cpu) {
        settle();
        Remote* r = find(seat);
        const uint32_t logic = bus_.read32(kLogicCounter);
        if (!r || r->ahead <= 0 || !r->making || r->camera.empty() || r->own_logic + 1 != logic) return false;
        capture_ = true;
        side_run(main_, *r, kModeDispatch, nullptr, true, logic, 0, true);
        capture_ = false;
        r->own_logic = logic;
        if (captured_.empty()) return false;
        ram = std::move(captured_), cpu = captured_cpu_;
        if (idle_msr_) cpu.msr = idle_msr_; // (its MSR there as a board's is, not the side run's)
        captured_.clear();
        stop_making(seat);
        return true;
    }
    // From now its blocks come from its shadow (its link's own): none made here, no side runs.
    void stop_making(std::size_t seat) {
        settle();
        if (Remote* r = find(seat))
            r->making = false, r->shadowed = true, r->pending = ~0ull, r->camera.clear(), r->hand_back_asked = false, r->stop_after = 0;
    }
    // Its main board still running, about to go (its network board `netboard`): taken as its shadow, so that
    // ask_hand_back hands it over as a shadow's (where it stops, a slave's next count caught up here).
    void running(std::size_t seat, m3::NetBoardDevice& netboard) {
        settle();
        if (Remote* r = find(seat)) r->netboard = &netboard, r->making = false, r->shadowed = true, r->camera.clear();
    }
    bool shadowed(std::size_t seat) {
        const Remote* r = find(seat);
        return r && r->shadowed;
    }
    // Back from its shadow to side runs: asked now; at this board's next dispatcher (count L) the count its shadow
    // is to stop after is set (L-1 for a slave, a frame behind; L+1 for the master, ahead), and for a slave this
    // board's world kept, to run its count L on once handed back. 0: not set yet.
    void ask_hand_back(std::size_t seat) {
        settle();
        if (Remote* r = find(seat); r && r->shadowed && !r->hand_back_asked) r->hand_back_asked = true, r->stop_after = 0;
    }
    uint32_t stop_after(std::size_t seat) {
        const Remote* r = find(seat);
        return r && r->shadowed ? r->stop_after : 0;
    }
    // Its shadow to be handed back, where it stops (known): any time now, from its own board's thread.
    bool hand_back_pending(std::size_t seat) {
        const Remote* r = find(seat);
        return r && r->shadowed && r->hand_back_asked && r->stop_after != 0;
    }
    // Its shadow stopped where it finished count `seed.own_logic` (0x36E94): `seed` its own variables, camera
    // and regions there, `ram` its RAM (its link memory and variables), `last` the frame of its last link.
    void hand_back(std::size_t seat, const Seed& seed, std::span<const uint8_t> ram, uint64_t last);
    // Its own variables' addresses and the bits of each that are its own.
    const std::vector<uint32_t>& own_addresses_list() const { return own_; }
    const std::vector<uint8_t>& own_masks() const { return own_mask_; }
    bool making(std::size_t seat) const {
        for (const Remote& r : remotes_)
            if (r.seat == seat) return r.making;
        return false;
    }

    // The board's hook, for every hooked block: true when it was one of
    // these (or a side run's, which runs no other hooks either).
    bool hook(uint32_t pc);
    // The camera's write watch (bus.watch_writes(kCamera, kCamera +
    // kCameraBytes, &Cabinets::camera_written, this)): other code's writes
    // into this board's camera (the game's events) made to the others' too.
    static void camera_written(void* user, uint32_t addr, uint32_t value, uint32_t size);
    // At each of this board's meetings (cycle `t` of its frame `frame`) and
    // at its frame's end: the blocks due written, the network boards brought
    // up to then.
    void meet(uint64_t frame, uint64_t t);
    void end_frame(uint64_t frame);

    // Called as each other cabinet's camera update on the side starts (its
    // seat, the count, its camera then): for checks against a recording of it.
    std::function<void(std::size_t seat, uint32_t logic, const std::vector<uint8_t>& camera)> on_camera;

    // Tasks mode: each other cabinet's flow run together with its frame's tasks up to the drawing (its
    // characters' updates and animation, its camera, its shakes and events: all its own code), its own
    // variables, camera and regions kept from them, instead of the camera, shake and animation runs alone.
    void use_tasks(bool on) { tasks_mode_ = on; }
    // Each record made, before it goes out (network play: tools::NodeCheck::fix_record, the bytes a cabinet keeps
    // for itself from its real records): the seat, the block, the record's byte offset in it.
    using RecordFix = std::function<void(std::size_t seat, Block& next, uint32_t rec)>;
    void set_record_fix(RecordFix fix) { record_fix_ = std::move(fix); }
    // And its block's header after its link run (tools::NodeCheck::fix_header): `put(offset in the node, byte)`.
    using HeaderFix = std::function<void(std::size_t seat, const std::function<void(std::size_t, uint8_t)>& put)>;
    void set_header_fix(HeaderFix fix) { header_fix_ = std::move(fix); }

    // In a side run (the board's other hooks are skipped then too).
    bool inside() const { return main_.inside; }
    uint64_t made() const { return made_; }
    // Another cabinet's frames on side runs whose tasks (its 2D's with them) took over kHeavyWork cycles (a
    // banner's: TIME OVER's tiles unpacked in one go; usually about 0.3M): a frame it may have lost (its drawing,
    // not run here, decides), which side runs can't follow. A shadow must cover those; the rules start one
    // beforehand. The first such frame, or 0.
    uint64_t heavy_frames() const { return heavy_frames_; }
    uint64_t first_heavy() const { return first_heavy_; }
    static constexpr uint64_t kHeavyWork = 1'000'000;
    double side_seconds() const { return side_seconds_; }
    // (and with what each costs around it: the other cabinet's state put in and taken out, RAM put back; and the
    // time its network boards took, brought up to this board's meetings)
    double whole_seconds() const { return whole_seconds_; }
    double netboard_seconds() const { return netboard_seconds_; }
    // (side runs on the workers: how many, their seconds in them with their set-ups, all of them; this board's
    // waiting for them and handing jobs to them)
    std::size_t workers() const { return workers_.size(); }
    double worker_seconds() const {
        settle();
        double s = 0;
        for (const auto& w : workers_) s += w->whole;
        return s;
    }
    double worker_wait() const { return worker_wait_; }
    double worker_hand() const { return worker_hand_; }
    uint64_t journal_bytes() const { return journal_bytes_; } // (RAM bytes its side runs wrote, put back after)
    uint64_t side_runs() const { return side_runs_; }
    // (where the side runs' time went, by kind: runs, ms set up, ms in the game code, ms put back)
    std::string profile() const {
        settle();
        std::string out;
        char line[160];
        for (const auto& [pc, p] : profile_) {
            std::snprintf(line, sizeof line, "  %06x%s: %llu runs, %.0f ms set up, %.0f ms game code, %.0f ms put back\n", pc & ~1u,
                          (pc & 1) ? " (its tasks)" : "", (unsigned long long)p.runs, 1000 * p.setup, 1000 * p.game, 1000 * p.after);
            out += line;
        }
        return out;
    }
    std::string failed() const {
        settle();
        return failed_;
    }
    // (measuring: the bytes its own state holds, what a copy of it every frame would cost)
    std::size_t state_bytes() const {
        settle();
        std::size_t n = 0;
        for (const Remote& r : remotes_) {
            n += r.camera.size() + r.own.size() + r.link.size() + r.link_vars.size() + r.state_own.size() + r.saved_world.size() +
                 r.view_world.size() + r.regions.size() + r.heading_at.size() * 8 + r.view_at.size() * 12 + r.frame_at.size() * 12;
            for (const auto& [k, d] : r.flow_delta) n += d.size() * 8;
            for (const auto& [k, s] : r.own_snap) n += s.own.size() + s.camera.size() + s.regions.size();
            for (const auto& [k, s] : r.link_snap) n += s.first.size() + s.second.size();
        }
        for (const auto& [k, v] : view_chars_) n += v.size() * sizeof(ppc::CpuState);
        return n + players_at_.size() * sizeof(Players) + ahead_record_.size() * 4 + captured_.size();
    }

private:
    struct Remote {
        std::size_t seat = 0;
        int ahead = 0;
        m3::NetBoardDevice* netboard = nullptr;
        Seeder seeder;
        std::vector<uint8_t> camera, own; // (empty: not started)
        uint32_t own_logic = 0;           // (its own variables are after its flow of this count)
        uint64_t toggled_at = ~0ull;      // (the frame its toggles were flipped for)
        std::map<uint32_t, uint16_t> heading_at;                   // (after its update of a count)
        std::set<uint32_t> unwritten; // (counts whose record its player's update left as before: +0x08 to +0x0F kept)
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> view_at; // ((count, character): its first word after the test)
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint8_t>>> flow_delta; // (its own bytes its flow of a count changed: index, value)
        bool making = false;
        // (once made here, in its own order (its link, then its flow, a count at a time): its link memory, link
        // variables and own variables as its last link run left them; its flows' changes since applied before the next)
        std::vector<uint8_t> link, link_vars, state_own;
        uint32_t flow_applied = 0;
        Block block{};                     // (its last block)
        uint64_t written_frame = ~0ull;
        uint64_t pending = ~0ull;     // (a frame whose link run waits for its camera's view of its player)
        uint64_t link_from = 0;       // (its link run of this frame wanted from this time, not write_at's: its shadow's start)
        std::vector<uint64_t> missed; // (its frames lost, no block written)
        // (once made here: its logic count, the frame it reached it, and each count's frame, for its buttons)
        uint32_t logic = 0;
        uint64_t logic_frame = 0;
        std::map<uint32_t, uint64_t> frame_at;
        // (its working record after its flow and tasks of a count, run here: its next count's record, as its link
        // takes it; this board's own can come later in the frame than its link run)
        std::map<uint32_t, std::array<uint32_t, 0x80>> working_at;
        uint32_t viewed = 0; // (the count its own player's view was last tested for in its tasks' run)
        // (the world's bytes in its link memory (kLinkGameBytes) after its run of a count: its next link sends them)
        std::map<uint32_t, std::array<uint8_t, 32>> game_at;
        bool shadowed = false; // (a shadow board runs it: nothing here)
        // (each character's fields its record's entry takes a byte of (kEntryFields), which each cabinet keeps for
        // itself, as its main board had them when made here began, and this board's then: its own while this board's
        // stays the same (the character not set up again since))
        struct Kept {
            uint32_t mine = 0, theirs = 0;
            bool on = false;
        };
        std::array<std::array<Kept, 4>, 12> kept{};
        // (and each character's entry +8, the sum of its kSumFields: its own as its main board had them when made here
        // began, kept while this board's record has this board's sum of then; the fields themselves for a shadow)
        struct KeptSum {
            uint32_t mine = 0, theirs = 0;
            std::array<uint32_t, 7> mine_fields{}, their_fields{};
            bool on = false;
        };
        std::array<KeptSum, 12> kept_sum{};
        // (handing back from its shadow: asked; the count its shadow stops after; for a slave this board's world as
        // its dispatcher of the count after that started, to run that count on as it once handed back; the first
        // link run then takes its working record as its shadow left it)
        bool hand_back_asked = false, catch_up = false, first_link_working = false;
        uint32_t stop_after = 0, saved_count = 0;
        std::vector<uint8_t> saved_world, view_world; // (this board's world before its tasks of that count, and after it)
        std::vector<uint8_t> regions; // (its characters' pose buffers, kPoses-, from its animation steps run here)
        // (for a shadow: its own variables and camera as its flow of a count starts (after the count before), its
        // link memory and variables after its link of a count)
        struct Snap {
            std::vector<uint8_t> own, camera, regions;
        };
        std::map<uint32_t, Snap> own_snap;
        std::map<uint32_t, std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> link_snap;
    };
    struct Players { // (as this board writes its own record of a count: each player's first word, +0x44, +0x2A, +0x74, +0x76)
        std::array<std::array<uint32_t, 5>, 4> p{};
        uint32_t in_play = ~0u; // (and the players in play then: kPlayersObject's +0x50)
    };
    // The players in play as this board has them now (~0: not known).
    uint32_t players_in_play() {
        const uint32_t at = bus_.read32(kPlayersObject) + 0x50;
        return at >= 0x100000 && at + 4 <= m3::kWorkRamSize ? bus_.read32(at) : ~0u;
    }
    // Whether a made cabinet's player's update wrote its record of count `x` (+0x08 to +0x0D: 0x55050's test): from this
    // board's own record of that count where it has one, else as made.
    bool wrote_at(const Remote& r, uint32_t x) const {
        const auto at = players_at_.find(x);
        if (at == players_at_.end()) return !r.unwritten.count(x);
        uint8_t mode = 0;
        for (std::size_t i = 0; i < own_.size() && i < r.own.size(); ++i)
            if (own_[i] == kOwnMode) mode = r.own[i];
        return !(at->second.p[r.seat][0] & 0x02u) && mode != 3 && (at->second.in_play & (0x80000000u >> r.seat));
    }

public:
    // Rollback (tools/rollback.hpp): everything it carries from one frame to the next, the other cabinets' made
    // here, to be put back with the boards (taken at a frame's end, outside any side run).
    struct Kept {
        std::vector<Remote> remotes;
        std::map<uint32_t, Players> players_at;
        std::vector<uint32_t> ahead_record;
        std::map<uint32_t, std::vector<ppc::CpuState>> view_chars;
        std::vector<uint8_t> captured;
        ppc::CpuState captured_cpu{};
        uint32_t idle_msr = 0;
    };
    Kept keep() const {
        settle();
        return {remotes_, players_at_, ahead_record_, view_chars_, captured_, captured_cpu_, idle_msr_}; }
    void put_back(const Kept& k) {
        settle();
        remotes_ = k.remotes, players_at_ = k.players_at, ahead_record_ = k.ahead_record, view_chars_ = k.view_chars;
        captured_ = k.captured, captured_cpu_ = k.captured_cpu, idle_msr_ = k.idle_msr;
    }

private:

    Remote* find(std::size_t seat) {
        for (Remote& r : remotes_)
            if (r.seat == seat) return &r;
        return nullptr;
    }
    bool started(Remote& r, uint32_t logic);
    // Game code at `pc` run as cabinet `r` (see above); `flags`: the
    // character's (r14) first word after it. Its camera and own variables
    // after it kept (`keep`).
    // `count`: a flow's, the count it runs as; `tasks`: its frame's tasks after it too, up to the drawing.
    // `character`: a view test's (r14), else the hook's.
    struct Exec;
    void side_run(Exec& e, Remote& r, uint32_t pc, uint32_t* flags, bool keep, uint32_t count = 0, uint32_t character = 0, bool tasks = false);
    void shake(Remote& r, uint32_t kind);
    // A byte as cabinet `r` has it: its own variables' where it is one of them, else this board's.
    uint8_t own_byte(const Remote& r, uint32_t addr) const {
        if (!r.own.empty())
            if (const auto at = std::find(own_.begin(), own_.end(), addr); at != own_.end()) {
                const std::size_t k = static_cast<std::size_t>(at - own_.begin());
                return static_cast<uint8_t>((bus_.read8(addr) & ~own_mask_[k]) | (r.own[k] & own_mask_[k]));
            }
        return bus_.read8(addr);
    }
    // Its link (0x6D758) run here as it, in its frame `frame`: its link memory and
    // variables in, its network board borrowed, its next record (made from this
    // board's) in its working record; the game's own code decides the rest (whether
    // its count goes on, who is in the game, its header) and writes its block.
    void link_run(Remote& r, uint64_t frame);
    // Its next link can run: its camera's view of its own player for the count before has (the record carries it,
    // and the other reads the newest record once).
    bool ready(const Remote& r) const;
    void merge_own(const std::vector<uint8_t>& own);
    // (a cabinet's own areas written in, through the bus (journalled), those of the link's players left as they are)
    void put_regions(const std::vector<uint8_t>& regions) { put_regions(bus_, regions); }
    void put_regions(m3::Bus& bus, const std::vector<uint8_t>& regions) {
        const auto ram = bus.ram();
        const uint32_t mask = bus.read32(kLinkPlayers);
        std::size_t n = 0, i = 0;
        for (const auto& [lo, hi] : kOwnRegions) {
            if (region_own(i++, mask))
                for (uint32_t at = lo; at < hi; at += 64) { // (64 bytes at a time compared first: most are the same)
                    const uint32_t end = std::min<uint32_t>(at + 64, hi);
                    if (std::memcmp(&ram[at], &regions[n + at - lo], end - at) == 0) continue;
                    for (uint32_t a = at; a < end; ++a)
                        if (ram[a] != regions[n + a - lo]) bus.write8(a, regions[n + a - lo]);
                }
            n += hi - lo;
        }
    }
    // (its own areas back from RAM: gather_regions, into the buffer it has)
    static void regather(std::vector<uint8_t>& regions, std::span<const uint8_t> ram) {
        std::size_t n = 0;
        for (const auto& [lo, hi] : kOwnRegions) {
            std::memcpy(regions.data() + n, ram.data() + lo, hi - lo);
            n += hi - lo;
        }
    }
    bool missed(const Remote& r, uint64_t frame) const { return std::find(r.missed.begin(), r.missed.end(), frame) != r.missed.end(); }
    // Its frame for count `logic` (its buttons then): counted on from its last
    // count while made here, missed frames skipped; before, this board's less its offset.
    uint64_t frame_for(const Remote& r, uint32_t logic) const;
    void make_record(Remote& r, Block& next, const Block& mine, uint32_t logic, uint64_t frame, uint32_t old);
    uint32_t field(uint32_t a, uint32_t size) { return size == 2 ? uint32_t{bus_.read16(a)} : bus_.read32(a); }
    uint8_t get8(const Block& b, uint32_t byte) const { return static_cast<uint8_t>(b[byte / 4] >> (8 * (3 - byte % 4))); }
    void put8(Block& b, uint32_t byte, uint8_t v) const {
        uint32_t& w = b[byte / 4];
        const uint32_t shift = 8 * (3 - byte % 4);
        w = (w & ~(0xFFu << shift)) | (uint32_t{v} << shift);
    }
    static int newest(const Block& b) {
        int best = 0;
        for (int s = 1; s < 4; ++s)
            if (b[(0x100 + s * 0x200 + 0xDC) / 4] > b[(0x100 + best * 0x200 + 0xDC) / 4]) best = s;
        return best;
    }

    recomp::Context& c_;
    m3::Bus& bus_;
    recomp::FrameTiming& timing_;
    [[maybe_unused]] std::size_t local_; // (not read since a570048; clang's -Wunused-private-field)
    Held held_;
    uint64_t write_at_;
    std::vector<uint32_t> own_;
    std::vector<uint8_t> own_mask_; // (the bits of each that are a cabinet's own)
    std::vector<Remote> remotes_;
    std::map<uint32_t, Players> players_at_; // (by count)
    bool in_view_ = false, in_update_ = false, in_link_ = false;
    mutable std::vector<uint32_t> ahead_record_; // (the working record after a flow run on the side)
    std::map<uint32_t, std::vector<ppc::CpuState>> view_chars_; // (this board's view tests, by count: each as called, r14 the character)
    const std::vector<uint8_t>* world_ = nullptr;          // (a side run's world instead of this board's: a catch-up's)
    bool capture_ = false;                                 // (the master's side run: its RAM kept, for its shadow)
    uint32_t drawing_r1_ = 0;                              // (its stack pointer as its tasks were called)
    std::vector<uint8_t> captured_;
    ppc::CpuState captured_cpu_{};
    uint32_t idle_msr_ = 0; // (this board's MSR at its last 0x36F24)
    bool tasks_mode_ = false;
    RecordFix record_fix_;
    HeaderFix header_fix_;
    uint64_t made_ = 0;
    mutable uint64_t side_runs_ = 0; // (the workers' added in as they settle)
    uint64_t journal_bytes_ = 0;
    mutable uint64_t heavy_frames_ = 0, first_heavy_ = 0;
    mutable double side_seconds_ = 0;
    double whole_seconds_ = 0, netboard_seconds_ = 0;
    int timer_depth_ = 0;
    struct Prof {
        uint64_t runs = 0;
        double setup = 0, game = 0, after = 0; // (seconds: set up, its game code, put back)
    };
    mutable std::map<uint32_t, Prof> profile_; // (by the side run's PC, +1 with its tasks; kLink the link's)
    void profiled(std::map<uint32_t, Prof>& profile, uint32_t key, std::chrono::steady_clock::time_point begun,
                  std::chrono::steady_clock::time_point ran_from,
                  std::chrono::steady_clock::time_point ran_to) {
        Prof& p = profile[key];
        ++p.runs;
        p.setup += std::chrono::duration<double>(ran_from - begun).count();
        p.game += std::chrono::duration<double>(ran_to - ran_from).count();
        p.after += std::chrono::duration<double>(std::chrono::steady_clock::now() - ran_to).count();
    }
    struct Timer { // (adds the time it lived to `to`; one inside another counted once)
        double& to;
        int& depth;
        std::chrono::steady_clock::time_point from = std::chrono::steady_clock::now();
        Timer(double& t, int& d) : to(t), depth(d) { ++depth; }
        ~Timer() {
            if (--depth == 0) to += std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
        }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;
    };
    mutable std::string failed_;

    // ---- Side runs on other cores ----
    // Who runs a side run: this board's CPU and bus, or a worker's (another thread's: a context and bus of its own
    // on the same ROMs, its work RAM a copy of this board's kept up from the pages it wrote, its devices' registers
    // and network board's memory copied as the job starts). Each its own flags, and its own timer and counts.
    // Several workers (one a cabinet, as the cores allow): the cabinets' jobs handed out in turn, each from the
    // same world (this board's at its dispatcher), so the same as one after another.
    struct Worker;
    // (a worker's own counts of its side runs, added to this board's as it settles, in the workers' order)
    struct Stats {
        double side_seconds = 0;
        uint64_t side_runs = 0, heavy = 0, first_heavy = 0;
        std::map<uint32_t, Prof> profile;
        std::string failed;
        std::vector<uint32_t> ahead;
        std::vector<std::pair<Remote*, uint32_t>> lost; // (counts whose world step ran long: their cabinet's next frame lost)
    };
    // A count's world step (its tasks) that ran over 4/5 of a frame: its cabinet's main board would have run past its
    // next wake-up (its link, flow and drawing besides), so its frame after the one it reached the count in is lost
    // (no link: the count held a frame, as a real board's). Every cabinet does the same step, so this board loses one
    // too (2026-11-07: an area's set-up at count 0xC5C, 2.87 M cycles against ~160 k: the master held at 4132, this
    // board at 4133; 0x265D's 1.71 M, no frame lost: half a frame (1.44 M) was too few; tools/onepc-diff.ps1). A
    // guess from the step alone (the drawing's share unknown here): a wrong one shows in the real-block check.
    void lose_after(Remote& r, uint32_t count) const {
        const uint64_t frame = frame_for(r, count) + 1; // (a slave's step comes before its link reaches the count)
        if (!missed(r, frame)) r.missed.push_back(frame);
        if (std::getenv("ONEBOARD_LOST_LOG"))
            std::printf("seat %zu: count %u's world step long: its frame %llu lost\n", r.seat, count, static_cast<unsigned long long>(frame));
    }
    struct Exec {
        recomp::Context& c;
        m3::Bus& bus;
        double& whole;
        int& depth;
        bool inside = false, in_tasks = false, in_drawing = false;
        bool given = false; // (a worker's: its buttons and frames given it, worked out on this board's thread)
        Worker* paged = nullptr; // (a worker's: its RAM put back as the pages it marked, no journal)
        Stats* stats = nullptr;  // (a worker's: its counts, not this board's)
        uint32_t now = 0, was = 0;
        uint64_t frame = 0, at_frame = 0;
    };
    // A cabinet's flows and tasks of a count, and its view of its own player after (tasks mode, at this board's
    // dispatcher): its buttons, frame and this board's frame then, worked out here.
    struct Job {
        Remote* r;
        uint32_t logic, runs;
        uint64_t frame;
        uint32_t now, was;
        uint64_t at_frame;
    };
    // (its view of its own player after its tasks of `logic`, its camera's heading)
    void own_view(Exec& e, Remote& r, uint32_t logic) {
        // (its player's object cleared (its first word 0, here as on every cabinet): its own view bits of it gone too, as
        // the real cabinet's whole word is; kept, they came back with the player (00000110 for 00000100, 2026-11-11, the
        // owner's recorded game))
        if (bus_.read32(player_object(r.seat)) == 0)
            for (std::size_t i = 0; i < own_.size() && i < r.own.size(); ++i)
                if (own_[i] == player_object(r.seat) + 3) r.own[i] = static_cast<uint8_t>(r.own[i] & ~own_mask_[i]);
        if (r.viewed != logic && tasks_mode_) {
            // (its tasks' run, its game's own code, ran no view test of its player: its update didn't run (0x5185C: out of
            // play, a time over). Its view bits as they were, not tested here: the real cabinet's stay (on at its player's
            // leaving, 03200115), and come again only with its update (2026-11-11, the owner's recorded game))
            const uint32_t word = bus_.read32(player_object(r.seat));
            uint8_t own = static_cast<uint8_t>(word);
            for (std::size_t i = 0; i < own_.size() && i < r.own.size(); ++i)
                if (own_[i] == player_object(r.seat) + 3) own = static_cast<uint8_t>((own & ~own_mask_[i]) | (r.own[i] & own_mask_[i]));
            r.view_at[{logic, player_object(r.seat)}] = word == 0 ? 0 : (word & ~0xFFu) | own;
        } else if (r.viewed != logic) { // (else tested in its tasks' run, on their world)
            uint32_t flags = 0;
            side_run(e, r, kViewTest, &flags, false, 0, player_object(r.seat));
            r.view_at[{logic, player_object(r.seat)}] = flags;
        }
        r.heading_at[logic] = static_cast<uint16_t>(r.camera[kCameraHeading] << 8 | r.camera[kCameraHeading + 1]);
        // (tests: ONEBOARD_OWN_LOG=FROM-TO: its camera's heading and follows, its player's view bits as kept, by count)
        static const std::pair<uint32_t, uint32_t> own_log = [] {
            unsigned a = 0, b = 0;
            if (const char* v = std::getenv("ONEBOARD_OWN_LOG")) std::sscanf(v, "%u-%u", &a, &b);
            return std::pair<uint32_t, uint32_t>{a, b};
        }();
        if (logic >= own_log.first && logic <= own_log.second) {
            int bits = -1;
            for (std::size_t i = 0; i < own_.size() && i < r.own.size(); ++i)
                if (own_[i] == player_object(r.seat) + 3) bits = r.own[i];
            const auto v = r.view_at.find({logic, player_object(r.seat)});
            std::printf("own: seat %zu count %u: heading %02x%02x, follows %02x%02x%02x%02x, its player's view bits %02x, its word "
                        "here %08x, view %08x (%s)\n",
                        r.seat + 1, logic, r.camera[kCameraHeading], r.camera[kCameraHeading + 1], r.camera[kCameraFollows],
                        r.camera[kCameraFollows + 1], r.camera[kCameraFollows + 2], r.camera[kCameraFollows + 3], bits,
                        bus_.read32(player_object(r.seat)), v != r.view_at.end() ? v->second : 0xFFFFFFFFu,
                        r.viewed == logic ? "tested in its tasks" : "kept");
        }
        while (!r.heading_at.empty() && r.heading_at.begin()->first + 16 < logic) r.heading_at.erase(r.heading_at.begin());
        while (!r.view_at.empty() && r.view_at.begin()->first.first + 16 < logic) r.view_at.erase(r.view_at.begin());
    }
    // The network board as a worker reads it: this board's, its comm RAM's first part and I/O registers copied
    // (what side runs read: 0xC0000004, 0xC0000D00-0xC00021FC, 0xC0010114); anything else counted, read as 0.
    class FrozenNetBoard : public m3::NetBoardDevice {
    public:
        static constexpr uint32_t kComm = 0x4000, kIo = 0x200;
        void take(m3::NetBoardDevice& b) {
            if (!b.host_comm_view(comm_.data(), kComm))
                for (uint32_t i = 0; i < kComm; ++i) comm_[i] = b.read8(0xC0000000u + i);
            for (uint32_t i = 0; i < kIo; ++i) io_[i] = b.read8(0xC0010000u + i);
        }
        uint8_t read8(uint32_t addr) override {
            const uint32_t a = addr & 0x3FFFF;
            if (a < kComm) return comm_[a];
            if (a >= 0x10000 && a < 0x10000 + kIo) return io_[a - 0x10000];
            ++misses;
            return 0;
        }
        uint16_t read16(uint32_t addr) override { return static_cast<uint16_t>(read8(addr) << 8 | read8(addr + 1)); }
        uint32_t read32(uint32_t addr) override { return uint32_t{read16(addr)} << 16 | read16(addr + 2); }
        void write8(uint32_t, uint8_t) override {} // (held: never come)
        void write16(uint32_t, uint16_t) override {}
        void write32(uint32_t, uint32_t) override {}
        std::string describe() const override { return "a side-run worker's copy"; }
        uint64_t misses = 0;

    private:
        std::array<uint8_t, kComm> comm_{};
        std::array<uint8_t, kIo> io_{};
    };
    struct Worker {
        Worker(const m3::Bus& from, std::span<const recomp::FunctionEntry> functions) : bus(from.roms()), c(bus, functions) {
            auto board = std::make_unique<FrozenNetBoard>();
            net = board.get();
            bus.use_netboard(std::move(board));
            exec = std::make_unique<Exec>(Exec{c, bus, whole, depth});
            exec->given = true, exec->paged = this, exec->stats = &stats;
            bus.mark_pages(marks.data());
            c.set_hook(&Cabinets::worker_hook, exec.get());
        }
        m3::Bus bus;
        recomp::Context c;
        FrozenNetBoard* net = nullptr;
        std::unique_ptr<Exec> exec;
        double whole = 0;
        int depth = 0;
        Stats stats;
        std::vector<Job> jobs;
        bool has_jobs = false, quit = false;
        bool synced = false; // (its RAM a copy of the clean one: from then on the pages written brought over)
        std::mutex m;
        std::condition_variable wake, done;
        std::thread thread;
        // (no journal: a run's RAM put back as the pages its bus marked, from the clean copy; its writes on the fast
        // path, on every system alike. Its RAM has no system record kept: on Windows each page's first write after
        // one is reset costs a fault)
        std::vector<uint8_t> marks = std::vector<uint8_t>(m3::kWorkRamSize >> m3::Bus::kMarkShift, 0);
        const std::vector<uint8_t>* clean = nullptr;
        // A side run's RAM put back: the pages it wrote, from the clean copy.
        void put_back_pages() {
            const auto ram = bus.ram();
            constexpr std::size_t page = std::size_t{1} << m3::Bus::kMarkShift;
            for (std::size_t p = 0; p < marks.size(); ++p)
                if (marks[p]) {
                    std::memcpy(ram.data() + p * page, clean->data() + p * page, page);
                    marks[p] = 0;
                }
        }
    };
    static void worker_hook(recomp::Context&, uint32_t pc, void* user) {
        Exec& e = *static_cast<Exec*>(user);
        if (e.in_tasks && pc == kVblankWait) throw TasksDrawing{};
        if (e.inside && pc == kTransferLoop) e.bus.write8(kTransferBusy, 0);
    }
    // How many workers: SPIKEOUT_SIDE_WORKERS=N (SPIKEOUT_SIDE_THREAD=0: none, side runs on this board's thread); else
    // the system's threads less two (this board's and one for the rest: the window's sound and 2D), at least one,
    // no more than the other cabinets; none with a single thread.
    static std::size_t worker_count(std::size_t cabinets) {
        if (const char* v = std::getenv("SPIKEOUT_SIDE_THREAD"); v && v[0] == '0') return 0;
        if (const char* v = std::getenv("SPIKEOUT_SIDE_WORKERS")) return std::min<std::size_t>(std::strtoul(v, nullptr, 10), cabinets);
        const std::size_t threads = std::thread::hardware_concurrency();
        if (threads <= 1) return 0;
        return std::min(cabinets, std::max<std::size_t>(1, threads - 2));
    }
    bool workers_wanted() {
        if (worker_target_ < 0) worker_target_ = static_cast<int>(worker_count(remotes_.size()));
        return worker_target_ > 0;
    }
    // The jobs handed to the workers, in turn, with this board's RAM (its pages written since, into the clean copy
    // the workers bring theirs up from), devices and CPU as they are now.
    void start_workers(std::vector<Job> jobs) {
        settle(); // (one round of jobs at a time)
        const auto handing = std::chrono::steady_clock::now();
        if (workers_.empty())
            for (int k = 0; k < worker_target_; ++k) {
                workers_.push_back(std::make_unique<Worker>(bus_, c_.functions()));
                Worker* w = workers_.back().get();
                w->clean = &clean_;
                w->thread = std::thread([this, w] { worker_loop(*w); });
            }
        const auto ram = bus_.ram();
        if (mirrored_ && m3::watched::take_written(reader_, ram.data(), ram.size(), pages_)) {
            for (std::size_t p : pages_) {
                const std::size_t at = p * m3::watched::kPage, n = std::min(m3::watched::kPage, ram.size() - at);
                std::memcpy(clean_.data() + at, ram.data() + at, n);
            }
        } else { // (the first, or the record not there: all of it)
            m3::watched::forget_written(reader_, ram.data(), ram.size());
            clean_.assign(ram.begin(), ram.end());
            for (auto& w : workers_) w->synced = false;
            mirrored_ = true;
        }
        bus_.netboard().sync(bus_.now()); // (as a read of it by this board now would)
        last_job_worker_ = jobs.empty() ? -1 : static_cast<int>((jobs.size() - 1) % workers_.size());
        for (std::size_t k = 0; k < workers_.size(); ++k) {
            Worker& w = *workers_[k];
            w.bus.mirror_devices(bus_);
            w.net->take(bus_.netboard());
            w.c.follow(c_);
            w.jobs.clear();
            for (std::size_t j = k; j < jobs.size(); j += workers_.size()) w.jobs.push_back(jobs[j]);
        }
        workers_busy_ = true;
        if (std::getenv("SPIKEOUT_SIDE_INLINE")) { // (debugging: the workers' contexts and buses, on this thread)
            for (auto& w : workers_) run_jobs(*w);
            return;
        }
        for (auto& w : workers_) {
            {
                std::lock_guard l(w->m);
                w->has_jobs = true;
            }
            w->wake.notify_one();
        }
        worker_hand_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - handing).count();
    }
    void worker_loop(Worker& w) {
        std::unique_lock l(w.m);
        for (;;) {
            w.wake.wait(l, [&] { return w.has_jobs || w.quit; });
            if (w.quit) return;
            l.unlock();
            run_jobs(w);
            l.lock();
            w.has_jobs = false;
            w.done.notify_all();
        }
    }
    void run_jobs(Worker& w) {
        // (its RAM brought up from the clean copy: the pages this board wrote since, or all of it)
        const auto ram = w.bus.ram();
        if (!w.synced) {
            std::memcpy(ram.data(), clean_.data(), ram.size());
            w.synced = true;
        } else {
            for (std::size_t p : pages_) {
                const std::size_t at = p * m3::watched::kPage, n = std::min(m3::watched::kPage, ram.size() - at);
                std::memcpy(ram.data() + at, clean_.data() + at, n);
            }
        }
        if (worker_check_ && std::memcmp(ram.data(), clean_.data(), ram.size()) != 0)
            std::printf("side check: a worker's RAM differs from this board's\n");
        Exec& e = *w.exec;
        for (const Job& j : w.jobs) {
            e.frame = j.frame, e.now = j.now, e.was = j.was, e.at_frame = j.at_frame;
            try {
                for (uint32_t k = 0; k < j.runs; ++k)
                    side_run(e, *j.r, kModeDispatch, nullptr, true, j.logic - j.runs + k + 1, 0, k + 1 == j.runs);
                own_view(e, *j.r, j.logic);
            } catch (...) {
                if (w.stats.failed.empty()) w.stats.failed = "seat " + std::to_string(j.r->seat + 1) + ": a side-run worker stopped";
            }
        }
    }
    // This board waits here for the workers' jobs (their results are used from here on), their counts taken in.
    void settle() const {
        if (!workers_busy_) return;
        const auto from = std::chrono::steady_clock::now();
        for (const auto& w : workers_) {
            std::unique_lock l(w->m);
            w->done.wait(l, [&] { return !w->has_jobs; });
        }
        workers_busy_ = false;
        worker_wait_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
        for (const auto& w : workers_) { // (in the workers' order: the first failure and heavy frame as one after another)
            Stats& s = w->stats;
            side_seconds_ += s.side_seconds, side_runs_ += s.side_runs;
            for (const auto& [key, p] : s.profile) {
                Prof& to = profile_[key];
                to.runs += p.runs, to.setup += p.setup, to.game += p.game, to.after += p.after;
            }
            if (s.heavy) {
                if (!heavy_frames_ || s.first_heavy < first_heavy_) first_heavy_ = s.first_heavy;
                heavy_frames_ += s.heavy;
            }
            for (const auto& [r, count] : s.lost) lose_after(*r, count);
            s.lost.clear();
            if (failed_.empty() && !s.failed.empty()) failed_ = s.failed;
            if (w->net->misses && failed_.empty())
                failed_ = "a side run on a worker read the network board outside its copy (" + std::to_string(w->net->misses) + " reads)";
            s.side_seconds = 0, s.side_runs = 0, s.heavy = 0, s.profile.clear(), s.failed.clear();
        }
        if (last_job_worker_ >= 0) ahead_record_ = workers_[static_cast<std::size_t>(last_job_worker_)]->stats.ahead; // (the last job's)
        for (std::size_t i = 0; i < checks_.size(); ++i) {
            const Check& k = checks_[i];
            auto first = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) -> long {
                if (a.size() != b.size()) return -2;
                for (std::size_t n = 0; n < a.size(); ++n)
                    if (a[n] != b[n]) return static_cast<long>(n);
                return -1;
            };
            const long cam = first(k.r->camera, k.serial.camera), own = first(k.r->own, k.serial.own), reg = first(k.r->regions, k.serial.regions);
            const bool views = k.r->view_at != k.serial.view_at, heads = k.r->heading_at != k.serial.heading_at;
            const bool ahead = i + 1 == checks_.size() && ahead_record_ != k.ahead; // (the last job's is the one kept)
            if (cam != -1 || own != -1 || reg != -1 || views || heads || ahead)
                std::printf("side check: seat %zu count %u differs: camera %ld, own %ld (%08x), regions %ld, views %d, headings %d, record %d\n",
                            k.r->seat + 1, k.logic, cam, own, own >= 0 ? own_[static_cast<std::size_t>(own)] : 0u, reg, views, heads, ahead);
        }
        checks_.clear();
    }
    void stop_workers() {
        if (workers_.empty()) return;
        settle();
        for (auto& w : workers_) {
            {
                std::lock_guard l(w->m);
                w->quit = true;
            }
            w->wake.notify_one();
            w->thread.join();
        }
        workers_.clear();
    }
    int worker_target_ = -1; // (worker_count's, once asked)
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<uint8_t> clean_;      // (this board's RAM at the last hand-over, the workers' to bring theirs up from)
    std::vector<std::size_t> pages_;  // (the pages written before it)
    int reader_ = m3::watched::new_reader();
    bool mirrored_ = false;
    int last_job_worker_ = -1;
    mutable bool workers_busy_ = false;
    struct Check { // (SPIKEOUT_SIDE_CHECK: a job's cabinet as run here, to compare with the worker's)
        Remote* r;
        Remote serial;
        std::vector<uint32_t> ahead;
        uint32_t logic;
    };
    mutable std::vector<Check> checks_;
    bool worker_check_ = std::getenv("SPIKEOUT_SIDE_CHECK") != nullptr;
    mutable double worker_wait_ = 0, worker_hand_ = 0; // (seconds: this board waiting for them, handing jobs to them)
    Exec main_{c_, bus_, whole_seconds_, timer_depth_};
};

// ---- the implementation ----


inline bool Cabinets::started(Remote& r, uint32_t logic) {
    if (r.shadowed) return false;
    if (!r.camera.empty()) return true;
    Seed s;
    if (!r.seeder || !r.seeder(r.seat, logic, s) || s.camera.size() != kCameraBytes || s.own.size() != own_.size()) return false;
    r.camera = std::move(s.camera), r.own = std::move(s.own), r.own_logic = s.own_logic;
    if (s.regions.size() == kRegionBytes) r.regions = std::move(s.regions);
    else r.regions = gather_regions(bus_.ram());
    return true;
}

inline void Cabinets::side_run(Exec& e, Remote& r, uint32_t pc, uint32_t* flags, bool keep_camera, uint32_t count, uint32_t character, bool tasks) {
    if (&e == &main_) settle(); // (on this board: the worker's runs done first, its results in)
    const Timer whole{e.whole, e.depth};
    const auto begun = std::chrono::steady_clock::now(); // (profile_)
    const uint32_t logic = e.bus.read32(kLogicCounter);
    const recomp::Context::Aside aside = e.c.set_aside();
    if (!e.paged) e.bus.journal(true);
    if (world_) { // (a catch-up's: this board's world as it was then)
        const auto ram = e.bus.ram();
        for (uint32_t i = 0; i < world_->size(); ++i)
            if (ram[i] != (*world_)[i]) e.bus.write8(i, (*world_)[i]);
    }
    if (capture_ && r.link.size() == kLinkBytes) { // (the master's shadow: its link memory and variables, as its last link left them)
        const auto ram = e.bus.ram();
        for (uint32_t i = 0; i < kLinkBytes; ++i)
            if (ram[kLinkMemory + i] != r.link[i]) e.bus.write8(kLinkMemory + i, r.link[i]);
        std::size_t n = 0;
        for (const auto& [lo, hi] : kLinkVars)
            for (uint32_t a = lo; a < hi; ++a) e.bus.write8(a, r.link_vars[n++]);
    }
    for (uint32_t i = 0; i < kCameraBytes; ++i)
        if (e.bus.read8(kCamera + i) != r.camera[i]) e.bus.write8(kCamera + i, r.camera[i]);
    for (std::size_t k = 0; k < own_.size(); ++k) {
        const uint8_t here = e.bus.read8(own_[k]), v = static_cast<uint8_t>((here & ~own_mask_[k]) | (r.own[k] & own_mask_[k]));
        if (v != here) e.bus.write8(own_[k], v);
    }
    // (its buttons, 0x5094's words, for its frame of this count; its toggles flipped once a frame)
    const uint64_t frame = e.given ? e.frame : frame_for(r, logic); // (a worker's: given it, worked out on this board's thread)
    const uint32_t now = e.given ? e.now : held_(r.seat, frame), was = e.given ? e.was : held_(r.seat, frame - 1);
    e.bus.write32(0x100100, now), e.bus.write32(0x100104, now & ~was), e.bus.write32(0x100108, was & ~now);
    if (r.toggled_at != frame) e.bus.write32(0x10010C, e.bus.read32(0x10010C) ^ (now & ~was)), r.toggled_at = frame;
    // (its local player, its number, its time's entry: as 0x11C3C sets them)
    e.bus.write32(0x104024, player_object(r.seat));
    e.bus.write8(0x10065B, static_cast<uint8_t>(r.seat + 1));
    e.bus.write32(0x106008, 0x1062AE + 2 * static_cast<uint32_t>(r.seat + 1));
    e.c.set_scheduler(nullptr);
    e.c.cpu.msr &= ~recomp::Context::kMsrEE;
    e.c.limit_aside(30'000'000);
    e.inside = true;
    e.bus.hold_device_writes(true);
    std::vector<uint8_t> before; // (its flow's: its own bytes before, for what it changed)
    if (pc == kModeDispatch)
        for (uint32_t a : own_) before.push_back(e.bus.read8(a));
    if ((pc == kAnimationStep || tasks) && r.regions.size() == kRegionBytes) put_regions(e.bus, r.regions); // (its own areas: its 2D, its poses)
    const auto started_at = std::chrono::steady_clock::now();
    try {
        if (pc == kViewTest) { // (with this camera's view matrix, as the game has it in f20-f31 then)
            if (character) e.c.cpu.gpr[14] = character;
            for (int k = 0; k < 12; ++k) {
                const uint32_t bits = e.bus.read32(kCamera + kViewMatrix + 4 * static_cast<uint32_t>(k));
                float f;
                std::memcpy(&f, &bits, 4);
                const double d = f;
                std::memcpy(&e.c.cpu.fpr[static_cast<std::size_t>(20 + k)], &d, 8);
            }
            e.c.call(pc);
        } else if (pc == kCameraUpdate) { // (the update, then what makes its view)
            e.c.call(pc);
            e.c.cpu = aside.cpu;
            e.c.cpu.msr &= ~recomp::Context::kMsrEE;
            e.c.cpu.gpr[12] = kCamera;
            e.c.call(kCameraView);
        } else {
            e.c.call(pc);
            if (pc == kModeDispatch && tasks) { // (and its frame's tasks, up to the drawing: the world's step)
                e.c.cpu = aside.cpu;
                e.c.cpu.msr &= ~recomp::Context::kMsrEE;
                // (its 2D task too, on its own 2D area: a banner's unpacking shows in its work)
                if (capture_) { // (the master's shadow: on through its drawing and the main loop to its 0x36E94, kept
                                // there to go on from: its 2D task as it was)
                    e.in_drawing = true, drawing_r1_ = aside.cpu.gpr[1];
                    try {
                        e.c.call(kTasks);
                        e.c.call(kTasksReturn); // (the main loop on after its tasks: 0xCB84, then 0x36E94)
                    } catch (const DrawingDone&) {
                        captured_cpu_ = e.c.cpu, captured_cpu_.pc = kBeforeIdle;
                        captured_.assign(e.bus.ram().begin(), e.bus.ram().end());
                    }
                    e.in_drawing = false;
                } else {
                    // (its tasks walked as the main loop's walker (0x11DF4) does: from 0x110000, as many as the ROM's count
                    // (0x8CF64), each with bit 0 of its +0 run (r12 its entry, its routine +12), on by +8; the drawing's
                    // stops at its vblank wait and the walk goes on: the tasks after it (its 2D task and HUD, the
                    // header's copy 0x5E9F4 at 0x11CF80, whose run bit is the cabinet's own) are its too (2026-11-07:
                    // the walker itself stopped there with the drawing, those never ran). Its header's words as its own
                    // last link left them first: its copy, if it makes one, over them)
                    if (r.link.size() == kLinkBytes)
                        for (uint32_t a : {0x150u, 0x151u, 0x152u, 0x153u, 0x15Cu, 0x15Du, 0x15Eu, 0x15Fu}) e.bus.write8(kLinkMemory + a, r.link[a]);
                    e.in_tasks = true;
                    const uint64_t t0 = e.c.cycles();
                    const uint32_t task_count = e.bus.read32(kTaskCount);
                    uint32_t entry = kTaskList;
                    for (uint32_t i = 0; i < task_count && i < 1024; ++i) {
                        if (e.bus.read32(entry) & 1u) {
                            e.c.cpu = aside.cpu;
                            e.c.cpu.msr &= ~recomp::Context::kMsrEE;
                            e.c.cpu.gpr[12] = entry;
                            try {
                                e.c.call(e.bus.read32(entry + 12));
                            } catch (const TasksDrawing&) {
                            }
                        }
                        entry += e.bus.read32(entry + 8);
                    }
                    e.in_tasks = false;
                    if (e.c.cycles() - t0 > 400'000 && std::getenv("ONEBOARD_LOST_LOG"))
                        std::printf("seat %zu: count %u's world step %llu cycles\n", r.seat, count,
                                    static_cast<unsigned long long>(e.c.cycles() - t0));
                    if (e.c.cycles() - t0 > timing_.frame_cycles() * 4 / 5) {
                        if (e.stats) e.stats->lost.emplace_back(&r, count);
                        else lose_after(r, count);
                    }
                    if (e.c.cycles() - t0 > kHeavyWork) {
                        if (e.stats) { if (!e.stats->heavy++) e.stats->first_heavy = e.at_frame; }
                        else if (!heavy_frames_++) first_heavy_ = timing_.frames();
                    }
                }
                e.c.cpu = aside.cpu; // (and what makes its camera's view, which the drawing does)
                e.c.cpu.msr &= ~recomp::Context::kMsrEE;
                e.c.cpu.gpr[12] = kCamera;
                e.c.call(kCameraView);
                // (only while its player's update runs (0x5185C: the word at kPlayersObject's +0x10 + 4 * seat, bit 0): out of
                // play, its object emptied, the real cabinet's stays 00000000 and comes back 00000100, no view bits; tested
                // here it took 08 and 10 and sent them on its return (2026-11-11, the owner's recorded game))
                const uint32_t runs_at = e.bus.read32(kPlayersObject) + 0x10 + 4 * static_cast<uint32_t>(r.seat);
                const bool runs = runs_at < 0x100000 || runs_at + 4 > m3::kWorkRamSize || (e.bus.read32(runs_at) & 1u);
                if (!capture_ && count && runs) { // (and its view test of its own player, on the world its tasks made, as its
                                          // drawing's: a run of its own started from this board's world, a count behind,
                                          // saw its player a count late (2026-11-07, tools/onepc-diff.ps1))
                    e.c.cpu = aside.cpu;
                    e.c.cpu.msr &= ~recomp::Context::kMsrEE;
                    const uint32_t player = player_object(r.seat);
                    e.c.cpu.gpr[14] = player;
                    for (int k = 0; k < 12; ++k) {
                        const uint32_t bits = e.bus.read32(kCamera + kViewMatrix + 4 * static_cast<uint32_t>(k));
                        float f;
                        std::memcpy(&f, &bits, 4);
                        const double d = f;
                        std::memcpy(&e.c.cpu.fpr[static_cast<std::size_t>(20 + k)], &d, 8);
                    }
                    e.c.call(kViewTest);
                    r.view_at[{count, player}] = e.bus.read32(player);
                    r.viewed = count;
                }
            }
        }
    } catch (const std::exception& ex) {
        e.in_tasks = e.in_drawing = false;
        if (std::string& failed = e.stats ? e.stats->failed : failed_; failed.empty()) failed = "seat " + std::to_string(r.seat + 1) + ", side run at " + std::to_string(pc) + ": " + ex.what();
    }
    const auto ran_to = std::chrono::steady_clock::now();
    (e.stats ? e.stats->side_seconds : side_seconds_) += std::chrono::duration<double>(ran_to - started_at).count();
    ++(e.stats ? e.stats->side_runs : side_runs_);
    e.inside = false;
    e.bus.hold_device_writes(false);
    if (keep_camera)
        for (uint32_t i = 0; i < kCameraBytes; ++i) r.camera[i] = e.bus.read8(kCamera + i);
    for (std::size_t k = 0; k < own_.size(); ++k) r.own[k] = e.bus.read8(own_[k]);
    if ((pc == kAnimationStep || tasks) && r.regions.size() == kRegionBytes) regather(r.regions, e.bus.ram());
    if (flags) *flags = e.bus.read32(character ? character : aside.cpu.gpr[14]);
    if (pc == kModeDispatch) {
        std::vector<uint32_t>& ahead = e.stats ? e.stats->ahead : ahead_record_;
        ahead.resize(0x80);
        for (uint32_t i = 0; i < 0x80; ++i) ahead[i] = e.bus.read32(kWorkingRecord + 4 * i);
        if (tasks && count) {
            auto& w = r.working_at[count];
            std::copy(ahead.begin(), ahead.end(), w.begin());
            while (r.working_at.size() > 8) r.working_at.erase(r.working_at.begin());
            // (and the world's bytes in its link memory, as its run left them: its 2D task's HUD (0x13654) copies the
            // header's words (0x624F4, through the HUD object's routine 0x5E9F4) when it does, else they stay its own)
            auto& g = r.game_at[count];
            std::size_t n = 0;
            for (const auto& [lo, hi] : kLinkGameBytes)
                for (uint32_t a = lo; a < hi; ++a) g[n++] = e.bus.read8(a);
            while (r.game_at.size() > 8) r.game_at.erase(r.game_at.begin());
        }
        auto& d = r.flow_delta[count];
        d.clear();
        for (std::size_t k = 0; k < own_.size(); ++k)
            if (r.own[k] != before[k]) d.emplace_back(static_cast<uint32_t>(k), r.own[k]);
        while (!r.flow_delta.empty() && r.flow_delta.begin()->first + 16 < count) r.flow_delta.erase(r.flow_delta.begin());
    }
    if (e.paged) { // (a worker's: the pages it wrote from its clean copy)
        e.paged->put_back_pages();
    } else {
        journal_bytes_ += e.bus.journal_size();
        e.bus.undo_journal();
        e.bus.journal(false);
    }
    e.c.put_back(aside);
    profiled(e.stats ? e.stats->profile : profile_, pc | (tasks ? 1u : 0u), begun, started_at, ran_to);
}

inline void Cabinets::shake(Remote& r, uint32_t kind) {
    const uint32_t entry = kShakeTable + 8 * (kind - 1);
    r.camera[0xD2] = r.camera[0xD3] = bus_.read8(entry);
    for (uint32_t k = 0; k < 4; ++k) r.camera[0xD4 + k] = bus_.read8(entry + 4 + k);
}

inline bool Cabinets::hook(uint32_t pc) {
    if (main_.in_drawing) { // (the master's capture: on through its drawing to its main loop's 0x36E94)
        if (pc == kVblankLoop) bus_.write8(kVblankCount, static_cast<uint8_t>(bus_.read8(kVblankCount) + 1));
        if (pc == kTransferLoop) bus_.write8(kTransferBusy, 0);
        if (pc == kBeforeIdle) throw DrawingDone{};
        return true;
    }
    if (in_link_ && pc == kLinkSkipped) throw LinkSkipped{};
    if (main_.in_tasks && pc == kVblankWait) throw TasksDrawing{};
    if (main_.inside) { // (a transfer its tasks wait for (a banner's tiles): done at once, its device writes held)
        if (pc == kTransferLoop) bus_.write8(kTransferBusy, 0);
        return true;
    }
    if (pc == kBeforeIdle) { // (this board's MSR there, which a master's shadow starting there takes)
        idle_msr_ = c_.cpu.msr;
        // (and the game's own hook after it: the same PC is its decrementer wait (m3::games, kDecLoop), gone round at
        // once by its idle skip. Taken here, it never was: the staying board went round that wait up to 200,000 times
        // a frame, one board's ~1.5 ms a frame over every board's (2026-11-09))
        return false;
    }
    if (pc == kModeDispatch || pc == kViewTest || pc == kLinkSkipped) settle(); // (the worker's runs' results wanted here)
    if (pc == kLinkSkipped) // (a slave handed back: this board's world after the count it is to catch up, for its view tests)
        for (Remote& r : remotes_)
            if (!r.saved_world.empty() && r.saved_count == bus_.read32(kLogicCounter)) r.view_world.assign(bus_.ram().begin(), bus_.ram().end());
    const uint32_t logic = bus_.read32(kLogicCounter);
    switch (pc) {
    case kCameraView: // (its writes are the camera's own, until the next view test)
        in_view_ = true;
        return true;
    case kAfterUpdate: // (and the update's, until it returns)
        if (c_.cpu.lr == kAfterUpdateReturn) in_update_ = false;
        return true;
    case kRecordWrite: {
        // (none after this count: kept from before the count went back (a cabinet joining a link: its own count, run
        // alone in attract, set to the link's), else found again when the count gets there)
        players_at_.erase(players_at_.upper_bound(logic), players_at_.end());
        Players& at = players_at_[logic];
        for (std::size_t k = 0; k < 4; ++k) {
            const uint32_t p = player_object(k);
            at.p[k] = {bus_.read32(p), bus_.read32(p + 0x44), bus_.read16(p + 0x2A), bus_.read16(p + 0x74), bus_.read16(p + 0x76)};
        }
        at.in_play = players_in_play();
        while (!players_at_.empty() && players_at_.begin()->first + 16 < logic) players_at_.erase(players_at_.begin());
        return true;
    }
    case kModeDispatch: {
        std::vector<Job> jobs;
        for (Remote& r : remotes_) {
            if (r.hand_back_asked && r.shadowed && r.stop_after == 0) { // (a hand-back: where its shadow stops; a slave's world)
                r.stop_after = logic + static_cast<uint32_t>(r.ahead);
                if (r.ahead < 0) r.saved_world.assign(bus_.ram().begin(), bus_.ram().end()), r.saved_count = logic;
            }
            if (r.catch_up && !r.shadowed) { // (handed back, a slave: its count after the shadow's last, on the world kept)
                r.catch_up = false;
                if (r.saved_count == r.own_logic + 1) {
                    world_ = &r.saved_world;
                    side_run(main_, r, pc, nullptr, true, r.saved_count, 0, true);
                    world_ = nullptr;
                    r.own_logic = r.saved_count;
                    // (its view tests of that count, of what this board's took, on this board's world after it (before
                    // its next link moved the characters the other cabinets' records carry))
                    world_ = r.view_world.empty() ? nullptr : &r.view_world;
                    if (const auto chars = view_chars_.find(r.saved_count); chars != view_chars_.end())
                        for (const ppc::CpuState& called : chars->second) { // (with the registers its draw loop called it with)
                            const uint32_t ch = called.gpr[14];
                            const ppc::CpuState here = c_.cpu;
                            c_.cpu = called;
                            c_.cpu.msr = here.msr;
                            uint32_t flags = 0;
                            side_run(main_, r, kViewTest, &flags, false, 0, ch);
                            c_.cpu = here;
                            r.view_at[{r.saved_count, ch}] = flags;
                        }
                    world_ = nullptr;
                    r.heading_at[r.saved_count] = static_cast<uint16_t>(r.camera[kCameraHeading] << 8 | r.camera[kCameraHeading + 1]);
                }
                r.saved_world.clear(), r.saved_world.shrink_to_fit(), r.view_world.clear(), r.view_world.shrink_to_fit();
            }
            if (!started(r, logic) || r.own_logic >= logic) continue;
            r.own_snap[r.own_logic + 1] = {r.own, r.camera, r.regions};
            while (r.own_snap.size() > 4) r.own_snap.erase(r.own_snap.begin());
            const uint32_t runs = std::min<uint32_t>(logic - r.own_logic, 8);
            if (tasks_mode_ && on_camera) on_camera(r.seat, logic, r.camera);
            r.own_logic = logic;
            if (tasks_mode_ && workers_wanted()) { // (on a worker, while this board goes on with its own count)
                const uint64_t frame = frame_for(r, logic);
                jobs.push_back({&r, logic, runs, frame, held_(r.seat, frame), held_(r.seat, frame - 1), timing_.frames()});
                continue;
            }
            for (uint32_t k = 0; k < runs; ++k) side_run(main_, r, pc, nullptr, true, logic - runs + k + 1, 0, tasks_mode_ && k + 1 == runs);
            if (tasks_mode_) own_view(main_, r, logic);
        }
        if (!jobs.empty() && worker_check_) { // (SPIKEOUT_SIDE_CHECK: each run here too, on a copy, compared once settled)
            checks_.clear();
            for (const Job& j : jobs) {
                Remote copy = *j.r;
                for (uint32_t k = 0; k < j.runs; ++k) side_run(main_, copy, pc, nullptr, true, j.logic - j.runs + k + 1, 0, k + 1 == j.runs);
                own_view(main_, copy, j.logic);
                checks_.push_back({j.r, std::move(copy), ahead_record_, j.logic});
            }
        }
        if (!jobs.empty()) start_workers(std::move(jobs));
        return true;
    }
    case kCameraUpdate:
        in_update_ = true, in_view_ = false;
        if (tasks_mode_) return true; // (run in its tasks)
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            if (on_camera) on_camera(r.seat, logic, r.camera);
            side_run(main_, r, pc, nullptr, true);
            { // (its view of its own player too, now: its next link carries it, and this board's own test of it can
              // come late, after the frame's end)
                uint32_t flags = 0;
                side_run(main_, r, kViewTest, &flags, false, 0, player_object(r.seat));
                r.view_at[{logic, player_object(r.seat)}] = flags;
            }
            r.heading_at[logic] = static_cast<uint16_t>(r.camera[kCameraHeading] << 8 | r.camera[kCameraHeading + 1]);
            while (!r.heading_at.empty() && r.heading_at.begin()->first + 16 < logic) r.heading_at.erase(r.heading_at.begin());
            while (!r.view_at.empty() && r.view_at.begin()->first.first + 16 < logic) r.view_at.erase(r.view_at.begin());
        }
        return true;
    case kAnimationStep: // (a character's: as each of the others too, into its regions)
        if (tasks_mode_) return true;
        for (Remote& r : remotes_)
            if (!r.camera.empty() && r.regions.size() == kRegionBytes) side_run(main_, r, pc, nullptr, false);
        return true;
    case kViewTest:
        in_view_ = false;
        view_chars_.erase(view_chars_.upper_bound(logic), view_chars_.end()); // (as players_at_: none from before the count went back)
        view_chars_[logic].push_back(c_.cpu);
        while (view_chars_.size() > 4) view_chars_.erase(view_chars_.begin());
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            uint32_t flags = 0;
            side_run(main_, r, pc, &flags, false);
            r.view_at[{logic, c_.cpu.gpr[14]}] = flags;
            if (r.pending != ~0ull && ready(r)) link_run(r, r.pending), r.pending = ~0ull; // (its link, waiting for this)
        }
        return true;
    case kShakeByAnimation:
    case kShakeFollowed:
    case kShakeByHit: {
        if (tasks_mode_) return true; // (decided in its tasks)
        const uint32_t r13 = c_.cpu.gpr[13], character = c_.cpu.gpr[14];
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            uint32_t kind = 0;
            if (pc == kShakeByHit) { // (0x26070-0x260CC)
                const uint32_t hit = c_.cpu.gpr[15], local = player_object(r.seat);
                const uint32_t flags = uint32_t{own_byte(r, r13 - 16380)} << 24 | uint32_t{own_byte(r, r13 - 16379)} << 16 |
                                       uint32_t{own_byte(r, r13 - 16378)} << 8 | own_byte(r, r13 - 16377);
                const bool involved = local == character || local == hit;
                bool go;
                if (own_byte(r, r13 - 16376) != 3) go = involved;
                else go = (flags & 0x01080000u) && (!(bus_.read32(character) & 0x2u) || involved);
                const uint32_t h = bus_.read32(hit);
                if (go && ((h & 0x2u) || !(h & 0x04000000u))) kind = bus_.read8(character + 7784);
            } else if (pc == kShakeFollowed) {
                if (r.camera[kCameraFollows] == bus_.read8(character + 114)) kind = 7;
            } else { // (its mode and flags, -16376(r13) and -16380(r13), its own)
                const uint32_t local = player_object(r.seat);
                bool shake;
                if (own_byte(r, r13 - 16376) == 3)
                    shake = ((uint32_t{own_byte(r, r13 - 16380)} << 24 | uint32_t{own_byte(r, r13 - 16379)} << 16 |
                              uint32_t{own_byte(r, r13 - 16378)} << 8 | own_byte(r, r13 - 16377)) & 0x01080000u) != 0;
                else if (bus_.read32(character) & 0x04000000u) shake = false;
                else shake = local == character || ((bus_.read32(character + 68) & 0x1000u) && bus_.read32(character + 272) == local);
                if (shake) kind = bus_.read8(c_.cpu.gpr[28] + 4);
            }
            if (kind) this->shake(r, kind);
        }
        return true;
    }
    default:
        return false;
    }
}

inline void Cabinets::camera_written(void* user, uint32_t addr, uint32_t value, uint32_t size) {
    Cabinets& self = *static_cast<Cabinets*>(user);
    if (self.main_.inside || self.in_view_ || self.in_update_ || self.tasks_mode_) return; // (tasks mode: its own tasks make them)
    const uint32_t pc = self.c_.cpu.pc;
    if (pc == kShakeWrite) return; // (decided again for each, at its hooks)
    for (uint32_t w : kOwnCameraWriters)
        if (pc == w) return;
    for (Remote& r : self.remotes_) {
        if (r.camera.empty()) continue;
        for (uint32_t k = 0; k < size; ++k) {
            const uint32_t at = addr + k - kCamera;
            // (+0x00-0x07 each cabinet's flags and counter; +0x4A8-0x4BF 0x32470's, the camera's own)
            if (at < 8 || at >= kCameraBytes || (at >= 0x4A8 && at < 0x4C0)) continue;
            r.camera[at] = static_cast<uint8_t>(value >> (8 * (size - 1 - k)));
        }
    }
}

inline void Cabinets::make_record(Remote& r, Block& next, const Block& mine, uint32_t logic, uint64_t frame, uint32_t old) {
    const uint32_t slot = logic & 3, rec = 0x100 + 0x200 * slot;
    // (this board's record of the count; a count it hasn't reached (the other a count ahead): as its working
    // record has it now, filled by its game since; the link stamps the count in)
    int from = -1;
    for (int s = 0; s < 4; ++s)
        if (mine[(0x100 + s * 0x200 + 0xDC) / 4] == logic) from = s;
    const auto worked = r.working_at.find(logic - 1);
    if (from < 0 && worked != r.working_at.end()) // (its own, after its flow and tasks of the count before, run here)
        for (uint32_t i = 0; i < 0x80; ++i) next[rec / 4 + i] = worked->second[i];
    else if (from < 0 && logic == mine[0x1C / 4] + 2 && ahead_record_.size() == 0x80) // (two ahead: its own flow's, just run)
        for (uint32_t i = 0; i < 0x80; ++i) next[rec / 4 + i] = ahead_record_[i];
    else if (from < 0)
        for (uint32_t i = 0; i < 0x80; ++i) next[rec / 4 + i] = bus_.read32(kWorkingRecord + 4 * i);
    else
        for (uint32_t i = 0; i < 0x80; ++i) next[rec / 4 + i] = mine[(0x100 + 0x200 * static_cast<uint32_t>(from)) / 4 + i];
    // (its buttons)
    next[(rec + 0x04) / 4] = held_(r.seat, frame);
    // (its player's fields as this board had them as it wrote its own record of this count; else as now)
    const uint32_t player = player_object(r.seat);
    std::array<uint32_t, 5> p{bus_.read32(player), bus_.read32(player + 0x44), bus_.read16(player + 0x2A), bus_.read16(player + 0x74),
                              bus_.read16(player + 0x76)};
    if (const auto at = players_at_.find(logic); at != players_at_.end()) p = at->second.p[r.seat];
    // (its player's update (0x55050) writes +0x08 to +0x0D only with the player's word's bit 0x02 clear, its mode
    // (-16376(r13)) not 3 and the player in the game's players mask (the word at +0x50 of -16108(r13)'s object,
    // 0x111150, the shared game's: cleared as a player times out, set again as one plays on); else the last kept
    // (2026-11-11, the owner's recorded game, tools/onepc-diff.ps1 -MadeBoard 0: the host's player gone at 28,786, its
    // heading 07ab and +0x08 7e4e7e4e kept to 28,956, the made ones from its camera and its emptied object)
    uint8_t mode = 0;
    for (std::size_t i = 0; i < own_.size() && i < r.own.size(); ++i)
        if (own_[i] == kOwnMode) mode = r.own[i];
    // (the players in play at this count, as this board had them at its own record of it; a count it hasn't reached (the
    // master's, a count ahead): as now, put right by link_run's pass over its older records once this board has it)
    const auto players_then = players_at_.find(logic);
    const uint32_t in_play = players_then != players_at_.end() ? players_then->second.in_play : players_in_play();
    const bool writes = !(p[0] & 0x02u) && mode != 3 && (in_play & (0x80000000u >> r.seat));
    if (writes) r.unwritten.erase(logic);
    else r.unwritten.insert(logic);
    while (!r.unwritten.empty() && *r.unwritten.begin() + 16 < logic) r.unwritten.erase(r.unwritten.begin());
    next[(rec + 0x08) / 4] = writes ? p[2] << 16 | ((p[1] & 0x80000000u) ? p[4] : p[3]) : r.block[(old + 0x08) / 4];
    // (its camera's heading after its update of the count before; else as it was. Not written, above: as before, while
    // its camera goes on turning)
    const auto heading = r.heading_at.find(logic - 1);
    if (!writes) // (its heading, +0x0C's high half; the low half isn't 0x55050's)
        next[(rec + 0x0C) / 4] = (r.block[(old + 0x0C) / 4] & 0xFFFF0000u) | (next[(rec + 0x0C) / 4] & 0xFFFFu);
    else
        next[(rec + 0x0C) / 4] = (heading != r.heading_at.end() ? uint32_t{heading->second} << 16 : r.block[(old + 0x0C) / 4] & 0xFFFF0000u) |
                                 (next[(rec + 0x0C) / 4] & 0xFFFFu);
    // (its player's first word, its view bits from its camera's test of the count before)
    next[(rec + 0x10) / 4] = p[0];
    // (its own view bits; the rest the shared game's. None for a player gone: its word 0, as the real record sends it
    // (2026-11-11, the owner's recorded game: 00000010 sent for 00000000 after the host's player left))
    // (out of play, its update not running: as they were, own_view)
    if (const auto v = r.view_at.find({logic - 1, player}); v != r.view_at.end() && p[0] != 0)
        next[(rec + 0x10) / 4] = (p[0] & ~0x18u) | (v->second & 0x18u);
    // (the empty slots' leftovers, as they were: players 3 and 4's +4 and +8 (+0x30 and +0x34, +0x3C and +0x40), each
    // cabinet's own while their seat is empty (0x74808 writes them from its unused players' objects), the game's once a
    // player is in it (+4: with three players, player 4's +7 was the third cabinet's own); and the on-screen bits)
    const uint32_t players = bus_.read32(kLinkPlayers);
    if (!(players & (0x80000000u >> 2)))
        for (uint32_t at : {0x30u, 0x34u}) next[(rec + at) / 4] = r.block[(old + at) / 4];
    if (!(players & (0x80000000u >> 3)))
        for (uint32_t at : {0x3Cu, 0x40u}) next[(rec + at) / 4] = r.block[(old + at) / 4];
    // (an enemy's there only if it was in its block before: one just set up has the bits its set-up gave it (0x1E3FC),
    // the same on every cabinet, until its cabinet's view tests change them (2026-11-07: enemy 1 at an area's set-up,
    // 02 sent as 00, tools/onepc-diff.ps1))
    for (uint32_t byte : {0x47u, 0x5Fu, 0x6Bu, 0x77u})
        if (r.block[(old + byte - 3) / 4] & 0xFFFFFF00u) put8(next, rec + byte, get8(r.block, old + byte));
    // (every character's view bits, players 1-4 then enemies 1-8, from its camera's tests two counts before; none
    // for one gone since: this board's entry all zero)
    // (a character there only with flags above their low byte: this board's entry can keep its own on-screen bits
    // for one gone (its low byte alone), which the other's, its whole word 0, hasn't: sent with them, a three-player
    // game with players pressing all the time went out of step as an enemy left (2026-11-06, docs/netplay.md))
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t entry = rec + 0x14 + 12 * k;
        const auto v = r.view_at.find({logic - 2, character_object(k)});
        // (its own bytes of the character's fields, kept while this board's record has them as they were (the record's
        // byte, not the object's now: a character set up again between this board's record and this); a gone
        // character's too: its flags word goes, its other bytes stay as each cabinet last wrote them)
        for (uint32_t j = 0; j < 4; ++j) {
            auto& kept = r.kept[k][j];
            if (!kept.on) continue;
            if (std::getenv("KEPT_DEBUG") && j == 3)
                std::printf("kept: count %u frame %llu character %u: record %02x, here %02x, its %02x\n", logic,
                            static_cast<unsigned long long>(frame), k, get8(next, entry + 4 + j), kept.mine & 0xFF, kept.theirs & 0xFF);
            if (get8(next, entry + 4 + j) != static_cast<uint8_t>(kept.mine)) kept.on = false;
            else put8(next, entry + 4 + j, static_cast<uint8_t>(kept.theirs));
        }
        // (its own entry +8, kSumFields' sum, the same way: while this board's record sends this board's sum of then)
        if (auto& kept = r.kept_sum[k]; kept.on) {
            if (next[(entry + 8) / 4] != kept.mine) kept.on = false;
            else next[(entry + 8) / 4] = kept.theirs;
        }
        if (!(next[entry / 4] & 0xFFFFFF00u)) {
            next[entry / 4] = 0;
            // (and its own bits of the character's first word gone with it, as its record says: kept from its last
            // view test, a shadow started from them sent them (2026-11-07, tools/onepc-diff.ps1))
            if (k >= 4)
                for (std::size_t i = 0; i < own_.size(); ++i)
                    if (own_[i] == character_object(k) + 3) {
                        // (none yet before its first seed: gamerun --one-board-camera's first record, 2026-11-11)
                        if (i < r.own.size()) r.own[i] = static_cast<uint8_t>(r.own[i] & ~own_mask_[i]);
                        if (i < r.state_own.size()) r.state_own[i] = static_cast<uint8_t>(r.state_own[i] & ~own_mask_[i]);
                    }
        } else if (v != r.view_at.end()) put8(next, entry + 3, static_cast<uint8_t>(v->second));
    }
    if (record_fix_) record_fix_(r.seat, next, rec); // (the bytes its cabinet keeps for itself, from its real records)
}

inline uint64_t Cabinets::frame_for(const Remote& r, uint32_t logic) const {
    if (!r.making || r.logic_frame == 0) return timing_.frames() - static_cast<uint64_t>(static_cast<int64_t>(r.ahead));
    if (const auto at = r.frame_at.find(logic); at != r.frame_at.end()) return at->second;
    uint64_t f = r.logic_frame;
    for (uint32_t n = r.logic; n < logic;)
        if (!missed(r, ++f)) ++n;
    return f;
}

inline void Cabinets::merge_own(const std::vector<uint8_t>& own) {
    for (std::size_t k = 0; k < own_.size(); ++k) {
        const uint8_t here = bus_.read8(own_[k]), v = static_cast<uint8_t>((here & ~own_mask_[k]) | (own[k] & own_mask_[k]));
        if (v != here) bus_.write8(own_[k], v);
    }
}

inline void Cabinets::link_run(Remote& r, uint64_t frame) {
    settle();
    const Timer whole{whole_seconds_, timer_depth_};
    const auto begun = std::chrono::steady_clock::now(); // (profile_)
    const uint32_t logic = r.logic + 1; // (the count it goes on to, unless held back)
    // (two counts ahead of this board (this one missed a frame): its flow of the count before run now, on this
    // board's world after the count before that, its working record from it)
    ahead_record_.clear();
    if (logic == bus_.read32(kLogicCounter) + 2 && r.own_logic < logic - 1 && !r.camera.empty()) {
        side_run(main_, r, kModeDispatch, nullptr, false, logic - 1, 0, true); // (its camera's update for it comes at its hook)
        r.own_logic = logic - 1;
    }
    // (its next record, made from this board's before its memory goes in)
    Block mine{};
    for (uint32_t i = 0; i < kBlockWords; ++i) mine[i] = bus_.read32(kBlock + 4 * i);
    Block made = r.block;
    make_record(r, made, mine, logic, frame, 0x100 + 0x200 * static_cast<uint32_t>(newest(r.block)));
    const recomp::Context::Aside aside = c_.set_aside();
    bus_.journal(true);
    // (its link memory, its own variables as its last link left them, its flows since)
    const auto ram = bus_.ram();
    // (the world's bytes there: as its own run of the count before left them (Remote::game_at); else this board's now.
    // Not this board's by count or frame: cabinets a frame apart lose frames at different times, and one ahead has
    // them before this board, 2026-11-07)
    std::array<uint8_t, 32> game_bytes{};
    const auto g = r.game_at.find(logic - 1);
    if (g != r.game_at.end()) game_bytes = g->second;
    else {
        std::size_t n = 0;
        for (const auto& [lo, hi] : kLinkGameBytes)
            for (uint32_t a = lo; a < hi; ++a) game_bytes[n++] = ram[a];
    }
    // (the header's words with them, as every cabinet's link sends the copy its drawing made the count before (a rule
    // taking a slave's from two counts before was only ever in force with GAME_BYTES_LOG set, a slip: wrong once in
    // force, three cabinets, 2026-11-07))
    if (std::getenv("GAME_BYTES_LOG"))
        std::printf("link_run: seat %zu frame %llu count %u: header %s %02x%02x%02x%02x\n", r.seat, static_cast<unsigned long long>(frame),
                    logic, g != r.game_at.end() ? "made" : "this board's", game_bytes[7], game_bytes[8], game_bytes[9], game_bytes[10]);
    for (uint32_t at = 0; at < kLinkBytes; at += 64) { // (64 bytes at a time compared first, as put_regions)
        if (std::memcmp(&ram[kLinkMemory + at], &r.link[at], 64) == 0) continue;
        for (uint32_t i = at; i < at + 64; ++i)
            if (ram[kLinkMemory + i] != r.link[i]) bus_.write8(kLinkMemory + i, r.link[i]);
    }
    {
        std::size_t n = 0;
        for (const auto& [lo, hi] : kLinkGameBytes)
            for (uint32_t a = lo; a < hi; ++a, ++n)
                if (ram[a] != game_bytes[n]) bus_.write8(a, game_bytes[n]);
    }
    merge_own(r.state_own);
    if (r.regions.size() == kRegionBytes) put_regions(r.regions); // (its own areas: the entries it processes read its unused players' objects)
    for (auto d = r.flow_delta.upper_bound(r.flow_applied); d != r.flow_delta.end() && d->first <= r.logic; ++d)
        for (const auto& [k, v] : d->second) {
            const uint8_t here = bus_.read8(own_[k]), w = static_cast<uint8_t>((here & ~own_mask_[k]) | (v & own_mask_[k]));
            if (w != here) bus_.write8(own_[k], w);
        }
    r.flow_applied = r.logic;
    {
        std::size_t n = 0;
        for (const auto& [lo, hi] : kLinkVars)
            for (uint32_t a = lo; a < hi; ++a) bus_.write8(a, r.link_vars[n++]);
    }
    // (its older records' camera fields from its camera's runs since, which can come after a record was written:
    // the others read a record two counts on, and every block sends the last four again)
    // (oldest first: a record its player's update didn't write takes the one before's, put right first)
    std::array<uint32_t, 4> order{0, 1, 2, 3};
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return bus_.read32(kBlock + 0x100 + 0x200 * a + 0xDC) < bus_.read32(kBlock + 0x100 + 0x200 * b + 0xDC);
    });
    for (uint32_t s : order) {
        const uint32_t at = kBlock + 0x100 + 0x200 * s, x = bus_.read32(at + 0xDC);
        if (x > r.logic || x + 3 < r.logic) continue;
        // (its player's update's fields (0x55050): as this board's record of the count says it wrote them, now it has one
        // (made a count ahead, the players in play were this board's of the count before); else the record before's)
        const bool writes = wrote_at(r, x);
        const uint32_t before = kBlock + 0x100 + 0x200 * ((x - 1) & 3);
        if (!writes && bus_.read32(before + 0xDC) == x - 1) {
            bus_.write32(at + 0x08, bus_.read32(before + 0x08));
            bus_.write16(at + 0x0C, bus_.read16(before + 0x0C));
        } else if (writes) {
            if (const auto pa = players_at_.find(x); pa != players_at_.end()) {
                const auto& p = pa->second.p[r.seat];
                bus_.write32(at + 0x08, p[2] << 16 | ((p[1] & 0x80000000u) ? p[4] : p[3]));
            }
            if (const auto h = r.heading_at.find(x - 1); h != r.heading_at.end()) bus_.write16(at + 0x0C, h->second);
        }
        if (const auto v = r.view_at.find({x - 1, player_object(r.seat)}); v != r.view_at.end() && (bus_.read32(at + 0x10) & 0xFFFFFF00u))
            bus_.write8(at + 0x13, static_cast<uint8_t>(v->second)); // (a player gone: its word 0, none)
        for (uint32_t k = 0; k < 12; ++k)
            if (const auto v = r.view_at.find({x - 2, character_object(k)}); v != r.view_at.end() && bus_.read32(at + 0x14 + 12 * k) != 0)
                bus_.write8(at + 0x14 + 12 * k + 3, static_cast<uint8_t>(v->second));
    }
    // (its buttons for this frame, its identity, its player's view bits from its camera's test of the count before)
    const uint32_t now = held_(r.seat, frame), was = held_(r.seat, frame - 1);
    bus_.write32(0x100100, now), bus_.write32(0x100104, now & ~was), bus_.write32(0x100108, was & ~now);
    bus_.write32(0x104024, player_object(r.seat));
    bus_.write8(0x10065B, static_cast<uint8_t>(r.seat + 1));
    bus_.write32(0x106008, 0x1062AE + 2 * static_cast<uint32_t>(r.seat + 1));
    // (its loop count, -32764(r13), one up at its main loop's top; its frame counter, -32604(r13), too, unless -15360(r13)'s
    // 0x04000000)
    bus_.write32(kLoopCount, bus_.read32(kLoopCount) + 1);
    if (!(bus_.read32(0x104400) & 0x04000000u)) bus_.write32(0x1000A4, bus_.read32(0x1000A4) + 1);
    // (its working record: the one made, but +0x100, its flow's and link's own; its player's first word as the
    // record has it, which the link stamps in. Its first after a hand-back: the working record as its shadow's game
    // left it, the player's word still the made one's)
    const uint32_t rec = 0x100 + 0x200 * (logic & 3);
    bus_.write32(player_object(r.seat), made[(rec + 0x10) / 4]);
    if (!r.first_link_working) {
        for (uint32_t i = 0; i < 0x80; ++i)
            if (i != 0x100 / 4) bus_.write32(kWorkingRecord + 4 * i, made[rec / 4 + i]);
    }
    r.first_link_working = false;
    std::vector<uint8_t> was_own(own_.size());
    for (std::size_t k = 0; k < own_.size(); ++k) was_own[k] = bus_.read8(own_[k]);
    c_.set_scheduler(nullptr);
    c_.cpu.msr &= ~recomp::Context::kMsrEE;
    c_.limit_aside(30'000'000);
    main_.inside = in_link_ = true;
    bus_.hold_device_writes(true);
    bus_.borrow_netboard(r.netboard);
    const auto started_at = std::chrono::steady_clock::now();
    try {
        c_.call(kLink);
    } catch (const LinkSkipped&) {
    } catch (const std::exception& e) {
        if (failed_.empty()) { // (with the link's error word, -7544(r13): set, its run ends in the "link broken" halt, 0x74350)
            char w[64];
            std::snprintf(w, sizeof w, " (link error %08x, count %u)", bus_.read32(kLinkError), bus_.read32(kLogicCounter));
            failed_ = "seat " + std::to_string(r.seat + 1) + ", link run: " + e.what() + w;
        }
    }
    const auto ran_to = std::chrono::steady_clock::now();
    side_seconds_ += std::chrono::duration<double>(ran_to - started_at).count();
    ++side_runs_;
    if (header_fix_) // (its block's header bytes its cabinet keeps for itself, from its real ones: into its network board's copy)
        header_fix_(r.seat, [this](std::size_t p, uint8_t v) {
            bus_.write8(kNetBlock + static_cast<uint32_t>((p & ~std::size_t{3}) + (3 - p % 4)), v);
        });
    bus_.borrow_netboard(nullptr);
    bus_.hold_device_writes(false);
    main_.inside = in_link_ = false;
    // (kept: its link memory, variables, own variables; what the link changed of its link memory, into its
    // flows' too (not the characters' view bits: the received entries put the other cabinet's there, which its
    // own view tests write over))
    r.link.assign(ram.begin() + kLinkMemory, ram.begin() + kLinkMemory + kLinkBytes);
    {
        std::size_t n = 0;
        for (const auto& [lo, hi] : kLinkVars)
            for (uint32_t a = lo; a < hi; ++a) r.link_vars[n++] = ram[a];
    }
    for (std::size_t k = 0; k < own_.size(); ++k) {
        r.state_own[k] = ram[own_[k]];
        if (r.state_own[k] != was_own[k] && !r.own.empty() && own_[k] >= kLinkMemory && own_[k] < kLinkMemory + kLinkBytes)
            r.own[k] = r.state_own[k];
    }
    if (r.regions.size() == kRegionBytes) regather(r.regions, ram);
    for (uint32_t i = 0; i < kBlockWords; ++i) r.block[i] = bus_.read32(kBlock + 4 * i);
    const uint32_t now_logic = bus_.read32(kLogicCounter);
    journal_bytes_ += bus_.journal_size();
    bus_.undo_journal();
    bus_.journal(false);
    profiled(profile_, kLink, begun, started_at, ran_to);
    c_.put_back(aside);
    if (now_logic != r.logic) {
        r.logic = now_logic, r.logic_frame = frame, r.frame_at[now_logic] = frame;
        while (!r.frame_at.empty() && r.frame_at.begin()->first + 16 < now_logic) r.frame_at.erase(r.frame_at.begin());
    }
    r.link_snap[r.logic] = {r.link, r.link_vars};
    while (r.link_snap.size() > 4) r.link_snap.erase(r.link_snap.begin());
    ++made_;
}

inline void Cabinets::hand_back(std::size_t seat, const Seed& seed, std::span<const uint8_t> ram, uint64_t last) {
    settle();
    Remote* r = find(seat);
    if (!r || !r->netboard) return;
    m3::NetBoardDevice& netboard = *r->netboard;
    start_making(seat, netboard, ram, last);
    r->shadowed = false, r->hand_back_asked = false, r->stop_after = 0;
    r->camera = seed.camera, r->own = seed.own, r->own_logic = seed.own_logic;
    if (seed.regions.size() == kRegionBytes) r->regions = seed.regions;
    r->state_own = seed.own;
    r->first_link_working = true;
    r->catch_up = r->ahead < 0 && !r->saved_world.empty();
    r->own_snap.clear(), r->link_snap.clear(), r->flow_delta.clear(), r->heading_at.clear(), r->view_at.clear(), r->working_at.clear(), r->game_at.clear();
    // (what its records of the counts to come take from its camera's runs of this one: its heading, its view bits)
    const uint32_t count = seed.own_logic;
    r->heading_at[count] = static_cast<uint16_t>(seed.camera[kCameraHeading] << 8 | seed.camera[kCameraHeading + 1]);
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t ch = character_object(k);
        r->view_at[{count, ch}] = uint32_t(ram[ch]) << 24 | uint32_t(ram[ch + 1]) << 16 | uint32_t(ram[ch + 2]) << 8 | ram[ch + 3];
        // (and the count before's, which its next record carries: its working record, filled as this count ran, has
        // them; without, that record kept this board's own view bits (three cabinets, 2026-11-07, tools/onepc-diff.ps1))
        const uint32_t w = kWorkingRecord + 0x14 + 12 * k;
        r->view_at[{count - 1, ch}] = uint32_t(ram[w]) << 24 | uint32_t(ram[w + 1]) << 16 | uint32_t(ram[w + 2]) << 8 | ram[w + 3];
    }
}

inline bool Cabinets::shadow_state(std::size_t seat, uint32_t count, Shadow& out) {
    settle();
    Remote* r = find(seat);
    if (!r) return false;
    const auto own = r->own_snap.find(count + 1);
    const auto link = r->link_snap.find(count);
    if (own == r->own_snap.end() || link == r->link_snap.end() || own->second.camera.empty()) {
        std::printf("(shadow state for count %u: own variables after it %s, link after it %s; its count %u, own variables after %u; kept:",
                    count, own == r->own_snap.end() ? "missing" : own->second.camera.empty() ? "without a camera" : "there",
                    link == r->link_snap.end() ? "missing" : "there", r->logic, r->own_logic);
        for (const auto& [k, v] : r->own_snap) std::printf(" own %u", k - 1);
        for (const auto& [k, v] : r->link_snap) std::printf(" link %u", k);
        std::printf(")\n");
        return false;
    }
    out.own = own->second.own, out.camera = own->second.camera, out.regions = own->second.regions;
    out.link = link->second.first, out.link_vars = link->second.second;
    // (its next record, as its tasks of `count` would have left its working record)
    Block mine{};
    for (uint32_t i = 0; i < kBlockWords; ++i) mine[i] = bus_.read32(kBlock + 4 * i);
    Block made{};
    for (uint32_t i = 0; i < kBlockWords; ++i)
        made[i] = uint32_t(out.link[kBlock - kLinkMemory + 4 * i]) << 24 | uint32_t(out.link[kBlock - kLinkMemory + 4 * i + 1]) << 16 |
                  uint32_t(out.link[kBlock - kLinkMemory + 4 * i + 2]) << 8 | out.link[kBlock - kLinkMemory + 4 * i + 3];
    ahead_record_.clear();
    make_record(*r, made, mine, count + 1, frame_for(*r, count + 1), 0x100 + 0x200 * static_cast<uint32_t>(newest(made)));
    for (uint32_t i = 0; i < 0x80; ++i) out.working[i] = made[(0x100 + 0x200 * ((count + 1) & 3)) / 4 + i];
    out.fields.clear();
    for (uint32_t k = 0; k < 12; ++k)
        for (uint32_t j = 0; j < 4; ++j)
            if (const auto& kept = r->kept[k][j]; kept.on) {
                const uint32_t a = character_object(k) + kEntryFields[j].first;
                if (field(a, kEntryFields[j].second) == kept.mine) out.fields.push_back({a, kEntryFields[j].second, kept.theirs});
            }
    for (uint32_t k = 0; k < 12; ++k) // (and the fields of its entries' +8 sums, its own where this board's are as then)
        if (const auto& kept = r->kept_sum[k]; kept.on)
            for (uint32_t j = 0; j < 7; ++j) {
                const auto [off, size] = kSumFields[j];
                const uint32_t a = character_object(k) + off;
                if (field(a, size) == kept.mine_fields[j]) out.fields.push_back({a, size, kept.their_fields[j]});
            }
    return true;
}

inline bool Cabinets::ready(const Remote& r) const {
    return r.view_at.count({r.logic, player_object(r.seat)}) != 0;
}

inline void Cabinets::meet(uint64_t frame, uint64_t t) {
    const uint64_t at = timing_.frame_start(frame) + write_at_;
    for (Remote& r : remotes_) {
        if (!r.making) continue;
        const uint64_t due = r.link_from && r.link_from < at ? r.link_from : at; // (link_early: sooner)
        if (r.written_frame != frame && t >= due) {
            {
                const Timer nb{netboard_seconds_, timer_depth_};
                r.netboard->sync(std::min(at, t));
            }
            r.written_frame = frame;
            settle(); // (its link run wants its side runs' results)
            if (!missed(r, frame)) {
                if (ready(r)) link_run(r, frame);
                else r.pending = frame;
            }
        }
        const Timer nb{netboard_seconds_, timer_depth_};
        r.netboard->sync(t);
    }
}

inline void Cabinets::end_frame(uint64_t frame) {
    settle();
    const uint64_t end = timing_.frame_start(frame) + timing_.frame_cycles();
    meet(frame, end);
    for (Remote& r : remotes_) {
        if (r.link_from < end) r.link_from = 0; // (link_early's, if it was this frame's)
        if (!r.making) continue;
        // (its vblank's countdowns, once a frame (0x496C, from its interrupt, which side runs never take): those of its own
        // variables, its copy's, else they'd stay where its flow set them)
        if (!r.own.empty())
            for (std::size_t k = 0; k < own_.size(); ++k)
                if (own_[k] >= kVblankCountdowns && own_[k] < kVblankCountdowns + 12 && r.own[k] != 0) --r.own[k];
        if (r.pending == frame) link_run(r, frame), r.pending = ~0ull; // (the frame's end: its link as it is)
        const Timer nb{netboard_seconds_, timer_depth_};
        r.netboard->end_frame();
    }
}

} // namespace tools::remote
