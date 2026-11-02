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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <span>
#include <string>
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
constexpr uint32_t kTaskList = 0x110000, k2dTask = 0x135C0;
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
    {0x1062C7, 0x1062C8}, {0x1062DC, 0x1062DD}, {0x100100, 0x100120}, {0x140104, 0x140108}, {0x140300, 0x140304},
    {0x1A34A0, 0x1A34C0}, {0x1A74A0, 0x1A74C0}, {0x1AB4A0, 0x1AB4C0}, {0x1AF4A0, 0x1AF4C0}, {0x1B34A0, 0x1B34C0},
    {0x1B74A0, 0x1B74C0}, {0x1BB4A0, 0x1BB4C0}, {0x1BF4A0, 0x1BF4C0}, {0x1C34A0, 0x1C34C0}, {0x1C74A0, 0x1C74C0},
    {0x1CB4A0, 0x1CB4C0}, {0x1CF4A0, 0x1CF4C0},
    {0x1A0003, 0x1A0004}, {0x1A4003, 0x1A4004}, {0x1A8003, 0x1A8004}, {0x1AC003, 0x1AC004}, {0x1B0003, 0x1B0004},
    {0x1B4003, 0x1B4004}, {0x1B8003, 0x1B8004}, {0x1BC003, 0x1BC004}, {0x1C0003, 0x1C0004}, {0x1C4003, 0x1C4004},
    {0x1C8003, 0x1C8004}, {0x1CC003, 0x1CC004}};

inline std::vector<uint32_t> own_addresses() {
    std::vector<uint32_t> out;
    for (const auto& [lo, hi] : kCabinetOwn)
        for (uint32_t a = lo; a < hi; ++a) out.push_back(a);
    return out;
}
// A player's object (0..3), an enemy's (0..7).
constexpr uint32_t player_object(std::size_t seat) { return 0x1A0000 + 0x4000 * static_cast<uint32_t>(seat); }
constexpr uint32_t character_object(uint32_t k) { return k < 4 ? 0x1A0000 + 0x4000 * k : 0x1B0000 + 0x4000 * (k - 4); }

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
        for (std::size_t k = 0; k < own_.size(); ++k)
            for (uint32_t ch = 0; ch < 12; ++ch)
                if (own_[k] == character_object(ch) + 3) own_mask_[k] = 0x18;
    }

    // Another cabinet: its seat, its logic count less this board's at the
    // same time (+1 the master seen from a slave, -1 a slave from the
    // master, 0 one slave from another), and how its side runs start (asked
    // at this board's flows until it can; false: not yet).
    void add(std::size_t seat, int ahead, Seeder seeder) {
        Remote r;
        r.seat = seat, r.ahead = ahead, r.seeder = std::move(seeder);
        remotes_.push_back(std::move(r));
    }
    // From now its blocks are made here, into its network board (its main
    // board gone): `ram` its main board's work RAM as it stopped (its link
    // memory, link variables and own variables taken from it), after its
    // frame `last` (its link run there already).
    void start_making(std::size_t seat, m3::NetBoardDevice& netboard, std::span<const uint8_t> ram, uint64_t last) {
        Remote* r = find(seat);
        if (!r) return;
        auto word = [&](uint32_t a) { return uint32_t(ram[a]) << 24 | uint32_t(ram[a + 1]) << 16 | uint32_t(ram[a + 2]) << 8 | ram[a + 3]; };
        r->netboard = &netboard, r->making = true;
        for (uint32_t i = 0; i < kBlockWords; ++i) r->block[i] = word(kBlock + 4 * i);
        r->link.assign(ram.begin() + kLinkMemory, ram.begin() + kLinkMemory + kLinkBytes);
        r->link_vars.clear();
        for (const auto& [lo, hi] : kLinkVars)
            for (uint32_t a = lo; a < hi; ++a) r->link_vars.push_back(ram[a]);
        r->state_own.resize(own_.size());
        for (std::size_t k = 0; k < own_.size(); ++k) r->state_own[k] = ram[own_[k]];
        r->logic = word(kLogicCounter), r->logic_frame = last, r->frame_at[r->logic] = last, r->flow_applied = r->logic;
        r->written_frame = last;
    }
    // Its main board's frame `frame` lost (its work ran past the next
    // wake-up): no block written that frame, so its network board sends
    // the older bank again.
    void miss(std::size_t seat, uint64_t frame) {
        if (Remote* r = find(seat)) r->missed.push_back(frame);
    }
    // What a shadow of it (a whole board run as it, from this board's state
    // where it has finished count `count`, at 0x36E94) needs of it: its own
    // variables and camera after that count, its link memory and variables
    // after its link of it, its working record (its next record, made).
    struct Shadow {
        std::vector<uint8_t> own, camera, link, link_vars, regions;
        std::array<uint32_t, 0x80> working{};
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
        Remote* r = find(seat);
        const uint32_t logic = bus_.read32(kLogicCounter);
        if (!r || r->ahead <= 0 || !r->making || r->camera.empty() || r->own_logic + 1 != logic) return false;
        capture_ = true;
        side_run(*r, kModeDispatch, nullptr, true, logic, 0, true);
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
        if (Remote* r = find(seat))
            r->making = false, r->shadowed = true, r->pending = ~0ull, r->camera.clear(), r->hand_back_asked = false, r->stop_after = 0;
    }
    // Its main board still running, about to go (its network board `netboard`): taken as its shadow, so that
    // ask_hand_back hands it over as a shadow's (where it stops, a slave's next count caught up here).
    void running(std::size_t seat, m3::NetBoardDevice& netboard) {
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
        if (Remote* r = find(seat); r && r->shadowed && !r->hand_back_asked) r->hand_back_asked = true, r->stop_after = 0;
    }
    uint32_t stop_after(std::size_t seat) {
        const Remote* r = find(seat);
        return r && r->shadowed ? r->stop_after : 0;
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

    // In a side run (the board's other hooks are skipped then too).
    bool inside() const { return inside_; }
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
    uint64_t journal_bytes() const { return journal_bytes_; } // (RAM bytes its side runs wrote, put back after)
    uint64_t side_runs() const { return side_runs_; }
    std::string failed() const { return failed_; }

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
        bool shadowed = false; // (a shadow board runs it: nothing here)
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
    };

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
    void side_run(Remote& r, uint32_t pc, uint32_t* flags, bool keep, uint32_t count = 0, uint32_t character = 0, bool tasks = false);
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
    void put_regions(const std::vector<uint8_t>& regions) {
        const auto ram = bus_.ram();
        const uint32_t mask = bus_.read32(kLinkPlayers);
        std::size_t n = 0, i = 0;
        for (const auto& [lo, hi] : kOwnRegions) {
            if (region_own(i++, mask))
                for (uint32_t a = lo; a < hi; ++a)
                    if (ram[a] != regions[n + a - lo]) bus_.write8(a, regions[n + a - lo]);
            n += hi - lo;
        }
    }
    bool missed(const Remote& r, uint64_t frame) const { return std::find(r.missed.begin(), r.missed.end(), frame) != r.missed.end(); }
    // Its frame for count `logic` (its buttons then): counted on from its last
    // count while made here, missed frames skipped; before, this board's less its offset.
    uint64_t frame_for(const Remote& r, uint32_t logic) const;
    void make_record(Remote& r, Block& next, const Block& mine, uint32_t logic, uint64_t frame, uint32_t old);
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
    std::size_t local_;
    Held held_;
    uint64_t write_at_;
    std::vector<uint32_t> own_;
    std::vector<uint8_t> own_mask_; // (the bits of each that are a cabinet's own)
    std::vector<Remote> remotes_;
    std::map<uint32_t, Players> players_at_; // (by count)
    bool inside_ = false, in_view_ = false, in_update_ = false, in_link_ = false, in_tasks_ = false;
    std::vector<uint32_t> ahead_record_; // (the working record after a flow run on the side)
    std::map<uint32_t, std::vector<ppc::CpuState>> view_chars_; // (this board's view tests, by count: each as called, r14 the character)
    const std::vector<uint8_t>* world_ = nullptr;          // (a side run's world instead of this board's: a catch-up's)
    bool capture_ = false;                                 // (the master's side run: its RAM kept, for its shadow)
    bool in_drawing_ = false;                              // (that run going through its drawing)
    uint32_t drawing_r1_ = 0;                              // (its stack pointer as its tasks were called)
    std::vector<uint8_t> captured_;
    ppc::CpuState captured_cpu_{};
    uint32_t idle_msr_ = 0; // (this board's MSR at its last 0x36F24)
    bool tasks_mode_ = false;
    uint64_t made_ = 0, side_runs_ = 0;
    uint64_t journal_bytes_ = 0;
    uint64_t heavy_frames_ = 0, first_heavy_ = 0;
    double side_seconds_ = 0, whole_seconds_ = 0, netboard_seconds_ = 0;
    int timer_depth_ = 0;
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
    std::string failed_;
};

// ---- the implementation ----

struct LinkSkipped {};  // (a link run on the side holding its frame back: done)
struct TasksDrawing {}; // (a frame's tasks run on the side reaching the drawing: done)
struct DrawingDone {};  // (the master's capture through its drawing reaching its main loop's 0x36E94)

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

inline void Cabinets::side_run(Remote& r, uint32_t pc, uint32_t* flags, bool keep_camera, uint32_t count, uint32_t character, bool tasks) {
    const Timer whole{whole_seconds_, timer_depth_};
    const uint32_t logic = bus_.read32(kLogicCounter);
    const recomp::Context::Aside aside = c_.set_aside();
    bus_.journal(true);
    if (world_) { // (a catch-up's: this board's world as it was then)
        const auto ram = bus_.ram();
        for (uint32_t i = 0; i < world_->size(); ++i)
            if (ram[i] != (*world_)[i]) bus_.write8(i, (*world_)[i]);
    }
    if (capture_ && r.link.size() == kLinkBytes) { // (the master's shadow: its link memory and variables, as its last link left them)
        const auto ram = bus_.ram();
        for (uint32_t i = 0; i < kLinkBytes; ++i)
            if (ram[kLinkMemory + i] != r.link[i]) bus_.write8(kLinkMemory + i, r.link[i]);
        std::size_t n = 0;
        for (const auto& [lo, hi] : kLinkVars)
            for (uint32_t a = lo; a < hi; ++a) bus_.write8(a, r.link_vars[n++]);
    }
    for (uint32_t i = 0; i < kCameraBytes; ++i)
        if (bus_.read8(kCamera + i) != r.camera[i]) bus_.write8(kCamera + i, r.camera[i]);
    for (std::size_t k = 0; k < own_.size(); ++k) {
        const uint8_t here = bus_.read8(own_[k]), v = static_cast<uint8_t>((here & ~own_mask_[k]) | (r.own[k] & own_mask_[k]));
        if (v != here) bus_.write8(own_[k], v);
    }
    // (its buttons, 0x5094's words, for its frame of this count; its toggles flipped once a frame)
    const uint64_t frame = frame_for(r, logic);
    const uint32_t now = held_(r.seat, frame), was = held_(r.seat, frame - 1);
    bus_.write32(0x100100, now), bus_.write32(0x100104, now & ~was), bus_.write32(0x100108, was & ~now);
    if (r.toggled_at != frame) bus_.write32(0x10010C, bus_.read32(0x10010C) ^ (now & ~was)), r.toggled_at = frame;
    // (its local player, its number, its time's entry: as 0x11C3C sets them)
    bus_.write32(0x104024, player_object(r.seat));
    bus_.write8(0x10065B, static_cast<uint8_t>(r.seat + 1));
    bus_.write32(0x106008, 0x1062AE + 2 * static_cast<uint32_t>(r.seat + 1));
    c_.set_scheduler(nullptr);
    c_.cpu.msr &= ~recomp::Context::kMsrEE;
    c_.limit_aside(30'000'000);
    inside_ = true;
    bus_.hold_device_writes(true);
    std::vector<uint8_t> before; // (its flow's: its own bytes before, for what it changed)
    if (pc == kModeDispatch)
        for (uint32_t a : own_) before.push_back(bus_.read8(a));
    if ((pc == kAnimationStep || tasks) && r.regions.size() == kRegionBytes) put_regions(r.regions); // (its own areas: its 2D, its poses)
    const auto started_at = std::chrono::steady_clock::now();
    try {
        if (pc == kViewTest) { // (with this camera's view matrix, as the game has it in f20-f31 then)
            if (character) c_.cpu.gpr[14] = character;
            for (int k = 0; k < 12; ++k) {
                const uint32_t bits = bus_.read32(kCamera + kViewMatrix + 4 * static_cast<uint32_t>(k));
                float f;
                std::memcpy(&f, &bits, 4);
                const double d = f;
                std::memcpy(&c_.cpu.fpr[static_cast<std::size_t>(20 + k)], &d, 8);
            }
            c_.call(pc);
        } else if (pc == kCameraUpdate) { // (the update, then what makes its view)
            c_.call(pc);
            c_.cpu = aside.cpu;
            c_.cpu.msr &= ~recomp::Context::kMsrEE;
            c_.cpu.gpr[12] = kCamera;
            c_.call(kCameraView);
        } else {
            c_.call(pc);
            if (pc == kModeDispatch && tasks) { // (and its frame's tasks, up to the drawing: the world's step)
                c_.cpu = aside.cpu;
                c_.cpu.msr &= ~recomp::Context::kMsrEE;
                // (its 2D task too, on its own 2D area: a banner's unpacking shows in its work)
                if (capture_) { // (the master's shadow: on through its drawing and the main loop to its 0x36E94, kept
                                // there to go on from: its 2D task as it was)
                    in_drawing_ = true, drawing_r1_ = aside.cpu.gpr[1];
                    try {
                        c_.call(kTasks);
                        c_.call(kTasksReturn); // (the main loop on after its tasks: 0xCB84, then 0x36E94)
                    } catch (const DrawingDone&) {
                        captured_cpu_ = c_.cpu, captured_cpu_.pc = kBeforeIdle;
                        captured_.assign(bus_.ram().begin(), bus_.ram().end());
                    }
                    in_drawing_ = false;
                } else {
                    in_tasks_ = true;
                    const uint64_t t0 = c_.cycles();
                    try {
                        c_.call(kTasks);
                    } catch (const TasksDrawing&) {
                    }
                    in_tasks_ = false;
                    if (c_.cycles() - t0 > kHeavyWork && !heavy_frames_++) first_heavy_ = timing_.frames();
                }
                c_.cpu = aside.cpu; // (and what makes its camera's view, which the drawing does)
                c_.cpu.msr &= ~recomp::Context::kMsrEE;
                c_.cpu.gpr[12] = kCamera;
                c_.call(kCameraView);
            }
        }
    } catch (const std::exception& e) {
        in_tasks_ = in_drawing_ = false;
        if (failed_.empty()) failed_ = "seat " + std::to_string(r.seat + 1) + ", side run at " + std::to_string(pc) + ": " + e.what();
    }
    side_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
    ++side_runs_;
    inside_ = false;
    bus_.hold_device_writes(false);
    if (keep_camera)
        for (uint32_t i = 0; i < kCameraBytes; ++i) r.camera[i] = bus_.read8(kCamera + i);
    for (std::size_t k = 0; k < own_.size(); ++k) r.own[k] = bus_.read8(own_[k]);
    if ((pc == kAnimationStep || tasks) && r.regions.size() == kRegionBytes) r.regions = gather_regions(bus_.ram());
    if (flags) *flags = bus_.read32(character ? character : aside.cpu.gpr[14]);
    if (pc == kModeDispatch) {
        ahead_record_.resize(0x80);
        for (uint32_t i = 0; i < 0x80; ++i) ahead_record_[i] = bus_.read32(kWorkingRecord + 4 * i);
        auto& d = r.flow_delta[count];
        d.clear();
        for (std::size_t k = 0; k < own_.size(); ++k)
            if (r.own[k] != before[k]) d.emplace_back(static_cast<uint32_t>(k), r.own[k]);
        while (!r.flow_delta.empty() && r.flow_delta.begin()->first + 16 < count) r.flow_delta.erase(r.flow_delta.begin());
    }
    journal_bytes_ += bus_.journal_size();
    bus_.undo_journal();
    bus_.journal(false);
    c_.put_back(aside);
}

inline void Cabinets::shake(Remote& r, uint32_t kind) {
    const uint32_t entry = kShakeTable + 8 * (kind - 1);
    r.camera[0xD2] = r.camera[0xD3] = bus_.read8(entry);
    for (uint32_t k = 0; k < 4; ++k) r.camera[0xD4 + k] = bus_.read8(entry + 4 + k);
}

inline bool Cabinets::hook(uint32_t pc) {
    if (in_drawing_) { // (the master's capture: on through its drawing to its main loop's 0x36E94)
        if (pc == kVblankLoop) bus_.write8(kVblankCount, static_cast<uint8_t>(bus_.read8(kVblankCount) + 1));
        if (pc == kTransferLoop) bus_.write8(kTransferBusy, 0);
        if (pc == kBeforeIdle) throw DrawingDone{};
        return true;
    }
    if (in_link_ && pc == kLinkSkipped) throw LinkSkipped{};
    if (in_tasks_ && pc == kVblankWait) throw TasksDrawing{};
    if (inside_) { // (a transfer its tasks wait for (a banner's tiles): done at once, its device writes held)
        if (pc == kTransferLoop) bus_.write8(kTransferBusy, 0);
        return true;
    }
    if (pc == kBeforeIdle) { // (this board's MSR there, which a master's shadow starting there takes)
        idle_msr_ = c_.cpu.msr;
        return true;
    }
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
        Players& at = players_at_[logic];
        for (std::size_t k = 0; k < 4; ++k) {
            const uint32_t p = player_object(k);
            at.p[k] = {bus_.read32(p), bus_.read32(p + 0x44), bus_.read16(p + 0x2A), bus_.read16(p + 0x74), bus_.read16(p + 0x76)};
        }
        while (!players_at_.empty() && players_at_.begin()->first + 16 < logic) players_at_.erase(players_at_.begin());
        return true;
    }
    case kModeDispatch:
        for (Remote& r : remotes_) {
            if (r.hand_back_asked && r.shadowed && r.stop_after == 0) { // (a hand-back: where its shadow stops; a slave's world)
                r.stop_after = logic + static_cast<uint32_t>(r.ahead);
                if (r.ahead < 0) r.saved_world.assign(bus_.ram().begin(), bus_.ram().end()), r.saved_count = logic;
            }
            if (r.catch_up && !r.shadowed) { // (handed back, a slave: its count after the shadow's last, on the world kept)
                r.catch_up = false;
                if (r.saved_count == r.own_logic + 1) {
                    world_ = &r.saved_world;
                    side_run(r, pc, nullptr, true, r.saved_count, 0, true);
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
                            side_run(r, kViewTest, &flags, false, 0, ch);
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
            for (uint32_t k = 0; k < runs; ++k) side_run(r, pc, nullptr, true, logic - runs + k + 1, 0, tasks_mode_ && k + 1 == runs);
            r.own_logic = logic;
            if (tasks_mode_) { // (its camera's update was in its tasks: its view of its own player now, its heading)
                uint32_t flags = 0;
                side_run(r, kViewTest, &flags, false, 0, player_object(r.seat));
                r.view_at[{logic, player_object(r.seat)}] = flags;
                r.heading_at[logic] = static_cast<uint16_t>(r.camera[kCameraHeading] << 8 | r.camera[kCameraHeading + 1]);
                while (!r.heading_at.empty() && r.heading_at.begin()->first + 16 < logic) r.heading_at.erase(r.heading_at.begin());
                while (!r.view_at.empty() && r.view_at.begin()->first.first + 16 < logic) r.view_at.erase(r.view_at.begin());
            }
        }
        return true;
    case kCameraUpdate:
        in_update_ = true, in_view_ = false;
        if (tasks_mode_) return true; // (run in its tasks)
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            if (on_camera) on_camera(r.seat, logic, r.camera);
            side_run(r, pc, nullptr, true);
            { // (its view of its own player too, now: its next link carries it, and this board's own test of it can
              // come late, after the frame's end)
                uint32_t flags = 0;
                side_run(r, kViewTest, &flags, false, 0, player_object(r.seat));
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
            if (!r.camera.empty() && r.regions.size() == kRegionBytes) side_run(r, pc, nullptr, false);
        return true;
    case kViewTest:
        in_view_ = false;
        view_chars_[logic].push_back(c_.cpu);
        while (view_chars_.size() > 4) view_chars_.erase(view_chars_.begin());
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            uint32_t flags = 0;
            side_run(r, pc, &flags, false);
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
    if (self.inside_ || self.in_view_ || self.in_update_ || self.tasks_mode_) return; // (tasks mode: its own tasks make them)
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
    if (from < 0 && logic == mine[0x1C / 4] + 2 && ahead_record_.size() == 0x80) // (two ahead: its own flow's, just run)
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
    next[(rec + 0x08) / 4] = p[2] << 16 | ((p[1] & 0x80000000u) ? p[4] : p[3]);
    // (its camera's heading after its update of the count before; else as it was)
    const auto heading = r.heading_at.find(logic - 1);
    next[(rec + 0x0C) / 4] = (heading != r.heading_at.end() ? uint32_t{heading->second} << 16 : r.block[(old + 0x0C) / 4] & 0xFFFF0000u) |
                             (next[(rec + 0x0C) / 4] & 0xFFFFu);
    // (its player's first word, its view bits from its camera's test of the count before)
    next[(rec + 0x10) / 4] = p[0];
    if (const auto v = r.view_at.find({logic - 1, player}); v != r.view_at.end()) // (its own view bits; the rest the shared game's)
        next[(rec + 0x10) / 4] = (p[0] & ~0x18u) | (v->second & 0x18u);
    // (the empty slots' leftovers, as they were: players 3 and 4's +4 and +8 (+0x30 and +0x34, +0x3C and +0x40), each
    // cabinet's own while their seat is empty (0x74808 writes them from its unused players' objects), the game's once a
    // player is in it (+4: with three players, player 4's +7 was the third cabinet's own); and the on-screen bits)
    const uint32_t players = bus_.read32(kLinkPlayers);
    if (!(players & (0x80000000u >> 2)))
        for (uint32_t at : {0x30u, 0x34u}) next[(rec + at) / 4] = r.block[(old + at) / 4];
    if (!(players & (0x80000000u >> 3)))
        for (uint32_t at : {0x3Cu, 0x40u}) next[(rec + at) / 4] = r.block[(old + at) / 4];
    for (uint32_t byte : {0x47u, 0x5Fu, 0x6Bu, 0x77u}) put8(next, rec + byte, get8(r.block, old + byte));
    // (every character's view bits, players 1-4 then enemies 1-8, from its camera's tests two counts before; none
    // for one gone since: this board's entry all zero)
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t entry = rec + 0x14 + 12 * k;
        const auto v = r.view_at.find({logic - 2, character_object(k)});
        if (v != r.view_at.end() && next[entry / 4] != 0) put8(next, entry + 3, static_cast<uint8_t>(v->second));
    }
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
    const Timer whole{whole_seconds_, timer_depth_};
    const uint32_t logic = r.logic + 1; // (the count it goes on to, unless held back)
    // (two counts ahead of this board (this one missed a frame): its flow of the count before run now, on this
    // board's world after the count before that, its working record from it)
    ahead_record_.clear();
    if (logic == bus_.read32(kLogicCounter) + 2 && r.own_logic < logic - 1 && !r.camera.empty()) {
        side_run(r, kModeDispatch, nullptr, false, logic - 1, 0, true); // (its camera's update for it comes at its hook)
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
    for (uint32_t i = 0; i < kLinkBytes; ++i)
        if (ram[kLinkMemory + i] != r.link[i]) bus_.write8(kLinkMemory + i, r.link[i]);
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
    for (uint32_t s = 0; s < 4; ++s) {
        const uint32_t at = kBlock + 0x100 + 0x200 * s, x = bus_.read32(at + 0xDC);
        if (x > r.logic || x + 3 < r.logic) continue;
        if (const auto h = r.heading_at.find(x - 1); h != r.heading_at.end()) bus_.write16(at + 0x0C, h->second);
        if (const auto v = r.view_at.find({x - 1, player_object(r.seat)}); v != r.view_at.end())
            bus_.write8(at + 0x13, static_cast<uint8_t>(v->second));
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
    inside_ = in_link_ = true;
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
    side_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
    ++side_runs_;
    bus_.borrow_netboard(nullptr);
    bus_.hold_device_writes(false);
    inside_ = in_link_ = false;
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
    if (r.regions.size() == kRegionBytes) r.regions = gather_regions(ram);
    for (uint32_t i = 0; i < kBlockWords; ++i) r.block[i] = bus_.read32(kBlock + 4 * i);
    const uint32_t now_logic = bus_.read32(kLogicCounter);
    journal_bytes_ += bus_.journal_size();
    bus_.undo_journal();
    bus_.journal(false);
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
    r->own_snap.clear(), r->link_snap.clear(), r->flow_delta.clear(), r->heading_at.clear(), r->view_at.clear();
    // (what its records of the counts to come take from its camera's runs of this one: its heading, its view bits)
    const uint32_t count = seed.own_logic;
    r->heading_at[count] = static_cast<uint16_t>(seed.camera[kCameraHeading] << 8 | seed.camera[kCameraHeading + 1]);
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t ch = character_object(k);
        r->view_at[{count, ch}] = uint32_t(ram[ch]) << 24 | uint32_t(ram[ch + 1]) << 16 | uint32_t(ram[ch + 2]) << 8 | ram[ch + 3];
    }
}

inline bool Cabinets::shadow_state(std::size_t seat, uint32_t count, Shadow& out) {
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
